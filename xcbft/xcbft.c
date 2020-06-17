#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_renderutil.h>

#include "../utf8_utils/utf8.h"
#include "xcbft.h"

void
xcbft_done(void)
{
	FcFini();
}

int
xcbft_init(void)
{
	FcBool status;

	status = FcInit();
	if (status == FcFalse) {
		fprintf(stderr, "Could not initialize fontconfig");
	}

	return status == FcTrue;
}

/*
 * Do the font queries through fontconfig and return the info
 *
 * Assumes:
 *	Fontconfig is already init & cleaned outside
 *	the FcPattern return needs to be cleaned outside
 */
FcPattern*
xcbft_query_fontsearch(FcChar8 *fontquery)
{
	FcBool status;
	FcPattern *fc_finding_pattern, *pat_output;
	FcResult result;

	fc_finding_pattern = FcNameParse(fontquery);

	/* to match we need to fix the pattern (fill unspecified info) */
	FcDefaultSubstitute(fc_finding_pattern);
	status = FcConfigSubstitute(NULL, fc_finding_pattern, FcMatchPattern);
	if (status == FcFalse) {
		fprintf(stderr, "could not perform config font substitution");
		return NULL;
	}

	pat_output = FcFontMatch(NULL, fc_finding_pattern, &result);

	FcPatternDestroy(fc_finding_pattern);
	if (result == FcResultMatch) {
		return pat_output;
	} else if (result == FcResultNoMatch) {
		fprintf(stderr, "there wasn't a match");
	} else {
		fprintf(stderr, "the match wasn't as good as it should be");
	}
	return NULL;
}

/*
 * Query a font based on character support
 * Optionally pass a pattern that it'll use as the base for the search
 *
 * fallback of font added to the list or somehow done when drawing,
 * to do that we need to search by the charset we want to draw
 * and if they are in one of the font already specified, thus need to
 * know what the users want to insert as text. This needs more thinking
 * to be decoupled.

	Assumes the ft2 library is already loaded
	Assumes the face will be cleaned outside
 */
struct xcbft_face_holder
xcbft_query_by_char_support(FcChar32 character,
		const FcPattern *copy_pattern, long dpi)
{
	FcBool status;
	FcResult result;
	FcCharSet *charset;
	FcPattern *charset_pattern, *pat_output;
	struct xcbft_patterns_holder patterns;
	struct xcbft_face_holder faces = {0};

	/* add characters we need to a charset */
	charset = FcCharSetCreate();
	FcCharSetAddChar(charset, character);

	/* if we pass a pattern then copy it to get something close */
	if (copy_pattern != NULL) {
		charset_pattern = FcPatternDuplicate(copy_pattern);
	} else {
		charset_pattern = FcPatternCreate();
	}

	/* use the charset for the pattern search */
	FcPatternAddCharSet(charset_pattern, FC_CHARSET, charset);
	/* also force it to be scalable */
	FcPatternAddBool(charset_pattern, FC_SCALABLE, FcTrue);

	/* default & config substitutions, the usual */
	FcDefaultSubstitute(charset_pattern);
	status = FcConfigSubstitute(NULL, charset_pattern, FcMatchPattern);
	if (status == FcFalse) {
		fprintf(stderr, "could not perform config font substitution");
		FcCharSetDestroy(charset);
		return faces;
	}

	pat_output = FcFontMatch(NULL, charset_pattern, &result);

	FcPatternDestroy(charset_pattern);

	if (result != FcResultMatch) {
		fprintf(stderr, "there wasn't a match");
		FcCharSetDestroy(charset);
		return faces;
	}

	patterns.patterns = malloc(sizeof(FcPattern *));
	patterns.length = 1;
	patterns.patterns[0] = pat_output;

	faces = xcbft_load_faces(patterns, dpi);

	/* cleanup */
	xcbft_patterns_holder_destroy(patterns);
	FcCharSetDestroy(charset);

	return faces;
}

struct xcbft_patterns_holder
xcbft_query_fontsearch_all(FcStrSet *queries)
{
	struct xcbft_patterns_holder font_patterns;
	FcPattern *font_pattern;
    FcStrList *iterator;
    FcChar8 *fontquery = NULL;
	uint8_t current_allocated;

	font_patterns.patterns = NULL;
	font_patterns.length = 0;

	/* start with 5, expand if needed */
	current_allocated = 5;
	font_patterns.patterns = malloc(sizeof(FcPattern *)*current_allocated);

	/* safely iterate over set */
	iterator = FcStrListCreate(queries);
	FcStrListFirst(iterator);
	while ((fontquery = FcStrListNext(iterator)) != NULL) {
		font_pattern = xcbft_query_fontsearch(fontquery);
		if (font_pattern != NULL) {
			if (font_patterns.length + 1 > current_allocated) {
				current_allocated += 5;
				font_patterns.patterns = realloc(
					font_patterns.patterns,
					sizeof(FcPattern*) * current_allocated);
			}
			font_patterns.patterns[font_patterns.length] = font_pattern;
			font_patterns.length++;
		}
	}
	FcStrListDone(iterator);
	/* end of safely iterate over set */

	return font_patterns;
}

/* There's no way to predict the width and height of every characters */
/* as they can go outside the em, so let's just use whatever we have as */
/* the biggest pixel size so far in all the loaded patterns */
double
xcbft_get_pixel_size(struct xcbft_patterns_holder patterns)
{
	int i;
	double maximum_pix_size;
	FcValue fc_pixel_size;
	FcResult result;

	maximum_pix_size = 0;
	for (i = 0; i < patterns.length; i++) {
		result = FcPatternGet(patterns.patterns[i], FC_PIXEL_SIZE, 0, &fc_pixel_size);
		if (result != FcResultMatch || fc_pixel_size.u.d == 0) {
			fprintf(stderr, "font has no pixel size, using 12 by default");
			fc_pixel_size.type = FcTypeInteger;
			fc_pixel_size.u.d = 12.0;
		}
		if (fc_pixel_size.u.d > maximum_pix_size) {
			maximum_pix_size = fc_pixel_size.u.d;
		}
	}

	return maximum_pix_size;
}

struct xcbft_face_holder
xcbft_load_faces(struct xcbft_patterns_holder patterns, long dpi)
{
	int i;
	struct xcbft_face_holder faces;
	FcResult result;
	FcValue fc_file, fc_index, fc_matrix, fc_pixel_size;
	FT_Matrix ft_matrix;
	FT_Error error;
	FT_Library library;

	faces.length = 0;
	error = FT_Init_FreeType(&library);
	if (error != FT_Err_Ok) {
		perror(NULL);
		return faces;
	}

	/* allocate the same size as patterns as it should be <= its length */
	faces.faces = malloc(sizeof(FT_Face)*patterns.length);

	for (i = 0; i < patterns.length; i++) {
		/* get the information needed from the pattern */
		result = FcPatternGet(patterns.patterns[i], FC_FILE, 0, &fc_file);
		if (result != FcResultMatch) {
			fprintf(stderr, "font has not file location");
			continue;
		}
		result = FcPatternGet(patterns.patterns[i], FC_INDEX, 0, &fc_index);
		if (result != FcResultMatch) {
			fprintf(stderr, "font has no index, using 0 by default");
			fc_index.type = FcTypeInteger;
			fc_index.u.i = 0;
		}
		/* TODO: load more info like */
		/*	autohint */
		/*	hinting */
		/*	verticallayout */

		/* load the face */
		error = FT_New_Face(
				library,
				(const char *) fc_file.u.s,
				fc_index.u.i,
				&(faces.faces[faces.length]) );
		if (error == FT_Err_Unknown_File_Format) {
			fprintf(stderr, "wrong file format");
			continue;
		} else if (error == FT_Err_Cannot_Open_Resource) {
			fprintf(stderr, "could not open resource");
			continue;
		} else if (error) {
			fprintf(stderr, "another sort of error");
			continue;
		}
		if (faces.faces[faces.length] == NULL) {
			fprintf(stderr, "face was empty");
			continue;
		}

		result = FcPatternGet(patterns.patterns[i], FC_MATRIX, 0, &fc_matrix);
		if (result == FcResultMatch) {
			ft_matrix.xx = (FT_Fixed)(fc_matrix.u.m->xx * 0x10000L);
			ft_matrix.xy = (FT_Fixed)(fc_matrix.u.m->xy * 0x10000L);
			ft_matrix.yx = (FT_Fixed)(fc_matrix.u.m->yx * 0x10000L);
			ft_matrix.yy = (FT_Fixed)(fc_matrix.u.m->yy * 0x10000L);

			/* apply the matrix */
			FT_Set_Transform(
				faces.faces[faces.length],
				&ft_matrix,
				NULL);
		}

		result = FcPatternGet(patterns.patterns[i], FC_PIXEL_SIZE, 0, &fc_pixel_size);
		if (result != FcResultMatch || fc_pixel_size.u.d == 0) {
			fprintf(stderr, "font has no pixel size, using 12 by default");
			fc_pixel_size.type = FcTypeInteger;
			fc_pixel_size.u.d = 12;
		}
		/*error = FT_Set_Pixel_Sizes( */
		/*	faces.faces[faces.length], */
		/*	0, // width */
		/*	fc_pixel_size.u.d); // height */

		/* pixel_size/ (dpi/72.0) */
		FT_Set_Char_Size(
			faces.faces[faces.length], 0,
			(fc_pixel_size.u.d/((double)dpi/72.0))*64,
			dpi, dpi);
		if (error != FT_Err_Ok) {
			perror(NULL);
			fprintf(stderr, "could not char size");
			continue;
		}

		faces.length++;
	}

	faces.library = library;

	return faces;
}

FcStrSet*
xcbft_extract_fontsearch_list(char *string)
{
	FcStrSet *fontsearch = NULL;
	FcChar8 *fontquery;
	FcBool result = FcFalse;
	char *r = strdup(string);
	char *p_to_r = r;
	char *token = NULL;

	fontsearch = FcStrSetCreate();

	token = strtok(r, ",");
	while (token != NULL) {
		fontquery = (FcChar8*)token;
		result = FcStrSetAdd(fontsearch, fontquery);
		if (result == FcFalse) {
			fprintf(stderr,
				"Couldn't add fontquery to fontsearch set");
		}
		token = strtok(NULL, ",");
	}

	free(p_to_r);

	return fontsearch;
}



void
xcbft_patterns_holder_destroy(struct xcbft_patterns_holder patterns)
{
	int i = 0;

	for (; i < patterns.length; i++) {
		FcPatternDestroy(patterns.patterns[i]);
	}
	free(patterns.patterns);
}

void
xcbft_face_holder_destroy(struct xcbft_face_holder faces)
{
	int i = 0;

	for (; i < faces.length; i++) {
		FT_Done_Face(faces.faces[i]);
	}
	if (faces.faces) {
		free(faces.faces);
	}
	FT_Done_FreeType(faces.library);
}

xcb_render_picture_t
xcbft_create_pen(xcb_connection_t *c, xcb_render_color_t color)
{
	const xcb_render_query_pict_formats_reply_t *fmt_rep = xcb_render_util_query_formats(c);
	/* alpha can only be used with a picture containing a pixmap */
	xcb_render_pictforminfo_t *fmt = xcb_render_util_find_standard_format(
		fmt_rep,
		XCB_PICT_STANDARD_ARGB_32
	);

	xcb_drawable_t root = xcb_setup_roots_iterator(
			xcb_get_setup(c)
		).data->root;

	xcb_pixmap_t pm = xcb_generate_id(c);
    xcb_rectangle_t rect = {0, 0, 1, 1};
	xcb_render_picture_t picture = xcb_generate_id(c);

	uint32_t values[1];
	values[0] = XCB_RENDER_REPEAT_NORMAL;

	xcb_create_pixmap(c, 32, pm, root, 1, 1);
	xcb_render_create_picture(c,
		picture,
		pm,
		fmt->id,
		XCB_RENDER_CP_REPEAT,
		values);

	xcb_render_fill_rectangles(c,
		XCB_RENDER_PICT_OP_OVER,
		picture,
		color, 1, &rect);

	xcb_free_pixmap(c, pm);
	return picture;
}

struct xcbft_glyphset_and_advance
xcbft_load_glyphset(
	xcb_connection_t *c,
	struct xcbft_face_holder faces,
	struct utf_holder text,
	long dpi)
{
	unsigned int i, j;
	int glyph_index;
	xcb_render_glyphset_t gs;
	xcb_render_pictforminfo_t *fmt_a8;
	struct xcbft_face_holder faces_for_unsupported;
	const xcb_render_query_pict_formats_reply_t *fmt_rep =
		xcb_render_util_query_formats(c);
	FT_Vector total_advance, glyph_advance;
	struct xcbft_glyphset_and_advance glyphset_advance;

	total_advance.x = total_advance.y = 0;
	glyph_index = 0;
	faces_for_unsupported.length = 0;
	/* create a glyphset with a specific format */
	fmt_a8 = xcb_render_util_find_standard_format(
		fmt_rep,
		XCB_PICT_STANDARD_A_8
	);
	gs = xcb_generate_id(c);
	xcb_render_create_glyph_set(c, gs, fmt_a8->id);

	for (i = 0; i < text.length; i++) {
		for (j = 0; j < faces.length; j++) {
			glyph_index = FT_Get_Char_Index(
				faces.faces[j],
				text.str[i]);
			if (glyph_index != 0) break;
		}
		/* here use face at index j */
		if (glyph_index != 0) {
			glyph_advance = xcbft_load_glyph(c, gs, faces.faces[j], text.str[i]);
			total_advance.x += glyph_advance.x;
			total_advance.y += glyph_advance.y;
		} else {
			/* fallback */
			/* TODO pass at least some of the query (font size, italic, etc..) */

			glyph_index = 0;
			/* check if we already loaded that face as fallback */
			if (faces_for_unsupported.length > 0) {
				glyph_index = FT_Get_Char_Index(
					faces_for_unsupported.faces[0],
					text.str[i]);
			}
			if (glyph_index == 0) {
				if (faces_for_unsupported.length > 0) {
					xcbft_face_holder_destroy(faces_for_unsupported);
				}
				faces_for_unsupported = xcbft_query_by_char_support(
					text.str[i],
					NULL, dpi);
			}
			if (faces_for_unsupported.length == 0) {
				fprintf(stderr,
					"No faces found supporting character: %02x\n",
					text.str[i]);
				/* draw a block using whatever font */
				glyph_advance = xcbft_load_glyph(c, gs, faces.faces[0], text.str[i]);
				total_advance.x += glyph_advance.x;
				total_advance.y += glyph_advance.y;
			} else {
				FT_Set_Char_Size(
						faces_for_unsupported.faces[0],
						0, (faces.faces[0]->size->metrics.x_ppem/((double)dpi/72.0))*64,
						dpi, dpi);

				glyph_advance = xcbft_load_glyph(c, gs,
					faces_for_unsupported.faces[0],
					text.str[i]);
				total_advance.x += glyph_advance.x;
				total_advance.y += glyph_advance.y;
			}
		}
	}
	if (faces_for_unsupported.length > 0) {
		xcbft_face_holder_destroy(faces_for_unsupported);
	}

	glyphset_advance.advance = total_advance;
	glyphset_advance.glyphset = gs;
	return glyphset_advance;
}

FT_Vector
xcbft_load_glyph(
	xcb_connection_t *c, xcb_render_glyphset_t gs, FT_Face face, int charcode)
{
	uint32_t gid;
	int glyph_index, stride, y;
    uint8_t *tmpbitmap;
	FT_Vector glyph_advance;
	xcb_render_glyphinfo_t ginfo;
	FT_Bitmap *bitmap;

	FT_Select_Charmap(face, ft_encoding_unicode);
	glyph_index = FT_Get_Char_Index(face, charcode);

	FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT);

	bitmap = &face->glyph->bitmap;

	ginfo.x = -face->glyph->bitmap_left;
	ginfo.y = face->glyph->bitmap_top;
	ginfo.width = bitmap->width;
	ginfo.height = bitmap->rows;
	glyph_advance.x = face->glyph->advance.x/64;
	glyph_advance.y = face->glyph->advance.y/64;
	ginfo.x_off = glyph_advance.x;
	ginfo.y_off = glyph_advance.y;

    /*
	 * keep track of the max horiBearingY (yMax) and yMin
	 * 26.6 fractional pixel format
	 * yMax = face->glyph->metrics.horiBearingY/64; (yMax);
	 * yMin = -(face->glyph->metrics.height -
	 *		face->glyph->metrics.horiBearingY)/64;
     */

	gid = charcode;

	stride = (ginfo.width+3)&~3;
	tmpbitmap = calloc(sizeof(uint8_t),stride*ginfo.height);

	for (y = 0; y < ginfo.height; y++)
		memcpy(tmpbitmap+y*stride, bitmap->buffer+y*ginfo.width, ginfo.width);

	xcb_render_add_glyphs_checked(c,
		gs, 1, &gid, &ginfo, stride*ginfo.height, tmpbitmap);

	free(tmpbitmap);

	xcb_flush(c);
	return glyph_advance;
}

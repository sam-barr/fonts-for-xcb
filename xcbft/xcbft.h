#ifndef _XCBFT
#define _XCBFT

struct xcbft_patterns_holder {
	FcPattern **patterns;
	uint8_t length;
};

struct xcbft_face_holder {
	FT_Face *faces;
	uint8_t length;
	FT_Library library;
};

struct xcbft_glyphset_and_advance {
	xcb_render_glyphset_t glyphset;
	FT_Vector advance;
};

int xcbft_init(void);
void xcbft_done(void);
FcPattern* xcbft_query_fontsearch(FcChar8 *);
struct xcbft_face_holder xcbft_query_by_char_support(
		FcChar32, const FcPattern *, long);
struct xcbft_patterns_holder xcbft_query_fontsearch_all(FcStrSet *);
double xcbft_get_pixel_size(struct xcbft_patterns_holder patterns);
struct xcbft_face_holder xcbft_load_faces(
	struct xcbft_patterns_holder, long);
FcStrSet* xcbft_extract_fontsearch_list(char *);
void xcbft_patterns_holder_destroy(struct xcbft_patterns_holder);
void xcbft_face_holder_destroy(struct xcbft_face_holder);
xcb_render_picture_t xcbft_create_pen(xcb_connection_t*,
		xcb_render_color_t);
struct xcbft_glyphset_and_advance xcbft_load_glyphset(xcb_connection_t *,
	struct xcbft_face_holder, struct utf_holder, long);
FT_Vector xcbft_load_glyph(xcb_connection_t *, xcb_render_glyphset_t,
	FT_Face, int);

#endif

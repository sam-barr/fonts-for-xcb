#ifndef _UTF8_UTILS_
#define _UTF8_UTILS_

struct utf_holder {
	FcChar32 *str;
	unsigned int length;
};
struct utf_holder char_to_uint32(char *str);
void utf_holder_destroy(struct utf_holder holder);

#endif

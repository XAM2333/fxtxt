#ifndef FXTXT_FONT_H
#define FXTXT_FONT_H

#define FONT_FIRST 0x20
#define FONT_LAST  0x7E
#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1)
#define FONT_GLYPHS 8178
#define GB_MAP_COUNT 7445

extern const unsigned char font_ascii8[FONT_COUNT][8];
extern const unsigned char font_cjk8[FONT_GLYPHS][8];
extern const unsigned char font_lb8[FONT_COUNT];
extern const unsigned char font_adv8[FONT_COUNT];

#endif

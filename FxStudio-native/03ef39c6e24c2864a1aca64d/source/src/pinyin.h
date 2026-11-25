#ifndef FXTXT_PINYIN_H
#define FXTXT_PINYIN_H

#define PY_COUNT 413
#define PY_CHARS 9185
#define PY_NAME_LEN 1743

extern const unsigned short py_off[PY_COUNT + 1];
extern const char py_names[];
extern const unsigned short py_start[PY_COUNT + 1];
extern const unsigned short py_glyph[PY_CHARS];

#endif

/*
 * FXTXT - CASIO fx-9750GIII / fx-9860GIII 中文文本查看 / 编辑器
 * Copyright (C) 2026 FXTXT contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * General Public License for more details.
 */

//---
//	FXTXT - graphical text viewer / editor for the CASIO fx-9750GIII
//
//	UI: title bar, framed content area with scrollbar, status bar, dialogs.
//	Text: ASCII + GB2312/GBK + UTF-8 Chinese, rendered with the glyphs of the
//	user's FONT8.dlf / FONT12.dlf (compiled in, bracket glyphs repaired).
//---
#include <gint/display.h>
#include <gint/gint.h>
#include <gint/keyboard.h>
#include <gint/defs/call.h>
#include <gint/bfile.h>
#include <gint/clock.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "font.h"
#include "icons.h"
#include "pinyin.h"

#define TEXT_MAX    6144
#define TEXT_MIN    4096
static int text_max = TEXT_MIN;
#define MAX_ENTRIES 64
#define NAME_MAX_L  63

/* --- layout (128x64 screen) ---------------------------------------------- */
#define TITLE_H   9
#define CONTENT_Y 9
#define CONTENT_H 45
#define BOX_Y     54
#define STATUS_Y  56
#define SBAR_W    0
#define TEXT_W    128

#define CELL_NL    0xFFFFu
#define CELL_CJK   0x0100u

enum { MODE_LIST = 0, MODE_VIEW, MODE_KEYB, MODE_HELP, MODE_NAME, MODE_SEARCH, MODE_CONFIRM,
       };
enum { PAGE_PINYIN = 0, PAGE_PUNCT, PAGE_LOWER, PAGE_UPPER, PAGE_DIGIT, PAGE_SYMBOL };
#define PAGE_COUNT 6


/* Chinese punctuation, one UTF-8 sequence per grid cell (13 x 2). */
/* Chinese punctuation as GB2312 byte pairs, one grid cell each. */
static unsigned char const punct_gb[] = {
	0xA3, 0xAC, 0xA1, 0xA3,
	0xA1, 0xA2, 0xA3, 0xBF,
	0xA3, 0xA1, 0xA3, 0xBA,
	0xA3, 0xBB, 0xA1, 0xB0,
	0xA1, 0xB1, 0xA1, 0xAE,
	0xA1, 0xAF, 0xA3, 0xA8,
	0xA3, 0xA9, 0xA1, 0xB6,
	0xA1, 0xB7, 0xA1, 0xB4,
	0xA1, 0xB5, 0xA1, 0xAD,
	0xA1, 0xAA, 0xA1, 0xA4,
	0xA1, 0xBE, 0xA1, 0xBF,
	0xA1, 0xC0, 0xA1, 0xC1,
	0xA1, 0xDE, 0xA1, 0xDF,
	0xA1, 0xE0, 0xA1, 0xE1,
};
#define PUNCT_COUNT ((int)(sizeof punct_gb) / 2)

/* Byte offset of the n-th UTF-8 character in punct_list. */

static uint16_t *cells;
static int ncell;
static int chunk_base;		/* file offset of cells[0] */
static int chunk_bytes;		/* bytes of the file held in rawbuf */
static int chunk_more;		/* something follows this window */
static int chunk_lines;		/* lines preceding this window */
static int chunk_rows;		/* display rows preceding this window */
static int total_rows;		/* display rows of the whole file */
static unsigned char *rawbuf;

static int edit_mode;
static int alpha_lock;		/* sticky letter mode (SHIFT+ALPHA) */
static int caps_on;		/* F6: upper case while typing */
static int case_upper;
static int shift_armed;

static int alpha_armed;		/* capitals (name prompt F1) */
static int cn_mode = 1;		/* 1 = pinyin, 0 = direct letters */
static int dirty;

#define NAME_FC_MAX 64
static char (*names)[NAME_MAX_L + 1];
static uint16_t (*names_fc)[NAME_FC_MAX];	/* full path from the OS */
static unsigned char *is_dir;
static int name_count;
static int list_top;
static int list_sel;
static char cur_dir[64];
static char cur_path[256];
static char root_dev[16];		/* "/fls0" normally */

static int view_top;
static int hscroll;
static int cur_line, cur_col;

static int mode = MODE_LIST;
static int kb_page = PAGE_LOWER;
static int kb_row, kb_col;
static int kb_target;			/* 0 = insert, 1 = search */
static uint16_t kb_cells[24];		/* typed cells (search prompt) */
static int kb_len;
static uint16_t search_cells[32];
static int search_len;
static char status_msg[48];

/* pinyin input method state */
static char py_buf[8];
static int py_len;
static int py_syllable = -1;		/* matched syllable index, -1 = none */
static int py_pick;

/* forward declarations (helpers used by the chrome renderers) */
static int offset_to_line(int off);
static char ev_letter(key_event_t ev);
static char alpha_letter(int key);
static char key_ascii(int key);
static void ui_status(char const *text);
static int py_candidates(void);
static void py_reset(void);
static void run_search(void);
static uint16_t py_candidate(int i);

/*=== geometry =============================================================*/

static int lh(void)   { return 9; }
static int rows(void) { return CONTENT_H / lh(); }

/*=== primitives ===========================================================*/

static void px_on(int x, int y)
{
	if ((unsigned)x >= 128u || (unsigned)y >= 64u)
		return;
	((uint8_t *)gint_vram)[y * 16 + (x >> 3)] |= 0x80 >> (x & 7);
}

static void px_off(int x, int y)
{
	if ((unsigned)x >= 128u || (unsigned)y >= 64u)
		return;
	((uint8_t *)gint_vram)[y * 16 + (x >> 3)] &= ~(0x80 >> (x & 7));
}

/* One glyph pixel. With invert=0 only ink is painted: the frame is already
   white, and painting the blank part of a cell would erase the rightmost
   column of the previous glyph (proportional advance is narrower than the
   cell). With invert=1 the cell is painted white-on-black. */
static void put_px(int x, int y, int on, int invert)
{
	if (!on)
		return;			/* never paint the cell background */
	if (invert)
		px_off(x, y);		/* white ink on the already-black bar */
	else
		px_on(x, y);
}

static void blit8(int x, int y, unsigned char const *g, int invert)
{
	if (invert) {
		/* a search hit has no black bar behind it: lay one down first,
		   then the glyph is punched out of it below */
		for (int r = 0; r < 8; r++)
			for (int c = 0; c < 8; c++)
				px_on(x + c, y + r);
	}
	for (int r = 0; r < 8; r++) {
		unsigned b = g[r];
		for (int c = 0; c < 8; c++)
			put_px(x + c, y + r, (b >> (7 - c)) & 1, invert);
	}
}


static void draw_cell(int x, int y, uint16_t cell, int invert)
{
	if (cell == CELL_NL)
		return;
	if (cell < CELL_CJK) {
		int i = (int)cell;
		if (i < 0 || i >= FONT_COUNT)
			i = '?' - FONT_FIRST;
		blit8(x - font_lb8[i], y, font_ascii8[i], invert);
		return;
	}
	int g = (int)cell - CELL_CJK;
	if (g < 0 || g >= FONT_GLYPHS)
		g = '?' - FONT_FIRST;
	blit8(x, y, font_cjk8[g], invert);
}

/* Proportional advance of one cell, in pixels. */
static int cell_adv(uint16_t cell)
{
	if (cell == CELL_NL)
		return 0;
	if (cell < CELL_CJK) {
		int i = (int)cell;
		if (i < 0 || i >= FONT_COUNT)
			i = '?' - FONT_FIRST;
		return font_adv8[i];
	}
	/* 0xA1A4 (·) sits alone in the middle of its cell: do not pad it */
	if (cell == CELL_CJK + 3)
		return 4;
	return 8;			/* CJK glyphs are full width */
}

/* Chrome text may be Chinese: decode UTF-8 and draw with the small font. */
static int ui_clip_x = 128;
static int blink_on = 1;		/* caret phase, flips every 0.25 s */


static int draw_ui(int x, int y, char const *s, int invert)
{
	unsigned char const *p = (unsigned char const *)s;
	int n = (int)strlen(s), i = 0, cut = 0;
	while (i < n) {
		uint16_t cell;
		if (p[i] < 0x80) {
			cell = (uint16_t)(p[i] - 0x20);
			i++;
		} else if (i + 1 < n) {
			int g = (p[i] - 0xA1) * 94 + (p[i + 1] - 0xA1);
			cell = (g >= 0 && g < FONT_GLYPHS) ? (uint16_t)(CELL_CJK + g)
							   : (uint16_t)('?' - 0x20);
			i += 2;
		} else {
			cell = (uint16_t)('?' - 0x20);
			i++;
		}
		int adv = cell_adv(cell);
		if (x + adv > ui_clip_x) {
			cut = 1;
			break;
		}
		draw_cell(x, y, cell, invert);
		x += adv;
	}
	if (cut) {
		/* 0xA1 0xAD is … , glyph 12 of the symbol row */
		draw_cell(x, y, (uint16_t)(CELL_CJK + 12), invert);
		x += 8;
	}
	return x;
}

static void draw_small(int x, int y, char const *s, int invert)
{
	while (*s) {
		int i = (unsigned char)*s++ - FONT_FIRST;
		if (i < 0 || i >= FONT_COUNT)
			i = '?' - FONT_FIRST;
		blit8(x - font_lb8[i], y, font_ascii8[i], invert);
		x += font_adv8[i];
	}
}

#define SEARCH_MAX_HITS 512
static int search_hits[SEARCH_MAX_HITS];
static int search_n;			/* matches stored */
static int search_cur;			/* index of the match on screen */
static void search_show(void);
static void build_search_cells(void);

/*=== window chrome ========================================================*/

/* Width in pixels of a UTF-8 string in the small font. */
static int ui_width(char const *s)
{
	unsigned char const *p = (unsigned char const *)s;
	int n = (int)strlen(s), i = 0, x = 0;
	while (i < n) {
		uint16_t cell;
		if (p[i] < 0x80) {
			cell = (uint16_t)(p[i] - 0x20);
			i++;
		} else if (i + 1 < n) {
			int g = (p[i] - 0xA1) * 94 + (p[i + 1] - 0xA1);
			cell = (g >= 0 && g < FONT_GLYPHS) ? (uint16_t)(CELL_CJK + g)
							   : (uint16_t)('?' - 0x20);
			i += 2;
		} else {
			cell = (uint16_t)('?' - 0x20);
			i++;
		}
		x += cell_adv(cell);
	}
	return x;
}

/* The bottom bar shows key hints; transient messages override them until the
   next key press. */
static int status_sticky;
static char const *hint_list = "F1\xD0\xC2\xBD\xA8 F2\xC9\xCF\xB4\xCE DEL\xC9\xBE F6\xB0\xEF";
static char const *hint_view = "F2\xD5\xD2 F3\xB1\xE0\xBC\xAD F6\xB0\xEF\xD6\xFA";
static char const *hint_edit_cn = "\xD6\xD0 F1\xB4\xE6 F3\xCD\xCB F4\xB1\xEA F5\xD6\xD0\xD3\xA2";
static char const *hint_edit_en = "\xD3\xA2 F1\xB4\xE6 F3\xCD\xCB F4\xB1\xEA F5\xD6\xD0\xD3\xA2";

/* Black square with a white letter: A = letter lock (ALPHA), S = capitals (SHIFT). */
static void ui_badge(char ch)
{
	int x = 118, y = STATUS_Y;
	drect(x, y, x + 9, y + 7, C_BLACK);
	draw_cell(x + 1, y, (uint16_t)((unsigned char)ch - 0x20), 1);
}

/* 0: reserve room for the state boxes, 1: flush with the right edge */
static int ui_info_right;

static int ui_info_w;		/* width of the last info string drawn */

static void ui_hints(char const *hints, char const *info, int show_lock)
{
	(void)show_lock;
	ui_info_w = (info && info[0]) ? ui_width(info) : 0;
	if (info && info[0]) {
		int x = (ui_info_right ? 127 : 96) - ui_info_w;
		if (x < 40) x = 40;
		draw_ui(x, 1, info, 1);
	}
	(void)hints;
	if (status_sticky && status_msg[0])
		ui_status(status_msg);
	else
		ui_status("");
}

static char const *box_lab[6] = { 0, 0, 0, 0, 0, 0 };
static int help_back;		/* mode to return to when help closes */
static int fbar_page;	/* 0: page one, 1: page two (F6 flips) */

/* Six cups along the bottom edge (F1..F6): no bottom edge, no gaps, and each
   one carries its own short label. */
static void ui_fkeys(void)
{
	drect(0, BOX_Y, 127, BOX_Y, C_BLACK);          /* top edge */
	for (int i = 1; i < 6; i++) {
		int x = i * 21;
		drect(x, BOX_Y, x, 63, C_BLACK);          /* dividers to the bottom */
	}
	if (edit_mode) {
		/* F6 flips the bar: a centred 5x7 solid triangle */
		int cy = (BOX_Y + 63) / 2;
		for (int c = 0; c < 5; c++) {
			int half = (4 - c) * 7 / 10;
			for (int r = -half; r <= half; r++)
				px_on(114 + c, cy + r);
		}
	}
	static char const *hit[6] = {
		"\xC9\xCF\xD2\xBB", "\xCF\xC2\xD2\xBB", "\xD4\xD9\xB2\xE9",
		"", "\xCD\xCB\xB3\xF6", "\xB0\xEF\xD6\xFA"
	};
	int hit_mode = (!edit_mode && search_n > 0);
	for (int i = 0; i < 6; i++) {
		char const *s = hit_mode ? hit[i] : box_lab[i];
		if (i == 5 && edit_mode)
			continue;	/* the ▶ icon lives there */
		if (!s || !s[0])
			continue;
		int w = ui_width(s);
		int left = i * 21, right = (i == 5) ? 127 : i * 21 + 20;
		int x = (left + right + 1) / 2 - w / 2;
		if (x < left + 1)
			x = left + 1;
		draw_ui(x, STATUS_Y, s, 0);
	}
}

/* 9x9 box in the title bar: white letter on black (inverse of the bar). */
static void ui_topbox(int x, char ch)
{
	drect(x, 0, x + 8, 8, C_WHITE);
	draw_cell(x + 1, 1, (uint16_t)((unsigned char)ch - 0x20), 0);
}

/* Right side of the title bar: case and input mode.  Every box is
   9x9 with a one pixel gap, like the A/S badges. */
static void ui_top_state(int show_input)
{
	if (!show_input)
		return;		/* letter state only matters while editing */

	int x = 127;			/* right edge of the next box */

	if (shift_armed) {
		/* SHIFT wins: it switches to capitals for the next key, so the
		   letter lock is not shown at the same time. */
		drect(x - 8, 0, x, 8, C_WHITE);
		draw_cell(x - 7, 1, (uint16_t)('S' - 0x20), 0);
		x -= 10;
	} else if (alpha_lock || alpha_armed) {
		drect(x - 8, 0, x, 8, C_WHITE);
		draw_cell(x - 7, 1, (uint16_t)('A' - 0x20), 0);
		x -= 10;
	}

	{
		int g = cn_mode ? 5029 : 4701;	/* 中 / 英 */
		drect(x - 8, 0, x, 8, C_WHITE);
		draw_cell(x - 7, 1, (uint16_t)(CELL_CJK + g), 0);
		x -= 10;
	}
	{
		/* 大写 / 小写: F6 and SHIFT both act as a case lock */
		int g = caps_on ? 1868 : 4418;
		drect(x - 16, 0, x, 8, C_WHITE);
		draw_cell(x - 15, 1, (uint16_t)(CELL_CJK + g), 0);
		draw_cell(x - 7, 1, (uint16_t)(CELL_CJK + 4437), 0);
	}
}

static void ui_title_state(char const *text, int show_input)
{
	drect(0, 0, 127, TITLE_H - 1, C_BLACK);
	/* Stop the title before whatever occupies the right end: the state
	   boxes when editing, otherwise the page counter.  Both are measured
	   from fixed budgets because the caller builds the counter string
	   after the title. */
	int clip;
	if (show_input)
		clip = 127 - 46 - 3;	/* A / S / 中英 / 大小写 */
	else if (ui_info_right)
		clip = 127 - 44 - 3;	/* reading page counter */
	else
		clip = 96 - 24 - 3;	/* list page item count */
	if (clip < 24)
		clip = 24;
	ui_clip_x = clip;
	draw_ui(3, 1, text, 1);
	ui_clip_x = 128;
	ui_top_state(show_input);
}

static int ui_show_input;	/* one-shot: draw the state boxes */

static void ui_title(char const *text)
{
	if (ui_show_input) {
		ui_show_input = 0;
		ui_title_state(text, 1);
		return;
	}
	ui_title_state(text, 0);
}

static int ui_no_cups;		/* prompt pages have no F-key cups */

static void ui_status(char const *text)
{
	(void)text;			/* no toasts: the six boxes carry the hints */
	if (ui_no_cups)
		return;
	ui_fkeys();
}


/* File names are raw bytes: ASCII, or GB2312 double bytes for Chinese.
   Render both with the small font, clipped to the content width. */
static void draw_fname(int x, int y, char const *s, int invert)
{
	unsigned char const *p = (unsigned char const *)s;
	while (*p && x < TEXT_W - 4) {
		uint16_t cell;
		if (*p < 0x80) {
			cell = (uint16_t)(*p - 0x20);
			p++;
		} else if (p[1]) {
			int g = ((int)p[0] - 0xA0 - 1) * 94 + ((int)p[1] - 0xA0 - 1);
			cell = (g >= 0 && g < FONT_GLYPHS) ? (uint16_t)(CELL_CJK + g)
			                                   : (uint16_t)('?' - 0x20);
			p += 2;
		} else {
			cell = (uint16_t)('?' - 0x20);
			p++;
		}
		draw_cell(x, y, cell, invert);
		x += cell_adv(cell);
	}
}

/*=== icons ================================================================*/

static void blit_icon(unsigned char const *rows, int w, int h, int x, int y,
                      int invert)
{
	for (int r = 0; r < h; r++) {
		unsigned char bits = rows[r];
		for (int c = 0; c < w; c++)
			if (bits & (1 << (6 - c))) {
				if (invert)
					px_off(x + c, y + r);
				else
					px_on(x + c, y + r);
			}
	}
}

static void icon_folder(int x, int y, int invert)
{
	blit_icon(icon_folder_rows, icon_folder_w, icon_folder_h, x, y, invert);
}

static void icon_file(int x, int y, int invert)
{
	blit_icon(icon_file_rows, icon_file_w, icon_file_h, x, y, invert);
}

/*=== decoding =============================================================*/



static void decode_into_cells(unsigned char const *s, int n)
{
	ncell = 0;
	int i = 0;
	while (i < n && ncell < text_max) {
		unsigned char c = s[i];
		if (c == '\n') { cells[ncell++] = CELL_NL; i++; continue; }
		if (c == '\r') { i++; continue; }
		if (c == '\t') {
			for (int k = 0; k < 4 && ncell < text_max; k++)
				cells[ncell++] = 0;
			i++;
			continue;
		}
		if (c < 0x20) { i++; continue; }
		if (c >= 0x81 && i + 1 < n) {
			unsigned char t = s[i + 1];
			if (c >= 0xA1 && c <= 0xF7 && t >= 0xA1 && t <= 0xFE) {
				int qu = c - 0xA0, wei = t - 0xA0;
				int gi = (qu - 1) * 94 + (wei - 1);
				cells[ncell++] = (gi >= 0 && gi < FONT_GLYPHS)
					? (uint16_t)(CELL_CJK + gi) : 0;
			} else {
				cells[ncell++] = 0;
			}
			i += 2;
			continue;
		}
		if (c >= 0x80) { i++; continue; }
		cells[ncell++] = (uint16_t)(c - 0x20);
		i++;
	}
}

static int encode_from_cells(unsigned char *out, int max)
{
	int n = 0;
	for (int i = 0; i < ncell && n < max; i++) {
		uint16_t cell = cells[i];
		if (cell == CELL_NL) { out[n++] = '\n'; continue; }
		if (cell < CELL_CJK) { out[n++] = (unsigned char)(cell + 0x20); continue; }
		int g = (int)cell - CELL_CJK;
		int qu = g / 94 + 1, wei = g % 94 + 1;
		if (n + 1 >= max) break;
		out[n++] = (unsigned char)(qu + 0xA0);
		out[n++] = (unsigned char)(wei + 0xA0);
	}
	return n;
}

/*=== line helpers =========================================================*/

static int line_start(int n)
{
	int p = 0;
	while (n-- > 0 && p < ncell) {
		while (p < ncell && cells[p] != CELL_NL) p++;
		if (p < ncell) p++;
	}
	return p;
}

static int line_len(int start)
{
	int e = start;
	while (e < ncell && cells[e] != CELL_NL) e++;
	return e - start;
}

/* Soft wrapping: a display row holds as many cells as fit into TEXT_W. */

/* Cell index right after the display row that starts at p (consumes the
   newline when the row ends on one). */
static int next_row(int p)
{
	int x = 0;
	while (p < ncell && cells[p] != CELL_NL) {
		int a = cell_adv(cells[p]);
		if (x + a > TEXT_W && x > 0)
			break;
		x += a;
		p++;
	}
	if (p < ncell && cells[p] == CELL_NL)
		p++;
	if (p > ncell)
		p = ncell;
	return p;
}

static int row_start(int row)
{
	int p = 0;
	while (row-- > 0 && p < ncell)
		p = next_row(p);
	return p;
}

static int row_count(void)
{
	if (ncell == 0)
		return 1;
	int p = 0, n = 0;
	while (p < ncell) {
		int q = next_row(p);
		if (q <= p)
			break;
		p = q;
		n++;
	}
	return n ? n : 1;
}

/* Display row that contains the given cell index. */
static int row_of_cell(int target)
{
	int p = 0, row = 0;
	while (p < ncell && p <= target) {
		int q = next_row(p);
		if (target < q || q >= ncell)
			return row;
		p = q;
		row++;
	}
	return row;
}

static int line_count(void)
{
	int n = 1;
	for (int i = 0; i < ncell; i++)
		if (cells[i] == CELL_NL) n++;
	return n;
}

/*=== session state (remember the last position) ===========================*/

static void fs_list_dir(void);

/* Set once the volume has been probed and the first listing is in memory.
   The add-in image (and therefore this flag) survives a menu exit, so a
   re-entry can skip the slow probe and show the previous screen at once. */
static int app_ready;
static void build_paths(void);
static void fs_read_current(void);
static void rec_touch(char const *path, int line);
static int rec_line_of(char const *path);
static void resume_line(void);
static void undo_clear(void);


#define STATE_PATH "\\\\fls0\\FXTXT.CFG"
#define STATE_MAGIC "FXT1"

typedef struct {
	char magic[4];
	unsigned char version;
	unsigned char big_font;
	unsigned char has_file;
	unsigned char reserved;
	char root[16];
	char dir[64];
	char name[NAME_MAX_L + 1];
	int view_top;
	int hscroll;
	int cur_line;
	int cur_col;
	int text_len;
} save_state_t;

static save_state_t st;

static int read_state(save_state_t *out)
{
	char alt[32];
	FILE *f = fopen(STATE_PATH, "rb");
	if (!f) {
		snprintf(alt, sizeof alt, "/fls0/FXTXT.CFG");
		f = fopen(alt, "rb");
	}
	if (!f)
		f = fopen("\\\\fls0\\FXTXT.CFG", "rb");
	if (!f)
		return 0;
	int n = (int)fread(out, 1, sizeof *out, f);
	fclose(f);
	return n == (int)sizeof *out && !memcmp(out->magic, STATE_MAGIC, 4);
}

static void write_state(void)
{
	char alt[32];
	FILE *f = fopen(STATE_PATH, "wb");
	if (!f) {
		snprintf(alt, sizeof alt, "/fls0/FXTXT.CFG");
		f = fopen(alt, "wb");
	}
	if (!f)
		f = fopen("\\\\fls0\\FXTXT.CFG", "wb");
	if (!f)
		return;
	fwrite(&st, 1, sizeof st, f);
	fclose(f);
}

static void fill_state(void)
{
	memcpy(st.magic, STATE_MAGIC, 4);
	st.version = 2;
	memset(st.root, 0, sizeof st.root);
	if (root_dev[0])
		strncpy(st.root, root_dev, sizeof st.root - 1);
	st.big_font = 0;
	st.has_file = (mode == MODE_VIEW) && name_count > 0;
	strncpy(st.dir, cur_dir, sizeof st.dir - 1);
	st.dir[sizeof st.dir - 1] = 0;
	if (st.has_file) {
		strncpy(st.name, names[list_sel], sizeof st.name - 1);
		st.name[sizeof st.name - 1] = 0;
		st.view_top = view_top;
		st.hscroll = hscroll;
		st.cur_line = cur_line;
		st.cur_col = cur_col;
		st.text_len = ncell;
	} else {
		st.name[0] = 0;
		st.view_top = hscroll = cur_line = cur_col = 0;
		st.text_len = 0;
	}
}

/* Save the current state. When we are only browsing the list, the file and
   position recorded earlier are preserved so that quitting from the list does
   not forget where the reader was. */
static void fs_save_state(void)
{
	save_state_t prev;
	int have_prev = read_state(&prev);
	fill_state();
	if (have_prev && !st.has_file) {
		memcpy(st.name, prev.name, sizeof st.name);
		st.has_file = prev.has_file;
		st.view_top = prev.view_top;
		st.hscroll = prev.hscroll;
		st.cur_line = prev.cur_line;
		st.cur_col = prev.cur_col;
		st.text_len = prev.text_len;
	}
	write_state();
}

/* Returns 1 when the previous file was reopened (mode becomes MODE_VIEW). */
static int fs_load_state(void)
{
	save_state_t s;
	if (!read_state(&s))
		return 0;

	if (s.dir[0]) {
		strncpy(cur_dir, s.dir, sizeof cur_dir - 1);
		cur_dir[sizeof cur_dir - 1] = 0;
	}
	fs_list_dir();
	if (!s.has_file || !s.name[0])
		return 0;

	int idx = -1;
	for (int i = 0; i < name_count; i++)
		if (!strcmp(names[i], s.name)) {
			idx = i;
			break;
		}
	if (idx < 0 || is_dir[idx])
		return 0;

	list_sel = idx;
	list_top = idx > 4 ? idx - 4 : 0;
	build_paths();
	fs_read_current();
	if (ncell == 0 && s.text_len != 0)
		return 0;

	view_top = s.view_top;
	hscroll = s.hscroll;
	cur_line = s.cur_line;
	cur_col = s.cur_col;

	int tl = line_count();
	if (view_top > tl - 1) view_top = tl - 1;
	if (view_top < 0) view_top = 0;
	if (cur_line > tl - 1) cur_line = tl - 1;
	if (cur_line < 0) cur_line = 0;
	int len = line_len(line_start(cur_line));
	if (cur_col > len) cur_col = len;
	if (cur_col < 0) cur_col = 0;
	if (hscroll < 0) hscroll = 0;
	return 1;
}

static int restore_flag;

static void fs_restore_call(void)
{
	restore_flag = fs_load_state();
}

static int fs_restore(void)
{
	restore_flag = 0;
	gint_world_switch(GINT_CALL(fs_restore_call));
	return restore_flag;
}

/* Delete "<cur_dir>/<name>" (runs in the OS world). */
static void fs_delete_path(char const *name)
{
	uint16_t fc[128];
	int j = 0;
	char path[256];
	FILE *f;

	fc[j++] = '\\';
	fc[j++] = '\\';
	for (int i = (cur_dir[0] == '/') ? 1 : 0; cur_dir[i] && j < 116; i++)
		fc[j++] = (cur_dir[i] == '/') ? '\\' : (unsigned char)cur_dir[i];
	fc[j++] = '\\';
	for (int i = 0; name[i] && j < 124; i++)
		fc[j++] = (unsigned char)name[i];
	fc[j] = 0;

	if (BFile_Remove(fc) == 0)
		return;
	/* fallback: plain stdio remove with both separator styles */
	snprintf(path, sizeof path, "%s/%s", cur_dir, name);
	if (remove(path) == 0)
		return;
	{
		char alt[256];
		size_t k = 0;
		for (size_t i = 0; path[i] && k + 1 < sizeof alt; i++)
			alt[k++] = (path[i] == '/') ? '\\' : path[i];
		alt[k] = 0;
		if (remove(alt) == 0)
			return;
	}
	(void)f;
}

/*=== buffer lifetime ======================================================

   With gint_setrestart(1) the add-in can be parked in the OS menu and come
   back. On this model the image (and .bss) is rebuilt on the way in while the
   heap keeps what the previous instance allocated, so everything must be
   handed back before control returns to the OS, otherwise the next launch
   starts with the memory already gone. */

static void app_release(void)
{
	free(cells);
	free(rawbuf);
	free(names);
	free(names_fc);
	free(is_dir);
	cells = NULL;
	rawbuf = NULL;
	names = NULL;
	names_fc = NULL;
	is_dir = NULL;
	text_max = 0;
	name_count = 0;
	app_ready = 0;
}

/*=== storage memory =======================================================*/

/* Only .txt documents are listed (plus folders, so the tree stays navigable). */
/* ".TXT" test on the raw FONTCHARACTER path (independent of the encoding). */
static int fc_is_txt(uint16_t const *fc)
{
	int n = 0;
	while (fc[n] && n < NAME_FC_MAX)
		n++;
	if (n < 4)
		return 0;
	char c0 = (char)(fc[n - 4] & 0xFF), c1 = (char)(fc[n - 3] & 0xFF);
	char c2 = (char)(fc[n - 2] & 0xFF), c3 = (char)(fc[n - 1] & 0xFF);
	return c0 == '.' &&
	       (c1 == 't' || c1 == 'T') &&
	       (c2 == 'x' || c2 == 'X') &&
	       (c3 == 't' || c3 == 'T');
}

/* Copy a raw FONTCHARACTER path into a byte string.  Units below 0x100 are
   single bytes already (ASCII or GB2312 bytes); higher units are Unicode, which
   is converted back to GB2312 so a Chinese name keeps its real two bytes. */
static void fc_to_bytes(uint16_t const *fc, char *out, int outsz)
{
	int i = 0, k = 0;
	for (; fc[i] && k < outsz - 2; i++) {
		unsigned u = fc[i];
		if (u < 0x100) {
			out[k++] = (char)u;
		} else {
			out[k++] = '?';	/* wide units: no name mapping */
			out[k++] = '?';
		}
	}
	out[k] = 0;
}

static int name_is_txt(char const *n)
{
	size_t l = strlen(n);
	if (l < 5)
		return 0;
	char const *e = n + l - 4;
	return e[0] == '.' &&
	       (e[1] == 't' || e[1] == 'T') &&
	       (e[2] == 'x' || e[2] == 'X') &&
	       (e[3] == 't' || e[3] == 'T');
}

/* The storage volume is called "fls0", but the form accepted by opendir()
   differs between models/OS versions, so probe a few spellings once. */
static char const *root_candidates[] = { "\\\\fls0", "/fls0", "fls0", "/", NULL };

static int try_root(char const *cand)
{
	DIR *d = opendir(cand);
	if (!d)
		return 0;
	closedir(d);
	return 1;
}

static void fs_probe_root(void)
{
	save_state_t s;

	root_dev[0] = 0;
	/* A rememberd volume works first time: this is what makes a re-launch
	   quick instead of walking every spelling. */
	if (read_state(&s) && s.version >= 2 && s.root[0] &&
	    try_root(s.root)) {
		strncpy(root_dev, s.root, sizeof root_dev - 1);
		root_dev[sizeof root_dev - 1] = 0;
	} else
	/* The filesystem may only be mounted once it is first touched, so probe
	   several spellings a few times before giving up. */
	for (int attempt = 0; attempt < 1 && !root_dev[0]; attempt++) {
		for (int i = 0; root_candidates[i]; i++) {
			if (try_root(root_candidates[i])) {
				strncpy(root_dev, root_candidates[i], sizeof root_dev - 1);
				root_dev[sizeof root_dev - 1] = 0;
				break;
			}
		}
	}
	if (!root_dev[0])
		strcpy(root_dev, "/fls0");
	strncpy(cur_dir, root_dev, sizeof cur_dir - 1);
	cur_dir[sizeof cur_dir - 1] = 0;
}

static int entry_is_dir(char const *name)
{
	char path[96];
	DIR *d;
	snprintf(path, sizeof path, "%s/%s", cur_dir, name);
	d = opendir(path);
	if (d) { closedir(d); return 1; }
	return 0;
}

static void sort_entries(void)
{
	for (int i = 1; i < name_count; i++) {
		char key[NAME_MAX_L + 1];
		unsigned char kd = is_dir[i];
		strcpy(key, names[i]);
		int j = i - 1;
		while (j >= 0 && (is_dir[j] < kd ||
		       (is_dir[j] == kd && strcmp(names[j], key) > 0))) {
			memcpy(names[j + 1], names[j], NAME_MAX_L + 1);
			is_dir[j + 1] = is_dir[j];
			j--;
		}
		memcpy(names[j + 1], key, NAME_MAX_L + 1);
		is_dir[j + 1] = kd;
	}
}

/* Convert an ASCII path ("/fls0/DIR") to a FONTCHARACTER pattern.
   The OS form always starts with a double backslash: "\\fls0\DIR\*". */
/* Is the first path segment a known volume name ("fls0", "crd0", ...)? */
static int first_seg_is_volume(char const *p)
{
	static char const *vols[] = { "fls0", "fls1", "crd0", "crd1", 0 };
	for (int i = 0; vols[i]; i++) {
		int n = strlen(vols[i]);
		if (!strncmp(p, vols[i], n) &&
		    (p[n] == 0 || p[n] == '/' || p[n] == '\\'))
			return 1;
	}
	return 0;
}

static void to_fc_pattern(uint16_t *dst, int max)
{
	int j = 0;
	int i = 0;
	dst[j++] = '\\';
	dst[j++] = '\\';
	/* cur_dir may be "/fls0", "\\fls0" or even "//fls0": drop every
	   leading separator so exactly two are emitted below */
	while (cur_dir[i] == '/' || cur_dir[i] == '\\')
		i++;
	if (!cur_dir[i] || !first_seg_is_volume(cur_dir + i)) {
		char const *vol = "fls0";
		for (char const *s = vol; *s && j < max - 4; s++)
			dst[j++] = (uint16_t)(unsigned char)*s;
		if (cur_dir[i])
			dst[j++] = '\\';
	}
	for (; cur_dir[i] && j < max - 4; i++) {
		unsigned char c = (unsigned char)cur_dir[i];
		dst[j++] = (c == '/' || c == '\\') ? '\\' : c;
	}
	dst[j++] = '\\';
	dst[j++] = '*';
	dst[j] = 0;
}

/* List the current folder through the OS file API. This is what the calculator
   itself uses and it works before gint's stdio layer has been touched. */
static void fs_list_dir_depth(int depth);

/* Make sure cur_dir always uses the POSIX form "/fls0/..." internally. */
static void normalise_cur_dir(void)
{
	if (cur_dir[0] != '/' && cur_dir[0] != '\\')
		return;
	int i = 0;
	while (cur_dir[i] == '/' || cur_dir[i] == '\\')
		i++;
	if (i == 1 && cur_dir[0] == '/')
		return;				/* already canonical */
	char tmp[80];
	int j = 0;
	tmp[j++] = '/';
	for (; cur_dir[i] && j < (int)sizeof tmp - 2; i++) {
		unsigned char c = (unsigned char)cur_dir[i];
		tmp[j++] = (c == '\\') ? '/' : c;
	}
	tmp[j] = 0;
	strcpy(cur_dir, tmp);
}

static void fs_list_dir(void)
{
	normalise_cur_dir();
	fs_list_dir_depth(0);
}

/* Is the entry "<cur_dir>/<name>" a folder? Uses stat first, then opendir. */
static int path_is_dir(char const *name, int *fc_type)
{
	char path[256];
	int type = -1, size = 0;
	snprintf(path, sizeof path, "%s/%s", cur_dir, name);
	/* native FONTCHARACTER stat */
	{
		uint16_t fc[128];
		int j = 0;
		fc[j++] = '\\';
		fc[j++] = '\\';
		for (int i = (cur_dir[0] == '/') ? 1 : 0; cur_dir[i] && j < 120; i++)
			fc[j++] = (cur_dir[i] == '/') ? '\\' : (unsigned char)cur_dir[i];
		fc[j++] = '\\';
		for (int i = 0; name[i] && j < 126; i++)
			fc[j++] = (unsigned char)name[i];
		fc[j] = 0;
		if (BFile_Ext_Stat(fc, &type, &size) == 0 && type >= 0) {
			if (fc_type)
				*fc_type = type;
			return type == BFile_Type_Directory;
		}
	}
	if (fc_type)
		*fc_type = -1;
	return entry_is_dir(name);
}

static void add_entry(char const *name, int dir, uint16_t const *fc)
{
	if (name_count >= MAX_ENTRIES)
		return;
	size_t l = strlen(name);
	if (l > NAME_MAX_L)
		l = NAME_MAX_L;
	memcpy(names[name_count], name, l);
	names[name_count][l] = 0;
	is_dir[name_count] = (unsigned char)dir;
	if (fc) {
		int i = 0;
		for (; fc[i] && i < NAME_FC_MAX - 1; i++)
			names_fc[name_count][i] = fc[i];
		names_fc[name_count][i] = 0;
	} else {
		int i = 0;
		for (; name[i] && i < NAME_FC_MAX - 1; i++)
			names_fc[name_count][i] = (unsigned char)name[i];
		names_fc[name_count][i] = 0;
	}
	name_count++;
}

static void fs_list_dir_depth(int depth)
{
	(void)depth;
	name_count = 0;
	int first_real = 0;
	if (strcmp(cur_dir, root_dev)) {
		add_entry("..", 1, 0);
		first_real = 1;			/* skip the synthetic row when counting */
	}

	uint16_t pattern[64];
	uint16_t found[256];
	struct BFile_FileInfo info;
	int handle = 0;
	to_fc_pattern(pattern, 64);

	int rc = BFile_FindFirst(pattern, &handle, found, &info);
	while (rc == 0 && name_count < MAX_ENTRIES) {
		int last = -1;
		for (int i = 0; found[i]; i++)
			if (found[i] == '\\')
				last = i;
		char base[NAME_MAX_L + 2];
		fc_to_bytes(found + last + 1, base, (int)sizeof base);
		if (base[0] && strcmp(base, ".") && strcmp(base, "..")) {
			int dir = (info.type == BFile_Type_Directory);
			int txt = fc_is_txt(found);
			if (!dir && !txt) {
				/* the search API may mislabel folders: check properly */
				int type = -1;
				dir = path_is_dir(base, &type);
			}
			if (dir || txt)
				add_entry(base, dir, found);
		}
		rc = BFile_FindNext(handle, found, &info);
	}
	if (handle)
		BFile_FindClose(handle);

	for (int i = first_real; i < name_count; i++) {
		if (!strcmp(names[i], "fls0") || !strcmp(names[i], "crd0")) {
			for (int k = i; k + 1 < name_count; k++) {
				memcpy(names[k], names[k + 1], sizeof names[0]);
				memcpy(names_fc[k], names_fc[k + 1], sizeof names_fc[0]);
				is_dir[k] = is_dir[k + 1];
			}
			name_count--;
			i--;
		}
	}

	/* Drop the bogus volume row, then merge in whatever opendir can see:
	   the search API is the only one reporting non-ASCII names, opendir is
	   the more reliable one for plain files. */
	int real = name_count - first_real;
	/* The search API reports real file names (including Chinese ones) once the
	   pattern carries a volume, so opendir is only a fallback now. */
	int degenerate = (real == 0);

	if (degenerate) {
		/* remove the volume entry */
		for (int i = first_real; i < name_count; i++) {
			if (is_dir[i] && !strcmp(names[i], "fls0")) {
				for (int k = i; k + 1 < name_count; k++) {
					memcpy(names[k], names[k + 1], sizeof names[0]);
					memcpy(names_fc[k], names_fc[k + 1], sizeof names_fc[0]);
					is_dir[k] = is_dir[k + 1];
				}
				name_count--;
				i--;
			}
		}
		DIR *d = opendir(cur_dir);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d))) {
				char const *n = e->d_name;
				if (!n || !n[0] || !strcmp(n, ".") || !strcmp(n, ".."))
					continue;
				if (name_count >= MAX_ENTRIES)
					break;
				int dup = 0;
				for (int i = 0; i < name_count; i++)
					if (!strcmp(names[i], n))
						dup = 1;
				if (dup)
					continue;
				int dir = (e->d_type == DT_DIR) || entry_is_dir(n);
				if (!dir && !name_is_txt(n))
					continue;
				add_entry(n, dir, 0);
			}
			closedir(d);
		}
	}

	sort_entries();
	snprintf(status_msg, sizeof status_msg, "%d \xB8\xF6", name_count - first_real);
}

/* Restore the position remembered for the file we have just opened. */
static void apply_saved_pos(void)
{
	save_state_t s;
	if (!read_state(&s) || !s.has_file || !s.name[0])
		return;
	if (strcmp(s.name, names[list_sel]) || strcmp(s.dir, cur_dir))
		return;
	int tl = line_count();
	view_top = s.view_top;
	if (view_top > tl - 1) view_top = tl - 1;
	if (view_top < 0) view_top = 0;
	cur_line = s.cur_line;
	if (cur_line > tl - 1) cur_line = tl - 1;
	if (cur_line < 0) cur_line = 0;
	int len = line_len(line_start(cur_line));
	cur_col = s.cur_col;
	if (cur_col > len) cur_col = len;
	if (cur_col < 0) cur_col = 0;
	hscroll = s.hscroll < 0 ? 0 : s.hscroll;
}

static void fs_enter_dir(void)
{
	char next[64];
	if (strlen(cur_dir) + strlen(names[list_sel]) + 2 > sizeof next) {
		snprintf(status_msg, sizeof status_msg, "\xC2\xB7\xBE\xB6\xCC\xAB\xB3\xA4");
		return;
	}
	{
		size_t j = 0;
		for (size_t i = 0; cur_dir[i] && j + 1 < sizeof next; i++)
			next[j++] = cur_dir[i];
		if (j == 0 || next[j - 1] != '/')
			next[j++] = '/';
		for (size_t i = 0; names[list_sel][i] && j + 1 < sizeof next; i++)
			next[j++] = names[list_sel][i];
		next[j] = 0;
	}
	strncpy(cur_dir, next, sizeof cur_dir - 1);
	cur_dir[sizeof cur_dir - 1] = 0;
	fs_list_dir();
}

static void fs_leave_dir(void)
{
	char *slash = strrchr(cur_dir, '/');
	if (slash == cur_dir)
		cur_dir[0] = 0;		/* "/dir" -> "" (that is the root) */
	else if (slash)
		*slash = 0;		/* "/a/b" -> "/a" */
	if (!cur_dir[0])
		strcpy(cur_dir, root_dev);
	fs_list_dir();
}

/* Native path for the selected entry: "\\fls0\DIR\NAME" */
static void build_paths(void)
{
	int j = 0;
	cur_path[j++] = '\\';
	cur_path[j++] = '\\';
	for (int i = (cur_dir[0] == '/') ? 1 : 0; cur_dir[i] && j < 70; i++) {
		char c = cur_dir[i];
		cur_path[j++] = (c == '/') ? '\\' : c;
	}
	cur_path[j++] = '\\';
	cur_path[j] = 0;
	strncat(cur_path, names[list_sel], sizeof cur_path - j - 1);
}

static FILE *open_any(char const *path, char const *mode)
{
	char alt[256];
	FILE *f = fopen(path, mode);
	if (f)
		return f;
	/* retry with the other separator style, then with a \\?\?\ prefix */
	size_t j = 0;
	for (size_t i = 0; path[i] && j + 3 < sizeof alt; i++) {
		char c = path[i];
		if (c == '/')
			alt[j++] = '\\';
		else if (c == '\\')
			alt[j++] = '/';
		else
			alt[j++] = c;
	}
	alt[j] = 0;
	f = fopen(alt, mode);
	if (f)
		return f;
	/* drop a leading separator and retry (some OS versions want the volume bare) */
	if (path[0] == '\\' || path[0] == '/') {
		snprintf(alt, sizeof alt, "%s", path + 1);
		f = fopen(alt, mode);
	}
	return f;
}

static FILE *open_read(void) { return open_any(cur_path, "rb"); }
static FILE *open_write(void) { return open_any(cur_path, "wb"); }

/* Name prompt state */
static char name_buf[64];
static int name_len;
static int name_pos;
static int create_ok;		/* 1 = created (open it), 2 = renamed (back) */
static int name_rename;		/* prompt is renaming the selected file */

/* gint's fx port has no BFile_Rename syscall, so a rename is a copy followed
   by a delete. Returns 0 on success, -2 when the file is too large to copy. */
static int name_rename_copy(char const *oldn, char const *newn)
{
	char po[256], pn[256];
	enum { CAP = 65536 };
	snprintf(po, sizeof po, "%s/%s", cur_dir, oldn);
	snprintf(pn, sizeof pn, "%s/%s", cur_dir, newn);

	FILE *in = open_any(po, "rb");
	if (!in)
		return -1;
	unsigned char *buf = malloc(CAP);
	if (!buf) {
		fclose(in);
		return -1;
	}
	size_t n = fread(buf, 1, CAP, in);
	fclose(in);
	if (n >= CAP) {
		free(buf);
		return -2;
	}
	FILE *out = open_any(pn, "wb");
	if (!out) {
		free(buf);
		return -1;
	}
	size_t w = fwrite(buf, 1, n, out);
	fclose(out);
	free(buf);
	if (w != n)
		return -1;
	{
		FILE *chk = open_any(pn, "rb");
		if (!chk)
			return -1;
		fclose(chk);
	}
	fs_delete_path(oldn);
	return 0;
}

/* Start renaming the selected entry: prefill the prompt with its base name. */
static void name_begin_rename(void)
{
	if (name_count == 0)
		return;
	if (!strcmp(names[list_sel], "..") || is_dir[list_sel]) {
		snprintf(status_msg, sizeof status_msg, "\xCE\xC4\xBC\xFE\xBC\xD0\xB2\xBB\xC4\xDC\xB8\xC4\xC3\xFB");
		status_sticky = 1;
		return;
	}
	snprintf(name_buf, sizeof name_buf, "%s", names[list_sel]);
	size_t n = strlen(name_buf);
	if (n > 4 && !strcmp(name_buf + n - 4, ".TXT"))
		name_buf[n - 4] = 0;
	name_len = (int)strlen(name_buf);
	name_pos = name_len;
	name_rename = 1;
}

/* Characters that may appear in a file name (FAT illegal set excluded). */
static int name_char_ok(char c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return 1;
	return strchr("-_.,+()&'!#$%@~;=[]{}^", c) != NULL;
}

/* First free "NEWn" name in the current folder (runs in the OS world). */
static void name_default(void)
{
	char base[12], path[256];
	for (int n = 1; n <= 99; n++) {
		snprintf(base, sizeof base, "NEW%d", n);
		snprintf(path, sizeof path, "%s/%s.TXT", cur_dir, base);
		FILE *f = open_any(path, "rb");
		if (f) {
			fclose(f);
			continue;
		}
		name_len = (int)strlen(base);
		memcpy(name_buf, base, name_len + 1);
		return;
	}
	name_len = 4;
	strcpy(name_buf, "NEW1");
}

/* Create "<name>.TXT" in the current folder and open it for editing. */
static void name_commit(void)
{
	char clean[16], name[NAME_MAX_L + 1], path[256];
	int n = 0;
	create_ok = 0;
	int cap = name_rename ? (NAME_MAX_L - 4) : 12;
	for (int i = 0; name_buf[i] && n < cap; i++) {
		char c = name_buf[i];
		if (!name_char_ok(c))
			continue;
		clean[n++] = c;
	}
	clean[n] = 0;
	if (!n) {
		snprintf(status_msg, sizeof status_msg, "\xC3\xFB\xD7\xD6\xB2\xBB\xC4\xDC\xCE\xAA\xBF\xD5");
		return;
	}
	snprintf(name, sizeof name, "%s.TXT", clean);
	snprintf(path, sizeof path, "%s/%s", cur_dir, name);

	if (name_rename) {
		char oldn[NAME_MAX_L + 1];
		int rc;
		snprintf(oldn, sizeof oldn, "%s", names[list_sel]);
		if (!strcmp(oldn, name)) {
			name_rename = 0;
			return;
		}
		name_rename = 0;
		rc = name_rename_copy(oldn, name);
		if (rc == -2) {
			snprintf(status_msg, sizeof status_msg, "\xCE\xC4\xBC\xFE\xCC\xAB\xB4\xF3\xCE\xDE\xB7\xA8\xB8\xC4\xC3\xFB");
			return;
		}
		if (rc != 0) {
			snprintf(status_msg, sizeof status_msg, "\xB8\xC4\xC3\xFB\xCA\xA7\xB0\xDC");
			return;
		}
		fs_list_dir();
		for (int i = 0; i < name_count; i++)
			if (!strcmp(names[i], name)) {
				list_sel = i;
				break;
			}
		if (list_sel < list_top)
			list_top = list_sel;
		if (list_sel >= list_top + 5)
			list_top = list_sel - 4;
		create_ok = 2;
		return;
	}

	FILE *f = open_any(path, "rb");
	if (f) {
		fclose(f);
		snprintf(status_msg, sizeof status_msg, "\xD2\xD1\xB4\xE6\xD4\xDA\xCD\xAC\xC3\xFB\xCE\xC4\xBC\xFE");
		return;
	}
	f = open_any(path, "wb");
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "\xD0\xC2\xBD\xA8\xCA\xA7\xB0\xDC");
		return;
	}
	fclose(f);

	fs_list_dir();
	int idx = -1;
	for (int i = 0; i < name_count; i++)
		if (!strcmp(names[i], name)) {
			idx = i;
			break;
		}
	if (idx < 0) {
		snprintf(status_msg, sizeof status_msg, "\xD0\xC2\xBD\xA8\xCA\xA7\xB0\xDC");
		return;
	}
	list_sel = idx;
	if (list_sel < list_top) list_top = list_sel;
	build_paths();
	ncell = 0;
	cur_line = cur_col = 0;
	view_top = 0;
	edit_mode = 1;
	dirty = 0;
	py_reset();
	snprintf(status_msg, sizeof status_msg, "\xD0\xC2\xBD\xA8 %s", name);
	status_sticky = 1;
	create_ok = 1;
}

static void draw_name(void)
{
	ui_no_cups = 1;
	name_len = (int)strlen(name_buf);
	if (name_pos > name_len)
		name_pos = name_len;
	if (name_pos < 0)
		name_pos = 0;
	ui_title(name_rename ? "\xD6\xD8\xC3\xFC\xC3\xFB"
	                     : "\xD0\xC2\xBD\xA8\xCE\xC4\xB5\xB5");
	int y = CONTENT_Y + 2;
	char line[64];
	snprintf(line, sizeof line, "\xC4\xBF\xC2\xBC %s", cur_dir);
	draw_ui(2, y, line, 0);
	y += 10;
	draw_ui(2, y, "\xC3\xFB\xB3\xC6", 0);
	{
		/* split at the caret so it shows where typing will happen */
		char head[64], rest[64];
		int n = name_pos;
		if (n > name_len)
			n = name_len;
		if (n < 0)
			n = 0;
		memcpy(head, name_buf, (size_t)n);
		head[n] = 0;
		snprintf(rest, sizeof rest, "%s", name_buf + n);
		int x = draw_ui(28, y, head, 0);
		for (int k = 0; k < 7; k++)
			px_on(x + 1, y + k);
		draw_ui(x + 3, y, rest, 0);
	}
	y += 10;
	{
		char const *tail = name_rename
			? ""			/* an existing extension is kept as it is */
			: (case_upper
			   ? "\xB4\xF3\xD0\xB4 F1\xC7\xD0\xBB\xBB  \xD7\xD4\xB6\xAF\xBC\xD3 .TXT"
			   : "\xD0\xA1\xD0\xB4 F1\xC7\xD0\xBB\xBB  \xD7\xD4\xB6\xAF\xBC\xD3 .TXT");
		if (tail[0])
			draw_ui(2, y, tail, 0);
		else
			draw_ui(2, y, case_upper
			        ? "\xB4\xF3\xD0\xB4 F1\xC7\xD0\xBB\xBB"
			        : "\xD0\xA1\xD0\xB4 F1\xC7\xD0\xBB\xBB", 0);
	}
	ui_hints(name_rename ? "ALPH\xD7\xD6\xC4\xB8 EXE\xB8\xC4 EXIT\xCD\xCB"
	                     : "ALPH\xD7\xD6\xC4\xB8 EXE\xBD\xA8 EXIT\xCD\xCB", "", 1);
	dupdate();
}

static void handle_name_key(key_event_t ev)
{
	name_len = (int)strlen(name_buf);
	if (name_pos > name_len)
		name_pos = name_len;

	if (ev.key == KEY_EXIT) {		/* always leave this page */
		mode = MODE_LIST;
		return;
	}

	if (ev.key == KEY_LEFT || ev.key == KEY_RIGHT) {
		/* must come before the "plain key is a character" fallback,
		   which would otherwise swallow the arrow */
		if (ev.key == KEY_LEFT) {
			if (name_pos > 0)
				name_pos--;
		} else if (name_pos < name_len) {
			name_pos++;
		}
		return;
	}
	if (ev.key == KEY_F1) {
		case_upper = !case_upper;
		snprintf(status_msg, sizeof status_msg,
		         case_upper ? "\xB4\xF3\xD0\xB4\xD7\xD6\xC4\xB8" : "\xD0\xA1\xD0\xB4\xD7\xD6\xC4\xB8");
		status_sticky = 1;
		return;
	}

	char c = ev_letter(ev);
	if (!c && alpha_lock)
		c = alpha_letter(ev.key);
	if (!c && !ev.alpha && ev.shift && ev.key == KEY_SUB)
		c = '_';			/* SHIFT + [-] = 下划线 */
	if (!c && !ev.alpha && !ev.shift &&
	    ev.key != KEY_EXE && ev.key != KEY_EXIT && ev.key != KEY_DEL &&
	    ev.key != KEY_F1 && ev.key != KEY_F2)
		c = key_ascii(ev.key);
	if (c) {
		if (case_upper && c >= 'a' && c <= 'z')
			c = (char)(c - 32);
		if (!name_char_ok(c)) {
			snprintf(status_msg, sizeof status_msg, "\xB8\xC3\xB7\xFB\xBA\xC5\xB2\xBB\xC4\xDC\xD7\xF6\xCE\xC4\xBC\xFE\xC3\xFB");
			status_sticky = 1;
			return;
		}
		if (name_len < (int)sizeof name_buf - 1) {
			for (int i = name_len; i > name_pos; i--)
				name_buf[i] = name_buf[i - 1];
			name_buf[name_pos] = c;
			name_len++;
			name_pos++;
			name_buf[name_len] = 0;
		}
		return;
	}

	switch (ev.key) {
	case KEY_LEFT:
		if (name_pos > 0)
			name_pos--;
		return;
	case KEY_RIGHT:
		if (name_pos < name_len)
			name_pos++;
		return;
	case KEY_DEL:
	case KEY_F2:
		if (name_pos > 0) {
			for (int i = name_pos - 1; i < name_len; i++)
				name_buf[i] = name_buf[i + 1];
			name_len--;
			name_pos--;
		}
		return;
	case KEY_EXE:
		gint_world_switch(GINT_CALL(name_commit));
		if (create_ok == 1)
			mode = MODE_VIEW;
		else if (create_ok == 2)
			mode = MODE_LIST;
		return;
	case KEY_EXIT:
		mode = MODE_LIST;
		return;
	default:
		return;
	}
}

/* Read the window that starts at byte `base`. Windows that do not reach EOF
   are cut back to the last newline, so the next window starts on a line
   boundary and scrolling can just keep going. */
static int load_chunk(int base)
{
	ncell = 0;
	chunk_bytes = 0;
	chunk_more = 0;
	FILE *f = open_read();
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "\xB4\xF2\xB2\xBB\xBF\xAA");
		return 0;
	}
	if (base > 0 && fseek(f, (long)base, SEEK_SET) != 0) {
		fclose(f);
		return 0;
	}
	int n = (int)fread(rawbuf, 1, text_max, f);
	int more = (n == text_max) && (fgetc(f) != EOF);
	fclose(f);
	if (n < 0)
		n = 0;
	if (more) {
		int cut = -1;
		for (int i = n - 1; i > 0; i--)
			if (rawbuf[i] == '\n') {
				cut = i;
				break;
			}
		if (cut > 0)
			n = cut + 1;
		chunk_more = 1;
	}
	chunk_base = base;
	chunk_bytes = n;
	decode_into_cells(rawbuf, n);
	undo_clear();
	snprintf(status_msg, sizeof status_msg, "%d\xD7\xD6\xBD\xDA GB", n);
	return n;
}

/* Display rows from the start of the file up to byte `upto`. */
static int rows_upto(int upto)
{
	int total = 0, base = 0;
	for (int guard = 0; guard < 512; guard++) {
		int n = load_chunk(base);
		if (upto > 0 && chunk_base + n >= upto)
			break;
		total += row_count();
		if (!chunk_more || n <= 0)
			break;
		base = chunk_base + n;
	}
	return total;
}

/* Whole document: rows before the first window and its total. */
static int count_total_rows(void)
{
	int total = 0, base = 0;
	for (int guard = 0; guard < 512; guard++) {
		int n = load_chunk(base);
		total += row_count();
		if (!chunk_more || n <= 0)
			break;
		base = chunk_base + n;
	}
	return total;
}

static void fs_read_current(void)
{
	chunk_lines = 0;
	chunk_rows = 0;
	total_rows = count_total_rows();
	chunk_rows = 0;
	load_chunk(0);
}

/* Re-read the open file and decode it with the forced encoding. */
static void fs_redecode(void)
{
	FILE *f = open_read();
	ncell = 0;
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "open failed");
		return;
	}
	int n = (int)fread(rawbuf, 1, text_max, f);
	fclose(f);
	if (n < 0)
		n = 0;
	decode_into_cells(rawbuf, n);
	undo_clear();
	if (cur_line > line_count() - 1)
		cur_line = line_count() - 1;
	if (cur_line < 0)
		cur_line = 0;
	snprintf(status_msg, sizeof status_msg, "%d B GB", n);
}

static char relist_name[NAME_MAX_L + 1];

/* Re-select the file we just wrote (keeps paths in sync after a re-list). */
static void fs_relist_keep(void)
{
	fs_list_dir();
	for (int i = 0; i < name_count; i++) {
		if (!strcmp(names[i], relist_name)) {
			list_sel = i;
			if (list_sel < list_top)
				list_top = list_sel;
			build_paths();
			return;
		}
	}
}

static void fs_write_current(void)
{
	int n = encode_from_cells(rawbuf, text_max);
	FILE *f = NULL;
	if (chunk_more || chunk_base > 0) {
		/* only part of the file is in memory: overwrite that byte range
		   instead of recreating the file, or the rest would be lost */
		char full[80];
		snprintf(full, sizeof full, "%s/%s", cur_dir,
		         name_count > 0 ? names[list_sel] : "");
		f = fopen(full, "r+b");
		if (f && fseek(f, (long)chunk_base, SEEK_SET) != 0) {
			fclose(f);
			f = NULL;
		}
	}
	if (!f)
		f = open_write();
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "\xB4\xE6\xB2\xBB\xC1\xCB");
		return;
	}
	fwrite(rawbuf, 1, n, f);
	fclose(f);
	dirty = 0;
	snprintf(status_msg, sizeof status_msg, "\xD2\xD1\xB4\xE6 %d \xD7\xD6\xBD\xDA", n);
}

static void fs_make_demo(void)
{
	static unsigned char const gb[] = {
		'F','X','T','X','T',' ',0xD6,0xD0,0xCE,0xC4,0xB2,0xE2,0xCA,0xD4,'\n',
		'(',' ',')',' ','[',' ',']',' ','{',' ','}',' ','<',' ','>','\n',
		'A','B','C','a','b','c',' ','0','1','2','3','\n',
		'W','W','W','-','1','1','1',' ','M','-','0',' ','|','W','|','\n',
		'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p',
		'q','r','s','t','u','v','w','x','y','z','0','1','2','3','4','5',
		'6','7','8','9','A','B','C','D','E','F','G','H','I','J','K','L','\n',
		0xC4,0xE3,0xBA,0xC3,0xA3,0xAC,0xCA,0xC0,0xBD,0xE7,'\n',
	};
	static unsigned char const u8[] = {
		'F','X','T','X','T',' ',0xE4,0xB8,0xAD,0xE6,0x96,0x87,
		0xE6,0xB5,0x8B,0xE8,0xAF,0x95,'\n',
		'A','B','C','a','b','c',' ','0','1','2','3','\n',
		0xE4,0xBD,0xA0,0xE5,0xA5,0xBD,0xEF,0xBC,0x8C,
		0xE4,0xB8,0x96,0xE7,0x95,0x8C,'\n',
	};
	char p1[96], p2[96];
	snprintf(p1, sizeof p1, "\\\\fls0\\DEMO.TXT");
	snprintf(p2, sizeof p2, "\\\\fls0\\DEMOU8.TXT");
	FILE *f = fopen(p1, "wb");
	if (!f) { f = fopen("/fls0/DEMO.TXT", "wb"); }
	if (f) { fwrite(gb, 1, sizeof gb, f); fclose(f); }
	f = fopen(p2, "wb");
	if (!f) { f = fopen("/fls0/DEMOU8.TXT", "wb"); }
	if (f) { fwrite(u8, 1, sizeof u8, f); fclose(f); }
	f = fopen("\\\\fls0\\LONG_FILENAME_DEMO.TXT", "wb");
	if (f) {
		fwrite(gb, 1, sizeof gb, f);
		fclose(f);
	}
	/* big paginated document: a token deep inside, repeated digits */
	f = fopen("\\\\fls0\\CCZD.TXT", "wb");
	if (f) {
		for (int i = 1; i <= 1200; i++) {
			char line[64];
			int n = snprintf(line, sizeof line, "LINE %04d abcdefghij", i);
			if (i == 900)
				n += snprintf(line + n, sizeof line - n, " 777");
			fwrite(line, 1, (size_t)n, f);
			fputc(10, f);
		}
		fclose(f);
	}

	/* two nesting levels (txt/sub), like the reported folder layout */
	f = fopen("\\\\fls0\\YW-JCSD.TXT", "wb");
	if (f) {
		fwrite(gb, 1, sizeof gb, f);
		fclose(f);
	}
	mkdir("\\\\fls0\\txt", 0777);
	f = fopen("\\\\fls0\\txt\\A.TXT", "wb");
	if (f) {
		fwrite(gb, 1, sizeof gb, f);
		fclose(f);
	}
	mkdir("\\\\fls0\\txt\\sub", 0777);
	f = fopen("\\\\fls0\\txt\\sub\\B.TXT", "wb");
	if (f) {
		fwrite(gb, 1, sizeof gb, f);
		fclose(f);
	}
	{
		char line[40];
		f = fopen("\\\\fls0\\BIG.TXT", "wb");
		if (f) {
			for (int i = 1; i <= 500; i++) {
				int k = snprintf(line, sizeof line,
				                 "LINE %03d  abcdefghijklmnop\n", i);
				fwrite(line, 1, k, f);
			}
			fclose(f);
		}
	}
	snprintf(status_msg, sizeof status_msg, "\xCA\xBE\xC0\xFD\xD2\xD1\xBD\xA8\xBA\xC3");
	fs_list_dir();
}

/*=== list view ============================================================*/

/*=== recent files ========================================================*/

#define RECENT_MAX 4
#define RECENT_LEN 40

static char *rec_buf;			/* RECENT_MAX * RECENT_LEN, malloc'd */
static int rec_line[RECENT_MAX];
static int rec_n;
static int list_recent;			/* the list page shows the recents */
static int rec_sel;

static FILE *rec_fopen(char const *mode)
{
	static char const *c[] = { "\\\\fls0\\FXTXT.REC", "/fls0/FXTXT.REC", "FXTXT.REC", NULL };
	for (int i = 0; c[i]; i++) {
		FILE *f = fopen(c[i], mode);
		if (f)
			return f;
	}
	return NULL;
}

/* BFile save: the stdio path needs a buffer and reboots the unit while the
   document still owns the heap; this one allocates nothing. A BFile has a
   fixed size and can only be written once, so recreate it every time. */
static int rec_bfile_save(char const *data, int len)
{
	static char const *cand[] = { "\\\\fls0\\FXTXT.REC", "/fls0/FXTXT.REC", NULL };
	uint16_t fc[48];

	if (len <= 0)
		return -1;
	for (int k = 0; cand[k]; k++) {
		int n = 0;
		for (int i = 0; cand[k][i] && n < 47; i++)
			fc[n++] = (uint16_t)(unsigned char)cand[k][i];
		fc[n] = 0;

		BFile_Remove(fc);
		int size = len;
		if (BFile_Create(fc, BFile_File, &size) < 0)
			continue;
		int fd = BFile_Open(fc, BFile_WriteOnly);
		if (fd < 0)
			continue;
		int w = BFile_Write(fd, data, len);
		BFile_Close(fd);
		if (w == len)
			return 0;
	}
	return -1;
}

static char *rec_at(int i) { return rec_buf + i * RECENT_LEN; }

static void rec_load(void)
{
	rec_n = 0;
	if (!rec_buf)
		return;
	FILE *f = rec_fopen("rb");
	if (!f)
		return;
	char line[RECENT_LEN + 16];
	while (rec_n < RECENT_MAX && fgets(line, sizeof line, f)) {
		int n = 0;
		while (line[n] && line[n] != '\r' && line[n] != '\n')
			n++;
		line[n] = 0;
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = 0;
		rec_line[rec_n] = atoi(line);
		snprintf(rec_at(rec_n), RECENT_LEN, "%s", tab + 1);
		rec_n++;
	}
	fclose(f);
}

static void rec_save(void)
{
	/* built on the stack: RECENT_MAX * (RECENT_LEN + 16) is small, and
	   no stdio buffer is involved, which is what used to reboot the
	   unit when the document still owned the heap */
	char out[RECENT_MAX * (RECENT_LEN + 16)];	/* 224 bytes */
	int n = 0;
	for (int i = 0; i < rec_n; i++) {
		int w = snprintf(out + n, sizeof out - n, "%d\t%s\n",
		                 rec_line[i], rec_at(i));
		if (w < 0 || n + w >= (int)sizeof out)
			break;
		n += w;
	}
	rec_bfile_save(out, n);
}

/* move path to the front of the list with its current line number */
static void rec_touch(char const *path, int line)
{
	if (!rec_buf || !path[0])
		return;
	if (!rec_n)
		rec_load();
	int at = -1;
	for (int i = 0; i < rec_n; i++)
		if (!strcmp(rec_at(i), path)) {
			at = i;
			break;
		}
	if (line < 0)
		line = (at >= 0) ? rec_line[at] : 1;
	if (at < 0)
		at = (rec_n < RECENT_MAX) ? rec_n++ : RECENT_MAX - 1;
	for (int i = at; i > 0; i--) {
		memcpy(rec_at(i), rec_at(i - 1), RECENT_LEN);
		rec_line[i] = rec_line[i - 1];
	}
	snprintf(rec_at(0), RECENT_LEN, "%s", path);
	rec_line[0] = line;
	rec_save();
}

/* Line this file was left on last time, or 0 when it is unknown. */
static int rec_line_of(char const *path)
{
	if (!rec_buf || !path[0])
		return 0;
	rec_load();
	for (int i = 0; i < rec_n; i++)
		if (!strcmp(rec_at(i), path))
			return rec_line[i];
	return 0;
}

/* Jump to the remembered line of the document that was just read. */
static void resume_line(void)
{
	/* the REC file is only read on demand now that the 近期 page is
	   gone; make sure the table is in memory before looking up */
	if (!rec_n)
		gint_world_switch(GINT_CALL(rec_load));
	if (name_count <= 0 || list_sel < 0 || list_sel >= name_count)
		return;
	char full[80];
	snprintf(full, sizeof full, "%s/%s", cur_dir, names[list_sel]);
	int line = rec_line_of(full);
	if (line <= 1)
		return;
	int total = line_count();
	if (line > total)
		line = total;
	cur_line = line - 1;
	cur_col = 0;
	view_top = row_of_cell(line_start(cur_line));
}

/* open the highlighted recent entry, resuming its line */
static void rec_open_sel(void)
{
	if (rec_sel < 0 || rec_sel >= rec_n)
		return;
	char path[RECENT_LEN];
	snprintf(path, sizeof path, "%s", rec_at(rec_sel));
	char *slash = strrchr(path, '/');
	if (!slash)
		return;
	*slash = 0;
	snprintf(cur_dir, sizeof cur_dir, "%s", path[0] ? path : root_dev);
	fs_list_dir();
	for (int i = 0; i < name_count; i++)
		if (!strcmp(names[i], slash + 1)) {
			list_sel = i;
			break;
		}
	fs_read_current();
	mode = MODE_VIEW;
}

static void draw_list(void)
{
	ui_no_cups = 0;
	char head[24];
	char const *folder = strrchr(cur_dir, '/');
	folder = (folder && folder[1]) ? folder + 1 : "fls0";
	snprintf(head, sizeof head, "FXTXT - %s",
	         list_recent ? "\xBD\xFC\xC6\xDA" : folder);
	ui_title(head);

	static char const *lab_list[6] = { "\xD0\xC2\xBD\xA8", "\xB8\xC4\xC3\xFB", "", "", "", "\xB0\xEF\xD6\xFA" };
	{
		for (int i = 0; i < 5; i++)
			box_lab[i] = lab_list[i];
		box_lab[5] = "\xB0\xEF\xD6\xFA";
	}

	int y = CONTENT_Y;
	if (list_sel < list_top)
		list_top = list_sel;
	if (list_sel >= list_top + 5)
		list_top = list_sel - 4;

	if (list_recent) {
		for (int i = 0; i < 5; i++) {
			int idx = list_top + i;
			if (idx >= rec_n)
				break;
			int sel = (idx == rec_sel);
			if (sel)
				drect(0, y - 1, TEXT_W - 1, y + 7, C_BLACK);
			char const *p = rec_at(idx);
			char const *nm = strrchr(p, '/');
			icon_file(1, y, sel);
			draw_fname(10, y, nm ? nm + 1 : p, sel ? 1 : 0);
			y += 9;
		}
		if (rec_n == 0)
			draw_small(10, CONTENT_Y + 8, "(empty)", 0);
	} else
	for (int i = 0; i < 5; i++) {
		int idx = list_top + i;
		if (idx >= name_count)
			break;
		int sel = (idx == list_sel);
		if (sel)
			drect(0, y - 1, TEXT_W - 1, y + 7, C_BLACK);
		if (is_dir[idx])
			icon_folder(1, y, sel);
		else
			icon_file(1, y, sel);
		draw_fname(10, y, names[idx], sel ? 1 : 0);
		y += 9;
	}
	if (name_count == 0)
		draw_small(10, CONTENT_Y + 8, "(empty)", 0);

	{
		char info[16];
		int n = list_recent ? rec_n
		      : name_count - (strcmp(cur_dir, root_dev) ? 1 : 0);
		snprintf(info, sizeof info, "%d \xB8\xF6", n);
		ui_info_right = 1;	/* the count sits at the very right */
		ui_hints(hint_list, info, 0);
	}
	dupdate();
}

/*=== text view ============================================================*/

/* Inline pinyin composition: buffer + candidate row on the status line. */
static void comp_draw(void)
{
	dhline(STATUS_Y - 1, C_BLACK);
	draw_small(2, STATUS_Y, py_buf, 0);
	int x = 2;
	for (char const *p = py_buf; *p; p++)
		x += font_adv8[(unsigned char)*p - FONT_FIRST];
	if (!py_len) {
		ui_status("ALPHA+\xBC\xFC\xB4\xF2\xD7\xD6\xC4\xB8");
		return;
	}
	int n = py_candidates();
	if (n <= 0) {
		draw_ui(x + 6, STATUS_Y, "\xCE\xDE\xC6\xA5\xC5\xE4", 0);
		return;
	}
	int room = (127 - 44) / 10;
	if (room < 1) room = 1;
	int shown = n > room ? room : n;
	int start = py_pick >= shown ? py_pick - shown + 1 : 0;
	x = 44;
	for (int i = 0; i < shown; i++) {
		int idx = start + i;
		if (idx >= n || x + 9 > 127)
			break;
		if (idx == py_pick) {
			drect(x - 1, STATUS_Y - 1, x + 8, STATUS_Y + 7, C_BLACK);
			draw_cell(x, STATUS_Y, py_candidate(idx), 1);
		} else {
			draw_cell(x, STATUS_Y, py_candidate(idx), 0);
		}
		x += 10;
	}
}

static void draw_view(void)
{
	ui_no_cups = 0;
	int rows_total = row_count();
	{
		int lim = rows_total - rows();
		if (lim < 0) lim = 0;
		if (view_top > lim) view_top = lim;
		if (view_top < 0) view_top = 0;
	}

	char head[40];
	snprintf(head, sizeof head, "%s%s", edit_mode ? "*" : "", names[list_sel]);
	ui_title_state(head, edit_mode);

	/* where is the cursor in display-row space? */
	int ccell = -1, crow = -1;
	if (edit_mode) {
		ccell = line_start(cur_line) + cur_col;
		if (ccell > ncell) ccell = ncell;
		crow = row_of_cell(ccell);
		if (crow < view_top) view_top = crow;
		if (crow >= view_top + rows()) view_top = crow - rows() + 1;
	}

	static char const *lab_read[6] = { "\xB2\xE9\xD5\xD2", "\xB1\xE0\xBC\xAD", "", "", "", "" };
	/* search result: 上一 / 下一 / 再查 ... 退出 / 帮助 */
	static char const *lab_hit[6] = {
		"\xC9\xCF\xD2\xBB", "\xCF\xC2\xD2\xBB", "\xD4\xD9\xB2\xE9",
		"", "\xCD\xCB\xB3\xF6", "\xB0\xEF\xD6\xFA"
	};
	static char const *lab_edit[6] = { "\xB1\xA3\xB4\xE6", "\xCD\xCB\xB3\xF6", "\xB4\xF3\xD0\xB4", "\xB1\xEA\xB5\xE3", "\xD6\xD0\xD3\xA2", "\xB4\xF3\xD0\xB4" };
	if (!edit_mode && search_n > 0)
		for (int i = 0; i < 6; i++)
			box_lab[i] = lab_hit[i];
	{
		static char const *r1[5] = { "\xB2\xE9\xD5\xD2", "\xB1\xE0\xBC\xAD", "", "", "\xB0\xEF\xD6\xFA" };
		static char const *e1[5] = { "\xB3\xB7\xCF\xFA", "\xB2\xE9\xD5\xD2",
		                             "", "", "\xB0\xEF\xD6\xFA" };
		for (int i = 0; i < 5; i++)
			box_lab[i] = edit_mode
				? (fbar_page ? e1[i] : lab_edit[i])
				: lab_read[i];
		box_lab[5] = edit_mode ? "\xB7\xAD\xD2\xB3" : "\xB0\xEF\xD6\xFA";
	}

	int caret_x = -1, caret_y = -1;
	int r = rows(), y = CONTENT_Y, row = view_top;
	for (int i = 0; i < r && row < rows_total; i++, row++) {
		int p = row_start(row);
		int q = next_row(p);
		int x = 0;
		int caretx = -1;
		int hl = -1;
		if (!edit_mode && search_n > 0)
			hl = search_hits[search_cur];
		for (int c = p; c < q; c++) {
			if (edit_mode && c == ccell)
				caretx = x;
			int inv = (hl >= 0 && c >= hl && c < hl + search_len);
			draw_cell(x, y, cells[c], inv);
			x += cell_adv(cells[c]);
		}
		if (edit_mode && row == crow && ccell >= q &&
		    (q >= ncell || cells[q] == CELL_NL))
			caretx = x;
		if (caretx >= 0 && caretx < TEXT_W)
			caret_x = caretx, caret_y = y;
		y += lh();
	}

	/* the caret blinks: while the phase is off the cell stays empty */
	if (caret_x >= 0 && blink_on) {
		for (int k = 0; k < lh() - 1; k++)
			px_on(caret_x, caret_y + k);
	}

	if (edit_mode && py_len > 0) {
		comp_draw();
	} else {
		char info[16];
		info[0] = 0;
		ui_info_right = 0;
		if (!edit_mode) {
			if (search_n > 0)
				snprintf(info, sizeof info, "%d/%d",
				         search_cur + 1, search_n);
			else
				snprintf(info, sizeof info, "%d/%d",
				         chunk_rows + view_top + 1,
				         total_rows > 0 ? total_rows : rows_total);
			ui_info_right = 1;
		}
		ui_hints(edit_mode ? (cn_mode ? hint_edit_cn : hint_edit_en) : hint_view, info,
		         edit_mode);
		ui_info_right = 0;
	}
	dupdate();
}

/*=== ALPHA-letter input ===================================================

   The fx-9750GIII keypad prints a letter in red above most keys; pressing
   ALPHA and then that key types the letter (the calculator's own alphabet
   input). gint reports the base key plus the ALPHA modifier flag. */
static char alpha_letter(int key)
{
	switch (key) {
	case KEY_XOT:    return 'a';
	case KEY_LOG:    return 'b';
	case KEY_LN:     return 'c';
	case KEY_SIN:    return 'd';
	case KEY_COS:    return 'e';
	case KEY_TAN:    return 'f';
	case KEY_FRAC:   return 'g';
	case KEY_FD:     return 'h';
	case KEY_LEFTP:  return 'i';
	case KEY_RIGHTP: return 'j';
	case KEY_COMMA:  return 'k';
	case KEY_ARROW:  return 'l';
	case KEY_7:      return 'm';
	case KEY_8:      return 'n';
	case KEY_9:      return 'o';
	/* DEL and AC/ON have no printed letter */
	case KEY_4:      return 'p';
	case KEY_5:      return 'q';
	case KEY_6:      return 'r';
	case KEY_MUL:    return 's';
	case KEY_DIV:    return 't';
	case KEY_1:      return 'u';
	case KEY_2:      return 'v';
	case KEY_3:      return 'w';
	case KEY_ADD:    return 'x';
	case KEY_SUB:    return 'y';
	case KEY_0:      return 'z';
	default:         return 0;
	}
}

/* Characters printed on the ALPHA overlay that are not letters. */
static char alpha_symbol(int key)
{
	if (key == KEY_DOT)
		return ' ';		/* 空格键 = [.] */
	if (key == KEY_EXP)
		return '"';		/* \xD2\xFD\xBA\xC5\xBC\xFC = [x10^x] */
	return 0;
}

static char ev_letter(key_event_t ev)
{
	if (!ev.alpha)
		return 0;
	char c = alpha_symbol(ev.key);
	if (c)
		return c;
	c = alpha_letter(ev.key);
	if (c && ev.shift)
		c = (char)(c - 32);
	return c;
}

/* Digits and operators typed directly (no ALPHA), like the calculator itself. */
static char key_ascii(int key)
{
	switch (key) {
	case KEY_0: return '0'; case KEY_1: return '1'; case KEY_2: return '2';
	case KEY_3: return '3'; case KEY_4: return '4'; case KEY_5: return '5';
	case KEY_6: return '6'; case KEY_7: return '7'; case KEY_8: return '8';
	case KEY_9: return '9';
	case KEY_ADD: return '+'; case KEY_SUB: return '-';
	case KEY_MUL: return '*'; case KEY_DIV: return '/';
	case KEY_DOT: return '.';
	case KEY_COMMA: return ',';
	case KEY_LEFTP: return '(';
	case KEY_RIGHTP: return ')';
	default: return 0;
	}
}

/*=== pinyin input method ==================================================*/

/* Find the syllable that starts with the current buffer (sorted table). */
static int py_match(void)
{
	if (!py_len)
		return -1;
	/* lower bound: first syllable that is not smaller than the buffer */
	int lo = 0, hi = PY_COUNT;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		char const *name = py_names + py_off[mid];
		if (strncmp(name, py_buf, py_len) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < PY_COUNT && !strncmp(py_names + py_off[lo], py_buf, py_len))
		return lo;
	return -1;
}

static int py_candidates(void)
{
	if (py_syllable < 0)
		return 0;
	return py_start[py_syllable + 1] - py_start[py_syllable];
}

static uint16_t py_candidate(int i)
{
	int g = py_glyph[py_start[py_syllable] + i];
	return (uint16_t)(CELL_CJK + g);
}

static void py_reset(void)
{
	py_len = 0;
	py_buf[0] = 0;
	py_syllable = -1;
	py_pick = 0;
}

static void py_refresh(void)
{
	py_syllable = py_match();
	py_pick = 0;
}

/* Cell of the n-th punctuation mark (0 if the index runs past the list). */
static uint16_t punct_cell(int n)
{
	if (n < 0 || n >= PUNCT_COUNT || !punct_gb[n * 2])
		return 0;
	int g = (punct_gb[n * 2] - 0xA1) * 94 + (punct_gb[n * 2 + 1] - 0xA1);
	return (g >= 0 && g < FONT_GLYPHS) ? (uint16_t)(CELL_CJK + g) : 0;
}

/*=== search prompt ========================================================*/

static void draw_search(void)
{
	ui_no_cups = 0;
	ui_show_input = 1;
	ui_title("\xCB\xD1\xCB\xF7");
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	box_lab[0] = "\xB2\xE9\xD5\xD2";		/* F1  查找 */
	box_lab[1] = "\xC9\xCF\xD2\xBB";		/* F2  上一条 */
	box_lab[2] = "\xCF\xC2\xD2\xBB";		/* F3  下一条 */
	box_lab[4] = "\xB0\xEF\xD6\xFA";		/* F5  帮助 */
	int y = CONTENT_Y + 2;
	draw_ui(2, y, "\xB2\xE9\xD5\xD2", 0);
	int x = 30;
	for (int i = 0; i < search_len && x < 120; i++) {
		draw_cell(x, y, search_cells[i], 0);
		x += cell_adv(search_cells[i]);
	}
	for (int k = 0; k < 7; k++)
		px_on(x + 1, y + k);

	y += 10;
	draw_small(2, y, py_buf, 0);
	int n = py_candidates();
	if (n > 0) {
		int room = (127 - 40) / 10;
		if (room < 1) room = 1;
		int shown = n > room ? room : n;
		int start = py_pick >= shown ? py_pick - shown + 1 : 0;
		int cx = 40;
		for (int i = 0; i < shown; i++) {
			int idx = start + i;
			if (idx >= n || cx + 9 > 127)
				break;
			if (idx == py_pick) {
				drect(cx - 1, y - 1, cx + 8, y + 7, C_BLACK);
				draw_cell(cx, y, py_candidate(idx), 1);
			} else {
				draw_cell(cx, y, py_candidate(idx), 0);
			}
			cx += 10;
		}
	}
	ui_hints(py_len ? "EXE\xC9\xCF\xC6\xC1 \xA1\xFB\xA1\xFA\xD1\xA1" : "EXE\xCB\xD1\xCB\xF7 EXIT\xCD\xCB", "", 1);
	dupdate();
}

static void handle_search_key(key_event_t ev)
{
	if (py_len == 0) {
		if (ev.key == KEY_F2 || ev.key == KEY_F3) {
			if (search_n > 0) {
				search_cur = (search_cur +
				              (ev.key == KEY_F2 ? search_n - 1 : 1)) % search_n;
				search_show();
				mode = MODE_VIEW;
			}
			return;
		}
		if (ev.key == KEY_F1) {
			/* the query lives in search_cells already; rebuilding it from
			   the old keyboard buffer used to wipe what was just typed */
			if (search_len) {
				run_search();
				mode = MODE_VIEW;
			}
			return;
		}
		if (ev.key == KEY_F5 && ev.shift == 0) {
			help_back = MODE_SEARCH;
			mode = MODE_HELP;
			return;
		}
	}
	if (py_len > 0) {
		int n = py_candidates();
		switch (ev.key) {
		case KEY_LEFT:
			if (n > 0) py_pick = (py_pick + n - 1) % n;
			return;
		case KEY_RIGHT:
			if (n > 0) py_pick = (py_pick + 1) % n;
			return;
		case KEY_EXE:
			if (n > 0) {
				if (search_len < 31)
					search_cells[search_len++] = py_candidate(py_pick);
				py_reset();
				return;
			}
			break;
		case KEY_DEL:
		case KEY_F2:
			py_buf[--py_len] = 0;
			py_refresh();
			return;
		case KEY_EXIT:
			py_reset();
			return;
		default:
			break;
		}
	}

	if (ev.key == KEY_EXIT) {
		py_reset();
		mode = MODE_VIEW;
		return;
	}
	if (ev.key == KEY_EXE) {
		py_reset();
		run_search();
		mode = MODE_VIEW;
		return;
	}
	if (ev.key == KEY_DEL || ev.key == KEY_F2) {
		if (search_len > 0)
			search_len--;
		return;
	}

	char c = ev_letter(ev);
	if (!c && alpha_lock)
		c = alpha_letter(ev.key);
	if (!c && !ev.alpha && !ev.shift)
		c = key_ascii(ev.key);
	if (!c)
		return;
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
		if (py_len < 7) {
			py_buf[py_len++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
			py_buf[py_len] = 0;
			py_refresh();
		}
	} else if (search_len < 31) {
		search_cells[search_len++] = (uint16_t)((unsigned char)c - 0x20);
	}
}

/*=== on-screen keyboard (dialog) ==========================================*/

static char const *pages[] = {
	"abcdefghijklmnopqrstuvwxyz .?!",
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ ,'\"-",
	"0123456789+-*/=<>()[]{}:;",
	"#$%&@^_\\|~`\x7f",
};

/* Grid geometry: pinyin uses 13x2 letters, the ASCII pages use 6x5. */
static int kb_cols(void)
{
	return (kb_page == PAGE_PINYIN || kb_page == PAGE_PUNCT) ? 13 : 6;
}

static char kb_char(int row, int col)
{
	if (kb_page == PAGE_PUNCT)
		return 0;
	if (kb_page == PAGE_PINYIN) {
		int i = row * 13 + col;
		return (i >= 0 && i < 26) ? (char)('a' + i) : 0;
	}
	char const *p = pages[kb_page - 2];
	int n = (int)strlen(p);
	int i = row * 6 + col;
	return (i >= 0 && i < n) ? p[i] : 0;
}

/*=== undo =================================================================*/

#define UNDO_MAX 64

typedef struct {
	unsigned char kind;	/* 0: the edit added this cell, 1: it removed it */
	int pos;
	uint16_t cell;
} undo_rec_t;

static undo_rec_t undo_buf[UNDO_MAX];
static int undo_n;

static void undo_push(int kind, int pos, uint16_t cell)
{
	if (undo_n == UNDO_MAX) {
		memmove(undo_buf, undo_buf + 1,
		        sizeof undo_buf[0] * (UNDO_MAX - 1));
		undo_n--;
	}
	undo_buf[undo_n].kind = (unsigned char)kind;
	undo_buf[undo_n].pos = pos;
	undo_buf[undo_n].cell = cell;
	undo_n++;
}

static void undo_apply(void)
{
	if (undo_n <= 0)
		return;
	undo_rec_t r = undo_buf[--undo_n];

	if (r.kind == 0) {
		/* take the cell we added out again */
		if (r.pos >= 0 && r.pos < ncell) {
			memmove(cells + r.pos, cells + r.pos + 1,
			        sizeof(uint16_t) * (size_t)(ncell - r.pos - 1));
			ncell--;
		}
	} else {
		/* put back the cell we deleted */
		if (ncell < text_max && r.pos >= 0 && r.pos <= ncell) {
			memmove(cells + r.pos + 1, cells + r.pos,
			        sizeof(uint16_t) * (size_t)(ncell - r.pos));
			cells[r.pos] = r.cell;
			ncell++;
		}
	}

	int pos = r.pos;
	if (pos > ncell)
		pos = ncell;
	if (pos < 0)
		pos = 0;
	cur_line = offset_to_line(pos);
	cur_col = pos - line_start(cur_line);
	dirty = 1;
}

static void undo_clear(void)
{
	undo_n = 0;
}

static void text_insert(uint16_t cell)
{
	if (ncell >= text_max)
		return;
	int off = line_start(cur_line) + cur_col;
	if (off > ncell)
		off = ncell;
	memmove(cells + off + 1, cells + off, (ncell - off) * sizeof(uint16_t));
	cells[off] = cell;
	ncell++;
	cur_col++;
	dirty = 1;
	undo_push(0, off, cell);
}

static void text_newline(void)
{
	if (ncell >= text_max)
		return;
	int off = line_start(cur_line) + cur_col;
	if (off > ncell)
		off = ncell;
	memmove(cells + off + 1, cells + off, (ncell - off) * sizeof(uint16_t));
	cells[off] = CELL_NL;
	ncell++;
	cur_line++;
	cur_col = 0;
	dirty = 1;
	undo_push(0, off, CELL_NL);
}

static void text_backspace(void)
{
	int off = line_start(cur_line) + cur_col;

	if (off <= 0 || ncell <= 0)
		return;
	undo_push(1, off - 1, cells[off - 1]);
	memmove(cells + off - 1, cells + off, (ncell - off) * sizeof(uint16_t));
	ncell--;
	if (cur_col > 0) {
		cur_col--;
	} else if (cur_line > 0) {
		/* joined with the previous line: caret lands on the join point */
		cur_line--;
		cur_col = (off - 1) - line_start(cur_line);
		if (cur_col < 0) cur_col = 0;
	}
	dirty = 1;
}

/* Append one cell to the current target (document or search prompt). */
static void kb_put(uint16_t cell)
{
	if (kb_target == 1) {
		if (kb_len < (int)(sizeof kb_cells / sizeof kb_cells[0]))
			kb_cells[kb_len++] = cell;
		return;
	}
	text_insert(cell);
}

/* ASCII punctuation -> its full-width counterpart, resolved through the
   Unicode -> glyph table (the full-width forms live in GB2312 row 3, not 1). */
static uint16_t kb_fullwidth(uint16_t cell)
{
	static unsigned char const map[][3] = {
	{',', 0xA3, 0xAC}, {'.', 0xA1, 0xA3},
	{':', 0xA3, 0xBA}, {';', 0xA3, 0xBB},
	{'?', 0xA3, 0xBF}, {'!', 0xA3, 0xA1},
	{'(', 0xA3, 0xA8}, {')', 0xA3, 0xA9},
	{'[', 0xA3, 0xDB}, {']', 0xA3, 0xDD},
	{'{', 0xA3, 0xFB}, {'}', 0xA3, 0xFD},
	};
	if (cell >= CELL_CJK)
		return cell;
	char c = (char)(cell + 0x20);
	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
		if ((char)map[i][0] == c) {
			int g = (map[i][1] - 0xA1) * 94 + (map[i][2] - 0xA1);
			if (g >= 0 && g < FONT_GLYPHS)
				return (uint16_t)(CELL_CJK + g);
		}
	}
	return cell;
}

static void kb_type(char ch)
{
	uint16_t cell = (uint16_t)((unsigned char)ch - 0x20);
	if (cn_mode)
		cell = kb_fullwidth(cell);
	kb_put(cell);
}

static void kb_backspace(void)
{
	if (kb_target == 1) {
		if (kb_len > 0)
			kb_len--;
		return;
	}
	text_backspace();
}

static void draw_keyboard(void)
{
	ui_no_cups = 0;
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	int pinyin = (kb_page == PAGE_PINYIN);
	int punct = (kb_page == PAGE_PUNCT);
	if (punct) {
		box_lab[0] = "\xBB\xBB\xD2\xB3";	/* 换页 */
		box_lab[5] = "\xCD\xEA\xB3\xC9";	/* 完成 */
	} else if (pinyin) {
		box_lab[0] = "\xBB\xBB\xD2\xB3";
		box_lab[2] = "\xCF\xC2\xD1\xA1";	/* 下选 */
		box_lab[3] = "\xC9\xCF\xD1\xA1";	/* 上选 */
		box_lab[4] = "\xC9\xCF\xC6\xC1";	/* 上屏 */
		box_lab[5] = "\xCD\xEA\xB3\xC9";
	} else {
		box_lab[0] = "\xBB\xBB\xD2\xB3";
		box_lab[5] = "\xCD\xEA\xB3\xC9";
	}
	ui_title(punct ? "\xB1\xEA\xB5\xE3\xB7\xFB\xBA\xC5"
	               : pinyin ? (kb_target == 1 ? "\xC6\xB4\xD2\xF4\xCB\xD1\xCB\xF7" : "\xC6\xB4\xD2\xF4\xCA\xE4\xC8\xEB")
	                        : (kb_target == 1 ? "\xCB\xD1\xCB\xF7\xCE\xC4\xD7\xD6" : "\xB2\xE5\xC8\xEB\xD7\xD6\xB7\xFB"));

	int grid_y;
	if (pinyin) {
		/* input line */
		char line[16];
		if (py_len)
			snprintf(line, sizeof line, "%s_", py_buf);
		else
			snprintf(line, sizeof line, "\xCA\xE4\xC8\xEB\xC6\xB4\xD2\xF4");
		draw_ui(2, CONTENT_Y + 1, line, 0);

		/* candidate row */
		int ncand = py_candidates();
		int y = CONTENT_Y + 11;
		if (ncand > 0) {
			int shown = ncand > 10 ? 10 : ncand;
			int start = 0;
			if (py_pick >= shown)
				start = py_pick - shown + 1;
			int x = 2;
			for (int i = 0; i < shown; i++) {
				int idx = start + i;
				if (idx >= ncand || x + 10 > 126)
					break;
				if (idx == py_pick) {
					drect(x - 1, y - 1, x + 9, y + 9, C_BLACK);
					draw_cell(x + 1, y, py_candidate(idx), 1);
				} else {
					draw_cell(x + 1, y, py_candidate(idx), 0);
				}
				x += 11;
			}
		} else if (py_len) {
			draw_ui(2, y, "\xCE\xDE\xC6\xA5\xC5\xE4", 0);
		}
		grid_y = CONTENT_Y + 22;
	} else {
		grid_y = CONTENT_Y + 2;
	}

	int gc = kb_cols();
	int gr = punct ? 3 : (pinyin ? 2 : 5);
	for (int r = 0; r < gr; r++) {
		for (int c = 0; c < gc; c++) {
			char ch = kb_char(r, c);
			uint16_t pcell = punct ? punct_cell(r * 13 + c) : 0;
			if (!ch && !pcell)
				continue;
			int x = 3 + c * ((pinyin || punct) ? 9 : 20);
			int y = grid_y + r * 9;
			if (y + 8 > STATUS_Y - 2)
				continue;
			int sel = (r == kb_row && c == kb_col);
			if (sel)
				drect(x - 1, y - 1, x + 8, y + 8, C_BLACK);
			draw_cell(x, y, pcell ? pcell : (uint16_t)((unsigned char)ch - 0x20),
			          sel ? 1 : 0);
		}
	}

	/* status: echo the search prompt, or key hints */
	if (kb_target == 1) {
		dhline(STATUS_Y - 1, C_BLACK);
		int x = 2;
		for (int i = 0; i < kb_len && x < 118; i++) {
			draw_cell(x, STATUS_Y, kb_cells[i], 0);
			x += cell_adv(kb_cells[i]);
		}
		for (int k = 0; k < 7; k++)
			px_on(x + 1, STATUS_Y + k);
	} else if (pinyin) {
		ui_status(py_candidates() ? "F3/F4 \xD1\xA1\xD7\xD6   F5 \xC9\xCF\xC6\xC1" : "F1 \xBB\xBB\xD2\xB3   F6 \xCD\xEA\xB3\xC9");
	} else if (punct) {
		ui_status("EXE \xCA\xE4\xC8\xEB   F1 \xBB\xBB\xD2\xB3   F6 \xCD\xEA\xB3\xC9");
	} else {
		ui_status("F1 \xBB\xBB\xD2\xB3   F6 \xCD\xEA\xB3\xC9");
	}
	dupdate();
}

/*=== help =================================================================*/

static int help_page;
static int help_back = MODE_LIST;	/* where to return after help */
static char del_name[NAME_MAX_L + 1];	/* file waiting for confirmation */
static int confirm_kind;		/* 0 = delete, 1 = discard edits */

static int help_page;

static void draw_help(void)
{
	ui_no_cups = 0;
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	static char const *page1[] = {
		"\xA1\xFC\xA1\xFD\x20\xD1\xA1\xD4\xF1\x20\x20\x45\x58\x45\x20\xB4\xF2\xBF\xAA",
		"\x46\x31\x20\xD0\xC2\xBD\xA8\x20\x20\x46\x32\x20\xB8\xC4\xC3\xFB",
		"\x46\x33\x20\xBD\xFC\xC6\xDA\x20\x20\x46\x36\x20\xB0\xEF\xD6\xFA",
		"\x44\x45\x4C\x20\xC9\xBE\xB3\xFD\x20\x20\x45\x58\x49\x54\x20\xC9\xCF\xBC\xB6",
	};
	static char const *page2[] = {
		"\xA1\xFC\xA1\xFD\x20\xB9\xF6\xB6\xAF\x20\x20\xA1\xFB\xA1\xFA\x20\xB7\xAD\xD2\xB3",
		"\x46\x31\x20\xB2\xE9\xD5\xD2\x20\x20\x46\x32\x20\xB1\xE0\xBC\xAD",
		"\x46\x36\x20\xB0\xEF\xD6\xFA\x20\x20\x45\x58\x49\x54\x20\xC1\xD0\xB1\xED",
		"\xA1\xFC\xA1\xFD\x20\xC5\xD0\xB6\xA8\xCE\xBB\xD6\xC3",
	};
	static char const *page3[] = {
		"\x46\x31\x20\xB1\xA3\xB4\xE6\x20\x20\x46\x32\x20\xCD\xCB\xB3\xF6",
		"\x46\x33\x20\xB4\xF3\xD0\xB4\x20\x20\x46\x34\x20\xB1\xEA\xB5\xE3",
		"\x46\x35\x20\xD6\xD0\xD3\xA2\x20\x20\x46\x36\x20\xB7\xAD\xD2\xB3",
		"\x45\x58\x45\x20\xBB\xBB\xD0\xD0\x20\x20\x44\x45\x4C\x20\xC9\xBE\xD7\xD6",
	};
	static char const *titles[] = { "\xC1\xD0\xB1\xED\xD2\xB3", "\xD4\xC4\xB6\xC1\xD2\xB3", "\xB1\xE0\xBC\xAD\xD2\xB3" };
	char const **lines = help_page == 0 ? page1 : (help_page == 1 ? page2 : page3);
	char head[32];
	snprintf(head, sizeof head, "\xB0\xEF\xD6\xFA %d/3", help_page + 1);
	ui_title(head);

	/* page name on the first content line, then four key lines */
	draw_ui(2, CONTENT_Y, titles[help_page], 0);

	int y = CONTENT_Y + 9;
	for (int i = 0; i < 4; i++, y += 9)
		draw_ui(2, y, lines[i], 0);

	ui_hints("\xA1\xFB\xA1\xFA \xB7\xAD\xD2\xB3  EXIT \xB9\xD8\xB1\xD5", "", 0);
	dupdate();
}

/*=== search ===============================================================*/

static void build_search_cells(void)
{
	search_len = 0;
	for (int i = 0; i < kb_len && search_len < 31; i++)
		search_cells[search_len++] = kb_cells[i];
}

/* Store every occurrence of the query, remembering where the caret is. */
static void search_collect(void)
{
	search_n = 0;
	search_cur = 0;
	if (!search_len)
		return;
	int here = line_start(cur_line) + cur_col;

	for (int i = 0; i + search_len <= ncell && search_n < SEARCH_MAX_HITS; i++) {
		int k = 0;
		while (k < search_len && cells[i + k] == search_cells[k])
			k++;
		if (k != search_len)
			continue;
		search_hits[search_n++] = i;
		if (i < here)
			search_cur = search_n;	/* the next one is the first after us */
	}
	if (search_cur >= search_n)
		search_cur = 0;
}

/* Show match #search_cur and put the caret right behind it. */
static void search_show(void)
{
	if (!search_n)
		return;
	int hit = search_hits[search_cur];
	cur_line = offset_to_line(hit);
	cur_col = hit - line_start(cur_line) + search_len;
	int llen = line_len(line_start(cur_line));
	if (cur_col > llen)
		cur_col = llen;
	view_top = row_of_cell(hit);
	if (view_top > row_count() - rows())
		view_top = row_count() - rows();
	if (view_top < 0)
		view_top = 0;
	hscroll = 0;
	snprintf(status_msg, sizeof status_msg, "ÃüÖÐ%d %d/%d",
	         search_n, search_cur + 1, search_n);
}

static int find_cells(int from)
{
	if (!search_len) return -1;
	for (int i = from; i + search_len <= ncell; i++) {
		int k = 0;
		while (k < search_len && cells[i + k] == search_cells[k]) k++;
		if (k == search_len) return i;
	}
	return -1;
}

static int offset_to_line(int off)
{
	int line = 0;
	for (int i = 0; i < off && i < ncell; i++)
		if (cells[i] == CELL_NL) line++;
	return line;
}

static void run_search(void)
{
	for (int guard = 0; guard < 64; guard++) {
		search_collect();
		if (!chunk_more)
			break;
		load_chunk(chunk_base + chunk_bytes);
		view_top = 0;
	}
	if (!search_n) {
		snprintf(status_msg, sizeof status_msg, "\xC3\xBB\xD5\xD2\xB5\xBD");
		return;
	}
	search_show();
}

/*=== input ================================================================*/

static void move_cursor_line(int delta)
{
	int tl = line_count();
	int nl = cur_line + delta;
	if (nl < 0) nl = 0;
	if (nl >= tl) nl = tl - 1;
	int len = line_len(line_start(nl));
	cur_line = nl;
	if (cur_col > len) cur_col = len;
	if (cur_line < view_top) view_top = cur_line;
	if (cur_line >= view_top + rows()) view_top = cur_line - rows() + 1;
}

static int row_start_of(int p)
{
	int q = 0;

	while (q < ncell) {
		int n = next_row(q);
		if (n <= q) break;
		if (p < n) return q;
		q = n;
	}
	return q;
}

/* Move the caret one *display* row (a wrapped line spans several rows). */
static void move_cursor_row(int delta)
{
	int ccell = line_start(cur_line) + cur_col;

	if (ccell > ncell) ccell = ncell;
	int rs = row_start_of(ccell);
	int off = ccell - rs;
	int target;

	if (delta > 0) {
		int n = next_row(rs);
		if (n <= rs || n >= ncell) {
			target = ncell;
			off = 0;
		} else
			target = n;
	} else {
		if (rs == 0) return;
		int prev = 0, q = 0;
		while (q < rs) {
			int n = next_row(q);
			if (n <= q) break;
			prev = q;
			q = n;
		}
		target = prev;
	}

	int end = next_row(target);
	if (end <= target) end = ncell;
	/* a row that ends with a newline cannot hold the caret after it */
	if (end > target && end <= ncell && cells[end - 1] == CELL_NL)
		end--;
	if (off > end - target) off = end - target;
	if (off < 0) off = 0;

	int cell = target + off;
	if (cell > ncell) cell = ncell;
	cur_line = offset_to_line(cell);
	cur_col = cell - line_start(cur_line);

	int crow = row_of_cell(cell);
	if (crow < view_top) view_top = crow;
	if (crow >= view_top + rows()) view_top = crow - rows() + 1;
	if (view_top < 0) view_top = 0;
}

static void open_keyboard(int target, int page)
{
	kb_target = target;
	kb_len = 0;
	kb_page = page;
	kb_row = kb_col = 0;
	py_reset();
	mode = MODE_KEYB;
}

static void handle_view_key(key_event_t ev)
{
	if (!edit_mode && search_n > 0) {
		/* F1 / F4 walk the matches; their normal meaning returns when
		   no search is active */
		if (ev.key == KEY_F1) {
			search_cur = (search_cur + search_n - 1) % search_n;
			search_show();
			return;
		}
		if (ev.key == KEY_F2 || ev.key == KEY_F4) {
			search_cur = (search_cur + 1) % search_n;
			search_show();
			return;
		}
		if (ev.key == KEY_F3) {		/* back to the query page */
			mode = MODE_SEARCH;
			return;
		}
		if (ev.key == KEY_F5) {		/* leave the result state */
			search_n = 0;
			search_len = 0;
			return;
		}
	}
	/* While a pinyin composition is open, its own keys win. */
	if (edit_mode && py_len > 0) {
		int n = py_candidates();
		switch (ev.key) {
		case KEY_UP:
		case KEY_DOWN:
			return;			/* nothing while composing */
		case KEY_LEFT:
			if (n > 0) py_pick = (py_pick + n - 1) % n;
			return;
		case KEY_RIGHT:
			if (n > 0) py_pick = (py_pick + 1) % n;
			return;
		case KEY_EXE:
			if (n > 0)
				text_insert(py_candidate(py_pick));
			py_reset();
			return;
		case KEY_DEL:
		case KEY_F2:
			py_buf[--py_len] = 0;
			py_refresh();
			return;
		case KEY_EXIT:
			py_reset();
			return;
		default:
			break;
		}
	}

	if (ev.key == KEY_F2)
		ev.key = KEY_F3;	/* F2 and F3 both toggle editing */	/* F2 编辑 is the old F3 */
	if (!edit_mode && ev.key == KEY_F1) {
		mode = MODE_SEARCH;
		py_reset();
		return;
	}
	if (ev.key == KEY_F6) {
		if (edit_mode) {
			fbar_page = fbar_page ? 0 : 1;
		} else {
			help_back = MODE_VIEW;
			mode = MODE_HELP;
		}
		return;
	}
	if (edit_mode && fbar_page && ev.key == KEY_F1) {
		undo_apply();
		return;
	}


	if (ev.key == KEY_F5 && (!edit_mode || ev.shift || fbar_page)) {
		help_back = MODE_VIEW;
		mode = MODE_HELP;
		return;
	}
	if (edit_mode && ev.key == KEY_F5 && !fbar_page) {
		if (ev.shift) {
			/* SHIFT+F5 = 字母锁（与 SHIFT+ALPHA 等价），只显示徽标 */
			alpha_lock = !alpha_lock;
		} else {
			/* only the leading 中/英 changes, no message */
			cn_mode = !cn_mode;
			py_reset();
		}
		return;
	}

	/* ALPHA + key types the printed letter; sticky mode does it for any key. */
	if (edit_mode) {
		char c = ev_letter(ev);
		if (!c && alpha_lock) {
			/* locked ALPHA types the printed overlay, so [.] is a space */
			c = alpha_symbol(ev.key);
			if (!c)
				c = alpha_letter(ev.key);
			if (c && ev.shift && c >= 'a' && c <= 'z')
				c = (char)(c - 32);
		}
		if (!c && !ev.alpha && !ev.shift &&
		    !(ev.key >= KEY_F1 && ev.key <= KEY_F6) &&
		    ev.key != KEY_EXE && ev.key != KEY_EXIT && ev.key != KEY_DEL &&
		    ev.key != KEY_UP && ev.key != KEY_DOWN &&
		    ev.key != KEY_LEFT && ev.key != KEY_RIGHT)
			c = key_ascii(ev.key);
		if (c) {
			if (caps_on && c >= 'a' && c <= 'z')
				c = (char)(c - 32);
			if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) && cn_mode) {
				/* start composing pinyin instead of inserting the letter */
				if (py_len < 7) {
					py_buf[py_len++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
					py_buf[py_len] = 0;
					py_refresh();
				}
			} else if (py_len == 0) {
				uint16_t cell = (uint16_t)((unsigned char)c - 0x20);
				if (cn_mode)
					cell = kb_fullwidth(cell);
				text_insert(cell);
			}
			return;
		}
	}

	switch (ev.key) {
	case KEY_EXIT:
		if (edit_mode && dirty) {
			confirm_kind = 1;
			mode = MODE_CONFIRM;
			return;
		}
		search_n = 0;
		search_len = 0;
		gint_world_switch(GINT_CALL(fs_save_state));
		if (name_count > 0 && list_sel >= 0 && list_sel < name_count) {
			char full[80];
			snprintf(full, sizeof full, "%s/%s", cur_dir,
			         names[list_sel]);
			rec_touch(full, cur_line + 1);
		}
		mode = MODE_LIST;
		return;
	case KEY_UP:
		if (edit_mode) {
			move_cursor_row(-1);
		} else {
			int total = row_count();
			view_top -= rows();
			if (view_top < 0) {
				view_top = 0;
				if (chunk_base > 0) {
					int back = chunk_base - text_max;
					if (back < 0)
						back = 0;
					int keep = back;
					chunk_rows = keep > 0 ? rows_upto(keep) : 0;
					load_chunk(keep);
					view_top = row_count() - rows();
					if (view_top < 0)
						view_top = 0;
					return;
				}
			}
			(void)total;
		}
		return;
	case KEY_DOWN:
		if (edit_mode) {
			move_cursor_row(1);
		} else {
			int total = row_count();
			int lim = total - rows();
			if (lim < 0)
				lim = 0;
			view_top += rows();
			if (view_top > lim) {
				view_top = lim;
				if (chunk_more) {
					chunk_rows += row_count();
					load_chunk(chunk_base + chunk_bytes);
					view_top = 0;
					hscroll = 0;
					return;
				}
			}
			if (view_top < 0)
				view_top = 0;
		}
		return;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (edit_mode) {
			/* Move by one cell in absolute terms: at a wrap point the
			   caret then hops to the neighbouring display row, which
			   column arithmetic inside one logical line cannot do. */
			int off = line_start(cur_line) + cur_col;
			off += (ev.key == KEY_RIGHT) ? 1 : -1;
			if (off < 0)
				off = 0;
			if (off > ncell)
				off = ncell;
			cur_line = offset_to_line(off);
			cur_col = off - line_start(cur_line);
		}
		return;
	case KEY_F2:
		/* page one: 大写锁定, page two: 查找 */
		if (edit_mode && !fbar_page) {
			caps_on = !caps_on;
			return;
		}
		mode = MODE_SEARCH;
		py_reset();
		return;
	case KEY_F1:
		if (edit_mode) {
			strncpy(relist_name, names[list_sel], NAME_MAX_L);
			relist_name[NAME_MAX_L] = 0;
			gint_world_switch(GINT_CALL(fs_write_current));
			dirty = 0;
			gint_world_switch(GINT_CALL(fs_relist_keep));
		}
		return;
	case KEY_F4:
		if (edit_mode)
			open_keyboard(0, PAGE_PUNCT);
		return;
	case KEY_F6:
		if (!edit_mode && fbar_page && ev.key == KEY_F5) {
			help_back = MODE_VIEW;
			mode = MODE_HELP;
			return;
		}
		fbar_page = fbar_page ? 0 : 1;
		return;
	case KEY_F3:
		if (edit_mode && dirty) {
			confirm_kind = 1;
			mode = MODE_CONFIRM;
			return;
		}
		edit_mode = !edit_mode;
		fbar_page = 0;		/* every page starts on F bar page one */
		if (edit_mode) {
			/* view_top counts display rows but cur_line counts
			   logical lines: convert, otherwise paging N times
			   moves the caret N*wrap lines further down. */
			int rp = row_start(view_top);
			cur_line = offset_to_line(rp);
			if (cur_line > line_count() - 1)
				cur_line = line_count() - 1;
			/* first cell of that logic line as shown on this page,
			   caret one character further right */
			int base = line_start(cur_line);
			int len = line_len(base);
			int col = rp - base;
			if (col < 0)
				col = 0;
			if (col > len)
				col = len;
			cur_col = (col < len) ? col + 1 : len;
		} else {
			py_reset();
		}
		return;
	case KEY_DEL:
		if (edit_mode) {
			if (ev.shift) {
				undo_apply();
				return;
			}
			text_backspace();
			return;
		}
		return;
	case KEY_EXE:
		if (edit_mode)
			text_newline();		/* 换行 = EXE */
		else {
			help_back = MODE_VIEW;
			mode = MODE_HELP;
		}
		return;
	default:
		return;
	}
}

static void handle_kb_key(key_event_t ev)
{
	char ch = kb_char(kb_row, kb_col);
	int pinyin = (kb_page == PAGE_PINYIN);
	int punct = (kb_page == PAGE_PUNCT);
	int gc = kb_cols(), gr = punct ? 3 : (pinyin ? 2 : 5);

	/* Letters: ALPHA+key everywhere, and a bare key on the pinyin page.
	   Function/arrow/DEL keys are always handled as themselves. */
	{
		int ui_key = (ev.key >= KEY_F1 && ev.key <= KEY_F6) ||
		             ev.key == KEY_EXE || ev.key == KEY_EXIT ||
		             ev.key == KEY_UP || ev.key == KEY_DOWN ||
		             ev.key == KEY_LEFT || ev.key == KEY_RIGHT ||
		             ev.key == KEY_DEL || ev.key == KEY_ACON;
		char c = ui_key ? 0 : ev_letter(ev);
		if (!c && !ui_key && !ev.alpha && !ev.shift && pinyin)
			c = alpha_letter(ev.key);
		if (c) {
			if (pinyin) {
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
					if (py_len < 7) {
						py_buf[py_len++] = (char)((c >= 'A' && c <= 'Z')
							? c + 32 : c);
						py_buf[py_len] = 0;
						py_refresh();
					}
				}
			} else {
				kb_type(c);
			}
			return;
		}
	}

	switch (ev.key) {
	case KEY_UP:    if (kb_row > 0) kb_row--; return;
	case KEY_DOWN:  if (kb_row < gr - 1) kb_row++; return;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (punct) {
			int i = kb_row * 13 + kb_col;
			int step = (ev.key == KEY_RIGHT) ? 1 : -1;
			for (int k = 0; k < 26; k++) {
				i += step;
				if (i < 0) i = 25;
				if (i > 25) i = 0;
				if (punct_cell(i))
					break;
			}
			kb_row = i / 13;
			kb_col = i % 13;
			return;
		}
		if (ev.key == KEY_LEFT) {
			if (kb_col > 0) kb_col--;
		} else {
			if (kb_col < gc - 1) kb_col++;
		}
		return;
	case KEY_F1:
		kb_page = (kb_page + 1) % PAGE_COUNT;
		kb_row = kb_col = 0;
		py_reset();
		return;
	case KEY_DEL:
	case KEY_F2:
		if (pinyin && py_len > 0) {
			py_buf[--py_len] = 0;
			py_refresh();
		} else {
			kb_backspace();
		}
		return;
	case KEY_F3:
		if (pinyin) {
			if (py_candidates() > 0)
				py_pick = (py_pick + 1) % py_candidates();
		}
		return;
	case KEY_F4:
		if (pinyin) {
			int n = py_candidates();
			if (n > 0)
				py_pick = (py_pick + n - 1) % n;
		} else {
			kb_type(' ');
		}
		return;
	case KEY_F5:
		if (pinyin) {
			if (py_candidates() > 0) {
				kb_put(py_candidate(py_pick));
				py_reset();
			}
		}
		return;
	case KEY_EXE:
		if (punct) {
			uint16_t pc = punct_cell(kb_row * 13 + kb_col);
			if (!pc) {			/* empty cell: jump to the next one */
				int i = kb_row * 13 + kb_col;
				while (i < PUNCT_COUNT - 1 && !punct_cell(++i))
					;
				if (i < 26) {
					kb_row = i / 13;
					kb_col = i % 13;
				}
				return;
			}
			if (pc) {
				kb_put(pc);
				if (kb_target == 0) {		/* back to the document */
					py_reset();
					mode = MODE_VIEW;
				}
			}
			return;
		}
		if (pinyin) {
			if (ch && py_len < 7) {
				py_buf[py_len++] = ch;
				py_buf[py_len] = 0;
				py_refresh();
			}
		} else if (ch) {
			kb_type(ch);
			if (kb_target == 0) {			/* back to the document */
				py_reset();
				mode = MODE_VIEW;
			}
		}
		return;
	case KEY_F6:
	case KEY_EXIT:
		if (kb_target == 1) {
			run_search();
			kb_target = 0;
		}
		py_reset();
		mode = MODE_VIEW;
		return;
	default:
		return;
	}
}

static void fs_delete_selected(void);

static void fs_delete_selected(void)
{
	char name[NAME_MAX_L + 1];
	uint16_t fc[NAME_FC_MAX];
	strncpy(name, del_name, NAME_MAX_L);
	name[NAME_MAX_L] = 0;
	memcpy(fc, names_fc[list_sel], sizeof fc);
	if (BFile_Remove(fc) != 0)
		fs_delete_path(name);
	fs_list_dir();
	if (list_sel >= name_count)
		list_sel = name_count ? name_count - 1 : 0;
	snprintf(status_msg, sizeof status_msg, "\xD2\xD1\xC9\xBE\xB3\xFD %s", name);
	status_sticky = 1;
}

/* Confirmation screen for deleting a file. */
#define DBG_MAX 60
#define DBG_PAGE 4




static void draw_confirm(void)
{
	ui_no_cups = 1;
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	if (confirm_kind == 1) {
		ui_title("\xCE\xB4\xB1\xA3\xB4\xE6");
		draw_ui(2, CONTENT_Y + 2, "\xB7\xC5\xC6\xFA\xD0\xDE\xB8\xC4\xA3\xBF", 0);
		draw_ui(2, CONTENT_Y + 12, "EXE\xB7\xC5\xC6\xFA", 0);
		draw_ui(2, CONTENT_Y + 22, "EXIT\xBC\xCC\xD0\xF8\xB1\xE0\xBC\xAD", 0);
		dupdate();
		return;
	}
	ui_title("\xC9\xBE\xB3\xFD\xCE\xC4\xBC\xFE");
	int y = CONTENT_Y + 4;
	draw_ui(2, y, "\xC8\xB7\xB6\xA8\xC9\xBE\xB3\xFD\xA3\xBF", 0);
	draw_fname(2, y + 12, del_name, 0);
	ui_hints("EXE\xC9\xBE\xB3\xFD  EXIT\xC8\xA1\xCF\xFB", "", 0);
	dupdate();
}

static void handle_confirm_key(key_event_t ev)
{
	if (confirm_kind == 1) {
		if (ev.key == KEY_EXE) {
			dirty = 0;
			edit_mode = 0;
			py_reset();
			gint_world_switch(GINT_CALL(fs_read_current));
			mode = MODE_VIEW;
		} else if (ev.key == KEY_EXIT) {
			mode = MODE_VIEW;
		}
		return;
	}
	if (ev.key == KEY_EXE) {
		gint_world_switch(GINT_CALL(fs_delete_selected));
		mode = MODE_LIST;
	} else if (ev.key == KEY_EXIT) {
		snprintf(status_msg, sizeof status_msg, "\xD2\xD1\xC8\xA1\xCF\xFB");
		status_sticky = 1;
		mode = MODE_LIST;
	}
}

static void handle_list_key(key_event_t ev)
{
	if (list_recent) {
		if (ev.key == KEY_UP) {
			if (rec_sel > 0)
				rec_sel--;
			list_sel = rec_sel;
			list_top = rec_sel < 5 ? 0 : rec_sel - 4;
		} else if (ev.key == KEY_DOWN) {
			if (rec_sel < rec_n - 1)
				rec_sel++;
			list_sel = rec_sel;
			list_top = rec_sel < 5 ? 0 : rec_sel - 4;
		} else if (ev.key == KEY_EXE && rec_n > 0) {
			gint_world_switch(GINT_CALL(rec_open_sel));
			list_recent = 0;
		} else if (ev.key == KEY_EXIT) {
			list_recent = 0;
			rec_sel = 0;
			list_sel = list_top = 0;
			gint_world_switch(GINT_CALL(fs_list_dir));
		}
		return;
	}
	switch (ev.key) {
	case KEY_UP:
		if (list_sel > 0) list_sel--;
		return;
	case KEY_DOWN:
		if (list_sel < name_count - 1) list_sel++;
		return;
	case KEY_F1:
		if (fbar_page) {
			gint_world_switch(GINT_CALL(rec_load));
			list_recent = rec_n > 0;
			rec_sel = 0;
			list_sel = list_top = 0;
			if (!list_recent)
				snprintf(status_msg, sizeof status_msg,
				         "\xCE\xDE\xD7\xEE\xBD\xFC\xBC\xC7\xC2\xBC");
			return;
		}
		name_rename = 0;
		gint_world_switch(GINT_CALL(name_default));
		mode = MODE_NAME;
		return;
	case KEY_F2:
		gint_world_switch(GINT_CALL(name_begin_rename));
		if (name_rename)
			mode = MODE_NAME;
		return;
	case KEY_F3:
		if (ev.shift) {
			/* test helper, not advertised */
			gint_world_switch(GINT_CALL(fs_make_demo));
		}
		return;
	case KEY_F5:
		if (fbar_page) {
			help_back = MODE_LIST;
			mode = MODE_HELP;
			return;
		}
		return;
	case KEY_F6:
		help_back = MODE_LIST;
		mode = MODE_HELP;
		return;
	case KEY_DEL:
		if (name_count == 0)
			return;
		if (!strcmp(names[list_sel], "..") || is_dir[list_sel]) {
			snprintf(status_msg, sizeof status_msg, "\xB2\xBB\xC4\xDC\xC9\xBE\xCE\xC4\xBC\xFE\xBC\xD0");
			status_sticky = 1;
			return;
		}
		strncpy(del_name, names[list_sel], NAME_MAX_L);
		del_name[NAME_MAX_L] = 0;
		confirm_kind = 0;
		mode = MODE_CONFIRM;
		return;
	case KEY_EXIT:
		/* back one level: leave the current folder */
		if (strcmp(cur_dir, root_dev)) {
			list_sel = list_top = 0;
			gint_world_switch(GINT_CALL(fs_leave_dir));
		}
		return;
	case KEY_EXE:
		if (name_count == 0) return;
		if (!strcmp(names[list_sel], "..")) {
			list_sel = list_top = 0;
			gint_world_switch(GINT_CALL(fs_leave_dir));
			return;
		}
		if (is_dir[list_sel]) {
			/* fs_enter_dir() reads names[list_sel]: reset afterwards */
			gint_world_switch(GINT_CALL(fs_enter_dir));
			list_sel = list_top = 0;
			return;
		}
		build_paths();
		{
			char full[RECENT_LEN];
			/* recorded when the document is closed instead: writing
			   FXTXT.REC here could stall the open on hardware */
		}
		cur_line = cur_col = 0;
		view_top = hscroll = 0;
		edit_mode = 0;
		dirty = 0;
		gint_world_switch(GINT_CALL(fs_read_current));
		apply_saved_pos();
		resume_line();
		mode = MODE_VIEW;
		return;
	default:
		return;
	}
}

/*=== key input ============================================================*/

/* getkey_opt(0) reports SHIFT and ALPHA as ordinary keys, so the modifier
   state is tracked here instead (one-shot, like the calculator itself). */
static int ev_repeat;	/* 1 when the event was synthesised by auto-repeat */

static key_event_t next_event(void)
{
	static int rep_key, rep_n;
	static int blink_n;

	for (;;) {
		volatile int timeout = 15;
		key_event_t e = getkey_opt(0, &timeout);
		key_event_t none = { 0 };

		if (e.type == KEYEV_NONE) {
			/* nothing happened: maybe the held arrow should repeat */
			if (rep_key && keydown(rep_key)) {
				if (++rep_n >= 40) {		/* ~0.6 s before repeating */
					rep_n = 32;		/* then every ~120 ms */
					key_event_t r = { 0 };
					r.key = rep_key;
					r.type = KEYEV_DOWN;
					r.mod = 1;
					r.shift = shift_armed;
					shift_armed = 0;
					ev_repeat = 1;
					return r;
				}
			} else {
				rep_key = 0;
				rep_n = 0;
			}
			/* the idle tick: getkey_opt() returns at once here, so the
			   delay has to be explicit or the blink counter races */
			sleep_ms(15);
			/* blink the caret: 33 * 15 ms is about 0.5 s.  The
			   idle event makes the main loop repaint. */
			if (++blink_n >= 33) {
				blink_n = 0;
				blink_on = !blink_on;
				return none;
			}
			continue;
		}

		blink_on = 1;
		blink_n = 0;
		if (e.type == KEYEV_UP) {
			if (e.key == rep_key) {
				rep_key = 0;
				rep_n = 0;
			}
			continue;
		}
		if (e.type != KEYEV_DOWN)
			continue;			/* ignore HOLD entirely */

		if (e.key == KEY_UP || e.key == KEY_DOWN ||
		    e.key == KEY_LEFT || e.key == KEY_RIGHT) {
			rep_key = e.key;
			rep_n = 0;
		} else {
			rep_key = 0;
			rep_n = 0;
		}

		if (e.key == KEY_SHIFT || e.key == KEY_ALPHA) {
			if (e.key == KEY_SHIFT)
				shift_armed = !shift_armed;	/* twice = cancel */
			else if (shift_armed) {
				alpha_lock = !alpha_lock;
				shift_armed = 0;
				alpha_armed = 0;
			} else if (alpha_lock) {
				alpha_lock = 0;
			} else {
				alpha_armed = 1;
			}
			/* let the caller redraw right away so the lock badge
			   appears on the same key press */
			key_event_t idle = { 0 };
			idle.type = KEYEV_DOWN;
			idle.mod = 1;
			ev_repeat = 1;			/* never repeats anything */
			return idle;
		} else {
			e.mod = 1;
			e.shift = shift_armed;
			e.alpha = alpha_armed;
			shift_armed = 0;
			alpha_armed = 0;
			ev_repeat = 0;
			return e;
		}
	}
}

/*=== main =================================================================*/

int main(void)
{
	gint_setrestart(1);

restart:
	;
	int reentry = app_ready || root_dev[0];

	if (cells) {
		/* re-entered from the main menu: the buffers from the previous run
		   are still allocated (the add-in image is kept), keep them */
		goto have_memory;
	}

	text_max = 0;
	for (int cap = 32768; cap >= TEXT_MIN; cap /= 2) {
		uint16_t *c = malloc((size_t)cap * sizeof(uint16_t));
		if (!c)
			continue;
		unsigned char *r = malloc((size_t)cap);
		if (!r) {
			free(c);
			continue;
		}
		cells = c;
		rawbuf = r;
		text_max = cap;
		break;
	}
	names = malloc(MAX_ENTRIES * (NAME_MAX_L + 1));
	names_fc = malloc(MAX_ENTRIES * NAME_FC_MAX * sizeof(uint16_t));
	is_dir = malloc(MAX_ENTRIES);
	if (!cells || !rawbuf || !names || !is_dir || !names_fc) {
		dclear(C_WHITE);
		dtext(2, 2, C_BLACK, "out of memory");
		dupdate();
		getkey();
		return 1;
	}

have_memory:
	/* The volume name survives a menu exit (the image is kept), so only probe
	   when we really do not know it yet. The listing is always rebuilt because
	   the buffers are released when we hand control back to the OS. */
	if (!root_dev[0])
		gint_world_switch(GINT_CALL(fs_probe_root));
	else
		normalise_cur_dir();
	if (!cur_dir[0] || !strcmp(cur_dir, "/"))
		snprintf(cur_dir, sizeof cur_dir, "%s", root_dev);

	gint_world_switch(GINT_CALL(fs_list_dir));

	if (!reentry) {
		list_sel = list_top = 0;

		/* reopen the file that was being read when the app was last closed */
		if (fs_restore())
			mode = MODE_VIEW;
	}
	/* coming back from the menu the document is gone: show the folder again */
	if (mode != MODE_LIST && !cells[0] && !ncell)
		mode = MODE_LIST;
	if (list_sel >= name_count)
		list_sel = list_top = 0;
	if (list_top > list_sel)
		list_top = list_sel;
	if (!rec_buf)
		rec_buf = malloc((size_t)RECENT_MAX * RECENT_LEN);
	app_ready = 1;

	for (;;) {
		dclear(C_WHITE);
		if (mode == MODE_LIST)
			draw_list();
		else if (mode == MODE_VIEW)
			draw_view();
		else if (mode == MODE_HELP)
			draw_help();
		else if (mode == MODE_NAME)
			draw_name();
		else if (mode == MODE_SEARCH)
			draw_search();
		else if (mode == MODE_CONFIRM)
			draw_confirm();
		else
			draw_keyboard();

		key_event_t ev = next_event();
		status_sticky = 0;

		if (ev.key == KEY_ACON && ev.shift) {
			/* SHIFT+AC/ON powers off; a bare AC/ON does nothing */
			gint_world_switch(GINT_CALL(fs_save_state));
			app_release();
			gint_poweroff(1);
			break;
		}
		if (ev.key == KEY_MENU) {
			/* The OS's USB dialog only appears in the main menu, and an
			   add-in cannot see the cable. So MENU parks us in the OS
			   menu (state kept); picking FXTXT there returns right
			   here, making "MENU, plug in, LINK" the USB workflow. */
			alpha_lock = 0;
			shift_armed = 0;
			alpha_armed = 0;
			py_reset();
			gint_world_switch(GINT_CALL(fs_save_state));
			/* the OS heap outlives this instance: hand the buffers
			   back before parking in the menu, then rebuild */
			app_release();
			gint_osmenu();
			gint_setrestart(1);
			goto restart;
		}
		if (mode == MODE_LIST) {
			handle_list_key(ev);
		} else if (mode == MODE_VIEW) {
			handle_view_key(ev);
		} else if (mode == MODE_NAME) {
			handle_name_key(ev);
		} else if (mode == MODE_SEARCH) {
			handle_search_key(ev);
		} else if (mode == MODE_CONFIRM) {
			handle_confirm_key(ev);
		} else if (mode == MODE_HELP) {
			if (!ev_repeat && ev.key == KEY_LEFT)
				help_page = (help_page + 2) % 3;
			else if (!ev_repeat && ev.key == KEY_RIGHT)
				help_page = (help_page + 1) % 3;
			else if (ev.key == KEY_EXIT || ev.key == KEY_EXE || ev.key == KEY_F2)
				mode = help_back;
		} else {
			handle_kb_key(ev);
		}
	}

	/* Keep every buffer allocated: gint restarts this image when the add-in is
	   launched again, so the next entry reuses them and appears instantly.
	   (Freeing here made the second re-entry run out of memory.) */
	return 0;
}

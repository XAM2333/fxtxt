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

/* CASIOWIN's fs gets inconsistent when BFile_Write() is handed an odd number
   of bytes (gint's bfile.h warns about exactly this), and an inconsistent fs
   is what reboots the unit.  Every write goes through here: the payload plus
   one NUL terminator, padded to an even size. */
static int bfile_write_even(int fd, void const *data, int len)
{
	unsigned char buf[1536];
	int n;

	if (len < 0)
		return -1;
	if (len > (int)sizeof buf - 2)
		len = (int)sizeof buf - 2;
	memcpy(buf, data, (size_t)len);
	buf[len] = 0;
	n = len + 1;
	if (n & 1)
		buf[n++] = 0;
	return BFile_Write(fd, buf, n) < 0 ? -1 : 0;
}

enum { MODE_LIST = 0, MODE_VIEW, MODE_KEYB, MODE_HELP, MODE_NAME, MODE_SEARCH, MODE_CONFIRM, MODE_BOOK,
       MODE_GOTO,
       MODE_SET, MODE_SET_INV, MODE_SET_UNIT, MODE_SET_IME, MODE_SET_CHARS, MODE_SET_PHR,
       MODE_SET_ABOUT,
       };
enum { PAGE_PINYIN = 0, PAGE_PUNCT, PAGE_LOWER, PAGE_UPPER, PAGE_DIGIT, PAGE_SYMBOL };

/* settings: plain statics, so they survive a menu round trip exactly like
   the bookmarks and the reading position do */
static int set_inv;		/* 1 = inverse video */
static int set_unit_lines;	/* 1 = the reader counter shows lines */
static int set_from;		/* page the settings were opened from */
static int set_edit_was;	/* edit mode when they were opened */
static int set_row;		/* the settings menu cursor */
static int set_sel;		/* cursor on the palette pages */
static int set_doc;		/* a document is loaded, so insert works */
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
static int total_lines;		/* lines of the whole file */
static unsigned char *rawbuf;

/* --- window table ---------------------------------------------------------
   The document does not fit into the text buffer, so it is read in windows of
   at most text_max bytes.  Every window but the last ends on a line boundary
   after a whole number of screen pages (see chunk_cut), so a page never
   straddles two windows.  The table records, for each window, its file offset
   and the display rows and lines that precede it; a window-local row plus
   chunk_rows is the document-wide row used by the page counter, the history,
   the bookmarks and the search.  Heap only: static RAM is 6 KiB. */
static int *chunk_off;		/* chunk_off[i] = file offset of window i */
static int *chunk_r0;		/* chunk_r0[i]  = display rows before i */
static int *chunk_l0;		/* chunk_l0[i]  = lines before i */
static int chunk_n;		/* windows in the table */
static int chunk_cap;		/* slots allocated in the three arrays */
static int chunk_idx;		/* window that lives in cells[] now */

static int chunk_of_row(int row);
static void chunk_load(int idx);
static void goto_row(int grow);
static void goto_line(int line);
static int global_row(void);
static int global_line(void);
static int page_total(void);
static void page_step(int dir);
static void open_goto(void);
static void handle_goto_key(key_event_t ev);
static int hit_chunk(int hit);
static int hit_cell(int hit);

static int edit_mode;
static int alpha_lock;		/* sticky letter mode (SHIFT+ALPHA) */
static int caps_on;		/* F6: upper case while typing */
static int case_upper;
static int shift_armed;

static int alpha_armed;		/* capitals (name prompt F1) */
static int cn_mode = 1;		/* 1 = pinyin, 0 = direct letters */
static int dirty;
static int text_gen;		/* bumped by every edit */
static int totals_gen;		/* text_gen the totals were built from */

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
static int kb_target;			/* 0 = doc, 1 = search prompt, 2 = 常用字, 3 = 常用词 */

/* the user's own IME lists, edited in the settings pages */
#define SET_CHAR_MAX 24
#define SET_PHR_MAX  8
#define SET_PHR_LEN  12

static uint16_t set_chars[SET_CHAR_MAX];
static int set_char_n;
static uint16_t set_phrs[SET_PHR_MAX][SET_PHR_LEN];
static int set_phr_len[SET_PHR_MAX];
static int set_phr_n;
static uint16_t phr_buf[SET_PHR_LEN];	/* the phrase being composed */
static int phr_buf_n;
/* which *.IME_* file we load from and write to; looked up once at startup so
   that saving never has to walk the directory again (the search API is
   documented as unreliable, and it is not worth risking on every key) */
static char ime_name[40];

/* syllable of every quick character and of every character inside a phrase,
   filled once when the lists are loaded or edited so the candidate row never
   has to scan the 9 KB pinyin table while typing */
#define SET_PY_NONE (-1)
static signed short set_char_py[SET_CHAR_MAX];
static signed short phr_py[SET_PHR_MAX][SET_PHR_LEN];
static void py_cache_lists(void);

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
static void open_keyboard(int target, int page);
static void cell_to_gb(uint16_t cell, char *out);
static void ime_flush(void);
static int ime_dirty;
static void ui_status(char const *text);
static void open_settings(void);
static int py_candidates(void);
static void py_reset(void);
static void run_search(void);
static uint16_t py_candidate(int i);
static int py_candidate_cells(int i, uint16_t *out);
static int py_candidate_w(int i);

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
   cell). With invert=1 the cell is painted white-on-black.  ui_inv flips
   both, which is all the inverse-video setting does. */
static int ui_inv;		/* 1 = black paper, white ink */
#define INKBAR (ui_inv ? C_WHITE : C_BLACK)
#define PAPER  (ui_inv ? C_BLACK : C_WHITE)

static void put_px(int x, int y, int on, int invert)
{
	if (!on)
		return;			/* never paint the cell background */
	if (invert ^ ui_inv)
		px_off(x, y);		/* white ink on the black bar */
	else
		px_on(x, y);
}

/* the colour that contrasts with a filled bar: paper on the bar */
static void paper_px(int x, int y)
{
	if (ui_inv)
		px_on(x, y);
	else
		px_off(x, y);
}

/* solid blocks (carets, selection boxes) are ink as well */
static void ink(int x, int y)
{
	if (ui_inv)
		px_off(x, y);
	else
		px_on(x, y);
}

static void blit8(int x, int y, unsigned char const *g, int invert)
{
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
		if (invert) {
			/* white on black: the box is exactly this glyph's advance,
			   so a wide left bearing cannot eat the previous column */
			for (int r = 0; r < 8; r++)
				for (int c = 0; c < font_adv8[i]; c++)
					ink(x + c, y + r);
		}
		blit8(x - font_lb8[i], y, font_ascii8[i], invert);
		return;
	}
	int g = (int)cell - CELL_CJK;
	if (g < 0 || g >= FONT_GLYPHS)
		g = '?' - FONT_FIRST;
	if (invert) {
		for (int r = 0; r < 8; r++)
			for (int c = 0; c < 8; c++)
				ink(x + c, y + r);
	}
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
static int ui_reserve_w = -1;	/* pixels the caller reserved on the right */
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

#define SEARCH_MAX_HITS 256
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
static char const *hint_view = "F1\xB1\xE0 F2\xD5\xD2 F5\xCA\xE9 F6\xB0\xEF";
static char const *hint_edit_cn = "\xD6\xD0 F1\xB4\xE6 F3\xCD\xCB F4\xB1\xEA F5\xD6\xD0\xD3\xA2";
static char const *hint_edit_en = "\xD3\xA2 F1\xB4\xE6 F3\xCD\xCB F4\xB1\xEA F5\xD6\xD0\xD3\xA2";

/* Black square with a white letter: A = letter lock (ALPHA), S = capitals (SHIFT). */
static void ui_badge(char ch)
{
	int x = 118, y = STATUS_Y;
	drect(x, y, x + 9, y + 7, INKBAR);
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
static int fbar_page;
static int book_sel;
static int book_v[6];
static int book_del_ask;
static int book_ask_sel;	/* snapshot, drawn every frame */
static int leave_to_list;	/* 0: back to the reader, 1: to the list */
static int book_line;	/* line captured when the bookmark page opens */
static int help_page;	/* which help page is shown, 0..3 */

/* Six cups along the bottom edge (F1..F6): no bottom edge, no gaps, and each
   one carries its own short label. */
/* hollow right arrow for the page-toggle cup: base 7 px, height 5 px */
static void draw_next_icon(int x, int y)
{
	for (int r = 0; r < 7; r++)
		ink(x, y + r);		/* the base, 7 px tall */
	for (int k = 1; k <= 4; k++) {
		int d = (3 * k + 2) / 4;	/* the two edges meet at the tip */
		ink(x + k, y + d);
		ink(x + k, y + 6 - d);
	}
}

static void ui_fkeys(void)
{
	drect(0, BOX_Y, 127, BOX_Y, INKBAR);          /* top edge */
	for (int i = 1; i < 6; i++) {
		int x = i * 21;
		drect(x, BOX_Y, x, 63, INKBAR);          /* dividers to the bottom */
	}
	for (int i = 0; i < 6; i++) {
		char const *s = box_lab[i];
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
	drect(x, 0, x + 8, 8, PAPER);
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
		drect(x - 8, 0, x, 8, PAPER);
		draw_cell(x - 7, 1, (uint16_t)('S' - 0x20), 0);
		x -= 10;
	} else if (alpha_lock || alpha_armed) {
		drect(x - 8, 0, x, 8, PAPER);
		draw_cell(x - 7, 1, (uint16_t)('A' - 0x20), 0);
		x -= 10;
	}

	{
		int g = cn_mode ? 5029 : 4701;	/* 中 / 英 */
		drect(x - 8, 0, x, 8, PAPER);
		draw_cell(x - 7, 1, (uint16_t)(CELL_CJK + g), 0);
		x -= 10;
	}
	{
		/* 大写 / 小写: F6 and SHIFT both act as a case lock */
		int g = caps_on ? 1868 : 4418;
		drect(x - 16, 0, x, 8, PAPER);
		draw_cell(x - 15, 1, (uint16_t)(CELL_CJK + g), 0);
		draw_cell(x - 7, 1, (uint16_t)(CELL_CJK + 4437), 0);
	}
}

static void ui_title_state(char const *text, int show_input)
{
	drect(0, 0, 127, TITLE_H - 1, INKBAR);
	/* Stop the title before whatever occupies the right end: the state
	   boxes when editing, otherwise the page counter.  Both are measured
	   from fixed budgets because the caller builds the counter string
	   after the title. */
	int clip;
	if (ui_reserve_w >= 0)
		clip = 127 - ui_reserve_w - 3;	/* the caller measured it already */
	else if (show_input)
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
					paper_px(x + c, y + r);
				else
					ink(x + c, y + r);
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

static int tail_row(void)
{
	/* the empty line after a final newline counts only at the end of a file */
	return (chunk_idx == chunk_n - 1 && ncell > 0 && cells[ncell - 1] == CELL_NL)
		? 1 : 0;
}

static int tail_row(void);

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

/* Deferred history write.  A stdio call made while the document buffers hold
   nearly all of the heap resets the calculator, so the reader only remembers
   where it stopped; the file is written at a moment when the heap is free
   (leaving for the main menu, powering off, and at the next start-up). */
static int rec_pend;
static int rec_pend_line;	/* location lives in cur_path */

static void rec_flush(void)
{
	if (!rec_pend)
		return;
	rec_pend = 0;
	rec_touch(cur_path, rec_pend_line);
}
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

/* gint's stdio cannot create a file on the unit (fopen "wb" simply fails),
   so the CFG is written through BFile instead.  One create + one write, no
   read-back: the read-modify-write pattern is what reset the calculator. */
static int state_fc(uint16_t *fc, int cap)
{
	static char const *const cand[2] = {
		"\\\\fls0\\FXTXT.CFG",
		"/fls0/FXTXT.CFG",
	};	/* a folder cannot be created here, so keep one plain file */
	/* Never add entries straight to \\fls0: the main menu rebuilds its
	   add-in list from that directory and extra files make it come up
	   short. One subfolder of our own keeps the root exactly as delivered. */
	for (int k = 0; k < 2; k++) {
		int n = 0;
		for (int i = 0; cand[k][i] && n < cap - 1; i++)
			fc[n++] = (unsigned char)cand[k][i];
		fc[n] = 0;
		if (BFile_Open(fc, BFile_ReadOnly) >= 0)
			return n;
	}
	int n = 0;
	for (int i = 0; cand[0][i] && n < cap - 1; i++)
		fc[n++] = (unsigned char)cand[0][i];
	fc[n] = 0;
	return n;
}

/*=== per-document history =================================================*/
/* Lives inside FXTXT.CFG.  Creating files in \fls0 is what made the add-in
   list vanish, so the preferred location is a folder of our own; the root
   copy exists only as a fallback when that folder cannot be created. */
#define POS_CAP 12
typedef struct {
	char name[32];
	int line;
	int vtop;
	int bm[6];		/* bookmark: absolute display row + 1, 0 = empty */
} pos_rec_t;

static pos_rec_t *pos_tab;	/* heap: static RAM is only 6 KiB */
static int pos_n;
static uint16_t pos_fc[96];	/* .POS path of the document now open */
static int pos_fc_ok;

static int pos_ready(void)
{
	if (!pos_tab) {
		pos_tab = malloc(sizeof(pos_rec_t) * POS_CAP);
		if (pos_tab) {
			memset(pos_tab, 0, sizeof(pos_rec_t) * POS_CAP);
			pos_n = 0;
		}
	}
	return pos_tab != NULL;
}

static int pos_upsert(char const *name)
{
	int k = -1;
	for (int i = 0; i < pos_n; i++)
		if (!strcmp(pos_tab[i].name, name)) { k = i; break; }
	if (k < 0) {
		if (pos_n < POS_CAP) k = pos_n++;
		else {			/* full: drop the oldest entry */
			memmove(pos_tab, pos_tab + 1, (size_t)(POS_CAP - 1) * sizeof pos_tab[0]);
			k = POS_CAP - 1;
		}
	}
	snprintf(pos_tab[k].name, sizeof pos_tab[k].name, "%s", name);
	return k;
}

/* called while the document is still open */

/*=== per-document position, DL22P style ===================================*/
/* One 24-byte file beside the document: <name>.POS.  Created once, then only
   ever overwritten in place with exactly 24 bytes. */
typedef struct {
	short line;		/* 0-based line */
	short vtop;		/* first display row of the page */
	short bm[6];		/* bookmarks, absolute row + 1, 0 = empty */
	short rsv[4];		/* padding: keeps the record at 24 bytes */
} pos24_t;	/* 24 bytes: 2 + 2 + 12 + 8 */

static int pos24_path(char const *name, uint16_t *fc, int cap)
{
	char p[192];
	/* cur_path looks like \\TXT\CCZD.txt: the volume name is missing, so it
	   has to be put back by hand. cur_dir is "/TXT", also without the volume. */
	{
		char const *q = cur_path;
		while (*q == '\\' || *q == '/')
			q++;			/* drop whatever separator style was used */
		if (*q)
			snprintf(p, sizeof p, "\\\\fls0\\%s", q);
		else {
			char const *d = cur_dir;
			while (*d == '\\' || *d == '/')
				d++;
			if (*d)
				snprintf(p, sizeof p, "\\\\fls0\\%s\\%s", d, name);
			else
				snprintf(p, sizeof p, "\\\\fls0\\%s", name);
		}
	}
	int n = (int)strlen(p);
	if (n <= 0 || n >= (int)sizeof p)
		return 0;
	/* swap the extension, but only the one after the last backslash */
	{
		char *dot = strrchr(p, '.');
		char *bs = strrchr(p, '\\');
		if (dot && (!bs || dot > bs))
			strcpy(dot, ".POS");
		else if (n + 5 < (int)sizeof p)
			strcat(p, ".POS");
	}
	int m = 0;
	for (int i = 0; p[i] && m < cap - 1; i++)
		fc[m++] = (unsigned char)p[i];
	fc[m] = 0;
	return m;
}

static void pos_file_load(char const *name)
{
	uint16_t fc[96];
	if (pos_fc_ok)
		for (int i = 0; i < 96; i++) fc[i] = pos_fc[i];
	else if (!pos24_path(name, fc, 96))
		return;
	if (!pos_ready())
		return;
	int fd = BFile_Open(fc, BFile_ReadOnly);
	if (fd < 0)
		return;
	pos24_t v;
	memset(&v, 0, sizeof v);
	int n = BFile_Read(fd, &v, (int)sizeof v, 0);
	BFile_Close(fd);
	if (n < 8 || v.line < 0 || v.line > 32000 || v.vtop < 0 || v.vtop > 32000)
		return;
	int k = pos_upsert(name);
	if (k < 0)
		return;
	pos_tab[k].line = v.line;
	pos_tab[k].vtop = v.vtop;
	for (int i = 0; i < 6; i++)
		pos_tab[k].bm[i] = v.bm[i];
}

static void pos_file_save(char const *name)
{
	uint16_t fc[96];
	if (pos_fc_ok)
		for (int i = 0; i < 96; i++) fc[i] = pos_fc[i];
	else if (!pos24_path(name, fc, 96))
		return;
	if (!pos_ready())
		return;
	int k = -1;
	for (int i = 0; i < pos_n; i++)
		if (!strcmp(pos_tab[i].name, name)) { k = i; break; }
	if (k < 0)
		return;
	pos24_t v;
	memset(&v, 0, sizeof v);
	v.line = (short)(pos_tab[k].line > 32000 ? 32000 : pos_tab[k].line);
	v.vtop = (short)(pos_tab[k].vtop > 32000 ? 32000 : pos_tab[k].vtop);
	for (int i = 0; i < 6; i++)
		v.bm[i] = (short)pos_tab[k].bm[i];
	int fd = BFile_Open(fc, BFile_ReadWrite);
	if (fd >= 0) {
		int cap = BFile_Size(fd);
		if (cap >= (int)sizeof v) {
			BFile_Seek(fd, 0);
			int w = BFile_Write(fd, &v, (int)sizeof v);
			BFile_Close(fd);
			if (w == (int)sizeof v)
				return;
		} else {
			BFile_Close(fd);
		}
	}
	int sz = (int)sizeof v;
	if (BFile_Create(fc, BFile_File, &sz) < 0)
		return;
	fd = BFile_Open(fc, BFile_WriteOnly);
	if (fd < 0)
		return;
	int n = sz & ~1;
	if (n > (int)sizeof v) n = (int)sizeof v;
	if (n > 0)
		BFile_Write(fd, &v, n);
	BFile_Close(fd);
}

static void pos_store(void)
{
	if (!pos_ready()) return;
	if (name_count <= 0 || list_sel < 0 || list_sel >= name_count || is_dir[list_sel])
		return;
	int k = pos_upsert(names[list_sel]);
	pos_tab[k].line = global_line();
	if (pos_tab[k].line < 0) pos_tab[k].line = 0;
	pos_tab[k].vtop = global_row();
	pos_file_save(names[list_sel]);
}

static int pos_lookup(char const *name, int *line, int *vtop)
{
	if (!pos_ready()) return 0;
	for (int i = pos_n - 1; i >= 0; i--)
		if (!strcmp(pos_tab[i].name, name)) {
			*line = pos_tab[i].line;
			*vtop = pos_tab[i].vtop;
			return 1;
		}
	pos_file_load(name);	/* first look after it */
	for (int i = pos_n - 1; i >= 0; i--)
		if (!strcmp(pos_tab[i].name, name)) {
			*line = pos_tab[i].line;
			*vtop = pos_tab[i].vtop;
			return 1;
		}
	return 0;
}

static int read_state(save_state_t *out)
{
	/* no file is ever read: see write_state() */
	if (1)
		return 0;
	uint16_t fc[40];
	state_fc(fc, 40);
	int fd = BFile_Open(fc, BFile_ReadOnly);
	if (fd < 0)
		return 0;
	
	unsigned char *blob = malloc(1024);
	if (!blob) {
		BFile_Close(fd);
		return 0;
	}
	int n = BFile_Read(fd, blob, 1024, 0);
	BFile_Close(fd);
	if (n < (int)sizeof(save_state_t)) {
		free(blob);
		return 0;
	}
	memcpy(out, blob, sizeof *out);
	if (memcmp(out->magic, STATE_MAGIC, 4)) {
		free(blob);
		return 0;
	}
	int off = (int)sizeof *out;
	if (n >= off + (int)sizeof pos_n) {
		int cnt = 0;
		memcpy(&cnt, blob + off, sizeof cnt);
		if (cnt > POS_CAP) cnt = POS_CAP;
		off += (int)sizeof cnt;
		int got = 0;
		while (got < cnt && n >= off + (int)sizeof(pos_rec_t)) {
			memcpy(&pos_tab[got], blob + off, sizeof(pos_rec_t));
			pos_tab[got].name[sizeof pos_tab[got].name - 1] = 0;
			off += (int)sizeof(pos_rec_t);
			got++;
		}
		pos_n = got;
	}
	free(blob);
	return 1;
}

static void state_fc_at(uint16_t *fc, int cap, int k)
{
	static char const *const cand[2] = {
		"\\\\fls0\\FXTXT.CFG",
		"/fls0/FXTXT.CFG",
	};
	int n = 0;
	for (int i = 0; cand[k][i] && n < cap - 1; i++)
		fc[n++] = (unsigned char)cand[k][i];
	fc[n] = 0;
}

static void write_state(void)
{
	/* Writing anything into \\fls0 -- even a single 1 KiB file -- makes
	   the main menu drop the add-in list on this unit. Keep the saved
	   position and the bookmarks in RAM instead: gint keeps the image
	   resident, so a menu round trip still returns to the same page. */
	if (1)
		return;
	uint16_t fc[40];
	state_fc(fc, 40);

	/* header + count + records, one blob, one file name, fixed 4 KiB size so
	   later saves only ever overwrite the file that already exists */
	
	unsigned char *blob = malloc(1024);
	if (!blob)
		return;
	memset(blob, 0, 1024);
	memcpy(blob, &st, sizeof st);
	int off = (int)sizeof st;
	memcpy(blob + off, &pos_n, sizeof pos_n);
	off += (int)sizeof pos_n;
	for (int i = 0; i < pos_n && off + (int)sizeof(pos_rec_t) <= 1024; i++) {
		memcpy(blob + off, &pos_tab[i], sizeof(pos_rec_t));
		off += (int)sizeof(pos_rec_t);
	}

	/* try every location: opening the saved one, then creating each in turn */
	for (int k = 0; k < 2; k++) {
		uint16_t p[40];
		state_fc_at(p, 40, k);
		int fd = BFile_Open(p, BFile_ReadWrite);
		if (fd >= 0) {
			int cap = BFile_Size(fd);	/* never write past the allocation */
			if (cap > 1024) cap = 1024;
			cap &= ~1;
			int w = -1;
			if (cap > 0) {
				BFile_Seek(fd, 0);
				w = BFile_Write(fd, blob, cap);
			}
			BFile_Close(fd);
			if (w == cap && cap > 0) {
				free(blob);
				return;
			}
		}
		/* create the file once, then only ever overwrite it in place:
		   repeatedly removing and recreating entries in \fls0 is what
		   made the main menu lose the add-in list */
		if (BFile_Open(p, BFile_ReadOnly) < 0) {
			int sz = 1024;
			if (BFile_Create(p, BFile_File, &sz) < 0)
				continue;
			fd = BFile_Open(p, BFile_WriteOnly);
			if (fd >= 0) {
				int n = sz & ~1;	/* the size the file actually got */
				if (n > 1024) n = 1024;
				int w = (n > 0) ? BFile_Write(fd, blob, n) : -1;
				BFile_Close(fd);
				if (w == n && n > 0) {
					free(blob);
					return;
				}
			}
		}
	}
	free(blob);
}
static void fill_state(void)
{
	memcpy(st.magic, STATE_MAGIC, 4);
	st.version = 2;
	memset(st.root, 0, sizeof st.root);
	if (root_dev[0])
		strncpy(st.root, root_dev, sizeof st.root - 1);
	st.big_font = 0;
	st.hscroll = 0;	/* never carry a sideways scroll across sessions */
	st.has_file = (mode == MODE_VIEW) && name_count > 0;
	strncpy(st.dir, cur_dir, sizeof st.dir - 1);
	st.dir[sizeof st.dir - 1] = 0;
	if (st.has_file) {
		strncpy(st.name, names[list_sel], sizeof st.name - 1);
		st.name[sizeof st.name - 1] = 0;
		st.view_top = global_row();
		st.hscroll = hscroll;
		/* remember the page, not the caret: scrolling only moves view_top,
		   so saving cur_line kept an old line and the position stopped
		   updating after the first reopen.  Both numbers are document-wide,
		   because a window-local row points at the wrong line as soon as the
		   document is read in more than one window. */
		st.cur_line = global_line();
		if (st.cur_line < 0) st.cur_line = 0;
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
	pos_store();
	if (have_prev && !st.has_file) {
		memcpy(st.name, prev.name, sizeof st.name);
		st.has_file = prev.has_file;
		st.view_top = prev.view_top;
		st.hscroll = 0;	/* a sideways scroll must not survive a session */
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

	hscroll = s.hscroll;
	cur_col = s.cur_col;
	/* s.view_top is a document-wide display row and s.cur_line a
	   document-wide line: open the window that holds them, then convert both
	   back to window-local numbers. */
	goto_row(s.view_top);
	cur_line = s.cur_line - chunk_lines;
	if (cur_line > line_count() - 1) cur_line = line_count() - 1;
	if (cur_line < 0) cur_line = 0;
	int len = line_len(line_start(cur_line));
	if (cur_col > len) cur_col = len;
	if (cur_col < 0) cur_col = 0;
	if (hscroll < 0) hscroll = 0;
	return 1;
}

static int restore_flag;

/* Drop back to the volume root and refresh the listing (runs in the OS world,
   because it has to ask the file system for the entries). */
static void fs_root_again(void)
{
	strncpy(cur_dir, root_dev, sizeof cur_dir - 1);
	cur_dir[sizeof cur_dir - 1] = 0;
	fs_list_dir();
	list_sel = 0;
	list_top = 0;
}

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

static char *rec_buf;	/* RECENT_MAX * RECENT_LEN, malloc'd */
static int rec_n;

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
	free(chunk_off);
	free(chunk_r0);
	free(chunk_l0);
	free(names);
	free(names_fc);
	free(is_dir);
	free(rec_buf);
	free(pos_tab);
	rec_buf = NULL;
	rec_n = 0;
	pos_tab = NULL;
	pos_n = 0;
	cells = NULL;
	rawbuf = NULL;
	chunk_off = NULL;
	chunk_r0 = NULL;
	chunk_l0 = NULL;
	chunk_n = 0;
	chunk_cap = 0;
	chunk_idx = 0;
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

/* Folders carry no extension here, documents always do, so the expensive
   opendir probe is only needed for names without a dot.  This turns a
   listing of forty files from forty opendir calls into one. */
static int entry_is_dir(char const *name)
{
	char path[96];
	DIR *d;

	if (strchr(name, '.'))
		return 0;
	snprintf(path, sizeof path, "%s/%s", cur_dir, name);
	d = opendir(path);
	if (!d)
		return 0;	/* one probe only, no retry */
	closedir(d);
	return 1;
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
				int dir = (e->d_type == DT_DIR) ||
					(e->d_type == DT_REG ? 0 : entry_is_dir(n));
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
	/* the path must be taken now: cur_path does not survive until EXIT */
	pos_fc_ok = 0;
	if (cur_path[0] &&
	    name_count > 0 && list_sel >= 0 && list_sel < name_count)
		pos_fc_ok = pos24_path(names[list_sel], pos_fc, 96);
	if (name_count <= 0 || list_sel < 0 || list_sel >= name_count)
		return;
	pos_file_load(names[list_sel]);
	int line, vtop;
	if (!pos_lookup(names[list_sel], &line, &vtop))
		return;
	if (line < 0)
		line = 0;
	/* the record holds document-wide numbers: open that window, then make
	   them window-local again */
	goto_row(vtop);
	cur_line = line - chunk_lines;
	if (cur_line > line_count() - 1) cur_line = line_count() - 1;
	if (cur_line < 0) cur_line = 0;
	cur_col = 0;
	hscroll = 0;
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
static int name_rename;
static uint16_t const *name_src_fc;	/* the FC the listing produced */		/* prompt is renaming the selected file */

/* gint's fx port has no BFile_Rename syscall, so a rename is a copy followed
   by a delete. Returns 0 on success, -2 when the file is too large to copy. */
/* Copy one file to a new name inside the current folder and remove the old
   one.  Paths use backslashes throughout: the BFile layer wants them and a
   mixed "\\fls0/NAME.TXT" is not something to rely on. */
static int txt_fc(uint16_t *fc, int cap, char const *name);

static int name_copy_file(char const *oldn, char const *newn)
{
	enum { CHUNK = 4096 };
	uint16_t fo[64], fn[64];
	unsigned char *buf;
	char po[256], pn[256];
	int rd, wr, total = 0, n;
	int ok = 0;

	/* the reader opens documents through open_any(), so read the same way:
	   BFile_Open is not reliable for an entry that the listing produced */
	if (name_src_fc)
		memcpy(fo, name_src_fc, sizeof fo);
	else
		txt_fc(fo, 64, oldn);
	txt_fc(fn, 64, newn);
	snprintf(po, sizeof po, "%s\\%s", cur_dir, oldn);
	snprintf(pn, sizeof pn, "%s\\%s", cur_dir, newn);

	buf = malloc(CHUNK + 2);	/* room for the even-length pad byte */
	if (!buf)
		return -4;

	rd = BFile_Open(fo, BFile_ReadOnly);
	if (rd < 0) {
		FILE *in = open_any(po, "rb");
		if (!in) {
			free(buf);
			return -1;	/* nothing could open the source */
		}
		{
			FILE *out = open_any(pn, "wb");
			if (!out) {
				fclose(in);
				free(buf);
				return -2;
			}
			while ((n = (int)fread(buf, 1, CHUNK, in)) > 0) {
				if (n & 1)
					buf[n] = 0;
				fwrite(buf, 1, (size_t)((n + 1) & ~1), out);
			}
			fclose(in);
			fclose(out);
			ok = 1;
		}
	} else {
		total = BFile_Size(rd);
		BFile_Close(rd);
		{
			int sz = (total + 1) & ~1;
			if (sz < 2)
				sz = 2;
			BFile_Remove(fn);
			if (BFile_Create(fn, BFile_File, &sz) != 0) {
				free(buf);
				return -2;
			}
			{
				int in = BFile_Open(fo, BFile_ReadOnly);
				int out = in >= 0 ? BFile_Open(fn, BFile_WriteOnly) : -1;
				if (in < 0 || out < 0) {
					if (in >= 0)
						BFile_Close(in);
					free(buf);
					return -2;
				}
				n = 0;
				while (n < total) {
					int want = total - n < CHUNK ? total - n : CHUNK;
					int got = BFile_Read(in, buf, want, -1);
					if (got <= 0) {
						ok = -3;
						break;
					}
					if (got & 1)
						buf[got] = 0;
					BFile_Write(out, buf, (got + 1) & ~1);
					n += got;
				}
				BFile_Close(in);
				BFile_Close(out);
				ok = ok < 0 ? ok : 1;
			}
		}
	}
	free(buf);
	if (ok <= 0)
		return ok ? ok : -3;
	fs_delete_path(oldn);
	return 0;
}

static int name_rename_copy(char const *oldn, char const *newn)
{
	name_src_fc = names_fc[list_sel];
	int rc = name_copy_file(oldn, newn);
	name_src_fc = NULL;
	char op[NAME_MAX_L + 6], np[NAME_MAX_L + 6];
	size_t n;

	if (rc != 0)
		return rc;
	/* the .POS sidecar belongs to the document, so it moves with it */
	n = strlen(oldn);
	if (n > NAME_MAX_L)
		n = NAME_MAX_L;
	memcpy(op, oldn, n);
	op[n] = 0;
	{
		char *dot = strrchr(op, '.');
		if (dot && dot > op)
			strcpy(dot, ".POS");
		else
			strcpy(op + n, ".POS");
	}
	n = strlen(newn);
	if (n > NAME_MAX_L)
		n = NAME_MAX_L;
	memcpy(np, newn, n);
	np[n] = 0;
	{
		char *dot = strrchr(np, '.');
		if (dot && dot > np)
			strcpy(dot, ".POS");
		else
			strcpy(np + n, ".POS");
	}
	name_copy_file(op, np);	/* a missing sidecar simply fails quietly */
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
	status_msg[0] = 0;	/* no stale size or count on the prompt */
	snprintf(name_buf, sizeof name_buf, "%s", names[list_sel]);
	size_t n = strlen(name_buf);
	if (n > 4) {
		char *ext = name_buf + n - 4;
		char low[5];
		for (int i = 0; i < 4; i++)
			low[i] = (char)(ext[i] >= 'a' && ext[i] <= 'z' ? ext[i] - 32 : ext[i]);
		low[4] = 0;
		if (!strcmp(low, ".TXT"))
			name_buf[n - 4] = 0;
	}
	name_len = (int)strlen(name_buf);
	name_pos = name_len;
	name_rename = 1;
	name_src_fc = names_fc[list_sel];
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
			create_ok = 2;	/* nothing to do: back to the list */
			return;
		}
		rc = name_rename_copy(oldn, name);
		if (rc == -2) {
			snprintf(status_msg, sizeof status_msg, "\xCE\xC4\xBC\xFE\xCC\xAB\xB4\xF3\xCE\xDE\xB7\xA8\xB8\xC4\xC3\xFB");
			return;
		}
		if (rc != 0) {
			char why[64];
			snprintf(why, sizeof why, "\xB8\xC4\xC3\xFB\xCA\xA7\xB0\xDC %d", rc);
			snprintf(status_msg, sizeof status_msg, "%s", why);
			return;
		}
		name_rename = 0;	/* only a successful rename leaves the page */
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
	/* the same badges the editor shows: case, SHIFT and ALPHA */
	ui_title_state(name_rename ? "\xD6\xD8\xC3\xFC\xC3\xFB"
	                           : "\xD0\xC2\xBD\xA8\xCE\xC4\xB5\xB5", 1);
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
			ink(x + 1, y + k);
		draw_ui(x + 3, y, rest, 0);
	}
	y += 10;
	if (status_msg[0])
		draw_ui(2, CONTENT_Y + 32, status_msg, 0);
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
	status_msg[0] = 0;
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
	if (ev.key == KEY_F6) {
		/* the editor's own input method, writing into the name field */
		open_keyboard(4, PAGE_PINYIN);
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

/*=== 分页 ==================================================================
   Two different things, easy to mix up:

     分页 (window / chunk)  - the slice of the document that fits the text
                              buffer; it is a memory limit, not a screen unit
     页   (page)            - what the screen shows at once, rows() = 5 rows

   Everything the user sees (page counter, bookmarks, history, search hits,
   goto) is in document rows/lines, never in window-local ones.

   The document is larger than the text buffer, so it is read in windows of at
   most text_max bytes.  A window must not end in the middle of a screen page,
   otherwise the last rows of one window and the first rows of the next do not
   join up and the page counter jumps.  The rule is:

     a window ends right after the last display row that completes a page
     (rows() = 5 display rows) *and* ends a logical line; a row that would
     overflow the page is left for the next window.

   No row is ever cut in half, so every window but the last holds a whole
   multiple of rows() rows, and page k of the document covers the document
   rows [rows()*k, rows()*k + rows()). */

/* Length in bytes of the window that starts at s[0] (`n` bytes readable, and
   `last` when the file ends inside this window).

   The walk repeats the cell rules of decode_into_cells() because a cell is not
   one byte (a GB2312 pair is two, a skipped control byte is none), so the byte
   offset of a page boundary cannot be derived from the cell index alone.
   Candidates, best first:
     page  - a page boundary that also ends a line (the normal answer)
     rowb  - a page boundary inside a very long line (keeps the pages whole)
     lineb - any line end (loses the page grid but never splits a line) */
static int chunk_cut(unsigned char const *s, int n, int last)
{
	int per = rows();
	int i = 0, x = 0, done = 0;
	int page = 0, rowb = 0, lineb = 0;

	if (last || n <= 0)
		return n;
	while (i < n) {
		unsigned char c = s[i];
		uint16_t cell = 0;
		int reps = 1, at = i;

		if (c == '\n') {
			i++;
			done++;			/* the newline closes the row */
			lineb = i;		/* ... and it is a line end */
			if (done % per == 0)
				page = rowb = i;
			x = 0;
			continue;
		}
		if (c == '\r') {
			i++;
			continue;
		}
		if (c == '\t') {
			cell = 0;
			reps = 4;		/* a tab is four empty cells */
			i++;
		} else if (c < 0x20) {
			i++;
			continue;
		} else if (c >= 0x81 && i + 1 < n) {
			unsigned char t = s[i + 1];
			if (c >= 0xA1 && c <= 0xF7 && t >= 0xA1 && t <= 0xFE) {
				int qu = c - 0xA0, wei = t - 0xA0;
				int gi = (qu - 1) * 94 + (wei - 1);
				cell = (gi >= 0 && gi < FONT_GLYPHS)
					? (uint16_t)(CELL_CJK + gi) : 0;
			} else
				cell = 0;
			i += 2;
		} else if (c >= 0x80) {
			i++;
			continue;
		} else {
			cell = (uint16_t)(c - 0x20);
			i++;
		}
		for (int k = 0; k < reps; k++) {
			int adv = cell_adv(cell);
			if (x + adv > TEXT_W && x > 0) {
				/* soft wrap: this cell opens the next display row */
				done++;
				x = 0;
				if (done % per == 0)
					rowb = at;
			}
			x += adv;
		}
	}
	if (page > 0)
		return page;
	if (rowb > 0)
		return rowb;
	if (lineb > 0)
		return lineb;
	return n;
}

/* One more slot in the window table; 0 when the heap is exhausted. */
static int chunk_grow(void)
{
	int cap;
	int *p;

	if (chunk_n < chunk_cap)
		return 1;
	cap = chunk_cap ? chunk_cap * 2 : 8;
	p = realloc(chunk_off, (size_t)cap * sizeof *p);
	if (!p)
		return 0;
	chunk_off = p;
	p = realloc(chunk_r0, (size_t)cap * sizeof *p);
	if (!p)
		return 0;
	chunk_r0 = p;
	p = realloc(chunk_l0, (size_t)cap * sizeof *p);
	if (!p)
		return 0;
	chunk_l0 = p;
	chunk_cap = cap;
	return 1;
}

/* Map the file: byte offset, display rows and lines before every window.  One
   pass over the document, done once per open (and after a save). */
static void build_chunk_table(void)
{
	FILE *f = open_read();
	int base = 0;

	chunk_n = 0;
	total_rows = 0;
	total_lines = 0;
	if (!chunk_grow())
		return;
	if (!f)
		return;
	for (;;) {
		int n, cut, last;

		if (!chunk_grow())
			break;			/* out of memory: keep what we have */
		if (base > 0 && fseek(f, (long)base, SEEK_SET) != 0)
			break;
		n = (int)fread(rawbuf, 1, text_max, f);
		if (n < 0)
			n = 0;
		last = (n < text_max) || (fgetc(f) == EOF);
		cut = chunk_cut(rawbuf, n, last);
		if (cut <= 0)
			cut = n;		/* cannot happen, but never loop */
		chunk_off[chunk_n] = base;
		chunk_r0[chunk_n] = total_rows;
		chunk_l0[chunk_n] = total_lines;
		decode_into_cells(rawbuf, cut);
		total_rows += row_count();
		/* the empty line after the file's last newline belongs to the
		   document, but only the last window can end with one */
		if (last && ncell > 0 && cells[ncell - 1] == CELL_NL)
			total_rows += 1;
		total_lines += line_count();
		if (last && ncell > 0 && cells[ncell - 1] == CELL_NL)
			total_lines += 1;
		chunk_n++;
		base += cut;
		if (last || n <= 0)
			break;
	}
	fclose(f);
	if (chunk_n == 0) {
		/* an empty document still needs one window to point at */
		chunk_off[0] = 0;
		chunk_r0[0] = 0;
		chunk_l0[0] = 0;
		chunk_n = 1;
		total_rows = 1;
		total_lines = 1;
	}
}

/* Read window `idx` into cells[] and make chunk_rows/chunk_lines describe it.
   They are never accumulated: the table holds the document-wide offset. */
static void chunk_load(int idx)
{
	FILE *f;
	int base, len;

	ncell = 0;
	chunk_bytes = 0;
	chunk_more = 0;
	if (chunk_n <= 0)
		return;
	if (idx < 0)
		idx = 0;
	if (idx > chunk_n - 1)
		idx = chunk_n - 1;
	f = open_read();
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "\xB4\xF2\xB2\xBB\xBF\xAA");
		return;
	}
	base = chunk_off[idx];
	if (base > 0 && fseek(f, (long)base, SEEK_SET) != 0) {
		fclose(f);
		return;
	}
	len = (idx + 1 < chunk_n) ? (chunk_off[idx + 1] - base) : -1;
	if (len <= 0 || len > text_max)
		len = (int)fread(rawbuf, 1, text_max, f);
	else
		len = (int)fread(rawbuf, 1, len, f);
	fclose(f);
	if (len < 0)
		len = 0;
	chunk_idx = idx;
	chunk_base = base;
	chunk_bytes = len;
	chunk_more = (idx + 1 < chunk_n);
	chunk_rows = chunk_r0[idx];
	chunk_lines = chunk_l0[idx];
	decode_into_cells(rawbuf, len);
	undo_clear();
	snprintf(status_msg, sizeof status_msg, "%d\xD7\xD6\xBD\xDA GB", len);
}

/* Window that holds document row `row` (the table is ordered). */
static int chunk_of_row(int row)
{
	int lo = 0, hi = chunk_n - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (chunk_r0[mid] <= row)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static int chunk_of_line(int line)
{
	int lo = 0, hi = chunk_n - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (chunk_l0[mid] <= line)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

/* Document-wide display row / line of the first row on the screen. */
static int global_row(void)
{
	return chunk_rows + view_top;
}

static int global_line(void)
{
	int l = chunk_lines + offset_to_line(row_start(view_top));
	return l < 0 ? 0 : l;
}

/* Show document row `grow` at the top of the screen (loads its window). */
static void goto_row(int grow)
{
	int c;

	if (total_rows <= 0)
		total_rows = 1;
	if (grow < 0)
		grow = 0;
	if (grow > total_rows - 1)
		grow = total_rows - 1;
	c = chunk_of_row(grow);
	if (c != chunk_idx || ncell == 0)
		chunk_load(c);
	view_top = grow - chunk_rows;
	if (view_top > row_count() + tail_row() - 1)
		view_top = row_count() + tail_row() - 1;
	if (view_top < 0)
		view_top = 0;
	/* a page is exactly rows() display rows, so every screen starts on the
	   grid: the row that would overflow belongs to the next page (bug 2) */
	view_top = (view_top / rows()) * rows();
}

/* Show document line `line` at the top of the screen. */
static void goto_line(int line)
{
	int c, l;

	if (total_lines <= 0)
		total_lines = 1;
	if (line < 0)
		line = 0;
	if (line > total_lines - 1)
		line = total_lines - 1;
	c = chunk_of_line(line);
	if (c != chunk_idx || ncell == 0)
		chunk_load(c);
	l = line - chunk_lines;
	if (l > line_count() + tail_row() - 1)
		l = line_count() + tail_row() - 1;
	if (l < 0)
		l = 0;
	cur_line = l;
	view_top = row_of_cell(line_start(l));
	if (view_top > row_count() + tail_row() - 1)
		view_top = row_count() + tail_row() - 1;
	if (view_top < 0)
		view_top = 0;
	view_top = (view_top / rows()) * rows();	/* stay on the page grid */
}

/* One screen up (dir < 0) or down.  Pages are stepped on the grid of rows()
   rows, so walking down and back up visits exactly the same pages even when a
   search or a bookmark left the screen between two of them. */
static void page_step(int dir)
{
	int g = chunk_rows + view_top;
	int last = (page_total() - 1) * rows();

	if (dir < 0) {
		if (g <= 0)
			return;
		goto_row(((g - 1) / rows()) * rows());
	} else {
		int next = (g / rows() + 1) * rows();
		if (next > last)
			return;
		goto_row(next);
	}
}

static void fs_read_current(void)
{
	build_chunk_table();
	chunk_load(0);
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

static int rec_line[RECENT_MAX];
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
		int w = bfile_write_even(fd, data, len);
		BFile_Close(fd);
		if (w == 0)
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
	cur_col = 0;
	/* REC keeps a document-wide 1-based line: goto_line opens the window
	   that holds it, which may be any window of the document. */
	goto_line(line - 1);
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
		box_lab[5] = "\xC9\xE8\xD6\xC3";
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
				drect(0, y - 1, TEXT_W - 1, y + 7, INKBAR);
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
			drect(0, y - 1, TEXT_W - 1, y + 7, INKBAR);
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

/*=== F bar ================================================================

   One table drives both the labels and the actions.  The bar used to be
   described three times - a label array for drawing, a chain of hand written
   key tests for the actions, and a third copy inside ui_fkeys() for the
   search result - and they drifted apart on nearly every edit: F2 was
   labelled 退出 but toggled the caps lock, F5 said 中英 but opened the
   bookmarks, and the blank keys of editor page two still fired page one's
   functions.  Here the label and the action of a key are the same row entry,
   so they cannot disagree any more. */

enum { FS_READ = 0, FS_HIT, FS_EDIT1, FS_EDIT2, FS_N };

enum {
	A_NONE = 0,
	A_EDIT, A_FIND, A_BOOK, A_HELP,			/* reading page */
	A_HIT_PREV, A_HIT_NEXT, A_HIT_QUERY, A_HIT_CLOSE,	/* result list */
	A_SAVE, A_LEAVE, A_CAPS, A_PUNCT, A_CN, A_PAGE, A_UNDO, A_GOTO,
	A_SET
};

static unsigned char const fk_act[FS_N][6] = {
	{ A_EDIT,     A_FIND,     A_NONE,      A_GOTO,  A_BOOK,      A_SET },
	{ A_HIT_PREV, A_HIT_NEXT, A_HIT_QUERY, A_NONE,  A_HIT_CLOSE, A_HELP },
	{ A_SAVE,     A_LEAVE,    A_CAPS,      A_PUNCT, A_CN,        A_PAGE },
	{ A_UNDO,     A_FIND,     A_GOTO,      A_NONE,  A_SET,       A_PAGE },
};

static char const *fk_lab[FS_N][6] = {
	{ "\xB1\xE0\xBC\xAD", "\xB2\xE9\xD5\xD2", 0, "\xCC\xF8\xD7\xAA", "\xCA\xE9\xC7\xA9", "\xC9\xE8\xD6\xC3" },
	{ "\xC9\xCF\xCC\xF5", "\xCF\xC2\xCC\xF5", "\xD4\xD9\xB2\xE9", 0, "\xCD\xCB\xB3\xF6", "\xB0\xEF\xD6\xFA" },
	{ "\xB1\xA3\xB4\xE6", "\xCD\xCB\xB3\xF6", "\xB4\xF3\xD0\xB4", "\xB1\xEA\xB5\xE3", "\xD6\xD0\xD3\xA2", "\xB7\xAD\xD2\xB3" },
	{ "\xB3\xB7\xCF\xFA", "\xB2\xE9\xD5\xD2", "\xCC\xF8\xD7\xAA", 0, "\xC9\xE8\xD6\xC3", "\xB7\xAD\xD2\xB3" },
};

static int fk_state(void)
{
	if (!edit_mode)
		return search_n > 0 ? FS_HIT : FS_READ;
	return fbar_page ? FS_EDIT2 : FS_EDIT1;
}

/*=== text view ====
/* Inline pinyin composition: buffer + candidate row on the status line. */
static void comp_draw(void)
{
	dhline(STATUS_Y - 1, INKBAR);
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
		uint16_t cc[SET_PHR_LEN];
		int cl = py_candidate_cells(idx, cc);
		int cw = py_candidate_w(idx);
		if (idx >= n || cl <= 0 || x + cw > 127)
			break;
		if (idx == py_pick)
			drect(x - 1, STATUS_Y - 1, x + cw, STATUS_Y + 7, INKBAR);
		for (int k = 0; k < cl; k++) {
			draw_cell(x, STATUS_Y, cc[k], idx == py_pick);
			x += cell_adv(cc[k]);
		}
		x += 2;
	}
}

/* How many pages the reader should show.  The empty row that a trailing
   newline leaves behind is not a page of its own, otherwise pressing down on
   a full last page opens a blank one that is counted (buglist2 27). */
static int page_total(void)
{
	int rr = total_rows;
	if (tail_row() && rr > 1 && rr % rows() == 1)
		rr--;
	return (rr + rows() - 1) / rows();
}

static void draw_view(void)
{
	ui_no_cups = 0;
	if (chunk_n <= 1 && totals_gen != text_gen) {
		total_rows = row_count() + tail_row();
		total_lines = line_count() + tail_row();
		totals_gen = text_gen;
	}
	int rows_total = row_count() + tail_row();
	{
		int lim = rows_total - 1;	/* the last page may be short */
		if (lim < 0) lim = 0;
		if (view_top > lim) view_top = lim;
		if (view_top < 0) view_top = 0;
	}

	/* Build the right-hand counter *before* the title: the title's clip
	   depends on how wide the counter is, and reading ui_info_right from the
	   previous frame made a long name truncate twice (bug 21). */
	char info[16];
	info[0] = 0;
	if (!edit_mode) {
		if (search_n > 0)
			snprintf(info, sizeof info, "%d/%d", search_cur + 1, search_n);
		else if (set_unit_lines) {
			/* the first visible row of the document, 1-based */
			int ln = chunk_rows + view_top + 1;
			int tl = total_rows;
			if (tl < 1) tl = 1;
			if (ln < 1) ln = 1;
			if (ln > tl) ln = tl;
			snprintf(info, sizeof info, "%d/%d", ln, tl);
		} else {
			int pg = (chunk_rows + view_top) / rows() + 1;
			int tp = page_total();
			if (tp < 1) tp = 1;
			snprintf(info, sizeof info, "%d/%d", pg, tp);
		}
	}
	ui_reserve_w = info[0] ? ui_width(info) + 4 : (edit_mode ? 46 : 0);

	char head[40];
	snprintf(head, sizeof head, "%s%s", edit_mode ? "*" : "", names[list_sel]);
	ui_title_state(head, edit_mode);
	ui_reserve_w = -1;

	/* where is the cursor in display-row space? */
	int ccell = -1, crow = -1;
	if (edit_mode) {
		ccell = line_start(cur_line) + cur_col;
		if (ccell > ncell) ccell = ncell;
		crow = row_of_cell(ccell);
		/* Row_of_cell() answers with the row *holding* the last newline, so a
		   caret sitting past the end of the document looked as if it had not
		   moved and the new empty line never appeared (buglist2 25). */
		if (tail_row() && ccell >= ncell)
			crow = row_count();
		if (crow < view_top || crow >= view_top + rows())
			view_top = (crow / rows()) * rows();	/* whole pages only */
	}

	{
		int st = fk_state();
		for (int i = 0; i < 6; i++)
			box_lab[i] = fk_lab[st][i];
		if (st == FS_EDIT1 || st == FS_EDIT2)
			box_lab[5] = NULL;	/* the cup shows the arrow instead */
	}

	int caret_x = -1, caret_y = -1;
	int next_icon = (fk_state() == FS_EDIT1 || fk_state() == FS_EDIT2);
	int r = rows(), y = CONTENT_Y, row = view_top;
	for (int i = 0; i < r && row < rows_total; i++, row++) {
		int p = row_start(row);
		int q = next_row(p);
		int x = 0;
		int caretx = -1;
		int hl = -1;
		if (!edit_mode && search_n > 0 &&
		    hit_chunk(search_hits[search_cur]) == chunk_idx)
			hl = hit_cell(search_hits[search_cur]);
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
		if (caretx > 0)
			caretx--;	/* sit on the glyph's left bearing, not its origin */
		if (caretx >= 0 && caretx < TEXT_W)
			caret_x = caretx, caret_y = y;
		y += lh();
	}

	/* the caret blinks: while the phase is off the cell stays empty */
	if (caret_x >= 0 && blink_on) {
		for (int k = 0; k < lh() - 1; k++)
			ink(caret_x, caret_y + k);
	}

	if (edit_mode && py_len > 0) {
		comp_draw();
	} else {
		ui_info_right = 0;
		if (!edit_mode)
			ui_info_right = 1;
		ui_hints(edit_mode ? (cn_mode ? hint_edit_cn : hint_edit_en) : hint_view, info,
		         edit_mode);
		ui_info_right = 0;
	}
	if (next_icon && mode == MODE_VIEW)
		draw_next_icon(112, 57);
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

/* ALPHA + [x10^x] types a quotation mark.  Writing a plain ASCII quote into a
   Chinese document reads wrong, so the key alternates between the full width
   pair A1B0/A1B1 the way a writer would: open, close, open, close. */
static uint16_t quote_cell(void)
{
	int n = 0;
	int from = line_start(cur_line);
	int to = from + cur_col;

	for (int i = from; i < to && i < ncell; i++)
		if (cells[i] == CELL_CJK + 15 || cells[i] == CELL_CJK + 16)
			n++;
	return (n & 1) ? (uint16_t)(CELL_CJK + 16) : (uint16_t)(CELL_CJK + 15);
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

/*=== candidate row ========================================================
   One entry may be a single character or a whole stored phrase, so the row is
   built once per keystroke into py_ent[][] and everything (drawing, picking,
   inserting) walks that array instead of py_glyph directly. */

#define PY_ENT_MAX 12		/* what the row can show anyway */
#define PY_ENT_PHRASE 0x8000u
static uint16_t py_src[PY_ENT_MAX];	/* cell, or PY_ENT_PHRASE | phrase index */
static int py_ent_count;

/* the syllable a glyph belongs to, or SET_PY_NONE */
static int py_syllable_of(uint16_t cell)
{
	int g = (int)cell - (int)CELL_CJK;
	if (g < 0)
		return SET_PY_NONE;
	for (int s = 0; s < PY_COUNT; s++) {
		int a = py_start[s], b = py_start[s + 1];
		for (int i = a; i < b; i++)
			if (py_glyph[i] == g)
				return s;
	}
	return SET_PY_NONE;
}

/* Remember what each list character resolves to. The table scan is the
   expensive part, so identical characters are looked up only once (a phrase
   list repeats characters a lot). */
static void py_cache_lists(void)
{
	uint16_t seen[SET_CHAR_MAX + SET_PHR_MAX * SET_PHR_LEN];
	signed short seen_py[SET_CHAR_MAX + SET_PHR_MAX * SET_PHR_LEN];
	int sn = 0;

	for (int i = 0; i < SET_CHAR_MAX; i++)
		set_char_py[i] = SET_PY_NONE;
	for (int w = 0; w < SET_PHR_MAX; w++)
		for (int c = 0; c < SET_PHR_LEN; c++)
			phr_py[w][c] = SET_PY_NONE;

#define PY_LOOKUP(cell)                                                       \
	({                                                                     \
		int _py = SET_PY_NONE;                                          \
		for (int _k = 0; _k < sn; _k++)                                 \
			if (seen[_k] == (cell)) {                                \
				_py = seen_py[_k];                                \
				break;                                            \
			}                                                             \
		if (_py == SET_PY_NONE && sn < (int)(sizeof seen / sizeof seen[0])) { \
			_py = py_syllable_of(cell);                               \
			seen[sn] = (cell);                                        \
			seen_py[sn++] = (signed short)_py;                        \
		}                                                             \
		_py;                                                          \
	})

	for (int i = 0; i < set_char_n; i++)
		set_char_py[i] = (signed short)PY_LOOKUP(set_chars[i]);
	for (int w = 0; w < set_phr_n; w++)
		for (int c = 0; c < set_phr_len[w]; c++)
			phr_py[w][c] = (signed short)PY_LOOKUP(set_phrs[w][c]);
#undef PY_LOOKUP
}

/* typed letters are a prefix of the phrase's initials */
static int phr_initials_hit(int w)
{
	int k = 0;
	for (int c = 0; c < set_phr_len[w]; c++) {
		int s = phr_py[w][c];
		if (s < 0)
			return 0;
		if (k >= py_len)
			return 1;
		if (py_names[py_off[s]] != py_buf[k])
			return 0;
		k++;
	}
	return k >= py_len;
}

/* typed letters are a prefix of the phrase's full pinyin */
static int phr_full_hit(int w)
{
	int k = 0;
	for (int c = 0; c < set_phr_len[w]; c++) {
		int s = phr_py[w][c];
		if (s < 0)
			return 0;
		char const *nm = py_names + py_off[s];
		for (int i = 0; nm[i]; i++) {
			if (k >= py_len)
				return 1;
			if (nm[i] != py_buf[k])
				return 0;
			k++;
		}
	}
	return k >= py_len;
}

static void py_entries_build(void)
{
	py_ent_count = 0;
	/* the user's own characters come first, in list order */
	if (py_syllable >= 0) {
		for (int i = 0; i < set_char_n && py_ent_count < PY_ENT_MAX; i++) {
			if (set_char_py[i] != py_syllable)
				continue;
			py_src[py_ent_count++] = set_chars[i];
		}
	}
	/* stored phrases need two or more letters before they show up */
	if (py_len >= 2) {
		for (int w = 0; w < set_phr_n && py_ent_count < PY_ENT_MAX; w++) {
			if (set_phr_len[w] < 1)
				continue;
			if (!phr_initials_hit(w) && !phr_full_hit(w))
				continue;
			py_src[py_ent_count++] = (uint16_t)(PY_ENT_PHRASE | w);
		}
	}
	/* then the syllable's own glyphs, minus the ones already on top */
	if (py_syllable >= 0) {
		int base = py_start[py_syllable];
		int cnt = py_start[py_syllable + 1] - base;
		for (int i = 0; i < cnt && py_ent_count < PY_ENT_MAX; i++) {
			uint16_t c = (uint16_t)(CELL_CJK + py_glyph[base + i]);
			int dup = 0;
			for (int k = 0; k < py_ent_count; k++) {
				if (py_src[k] == c) {
					dup = 1;
					break;
				}
			}
			if (dup)
				continue;
			py_src[py_ent_count++] = c;
		}
	}
}

static int py_candidates(void)
{
	return py_ent_count;
}

static int py_candidate_cells(int i, uint16_t *out)
{
	if (i < 0 || i >= py_ent_count)
		return 0;
	uint16_t s = py_src[i];

	if (s & PY_ENT_PHRASE) {
		int w = (int)(s & ~PY_ENT_PHRASE);
		if (w < 0 || w >= set_phr_n)
			return 0;
		for (int c = 0; c < set_phr_len[w]; c++)
			out[c] = set_phrs[w][c];
		return set_phr_len[w];
	}
	out[0] = s;
	return 1;
}

static uint16_t py_candidate(int i)
{
	uint16_t out[SET_PHR_LEN];

	return py_candidate_cells(i, out) > 0 ? out[0] : 0;
}

static int py_candidate_w(int i)
{
	uint16_t cc[SET_PHR_LEN];
	int n = py_candidate_cells(i, cc);
	int w = 0;

	for (int k = 0; k < n; k++)
		w += cell_adv(cc[k]);
	return w;
}

static void py_reset(void)
{
	py_len = 0;
	py_buf[0] = 0;
	py_syllable = -1;
	py_pick = 0;
	py_ent_count = 0;
}

static void py_refresh(void)
{
	py_syllable = py_match();
	py_pick = 0;
	py_entries_build();
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

/* The punctuation grid is 13 cells wide but only PUNCT_COUNT cells exist, and
   the last row is two cells short.  Column arithmetic therefore walked off the
   end of the table (bug 23) and could never reach 第三行 or ♂ (bug 24).  The
   cursor walks the cells that really exist, in reading order. */
static int punct_live_at(int pos)
{
	int k = 0;
	for (int i = 0; i < PUNCT_COUNT; i++)
		if (punct_cell(i)) {
			if (k == pos)
				return i;
			k++;
		}
	return -1;
}

static int punct_pos(void)
{
	int idx = kb_row * 13 + kb_col, k = 0;
	for (int i = 0; i < PUNCT_COUNT; i++)
		if (punct_cell(i)) {
			if (i == idx)
				return k;
			k++;
		}
	return 0;
}

static int punct_live_n(void)
{
	int k = 0;
	for (int i = 0; i < PUNCT_COUNT; i++)
		if (punct_cell(i))
			k++;
	return k ? k : 1;
}

static void punct_set(int pos)
{
	int n = punct_live_n();
	int idx = punct_live_at(((pos % n) + n) % n);
	if (idx >= 0) {
		kb_row = idx / 13;
		kb_col = idx % 13;
	}
}

static void punct_side(int dir)
{
	punct_set(punct_pos() + dir);
}

static void punct_move(int dir)		/* up / down: nearest cell in that row */
{
	int idx = punct_live_at(punct_pos());
	int col = idx % 13, trow = idx / 13 + dir;
	int best = -1, bestd = 99;

	if (idx < 0 || trow < 0 || trow > 2)
		return;
	for (int i = 0; i < PUNCT_COUNT; i++) {
		int d;
		if (i / 13 != trow || !punct_cell(i))
			continue;
		d = i % 13 - col;
		if (d < 0)
			d = -d;
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	if (best >= 0) {
		kb_row = best / 13;
		kb_col = best % 13;
	}
}

/*=== goto line ==================================================
  A line number page: the reader's F4 and the editor's second page F3 both
  lead here, so a long document can be reached without paging. */
static char goto_buf[8];
static int goto_len;

static void open_goto(void)
{
	goto_buf[0] = 0;
	goto_len = 0;
	py_reset();
	mode = MODE_GOTO;
}

static void draw_goto(void)
{
	char tmp[32];
	char view[12];

	ui_no_cups = 1;
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	ui_title("\xCC\xF8\xD7\xAA");
	snprintf(view, sizeof view, "%s_", goto_buf);
	draw_ui(2, CONTENT_Y + 2, "\xCC\xF8\xD7\xAA", 0);
	draw_ui(46, CONTENT_Y + 2, view, 0);
	{
		int tp = (total_rows + rows() - 1) / rows();
		if (tp < 1)
			tp = 1;
		snprintf(tmp, sizeof tmp, "%d / %d", goto_len ? atoi(goto_buf) : 0, tp);
	}
	draw_ui(2, CONTENT_Y + 12, tmp, 0);
	draw_ui(2, CONTENT_Y + 22, "\xCA\xE4\xC8\xEB\xD2\xB3\xC2\xEB", 0);
	draw_ui(2, STATUS_Y, "EXE \xCC\xF8\xD7\xAA   EXIT \xC8\xA1\xCF\xFB", 0);
	ui_hints("", "", 0);
	dupdate();
}

static void handle_goto_key(key_event_t ev)
{
	int line;

	if (ev.key == KEY_EXIT) {
		mode = MODE_VIEW;
		return;
	}
	if (ev.key == KEY_EXE) {
		if (goto_len > 0) {
			int tp = (total_rows + rows() - 1) / rows();
			if (tp < 1)
				tp = 1;
			line = atoi(goto_buf);	/* a page number now */
			if (line < 1)
				line = 1;
			if (line > tp)
				line = tp;
			goto_row((line - 1) * rows());
			if (edit_mode) {
				cur_line = offset_to_line(row_start(view_top));
				cur_col = 0;
			}
		}
		mode = MODE_VIEW;
		return;
	}
	if (ev.key == KEY_DEL) {
		if (goto_len > 0)
			goto_buf[--goto_len] = 0;
		return;
	}
	{
		/* key_ascii knows where each digit really is: the KEY_ constants
		   are not laid out in digit order */
		char c = key_ascii(ev.key);
		if (c >= '0' && c <= '9' && goto_len < 6) {
			goto_buf[goto_len++] = c;
			goto_buf[goto_len] = 0;
		}
	}
}

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
	box_lab[5] = "\xB0\xEF\xD6\xFA";		/* F6  帮助 */
	int y = CONTENT_Y + 2;
	draw_ui(2, y, "\xB2\xE9\xD5\xD2", 0);
	int x = 30;
	for (int i = 0; i < search_len && x < 120; i++) {
		draw_cell(x, y, search_cells[i], 0);
		x += cell_adv(search_cells[i]);
	}
	for (int k = 0; k < 7; k++)
		ink(x + 1, y + k);

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
			uint16_t cc[SET_PHR_LEN];
			int cl = py_candidate_cells(idx, cc);
			int cw = py_candidate_w(idx);
			if (idx >= n || cl <= 0 || cx + cw > 127)
				break;
			if (idx == py_pick)
				drect(cx - 1, y - 1, cx + cw, y + 7, INKBAR);
			for (int k = 0; k < cl; k++) {
				draw_cell(cx, y, cc[k], idx == py_pick);
				cx += cell_adv(cc[k]);
			}
			cx += 2;
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
		if (ev.key == KEY_F6 && ev.shift == 0) {
			help_page = 3;		/* the find page's own help */
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
					{
					uint16_t cc[SET_PHR_LEN];
					int cl = py_candidate_cells(py_pick, cc);
					for (int k = 0; k < cl && search_len < (int)(sizeof search_cells / sizeof search_cells[0]); k++)
						search_cells[search_len++] = cc[k];
				}
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
	text_gen++;
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
	text_gen++;
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

/* Where the keyboard goes back to when it is done. */
static void kb_back(void)
{
	ime_flush();
	py_reset();
	if (kb_target == 2 || kb_target == 3)
		mode = MODE_SET_IME;
	else if (kb_target == 4)
		mode = MODE_NAME;
	else
		mode = MODE_VIEW;
}

/* Append one cell to the current target: the document, the search prompt,
   the frequent-character list (2) or the phrase being composed (3). */
static void kb_back(void);
static void set_phrase_commit(void);
static int cfg_save(void);
static int ime_save(void);
/* the word list may be called FXTXT.IME_1, FXTXT.IME_mine, WORDS.IME_a ...
   anything that matches *.IME_*; find whichever one is there */
static int ime_find(char *name, int cap);
static void set_saved(int rc);
static void ime_flush(void);
static int ime_dirty;
static void open_keyboard(int target, int page);

static void kb_put(uint16_t cell)
{
	if (kb_target == 1) {
		if (kb_len < (int)(sizeof kb_cells / sizeof kb_cells[0]))
			kb_cells[kb_len++] = cell;
		return;
	}
	if (kb_target == 2) {
		if (!cell)
			return;
		for (int i = 0; i < set_char_n; i++) {
			if (set_chars[i] == cell) {
				set_sel = i;
				snprintf(status_msg, sizeof status_msg, "\xD2\xD1\xD4\xDA\xC1\xD0\xB1\xED\xD6\xD0");
				return;		/* already in the list, nothing to save */
			}
		}
		if (set_char_n < SET_CHAR_MAX) {
			set_chars[set_char_n++] = cell;
			set_sel = set_char_n - 1;
			py_cache_lists();
			ime_dirty = 1;
		}
		return;
	}
	if (kb_target == 3) {
		if (cell && phr_buf_n < SET_PHR_LEN)
			phr_buf[phr_buf_n++] = cell;
		return;
	}
	if (kb_target == 4) {
		char b[4];
		cell_to_gb(cell, b);
		for (int k = 0; b[k]; k++) {
			char c = b[k];
			if (!name_char_ok(c))
				continue;
			if (name_len >= (int)sizeof name_buf - 1)
				break;
			for (int i = name_len; i > name_pos; i--)
				name_buf[i] = name_buf[i - 1];
			name_buf[name_pos] = c;
			name_len++;
			name_pos++;
			name_buf[name_len] = 0;
		}
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
	if (kb_target == 2 || kb_target == 3) {
		ui_title(kb_target == 2 ? "\xCC\xED\xBC\xD3\xD7\xD6\xB4\xCA" : "\xCC\xED\xBC\xD3\xD7\xD6\xB4\xCA");
	} else {
		ui_title(punct ? "\xB1\xEA\xB5\xE3\xB7\xFB\xBA\xC5"
		               : pinyin ? (kb_target == 1 ? "\xC6\xB4\xD2\xF4\xCB\xD1\xCB\xF7" : "\xC6\xB4\xD2\xF4\xCA\xE4\xC8\xEB")
		                        : (kb_target == 1 ? "\xCB\xD1\xCB\xF7\xCE\xC4\xD7\xD6" : "\xB2\xE5\xC8\xEB\xD7\xD6\xB7\xFB"));
	}

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
				uint16_t cc[SET_PHR_LEN];
				int cl = py_candidate_cells(idx, cc);
				int cw = py_candidate_w(idx);
				if (idx >= ncand || cl <= 0 || x + cw > 126)
					break;
				if (idx == py_pick)
					drect(x - 1, y - 1, x + cw, y + 9, INKBAR);
				for (int k = 0; k < cl; k++) {
					draw_cell(x, y, cc[k], idx == py_pick);
					x += cell_adv(cc[k]);
				}
				x += 2;
			}
		} else if (py_len) {
			draw_ui(2, y, "\xCE\xDE\xC6\xA5\xC5\xE4", 0);
		}
		grid_y = CONTENT_Y + 22;
	} else {
		/* five letter rows have to fit above the status line: start one
		   row higher than the old grid did (its last row was clipped) */
		grid_y = CONTENT_Y + 1;
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
			if (y + 8 > STATUS_Y)	/* the fifth letter row used to be cut */
				continue;
			int sel = (r == kb_row && c == kb_col);
			if (sel)
				drect(x - 1, y - 1, x + 8, y + 8, INKBAR);
			draw_cell(x, y, pcell ? pcell : (uint16_t)((unsigned char)ch - 0x20),
			          sel ? 1 : 0);
		}
	}

	/* status: echo the search prompt, or key hints */
	if (kb_target == 1) {
		dhline(STATUS_Y - 1, INKBAR);
		int x = 2;
		for (int i = 0; i < kb_len && x < 118; i++) {
			draw_cell(x, STATUS_Y, kb_cells[i], 0);
			x += cell_adv(kb_cells[i]);
		}
		for (int k = 0; k < 7; k++)
			ink(x + 1, STATUS_Y + k);
	} else if (pinyin) {
		ui_status(py_candidates() ? "F3/F4 \xD1\xA1\xD7\xD6   F5 \xC9\xCF\xC6\xC1" : "F1 \xBB\xBB\xD2\xB3   F6 \xCD\xEA\xB3\xC9");
	} else if (punct) {
		ui_status("F1 \xBB\xBB\xD2\xB3   F6 \xCD\xEA\xB3\xC9");
	} else {
		ui_status("F1 \xBB\xBB\xD2\xB3   F6 \xCD\xEA\xB3\xC9");
	}
	dupdate();
}

/*=== help =================================================================*/

static int help_sel;
static int help_back = MODE_LIST;	/* where to return after help */
static char del_name[NAME_MAX_L + 1];	/* file waiting for confirmation */
static int confirm_kind;		/* 0 = delete, 1 = discard edits */

static void draw_help(void)
{
	ui_no_cups = 1;		/* no cups: the bottom row carries the hint */
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	/* four pages, all with a title, a page name on the first line and the
	   same paging hint - page four used to draw bookmark slots with no
	   title, no text and no dupdate() at all */
	static char const *page1[] = {
		"\xC9\xCF\xCF\xC2\xBC\xFC\x20\xD1\xA1\xD4\xF1\x20\x20\x20\x45\x58\x45\x20\xB4\xF2\xBF\xAA",
		"\x46\x31\x20\xD0\xC2\xBD\xA8\x20\x20\x20\x46\x32\x20\xB8\xC4\xC3\xFB",
		"\x44\x45\x4C\x20\xC9\xBE\xB3\xFD\x20\x20\x20\x46\x36\x20\xC9\xE8\xD6\xC3",
		"\x45\x58\x49\x54\x20\xB7\xB5\xBB\xD8\xC9\xCF\xBC\xB6",
	};
	static char const *page2[] = {
		"\xC9\xCF\xCF\xC2\xBC\xFC\x20\xB7\xAD\xD2\xB3\x20\x20\x20\x45\x58\x45\x20\xB0\xEF\xD6\xFA",
		"\x46\x31\x20\xB1\xE0\xBC\xAD\x20\x20\x20\x46\x32\x20\xB2\xE9\xD5\xD2",
		"\x46\x34\x20\xCC\xF8\xD7\xAA\x20\x20\x20\x46\x35\x20\xCA\xE9\xC7\xA9",
		"\x46\x36\x20\xC9\xE8\xD6\xC3\x20\x20\x20\x45\x58\x49\x54\x20\xB7\xB5\xBB\xD8",
	};
	static char const *page3[] = {
		"\x46\x31\x20\xB1\xA3\xB4\xE6\x20\x20\x20\x46\x32\x20\xCD\xCB\xB3\xF6",
		"\x46\x33\x20\xB4\xF3\xD0\xB4\x20\x20\x20\x46\x34\x20\xB1\xEA\xB5\xE3",
		"\x46\x35\x20\xD6\xD0\xD3\xA2\x20\x20\x20\x46\x36\x20\xB7\xAD\xD2\xB3",
		"\xB6\xFE\xD2\xB3\x20\x46\x35\x20\xC9\xE8\xD6\xC3\x20\x20\x45\x58\x45\x20\xBB\xBB\xD0\xD0",
	};
	static char const *page4[] = {
		"\x46\x31\x20\xCB\xD1\xCB\xF7\x20\x20\x20\x45\x58\x49\x54\x20\xB7\xB5\xBB\xD8",
		"\x46\x32\x20\xC9\xCF\xCC\xF5\x20\x20\x20\x46\x33\x20\xCF\xC2\xCC\xF5",
		"\x46\x36\x20\xB0\xEF\xD6\xFA\x20\x20\x20\xC9\xCF\xCF\xC2\xBC\xFC\x20\xBB\xBB\xC3\xFC\xD6\xD0",
		"\xBD\xE1\xB9\xFB\xD2\xB3\x20\x46\x35\x20\xCD\xCB\xB3\xF6\xBD\xE1\xB9\xFB",
	};
	static char const *page5[] = {
		"\x46\x31\x20\xB7\xB4\xC9\xAB\x20\x20\x20\x46\x32\x20\xCA\xE4\xC8\xEB\xB7\xA8",
		"\x46\x33\x20\xBC\xC6\xCA\xFD\x20\x20\x20\x46\x36\x20\xB0\xEF\xD6\xFA",
		"\xC9\xE8\xD6\xC3\xB4\xE6\x20\x46\x58\x54\x58\x54\x2E\x43\x46\x47",
		"\xD7\xD6\xB4\xCA\xB4\xE6\x20\x2A\x2E\x49\x4D\x45\x5F\x78",
	};
	static char const *titles[] = { "\xC1\xD0\xB1\xED\xD2\xB3", "\xD4\xC4\xB6\xC1\xD2\xB3", "\xB1\xE0\xBC\xAD\xD2\xB3", "\xB2\xE9\xD5\xD2\xD2\xB3", "\xC9\xE8\xD6\xC3\xD2\xB3" };
	char const **pages[5];
	char head[32];
	int y;

	pages[0] = page1;
	pages[1] = page2;
	pages[2] = page3;
	pages[3] = page4;
	pages[4] = page5;
	if (help_page < 0 || help_page > 4)
		help_page = 0;
	snprintf(head, sizeof head, "\xB0\xEF\xD6\xFA\x20\x25\x64\x2F\x35", help_page + 1);
	ui_title(head);
	draw_ui(2, CONTENT_Y, titles[help_page], 0);
	y = CONTENT_Y + 9;
	for (int i = 0; i < 4; i++, y += 9)
		draw_ui(2, y, pages[help_page][i], 0);
	{
		/* paging hint along the bottom edge, left aligned so the first
		   glyph is never clipped */
		draw_ui(2, STATUS_Y, "\xD7\xF3\xD3\xD2\xBC\xFC\x20\xB7\xAD\xD2\xB3\x20\x20\x45\x58\x49\x54\x20\xB9\xD8\xB1\xD5", 0);
	}
	ui_hints("", "", 0);
	dupdate();
}


/*=== settings =============================================================
   Three items, each on its own page; on those pages the F keys pick the
   value, exactly as the F bar says.  The values are plain statics: gint keeps
   the image resident, so they survive a menu round trip (writing them to
   \fls0 is what used to make the main menu drop the add-in list). */


/* GB2312 bytes -> display cells, the same mapping the font tables use */
static int gb_cells(char const *s, uint16_t *out, int cap)
{
	int n = 0;
	unsigned char const *p = (unsigned char const *)s;
	while (*p && n < cap) {
		if (*p < 0x80) {
			if (*p > 0x20)
				out[n++] = (uint16_t)(*p - 0x20);
			p++;
		} else if (p[1]) {
			int g = (p[0] - 0xA1) * 94 + (p[1] - 0xA1);
			out[n++] = (uint16_t)(CELL_CJK + g);
			p += 2;
		} else {
			break;
		}
	}
	return n;
}

/*=== settings and word-list files =========================================

   Two plain text files in the same folder as the documents, so a PC can edit
   them and copy them back:

     \\fls0\FXTXT.CFG     invert=0|1 / unit=page|line            (ASCII)
     \\fls0\FXTXT.IME_1   [chars] and [words] sections            (GB2312)

   Both are rewritten in place; a BFile is only created when it does not exist
   yet, because recreating files in \fls0 has dropped the add-in list here. */

#define CFG_FILE "FXTXT.CFG"
#define IME_FILE "FXTXT.IME_1"	/* created when no *.IME_* exists yet */
#define IME_PAT  "*.IME_*"	/* any suffix after the underscore */
#define IME_MAGIC "FIM1"
#define CFG_ALLOC 32		/* a few dozen bytes is all the settings need */
#define IME_ALLOC 512		/* binary word list: 4+1+48 + 8*(1+24) = 253 max */

static int txt_fc(uint16_t *fc, int cap, char const *name)
{
	char p[48];
	snprintf(p, sizeof p, "\\\\fls0\\%s", name);
	int m = 0;
	for (int i = 0; p[i] && m < cap - 1; i++)
		fc[m++] = (unsigned char)p[i];
	fc[m] = 0;
	return m;
}

static int txt_load(char const *name, char *buf, int cap)
{
	uint16_t fc[48];
	if (!txt_fc(fc, 48, name))
		return -1;
	int fd = BFile_Open(fc, BFile_ReadOnly);
	if (fd < 0) {
		/* stdio fallback for the emulator's image */
		char p[48];
		FILE *f;
		snprintf(p, sizeof p, "\\\\fls0\\%s", name);
		f = fopen(p, "rb");
		if (!f) {
			snprintf(p, sizeof p, "/fls0/%s", name);
			f = fopen(p, "rb");
		}
		if (!f)
			return -1;
		int n = (int)fread(buf, 1, (size_t)(cap - 1), f);
		fclose(f);
		if (n < 0)
			return -1;
		buf[n] = 0;
		return n;
	}
	int n = BFile_Read(fd, buf, cap - 1, 0);
	BFile_Close(fd);
	if (n < 0 || n >= cap)
		n = 0;
	buf[n] = 0;
	return n;
}

/* last resort for the emulator image, where BFile files are not always
   visible to the host: plain stdio into /fls0 */
static int txt_save_stdio(char const *name, char const *data, int len)
{
	char p[48];
	FILE *f;

	snprintf(p, sizeof p, "\\\\fls0\\%s", name);
	f = fopen(p, "wb");
	if (!f)
		return -1;
	int w = (int)fwrite(data, 1, (size_t)len, f);
	fputc(0, f);
	fclose(f);
	return w == len ? 0 : -1;
}

static int txt_save(char const *name, char const *data, int len, int alloc)
{
	uint16_t fc[48];
	if (!txt_fc(fc, 48, name))
		return -1;
	int fd = BFile_Open(fc, BFile_ReadWrite);
	if (fd >= 0) {
		if (BFile_Size(fd) < len + 2) {
			BFile_Close(fd);
			fd = -1;
		} else {
			BFile_Seek(fd, 0);
		}
	}
	if (fd < 0) {
		int sz = alloc > len + 2 ? alloc : len + 2;
		if (BFile_Create(fc, BFile_File, &sz) < 0)
			return txt_save_stdio(name, data, len);
		fd = BFile_Open(fc, BFile_WriteOnly);
		if (fd < 0)
			return txt_save_stdio(name, data, len);
	}
	int w = bfile_write_even(fd, data, len);
	BFile_Close(fd);
	return w == 0 ? 0 : -1;
}

static int ime_is_ime_name(char const *s)
{
	/* does the name contain ".IME_" in any case? */
	for (int i = 0; s[i]; i++) {
		static char const w[] = ".IME_";
		int k = 0;
		while (w[k] && s[i + k]) {
			char c = s[i + k];
			if (c >= 'a' && c <= 'z')
				c = (char)(c - 32);
			if (c != w[k])
				break;
			k++;
		}
		if (!w[k])
			return 1;
	}
	return 0;
}

static int ime_find(char *name, int cap)
{
	/* one wildcard only: "*.IME_*" has two, and bfile.h says multi-star
	   patterns are unsupported in the worst case */
	uint16_t pat[32], found[256];
	struct BFile_FileInfo fi;
	int h = 0, m = -1, n = 0;
	char const *s = "\\\\fls0\\*";

	for (; *s && n < 31; s++)
		pat[n++] = (unsigned char)*s;
	pat[n] = 0;
	if (BFile_FindFirst(pat, &h, found, &fi) < 0)
		return -1;
	for (;;) {
		int last = -1;
		char base[48];
		for (int i = 0; found[i]; i++)
			if (found[i] == '\\')
				last = i;
		fc_to_bytes(found + last + 1, base, (int)sizeof base);
		if (base[0] && fi.type != BFile_Type_Directory && ime_is_ime_name(base)) {
			snprintf(name, cap, "%s", base);
			m = 0;
			break;
		}
		if (BFile_FindNext(h, found, &fi) < 0)
			break;
	}
	BFile_FindClose(h);
	return m;
}

static int cfg_save(void)
{
	char b[64];
	int n = snprintf(b, sizeof b, "invert=%d\nunit=%s\n",
	                 set_inv ? 1 : 0, set_unit_lines ? "line" : "page");
	return txt_save(CFG_FILE, b, n > 0 ? n : 0, CFG_ALLOC);
}

static void cfg_load(void)
{
	char b[192];
	if (txt_load(CFG_FILE, b, sizeof b) <= 0)
		return;
	char *p = b;
	while (*p) {
		char *e = p;
		while (*e && *e != '\n' && *e != '\r')
			e++;
		char keep = *e;
		*e = 0;
		if (!strncmp(p, "invert=", 7))
			set_inv = atoi(p + 7) ? 1 : 0;
		else if (!strncmp(p, "unit=", 5))
			set_unit_lines = !strncmp(p + 5, "line", 4);
		if (!keep)
			break;
		p = e + 1;
	}
	ui_inv = set_inv;
}

static void cell_to_gb(uint16_t cell, char *out)
{
	if (cell < CELL_CJK) {
		out[0] = (char)(cell + 0x20);
		out[1] = 0;
		return;
	}
	int g = (int)cell - CELL_CJK;
	out[0] = (char)(0xA1 + g / 94);
	out[1] = (char)(0xA1 + g % 94);
	out[2] = 0;
}

static int ime_save(void)
{
	/* FIM1 | nchars | u16[nchars] | nwords | (u8 n | u16[n]) * nwords
	   little endian display cells: keeps the 24+8 entry lists in ~250 B */
	unsigned char b[IME_ALLOC];
	int n = 0;

	memcpy(b, IME_MAGIC, 4);
	n = 4;
	b[n++] = (unsigned char)set_char_n;
	for (int i = 0; i < set_char_n; i++) {
		b[n++] = (unsigned char)(set_chars[i] & 0xFF);
		b[n++] = (unsigned char)(set_chars[i] >> 8);
	}
	b[n++] = (unsigned char)set_phr_n;
	for (int i = 0; i < set_phr_n; i++) {
		int k = set_phr_len[i];
		if (k < 0)
			k = 0;
		if (k > SET_PHR_LEN)
			k = SET_PHR_LEN;
		b[n++] = (unsigned char)k;
		for (int j = 0; j < k; j++) {
			b[n++] = (unsigned char)(set_phrs[i][j] & 0xFF);
			b[n++] = (unsigned char)(set_phrs[i][j] >> 8);
		}
	}
	/* rewrite the file the name was picked for, so a name chosen on a PC
	   survives; create FXTXT.IME_1 only when there was none */
	if (!ime_name[0])
		snprintf(ime_name, sizeof ime_name, "%s", IME_FILE);
	return txt_save(ime_name, (char const *)b, n, IME_ALLOC);
}

static void ime_load_text(char *b)
{
	int sect = 0;		/* 0 none, 1 chars, 2 words */
	char *p = b;
	while (*p) {
		char *e = p;
		while (*e && *e != '\n' && *e != '\r')
			e++;
		char keep = *e;
		*e = 0;
		if (!strcmp(p, "[chars]")) {
			sect = 1;
		} else if (!strcmp(p, "[words]")) {
			sect = 2;
		} else if (*p && *p != '#' && *(unsigned char *)p >= 0x20) {
			if (sect == 1) {
				uint16_t c[1];
				if (gb_cells(p, c, 1) > 0 && set_char_n < SET_CHAR_MAX)
					set_chars[set_char_n++] = c[0];
			} else if (sect == 2) {
				uint16_t c[SET_PHR_LEN];
				int k = gb_cells(p, c, SET_PHR_LEN);
				if (k > 0 && set_phr_n < SET_PHR_MAX) {
					for (int i = 0; i < k; i++)
						set_phrs[set_phr_n][i] = c[i];
					set_phr_len[set_phr_n] = k;
					set_phr_n++;
				}
			}
		}
		if (!keep)
			break;
		p = e + 1;
	}
}

static void ime_load_bin(unsigned char const *b, int n)
{
	int p = 4;

	if (p >= n)
		return;
	int nc = b[p++];
	for (int i = 0; i < nc && p + 1 < n; i++, p += 2)
		if (set_char_n < SET_CHAR_MAX)
			set_chars[set_char_n++] = (uint16_t)(b[p] | (b[p + 1] << 8));
	if (p >= n)
		return;
	int nw = b[p++];
	for (int i = 0; i < nw && p < n; i++) {
		int k = b[p++];
		if (k <= 0 || k > SET_PHR_LEN || p + 2 * k > n)
			break;
		if (set_phr_n < SET_PHR_MAX) {
			for (int j = 0; j < k; j++)
				set_phrs[set_phr_n][j] =
					(uint16_t)(b[p + 2 * j] | (b[p + 2 * j + 1] << 8));
			set_phr_len[set_phr_n] = k;
			set_phr_n++;
		}
		p += 2 * k;
	}
}

static void ime_load(void)
{
	unsigned char b[IME_ALLOC];
	int n;

	if (!ime_name[0] && ime_find(ime_name, sizeof ime_name) < 0)
		snprintf(ime_name, sizeof ime_name, "%s", IME_FILE);
	n = txt_load(ime_name, (char *)b, sizeof b);
	if (n <= 0) {
		py_cache_lists();
		return;
	}
	if (n >= 8 && !memcmp(b, IME_MAGIC, 4)) {
		ime_load_bin(b, n);
		py_cache_lists();
		return;
	}
	ime_load_text((char *)b);	/* plain text still works */
	py_cache_lists();
}

/* say whether a save landed: the settings pages have no other feedback */
static void set_saved(int rc)
{
	snprintf(status_msg, sizeof status_msg,
	         rc == 0 ? "\xD2\xD1\xB1\xA3\xB4\xE6" : "\xB1\xA3\xB4\xE6\xCA\xA7\xB0\xDC");
	status_sticky = 1;
}

static void set_defaults(void)
{
	/* start empty: the lists are the user's own, nothing is preloaded */
	set_char_n = 0;
	set_phr_n = 0;
	phr_buf_n = 0;
}

/* finish the phrase being composed: append it to the list */
static void set_phrase_commit(void)
{
	for (int i = 0; i < set_phr_n && phr_buf_n > 0; i++) {
		if (set_phr_len[i] == phr_buf_n
		    && !memcmp(set_phrs[i], phr_buf, phr_buf_n * sizeof(uint16_t))) {
			set_sel = i;
			snprintf(status_msg, sizeof status_msg, "\xD2\xD1\xD4\xDA\xC1\xD0\xB1\xED\xD6\xD0");
			phr_buf_n = 0;
			return;
		}
	}
	if (phr_buf_n > 0 && set_phr_n < SET_PHR_MAX) {
		for (int i = 0; i < phr_buf_n; i++)
			set_phrs[set_phr_n][i] = phr_buf[i];
		set_phr_len[set_phr_n] = phr_buf_n;
		set_phr_n++;
		set_sel = set_phr_n - 1;
		py_cache_lists();
		ime_dirty = 1;
	}
	phr_buf_n = 0;
}

static void open_settings(void)
{
	set_from = mode;
	set_edit_was = edit_mode;
	set_doc = (ncell > 0);
	status_msg[0] = 0;
	set_sel = 0;
	set_row = 0;		/* every visit starts on the first row */
	mode = MODE_SET;
}

static void ime_flush(void)
{
	if (!ime_dirty)
		return;
	ime_dirty = 0;
	set_saved(ime_save());
}

static void settings_back(void)
{
	ime_flush();
	if (set_from == MODE_VIEW && set_doc) {
		mode = MODE_VIEW;
		edit_mode = set_edit_was;
		if (!edit_mode)
			py_reset();
		return;
	}
	mode = MODE_LIST;
	edit_mode = 0;
	fbar_page = 0;
}

static void set_bar(char const *a, char const *b, char const *c, char const *f)
{
	/* the message goes in the title bar (see the ui_hints calls below) so it
	   never covers the list, and handle_set_key() clears it on the next key */
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	box_lab[0] = a;
	box_lab[1] = b;
	box_lab[2] = c;
	box_lab[5] = f;
}

static void draw_set(void)
{
	static char const *item[5];
	static char const *val[5];
	char cnt[16];
	ui_clip_x = 128;
	ui_info_right = 0;
	item[0] = "\xB7\xB4\xC9\xAB";		/* 反色 */
	item[1] = "\xBC\xC6\xCA\xFD\xB5\xA5\xCE\xBB";	/* 计数单位 */
	item[2] = "\xD7\xD6\xB4\xCA";		/* 字词 */
	item[3] = "\xB0\xEF\xD6\xFA";		/* 帮助 */
	item[4] = "\xB9\xD8\xD3\xDA";		/* 关于 */
	val[0] = set_inv ? "\xBF\xAA" : "\xB9\xD8";
	val[1] = set_unit_lines ? "\xD0\xD0" : "\xD2\xB3";
	snprintf(cnt, sizeof cnt, "%d/%d", set_char_n, set_phr_n);
	val[2] = cnt;
	val[3] = "";
	val[4] = "";

	/* the F bar always belongs to the row the cursor is on */
	for (int i = 0; i < 6; i++)
		box_lab[i] = NULL;
	switch (set_row) {
	case 0:
		box_lab[0] = "\xB9\xD8";	/* 关 */
		box_lab[1] = "\xBF\xAA";	/* 开 */
		break;
	case 1:
		box_lab[0] = "\xD2\xB3";	/* 页 */
		box_lab[1] = "\xD0\xD0";	/* 行 */
		break;
	default:
		box_lab[0] = "\xBD\xF8\xC8\xEB";	/* 进入 */
		break;
	}
	ui_no_cups = 0;
	ui_title("\xC9\xE8\xD6\xC3");
	for (int i = 0; i < 5; i++) {
		int y = CONTENT_Y + i * 9;
		if (i == set_row)
			draw_next_icon(2, y + 1);
		draw_ui(12, y, item[i], 0);
		if (val[i][0]) {
			int w = ui_width(val[i]);
			int x = TEXT_W - 4 - w;
			if (x < 60)
				x = 60;
			ui_clip_x = 128;
			draw_ui(x, y, val[i], 0);
		}
	}
	ui_hints("", status_msg[0] ? status_msg : "\xC9\xCF\xCF\xC2\xD1\xA1\xD4\xF1", 0);
	dupdate();
}

static void draw_set_about(void)
{
	ui_clip_x = 128;
	ui_no_cups = 1;
	ui_info_right = 0;
	ui_title("\xB9\xD8\xD3\xDA");
	draw_ui(2, CONTENT_Y + 1, "FXTXT  1.0", 0);
	draw_ui(2, CONTENT_Y + 11, "\xC9\xE8\xD6\xC3\x20\x46\x58\x54\x58\x54\x2E\x43\x46\x47", 0);
	draw_ui(2, CONTENT_Y + 21, "\xD7\xD6\xB4\xCA\x20\x2A\x2E\x49\x4D\x45\x5F\x2A", 0);
	draw_ui(2, CONTENT_Y + 31, "\x45\x58\x49\x54\x20\xB7\xB5\xBB\xD8", 0);
	ui_hints("", "", 0);
	dupdate();
}

static void draw_set_inv(void)
{
	ui_clip_x = 128;
	ui_no_cups = 0;
	ui_info_right = 0;
	set_bar("\xB9\xD8", "\xBF\xAA", 0, "\xB0\xEF\xD6\xFA");
	ui_title("\xCF\xD4\xCA\xBE\xB7\xB4\xC9\xAB");
	draw_ui(2, CONTENT_Y + 1, "\xCF\xD4\xCA\xBE\xB7\xB4\xC9\xAB", 0);
	draw_ui(2, CONTENT_Y + 13, set_inv ? "\xB5\xB1\xC7\xB0\x20\xBF\xAA" : "\xB5\xB1\xC7\xB0\x20\xB9\xD8", 0);
	draw_ui(2, CONTENT_Y + 22, "\x46\x31\x20\xB9\xD8\x20\x20\x20\x46\x32\x20\xBF\xAA", 0);
	ui_hints("", status_msg, 0);
	dupdate();
}

static void draw_set_unit(void)
{
	ui_clip_x = 128;
	ui_no_cups = 0;
	ui_info_right = 0;
	set_bar("\xD2\xB3", "\xD0\xD0", 0, "\xB0\xEF\xD6\xFA");
	ui_title("\xBC\xC6\xCA\xFD\xB5\xA5\xCE\xBB");
	draw_ui(2, CONTENT_Y + 1, "\xD4\xC4\xB6\xC1\xD2\xB3\xBC\xC6\xCA\xFD", 0);
	draw_ui(2, CONTENT_Y + 13, set_unit_lines ? "\xB5\xB1\xC7\xB0\x20\xD0\xD0" : "\xB5\xB1\xC7\xB0\x20\xD2\xB3", 0);
	draw_ui(2, CONTENT_Y + 22, "\x46\x31\x20\xD2\xB3\x20\x20\x20\x46\x32\x20\xD0\xD0", 0);
	ui_hints("", status_msg, 0);
	dupdate();
}

static void draw_set_ime(void)
{
	int total = set_char_n + set_phr_n;
	ui_clip_x = 128;
	ui_no_cups = 0;
	ui_info_right = 0;
	for (int i = 0; i < 6; i++)
		box_lab[i] = NULL;
	box_lab[0] = "\xD4\xF6";	/* 增 */
	box_lab[1] = "\xB4\xCA";	/* 词 */
	box_lab[2] = "\xC9\xBE";	/* 删 */
	box_lab[3] = "\xD2\xC6";	/* 移 */
	box_lab[5] = "\xD6\xFA";	/* 助 */
	ui_title_state("\xD7\xD6\xB4\xCA", 1);
	if (total == 0) {
		draw_ui(2, CONTENT_Y + 2, "\x28\xBF\xD5\x29\x20\x20\x46\x31\x20\xBC\xD3\xD7\xD6\x20\x20\x20\x46\x32\x20\xBC\xD3\xB4\xCA", 0);
	} else {
		int top = set_sel - set_sel % 3;
		int y = CONTENT_Y + 2;
		for (int k = top; k < total && k < top + 3; k++, y += 12) {
			if (k == set_sel)
				draw_next_icon(2, y + 1);
			int x = 12;
			if (k < set_char_n) {
				draw_cell(x, y, set_chars[k], 0);
			} else {
				int w = k - set_char_n;
				for (int c = 0; c < set_phr_len[w]; c++) {
					draw_cell(x, y, set_phrs[w][c], 0);
					x += cell_adv(set_phrs[w][c]);
				}
			}
		}
	}
	ui_hints("", status_msg, 0);
	dupdate();
}

/* the old per-list pages now share the merged one */
static void draw_set_chars(void)
{
	draw_set_ime();
}

static void draw_set_phr(void)
{
	ui_clip_x = 128;
	ui_no_cups = 0;
	ui_info_right = 0;
	set_bar("\xCC\xED\xBC\xD3", "\xC9\xBE\xB3\xFD", 0, "\xB0\xEF\xD6\xFA");
	ui_title_state("\xBF\xEC\xBD\xDD\xB6\xCC\xD3\xEF", 1);
	if (set_phr_n == 0 && phr_buf_n == 0)
		draw_ui(2, CONTENT_Y + 1, "\x28\xBF\xD5\x29  F1 \xCC\xED\xBC\xD3", 0);
	for (int i = 0; i < set_phr_n; i++) {
		int y = CONTENT_Y + 1 + i * 10;
		if (i == set_sel)
			drect(0, y - 1, TEXT_W - 1, y + 7, INKBAR);
		int x = 3;
		for (int k = 0; k < set_phr_len[i]; k++) {
			draw_cell(x, y, set_phrs[i][k], i == set_sel);
			x += cell_adv(set_phrs[i][k]);
		}
	}
	if (phr_buf_n > 0) {
		/* the phrase being typed, on the line above the F bar */
		int x = 2;
		int y = CONTENT_Y + 26;
		draw_ui(2, y - 9, "\xCA\xE4\xC8\xEB\xD6\xD0\xA3\xBA", 0);
		for (int k = 0; k < phr_buf_n; k++) {
			draw_cell(x, y, phr_buf[k], 0);
			x += cell_adv(phr_buf[k]);
		}
		for (int k = 0; k < 7; k++)
			ink(x + 1, y + k);
	}
	ui_hints("", status_msg, 0);
	dupdate();
}

/* put one cell, or a whole phrase, into the document at the caret */
static void set_insert(uint16_t const *src, int n)
{
	if (!set_doc || ncell == 0 || n <= 0) {
		snprintf(status_msg, sizeof status_msg, "\xC3\xBB\xD3\xD0\xB4\xF2\xBF\xAA\xB5\xC4\xCE\xC4\xB5\xB5");
		status_sticky = 1;
		return;
	}
	{
		/* from the reader there is no caret on screen, so the cell went to
		   whatever position the last edit left behind (often line 0, far
		   from the page being read).  Put it on the first cell of the
		   visible page instead. */
		if (!set_edit_was) {
			cur_line = offset_to_line(row_start(view_top));
			if (cur_line < 0 || cur_line > line_count() - 1)
				cur_line = 0;
			cur_col = 0;
		}
	}
	for (int i = 0; i < n; i++)
		text_insert(src[i]);
	dirty = 1;
	/* show the row that got the text */
	{
		int c = line_start(cur_line) + cur_col;
		goto_row(chunk_rows + row_of_cell(c > ncell ? ncell : c));
	}
	snprintf(status_msg, sizeof status_msg, "\xD2\xD1\xB2\xE5\xC8\xEB");
	status_sticky = 1;
	settings_back();
}

static void handle_set_key(key_event_t ev)
{
	/* a save report lives for exactly one key press (buglist2 item 22) */
	status_msg[0] = 0;
	if (ev.key == KEY_EXIT) {
		/* buglist2 21: one level per page, never a shortcut to the top */
		if (mode == MODE_SET_CHARS || mode == MODE_SET_PHR)
			mode = MODE_SET_IME;
		else if (mode == MODE_SET)
			settings_back();
		else
			mode = MODE_SET;
		return;
	}
	if (ev.key == KEY_F6) {
		help_page = 0;		/* buglist2 15: help always starts on page 1 */
		help_back = mode;
		mode = MODE_HELP;
		return;
	}
	if (mode == MODE_SET) {
		if (ev.key == KEY_UP) {
			if (set_row > 0)
				set_row--;
		} else if (ev.key == KEY_DOWN) {
			if (set_row < 4)
				set_row++;
		} else if (ev.key == KEY_F1 || ev.key == KEY_F2 || ev.key == KEY_EXE) {
			int on = (ev.key == KEY_F2);
			if (set_row == 0 && ev.key != KEY_EXE) {
				if (set_inv == on) { status_msg[0] = 0; return; }
				set_inv = on, ui_inv = on;
				int rc = cfg_save();
				cfg_load();
				set_saved(rc == 0 && set_inv == on ? 0 : -1);
				return;
			}
			if (set_row == 1 && ev.key != KEY_EXE) {
				if (set_unit_lines == on) { status_msg[0] = 0; return; }
				set_unit_lines = on;
				int rc = cfg_save();
				cfg_load();
				set_saved(rc == 0 && set_unit_lines == on ? 0 : -1);
				return;
			}
			switch (set_row) {
			case 0: mode = MODE_SET_INV; break;
			case 1: mode = MODE_SET_UNIT; break;
			case 2: set_sel = 0; mode = MODE_SET_IME; break;
			case 3: help_page = 0; help_back = MODE_SET; mode = MODE_HELP; break;
			default: mode = MODE_SET_ABOUT; break;
			}
		}
		return;
	}
	if (mode == MODE_SET_INV) {
		if (ev.key == KEY_F1) {
			set_inv = 0, ui_inv = 0;
			int rc = cfg_save();
			cfg_load();
			set_saved(rc == 0 && !set_inv ? 0 : -1);
		} else if (ev.key == KEY_F2) {
			set_inv = 1, ui_inv = 1;
			int rc = cfg_save();
			cfg_load();
			set_saved(rc == 0 && set_inv ? 0 : -1);
		}
		return;
	}
	if (mode == MODE_SET_UNIT) {
		if (ev.key == KEY_F1) {
			set_unit_lines = 0;
			int rc = cfg_save();
			cfg_load();
			set_saved(rc == 0 && !set_unit_lines ? 0 : -1);
		} else if (ev.key == KEY_F2) {
			set_unit_lines = 1;
			int rc = cfg_save();
			cfg_load();
			set_saved(rc == 0 && set_unit_lines ? 0 : -1);
		}
		return;
	}
	if (mode == MODE_SET_IME) {
		int total = set_char_n + set_phr_n;
		if (ev.key == KEY_UP) {
			if (set_sel > 0)
				set_sel--;
		} else if (ev.key == KEY_DOWN) {
			if (set_sel < total - 1)
				set_sel++;
		} else if (ev.key == KEY_EXE && total > 0) {
			if (set_sel < set_char_n) {
				uint16_t one[1];
				one[0] = set_chars[set_sel];
				set_insert(one, 1);
			} else {
				set_insert(set_phrs[set_sel - set_char_n],
				           set_phr_len[set_sel - set_char_n]);
			}
		} else if (ev.key == KEY_F1) {
			open_keyboard(2, PAGE_PINYIN);
		} else if (ev.key == KEY_F2) {
			phr_buf_n = 0;
			open_keyboard(3, PAGE_PINYIN);
		} else if (ev.key == KEY_F3 && total > 0) {
			if (set_sel < set_char_n) {
				for (int i = set_sel; i < set_char_n - 1; i++)
					set_chars[i] = set_chars[i + 1];
				set_char_n--;
			} else {
				int p = set_sel - set_char_n;
				for (int i = p; i < set_phr_n - 1; i++) {
					memcpy(set_phrs[i], set_phrs[i + 1], sizeof set_phrs[0]);
					set_phr_len[i] = set_phr_len[i + 1];
				}
				set_phr_n--;
			}
			if (set_sel >= set_char_n + set_phr_n && set_sel > 0)
				set_sel--;
			py_cache_lists();
			ime_dirty = 1;
		} else if (ev.key == KEY_F4 && total > 0) {
			if (set_sel < set_char_n && set_sel > 0) {
				uint16_t c = set_chars[set_sel];
				for (int i = set_sel; i > 0; i--)
					set_chars[i] = set_chars[i - 1];
				set_chars[0] = c;
				set_sel = 0;
			} else if (set_sel >= set_char_n && set_sel > set_char_n) {
				int p = set_sel - set_char_n;
				uint16_t tmp[SET_PHR_LEN];
				int tl = set_phr_len[p];
				memcpy(tmp, set_phrs[p], sizeof tmp);
				for (int i = p; i > 0; i--) {
					memcpy(set_phrs[i], set_phrs[i - 1], sizeof set_phrs[0]);
					set_phr_len[i] = set_phr_len[i - 1];
				}
				memcpy(set_phrs[0], tmp, sizeof tmp);
				set_phr_len[0] = tl;
				set_sel = set_char_n;
			}
			py_cache_lists();
			ime_dirty = 1;
		}
		return;
	}
	if (mode == MODE_SET_CHARS) {
		int cols = 8;
		if (ev.key == KEY_LEFT) {
			if (set_sel > 0)
				set_sel--;
		} else if (ev.key == KEY_RIGHT) {
			if (set_sel < set_char_n - 1)
				set_sel++;
		} else if (ev.key == KEY_UP) {
			if (set_sel >= cols)
				set_sel -= cols;
		} else if (ev.key == KEY_DOWN) {
			if (set_sel + cols < set_char_n)
				set_sel += cols;
		} else if (ev.key == KEY_EXE) {
			if (set_char_n > 0)
				set_insert(&set_chars[set_sel], 1);
		} else if (ev.key == KEY_F1) {
			status_msg[0] = 0;
			open_keyboard(2, PAGE_PINYIN);
		} else if (ev.key == KEY_F2) {
			if (set_char_n > 0) {
				for (int i = set_sel; i + 1 < set_char_n; i++)
					set_chars[i] = set_chars[i + 1];
				set_char_n--;
				if (set_sel >= set_char_n && set_sel > 0)
					set_sel--;
				py_cache_lists();
				ime_dirty = 1;
			}
		} else if (ev.key == KEY_F3) {
			/* 前移: the list order is the candidate order, so this is
			   how a character is put in front of the others */
			if (set_char_n > 0 && set_sel > 0) {
				uint16_t c = set_chars[set_sel];
				for (int i = set_sel; i > 0; i--)
					set_chars[i] = set_chars[i - 1];
				set_chars[0] = c;
				set_sel = 0;
				py_cache_lists();
				ime_dirty = 1;
			}
		}
		return;
	}
	if (mode == MODE_SET_PHR) {
		if (ev.key == KEY_UP) {
			if (set_sel > 0)
				set_sel--;
		} else if (ev.key == KEY_DOWN) {
			if (set_sel < set_phr_n - 1)
				set_sel++;
		} else if (ev.key == KEY_EXE) {
			if (set_phr_n > 0)
				set_insert(set_phrs[set_sel], set_phr_len[set_sel]);
		} else if (ev.key == KEY_F1) {
			status_msg[0] = 0;
			phr_buf_n = 0;
			open_keyboard(3, PAGE_PINYIN);
		} else if (ev.key == KEY_F2) {
			if (set_phr_n > 0) {
				for (int i = set_sel; i + 1 < set_phr_n; i++) {
					for (int k = 0; k < SET_PHR_LEN; k++)
						set_phrs[i][k] = set_phrs[i + 1][k];
					set_phr_len[i] = set_phr_len[i + 1];
				}
				set_phr_n--;
				if (set_sel >= set_phr_n && set_sel > 0)
					set_sel--;
				py_cache_lists();
				ime_dirty = 1;
			}
		}
		return;
	}
}

/*=== search ===============================================================*/

static void build_search_cells(void)
{
	search_len = 0;
	for (int i = 0; i < kb_len && search_len < 31; i++)
		search_cells[search_len++] = kb_cells[i];
}

/* A hit is packed as (window << HIT_SHIFT) | cell, so one list can cover the
   whole document while only one window is in memory.  text_max never exceeds
   32768, so a window-local cell index fits into HIT_SHIFT bits. */
#define HIT_SHIFT 15
#define HIT_MASK  0x7FFF

static int hit_chunk(int hit) { return hit >> HIT_SHIFT; }
static int hit_cell(int hit)  { return hit & HIT_MASK; }

/* Append every occurrence found in the window that is loaded now. */
static void search_collect(void)
{
	if (!search_len || !ncell)
		return;
	for (int i = 0; i + search_len <= ncell && search_n < SEARCH_MAX_HITS; i++) {
		int k = 0;
		while (k < search_len && cells[i + k] == search_cells[k])
			k++;
		if (k != search_len)
			continue;
		search_hits[search_n++] = (chunk_idx << HIT_SHIFT) | (i & HIT_MASK);
	}
}

/* Show match #search_cur and put the caret right behind it. */
static void search_show(void)
{
	if (!search_n)
		return;
	int hit = search_hits[search_cur];
	int c = hit_chunk(hit);
	int cell = hit_cell(hit);
	if (c != chunk_idx || ncell == 0)
		chunk_load(c);
	if (cell > ncell - 1)
		cell = ncell > 0 ? ncell - 1 : 0;
	cur_line = offset_to_line(cell);
	cur_col = cell - line_start(cur_line) + search_len;
	int llen = line_len(line_start(cur_line));
	if (cur_col > llen)
		cur_col = llen;
	view_top = row_of_cell(cell);
	if (view_top > row_count() - 1)
		view_top = row_count() - 1;
	if (view_top < 0)
		view_top = 0;
	hscroll = 0;
	snprintf(status_msg, sizeof status_msg, "\xC3\xFC\xD6\xD0%d %d/%d",
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

/* Walk every window of the document, so a match near the end is found even
   when the reader is at the top.  The window the reader was in is put back
   afterwards (search_show then opens the window of the match). */
static void run_search(void)
{
	int here_chunk = chunk_idx;
	int here = line_start(cur_line) + cur_col;
	int from = 0;
	int save_idx = chunk_idx, save_top = view_top;

	search_n = 0;
	search_cur = 0;
	if (!search_len)
		return;
	for (int c = 0; c < chunk_n && search_n < SEARCH_MAX_HITS; c++) {
		if (c != chunk_idx)
			chunk_load(c);
		search_collect();
	}
	if (!search_n) {
		if (chunk_idx != save_idx)
			chunk_load(save_idx);
		view_top = save_top;
		snprintf(status_msg, sizeof status_msg, "\xC3\xBB\xD5\xD2\xB5\xBD");
		return;
	}
	/* the first match behind the caret, wrapping to the first one */
	for (int i = 0; i < search_n; i++) {
		int hc = hit_chunk(search_hits[i]), hl = hit_cell(search_hits[i]);
		if (hc > here_chunk || (hc == here_chunk && hl > here)) {
			from = i;
			break;
		}
	}
	search_cur = from;
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

static void book_load(void)
{
	for (int i = 0; i < 6; i++)
		book_v[i] = 0;
	if (!pos_ready() || name_count <= 0 || list_sel >= name_count
	    || is_dir[list_sel])
		return;
	int k = pos_upsert(names[list_sel]);
	for (int i = 0; i < 6; i++) {
		int v = pos_tab[k].bm[i];
		/* the record holds row + 1, 0 means "never stored"; keep the
		   stored value so the page can print it as the row number */
		book_v[i] = (v >= 1 && v < 200001) ? v : 0;
	}
}

static void draw_book(void)
{
	ui_clip_x = 128;
	ui_no_cups = 0;
	for (int i = 0; i < 6; i++)
		box_lab[i] = NULL;
	box_lab[0] = "\xB4\xE6";
	box_lab[1] = "\xB6\xC1\xC8\xA1";
	box_lab[5] = "\xC9\xBE\xB3\xFD";
	char binfo[16] = { 0 };
	{
		/* the page (or line) the reader was on, in the unit the counter
		   uses -- bug: the slots used to be rows while the title said 行 */
		char head[24];
		int here = set_unit_lines
			? chunk_rows + view_top + 1
			: (chunk_rows + view_top) / rows() + 1;
		int tot = set_unit_lines ? total_rows : page_total();
		if (tot < 1) tot = 1;
		if (here < 1) here = 1;
		if (here > tot) here = tot;
		snprintf(head, sizeof head, "%s", "\xCA\xE9\xC7\xA9");
		snprintf(binfo, sizeof binfo, "%d/%d", here, tot);
		ui_title(head);
	}
	for (int i = 0; i < 6; i++) {
		int col = i % 2, row = i / 2;
		int p = book_v[i];
		char line[24];
		int v = p > 0 ? (set_unit_lines ? p : (p - 1) / rows() + 1) : 0;
		if (p > 0)
			snprintf(line, sizeof line, "%d:%d", i + 1, v);
		else
			snprintf(line, sizeof line, "%d:-", i + 1);
		draw_ui(col ? 68 : 6, CONTENT_Y + row * 12, line, i == book_sel);
	}
	if (book_del_ask)
		draw_ui(2, CONTENT_Y + 36, "\xC8\xB7\xC8\xCF\xC9\xBE\xB3\xFD\x3F", 0);
	/* no hint line: the F bar already says what the keys do */
	ui_info_right = 1;
	ui_hints("", binfo, 0);		/* counter on the right + the F boxes */
	ui_info_right = 0;
	dupdate();		/* the loop draws before it waits for a key */
}

static void handle_book_key(key_event_t ev)
{
	int k = (pos_ready() && name_count > 0 && list_sel < name_count
		 && !is_dir[list_sel]) ? pos_upsert(names[list_sel]) : -1;

	if (ev.key == KEY_EXIT) {
		book_del_ask = 0;
		mode = MODE_VIEW;
		return;
	}
	if (ev.key == KEY_LEFT) {
		if (book_sel > 0) book_sel--;
		book_del_ask = 0;
	} else if (ev.key == KEY_RIGHT) {
		if (book_sel < 5) book_sel++;
		book_del_ask = 0;
	} else if (ev.key == KEY_UP) {
		if (book_sel >= 2) book_sel -= 2;
		book_del_ask = 0;
	} else if (ev.key == KEY_DOWN) {
		if (book_sel <= 3) book_sel += 2;
		book_del_ask = 0;
	} else if (ev.key == KEY_F1) {
		if (k >= 0) {
			pos_tab[k].bm[book_sel] = book_line;	/* already row + 1 */
			book_load();
		}
		book_del_ask = 0;
	} else if (ev.key == KEY_F2) {
		int p = book_v[book_sel];
		if (p > 0) {
			/* p is a document-wide page: any window can be reached */
			goto_row(p - 1);
			mode = MODE_VIEW;
			return;
		}
		book_del_ask = 0;
	} else if (ev.key == KEY_EXE) {
		/* buglist2 18: filled slot loads, empty slot stores */
		int p = book_v[book_sel];
		if (p > 0) {
			goto_row(p - 1);
			mode = MODE_VIEW;
			return;
		}
		if (k >= 0) {
			pos_tab[k].bm[book_sel] = book_line;
			book_load();
		}
		book_del_ask = 0;
	} else if (ev.key == KEY_F6) {
		book_ask_sel = book_sel;
		confirm_kind = 2;
		mode = MODE_CONFIRM;
		return;
	}
	/* the loop only repaints on its own events, so paint here */
	dclear(PAPER);
	draw_book();
	dupdate();
}

/* Caret on the row the screen starts with, then edit mode on. */
static void enter_edit(void)
{
	int rp, base, len, col;

	edit_mode = 1;
	fbar_page = 0;
	/* view_top counts display rows but cur_line counts logical lines:
	   convert, otherwise paging N times moves the caret N*wrap lines
	   further down. */
	rp = row_start(view_top);
	cur_line = offset_to_line(rp);
	if (cur_line > line_count() - 1)
		cur_line = line_count() - 1;
	/* first cell of that logical line as shown on this page, caret one
	   character further right */
	base = line_start(cur_line);
	len = line_len(base);
	col = rp - base;
	if (col < 0)
		col = 0;
	if (col > len)
		col = len;
	cur_col = (col < len) ? col + 1 : len;
}

/* Leave edit mode; unsaved text asks before it is thrown away. */
static void leave_edit(void)
{
	if (dirty) {
		leave_to_list = 0;
		confirm_kind = 1;
		mode = MODE_CONFIRM;
		return;
	}
	edit_mode = 0;
	fbar_page = 0;
	py_reset();
}

/* Reading / editing page.  Every F key resolves through fk_act[], so a key
   can only ever do what its label says. */
static void handle_view_key(key_event_t ev)
{
	int is_f = (ev.key >= KEY_F1 && ev.key <= KEY_F6);

	/* SHIFT / ALPHA + F1..F6 in the reader: store or recall a bookmark */
	if (!edit_mode && is_f && (ev.shift || ev.alpha)) {
		int slot = (int)(ev.key - KEY_F1) + 1;
		if (search_n == 0 && pos_ready() && name_count > 0
		    && list_sel < name_count && !is_dir[list_sel]) {
			int k = pos_upsert(names[list_sel]);
			int page = global_row() + 1;	/* document-wide row */
			if (ev.shift) {
				pos_tab[k].bm[slot - 1] = page;
			} else {
				int p = pos_tab[k].bm[slot - 1];
				if (p > 0)
					goto_row(p - 1);
			}
		}
		return;
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
				{
					uint16_t cc[SET_PHR_LEN];
					int cl = py_candidate_cells(py_pick, cc);
					for (int k = 0; k < cl; k++)
						text_insert(cc[k]);
				}
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

	if (is_f) {
		int st = fk_state();
		if (edit_mode && (ev.shift || ev.alpha)) {
			/* SHIFT+F5 is the letter lock; nothing else is bound */
			if (ev.shift && ev.key == KEY_F5)
				alpha_lock = !alpha_lock;
			return;
		}
		switch (fk_act[st][ev.key - KEY_F1]) {
		case A_EDIT:
			enter_edit();
			return;
		case A_FIND:
			mode = MODE_SEARCH;
			py_reset();
			return;
		case A_BOOK:
			book_sel = 0;
			book_load();
			book_line = global_row() + 1;
			mode = MODE_BOOK;
			return;
		case A_HELP:
			help_page = 0;	/* buglist2 15: always the first page */
			help_back = MODE_VIEW;
			mode = MODE_HELP;
			return;
		case A_HIT_PREV:
			if (search_n > 0) {
				search_cur = (search_cur + search_n - 1) % search_n;
				search_show();
			}
			return;
		case A_HIT_NEXT:
			if (search_n > 0) {
				search_cur = (search_cur + 1) % search_n;
				search_show();
			}
			return;
		case A_HIT_QUERY:
			help_page = 3;
			mode = MODE_SEARCH;
			return;
		case A_HIT_CLOSE:
			search_n = 0;
			search_len = 0;
			return;
		case A_GOTO:
			open_goto();
			return;
		case A_SET:
			open_settings();
			return;
		case A_SAVE:
			strncpy(relist_name, names[list_sel], NAME_MAX_L);
			relist_name[NAME_MAX_L] = 0;
			gint_world_switch(GINT_CALL(fs_write_current));
			dirty = 0;
			gint_world_switch(GINT_CALL(fs_relist_keep));
			return;
		case A_LEAVE:
			leave_edit();
			return;
		case A_CAPS:
			caps_on = !caps_on;
			return;
		case A_PUNCT:
			open_keyboard(0, PAGE_PUNCT);
			return;
		case A_CN:
			cn_mode = !cn_mode;
			py_reset();
			return;
		case A_PAGE:
			fbar_page = fbar_page ? 0 : 1;
			return;
		case A_UNDO:
			undo_apply();
			return;
		default:			/* A_NONE: a blank key does nothing */
			return;
		}
	}

	/* up / down walk the matches while a result list is on the screen */
	if (!edit_mode && search_n > 0 &&
	    (ev.key == KEY_UP || ev.key == KEY_DOWN)) {
		search_cur = (search_cur +
		              (ev.key == KEY_DOWN ? 1 : search_n - 1)) % search_n;
		search_show();
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
		if (!c && !ev.alpha && ev.shift) {
			switch (ev.key) {
			case KEY_MUL: c = '{'; break;
			case KEY_DIV: c = '}'; break;
			case KEY_ADD: c = '['; break;
			case KEY_SUB: c = ']'; break;
			default: break;
			}
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
				if (c == '"' && cn_mode)
					cell = quote_cell();	/* “ ” instead of " */
				text_insert(cell);
			}
			return;
		}
	}

	switch (ev.key) {
	case KEY_EXIT:
		if (edit_mode && dirty) {
			leave_to_list = ev.shift;
			confirm_kind = 1;
			mode = MODE_CONFIRM;
			return;
		}
		if (edit_mode && !ev.shift) {
			/* buglist2 17: EXIT leaves the editor but stays in the
			   reader, on the same page */
			edit_mode = 0;
			fbar_page = 0;
			py_reset();
			return;
		}
		/* SHIFT+EXIT, or the reader itself: back to the list */
		edit_mode = 0;
		fbar_page = 0;
		search_n = 0;
		search_len = 0;
		py_reset();
		gint_world_switch(GINT_CALL(fs_save_state));
		mode = MODE_LIST;
		return;
	case KEY_UP:
		if (edit_mode)
			move_cursor_row(-1);
		else
			page_step(-1);
		return;
	case KEY_DOWN:
		if (edit_mode)
			move_cursor_row(1);
		else
			page_step(1);
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
		if (edit_mode) {
			text_newline();
		totals_gen = -1;	/* buglist2 25: the new row must show */		/* 换行 = EXE */
		} else {
			help_page = 1;
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
	uint16_t nb[SET_PHR_LEN];
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
	case KEY_UP:
	case KEY_DOWN:
		if (punct) {
			punct_move(ev.key == KEY_DOWN ? 1 : -1);
			return;
		}
		if (ev.key == KEY_UP) {
			if (kb_row > 0) kb_row--;
		} else if (kb_row < gr - 1) {
			kb_row++;
		}
		return;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (punct) {
			punct_side(ev.key == KEY_RIGHT ? 1 : -1);
			return;
		}
		if (pinyin && py_len > 0) {
			int n = py_candidates();
			if (n > 0) {
				py_pick += (ev.key == KEY_RIGHT) ? 1 : -1;
				if (py_pick < 0)
					py_pick = n - 1;
				if (py_pick >= n)
					py_pick = 0;
			}
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
				{
					uint16_t cc[SET_PHR_LEN];
					int cl = py_candidate_cells(py_pick, cc);
					for (int k = 0; k < cl; k++)
						kb_put(cc[k]);
				}
				py_reset();
				if (kb_target == 2)	/* back to the list, one cell added */
					kb_back();
			}
		}
		return;
	case KEY_EXE:
		if (punct) {
			int pick = punct_live_at(punct_pos());
			uint16_t pc = pick >= 0 ? punct_cell(pick) : 0;
			if (pc) {
				kb_put(pc);
				if (kb_target == 0 || kb_target == 2)
					kb_back();
			}
			return;
		}
		if (pinyin && py_len > 0) {
			int n = py_candidates();
			if (n > 0 && py_candidate_cells(py_pick, nb) > 0) {
				int cl = py_candidate_cells(py_pick, nb);
				for (int i = 0; i < cl; i++)
					kb_put(nb[i]);
				py_reset();
				if (kb_target == 0 || kb_target == 2)
					kb_back();
				return;
			}
		}
		if (pinyin) {
			if (ch && py_len < 7) {
				py_buf[py_len++] = ch;
				py_buf[py_len] = 0;
				py_refresh();
			}
		} else if (ch) {
			kb_type(ch);
			if (kb_target == 0 || kb_target == 2)
				kb_back();
		}
		return;
	case KEY_F6:
	case KEY_EXIT:
		if (kb_target == 1) {
			run_search();
			kb_target = 0;
		} else if (kb_target == 3) {
			set_phrase_commit();
		}
		kb_back();
		return;
	default:
		return;
	}
}

static void fs_delete_selected(void);

/* A document's .POS sidecar has to go with it, otherwise the folder fills up
   with orphans for files that no longer exist. */
static void fs_delete_pos_for(char const *name)
{
	char pn[NAME_MAX_L + 6];
	size_t n = strlen(name);
	char *dot;

	if (n > NAME_MAX_L)
		n = NAME_MAX_L;
	memcpy(pn, name, n);
	pn[n] = 0;
	dot = strrchr(pn, '.');
	if (dot && dot > pn && !strchr(dot, '\\'))
		strcpy(dot, ".POS");
	else
		strcpy(pn + n, ".POS");
	fs_delete_path(pn);
	if (strcmp(cur_dir, "\\\\fls0") != 0) {
		char full[NAME_MAX_L + 40];
		snprintf(full, sizeof full, "%s\\%s", cur_dir, pn);
		fs_delete_path(full);
	}
}

static void fs_delete_selected(void)
{
	char name[NAME_MAX_L + 1];
	uint16_t fc[NAME_FC_MAX];
	strncpy(name, del_name, NAME_MAX_L);
	name[NAME_MAX_L] = 0;
	memcpy(fc, names_fc[list_sel], sizeof fc);
	if (BFile_Remove(fc) != 0)
		fs_delete_path(name);
	fs_delete_pos_for(name);
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
	if (confirm_kind == 2) {
		ui_title("\xC9\xBE\xB3\xFD\xCA\xE9\xC7\xA9");	/* 删除书签 */
		char tmp[24];
		snprintf(tmp, sizeof tmp, "%d", book_ask_sel + 1);
		draw_ui(2, CONTENT_Y + 4, "\xC8\xB7\xB6\xA8\xC9\xBE\xB3\xFD?", 0);
		draw_ui(2, CONTENT_Y + 16, tmp, 0);
		draw_ui(2, STATUS_Y, "\x45\x58\x45\x20\xC9\xBE\xB3\xFD\x20\x20\x20\x45\x58\x49\x54\x20\xC8\xA1\xCF\xFB", 0);
		ui_hints("", "", 0);
		dupdate();
		return;
	}
	if (confirm_kind == 1) {
		ui_no_cups = 0;
		box_lab[0] = "\xBC\xCC\xD0\xF8";	/* 继续 */
		box_lab[1] = "\xB4\xE6\xCD\xCB";	/* 存退 */
		box_lab[5] = "\xB7\xC5\xC6\xFA";	/* 放弃 */
		ui_title("\xCE\xB4\xB1\xA3\xB4\xE6");
		draw_ui(2, CONTENT_Y + 4,
		        leave_to_list ? "\xB7\xC5\xC6\xFA\xD0\xDE\xB8\xC4\xB2\xA2\xCD\xCB\xB3\xF6\xCE\xC4\xB5\xB5\xA3\xBF"
		                      : "\xB7\xC5\xC6\xFA\xD0\xDE\xB8\xC4\xB2\xA2\xBB\xD8\xB5\xBD\xD4\xC4\xB6\xC1\xC4\xA3\xCA\xBD\xA3\xBF", 0);
		ui_status("");		/* the F1 / F2 / F6 cups */
		dupdate();
		return;
	}
	ui_title("\xC9\xBE\xB3\xFD\xCE\xC4\xBC\xFE");
	int y = CONTENT_Y + 4;
	draw_ui(2, y, "\xC8\xB7\xB6\xA8\xC9\xBE\xB3\xFD\xA3\xBF", 0);
	draw_fname(2, y + 12, del_name, 0);
	draw_ui(2, STATUS_Y, "\x45\x58\x45\x20\xC9\xBE\xB3\xFD\x20\x20\x20\x45\x58\x49\x54\x20\xC8\xA1\xCF\xFB", 0);
	ui_hints("", "", 0);
	dupdate();
}

static void handle_confirm_key(key_event_t ev)
{
	if (confirm_kind == 2) {
		/* only EXE and EXIT answer this page: any other key used to drop
		   straight back to the bookmark list, which made it feel like the
		   page vanished by itself */
		if (ev.key == KEY_EXE && pos_ready() && name_count > 0
		    && list_sel < name_count && !is_dir[list_sel]) {
			int k = pos_upsert(names[list_sel]);
			pos_tab[k].bm[book_ask_sel] = 0;
			book_load();
			mode = MODE_BOOK;
		} else if (ev.key == KEY_EXIT) {
			mode = MODE_BOOK;
		}
		return;
	}
	if (confirm_kind == 1) {
		/* buglist2 9 / 10: F1 keeps editing, F2 saves and leaves,
		   F6 (or EXE / EXIT) throws the changes away */
		if (ev.key == KEY_F1 || ev.key == KEY_EXE || ev.key == KEY_EXIT) {
			mode = MODE_VIEW;	/* straight back into the editor */
			return;
		}
		if (ev.key == KEY_F2) {
			strncpy(relist_name, names[list_sel], NAME_MAX_L);
			relist_name[NAME_MAX_L] = 0;
			gint_world_switch(GINT_CALL(fs_write_current));
			dirty = 0;
			gint_world_switch(GINT_CALL(fs_relist_keep));
		} else if (ev.key != KEY_F6) {
			return;		/* any other key stays on this page */
		} else {
			dirty = 0;
			gint_world_switch(GINT_CALL(fs_read_current));
		}
		edit_mode = 0;
		fbar_page = 0;
		py_reset();
		if (leave_to_list) {
			gint_world_switch(GINT_CALL(fs_save_state));
			mode = MODE_LIST;
		} else {
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
		name_pos = name_len;	/* the caret waits after the default name */
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
		open_settings();
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
		fbar_page = 0;
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
		   are still allocated (the add-in image is kept), keep them, but
		   start the listing at the root folder again */
		/* stay in the folder that was open when the menu key was hit */
		if (name_count == 0 && cur_dir[0])
			gint_world_switch(GINT_CALL(fs_list_dir));
		list_sel = 0;
		list_top = 0;
		/* always come back to the list, never into the document that
		   happened to be open when the menu key was pressed */
		edit_mode = 0;
		search_n = 0;
		search_len = 0;
		py_reset();
		mode = MODE_LIST;
		goto have_memory;
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
		dclear(PAPER);
		dtext(2, 2, INKBAR, "out of memory");
		dupdate();
		getkey();
		return 1;
	}

have_memory:
	/* The volume name survives a menu exit (the image is kept), so only probe
	   when we really do not know it yet. The listing is always rebuilt because
	   the buffers are released when we hand control back to the OS. */
	if (!root_dev[0]) {
		/* fs_probe_root() resets cur_dir to the volume root, so the saved
		   folder has to be put back *after* it. Both the folder and the
		   per-document position are keyed by cur_dir: losing the folder
		   makes the history look empty. */
		gint_world_switch(GINT_CALL(fs_probe_root));
		fs_restore();
		normalise_cur_dir();
	} else {
		normalise_cur_dir();
		/* the static image survives a menu round trip, but the position
		   table only exists after a read: without this the history and
		   the bookmarks look empty after every re-entry */
		fs_restore();
		normalise_cur_dir();
	}
	if (!cur_dir[0])
		snprintf(cur_dir, sizeof cur_dir, "%s", root_dev);

	if (name_count == 0)
		gint_world_switch(GINT_CALL(fs_list_dir));

	/* Starting up or coming back from the menu both land on the root folder:
	   the document that was open is gone, and its position is remembered
	   separately (FXTXT.CFG) for the next time it is opened. */
	if (reentry || mode != MODE_LIST) {
		gint_world_switch(GINT_CALL(fs_root_again));
		fs_restore();			/* keep the state file readable, ignore the result */
		list_sel = list_top = 0;
		mode = MODE_LIST;
		edit_mode = 0;
		search_n = 0;
		search_len = 0;
		ncell = 0;
		cur_line = 0;
		cur_col = 0;
		view_top = 0;
		hscroll = 0;
		py_reset();
	}
	if (list_sel >= name_count)
		list_sel = list_top = 0;
	if (list_top > list_sel)
		list_top = list_sel;
	if (!rec_buf)
		rec_buf = malloc((size_t)RECENT_MAX * RECENT_LEN);
	set_defaults();		/* frequent characters */
	cfg_load();		/* settings come from \\fls0\\FXTXT.CFG */
	ime_load();		/* word list from \\fls0\\FXTXT.IME_1 */
	ui_inv = set_inv;
	app_ready = 1;

	for (;;) {
		dclear(PAPER);
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
		else if (mode == MODE_BOOK)
			draw_book();
		else if (mode == MODE_GOTO)
			draw_goto();
		else if (mode == MODE_SET)
			draw_set();
		else if (mode == MODE_SET_INV)
			draw_set_inv();
		else if (mode == MODE_SET_UNIT)
			draw_set_unit();
		else if (mode == MODE_SET_IME)
			draw_set_ime();
		else if (mode == MODE_SET_CHARS)
			draw_set_chars();
		else if (mode == MODE_SET_PHR)
			draw_set_phr();
		else if (mode == MODE_SET_ABOUT)
			draw_set_about();
		else
			draw_keyboard();

		key_event_t ev = next_event();
		if (ev.key == 0)
			continue;	/* idle / blink tick: repaint, do not dispatch */
		status_sticky = 0;

		if (ev.key == KEY_ACON && ev.shift) {
			/* SHIFT+AC/ON powers off; a bare AC/ON does nothing */
			gint_world_switch(GINT_CALL(fs_save_state));
			app_release();
			gint_poweroff(1);
			break;
		}
		if (ev.key == KEY_MENU && ev.shift) {
			/* buglist2 12: SHIFT+MENU is the shortcut to the settings */
			alpha_lock = 0;
			shift_armed = 0;
			alpha_armed = 0;
			py_reset();
			open_settings();
			continue;
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
		} else if (mode == MODE_BOOK) {
			handle_book_key(ev);
		} else if (mode == MODE_GOTO) {
			handle_goto_key(ev);
		} else if (mode >= MODE_SET && mode <= MODE_SET_ABOUT) {
			handle_set_key(ev);
} else if (mode == MODE_HELP) {
			if (!ev_repeat && ev.key == KEY_LEFT)
				help_page = (help_page + 4) % 5;
			else if (!ev_repeat && ev.key == KEY_RIGHT)
				help_page = (help_page + 1) % 5;
			else if (ev.key == KEY_EXIT || ev.key == KEY_EXE)
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

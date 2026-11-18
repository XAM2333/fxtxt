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
       MODE_DEBUG };
enum { PAGE_PINYIN = 0, PAGE_PUNCT, PAGE_LOWER, PAGE_UPPER, PAGE_DIGIT, PAGE_SYMBOL };
#define PAGE_COUNT 6

static int utf8_seq(unsigned char const *s, int n, int i, unsigned *cp);
static int gb_lookup(unsigned short uni);

/* Chinese punctuation, one UTF-8 sequence per grid cell (13 x 2). */
static char const *punct_list =
	"，。、？！：；“”‘’（）"
	"《》〈〉…—·「」『』【】〔〕";

/* Byte offset of the n-th UTF-8 character in punct_list. */
static int punct_offset(int n)
{
	unsigned cp = 0;
	int len = (int)strlen(punct_list), i = 0;
	while (i < len && n-- > 0)
		i += utf8_seq((unsigned char const *)punct_list, len, i, &cp);
	return i;
}

static uint16_t *cells;
static int ncell;
static unsigned char *rawbuf;

static int edit_mode;
static int alpha_lock;		/* sticky letter mode (SHIFT+ALPHA) */
static int caps_on;		/* F6: upper case while typing */
static int case_upper;
static int shift_armed;

static int alpha_armed;		/* capitals (name prompt F1) */
static int cn_mode = 1;		/* 1 = pinyin, 0 = direct letters */
static int dirty;
static int enc_utf8;

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
static int gb_lookup(unsigned short uni);
static int utf8_seq(unsigned char const *s, int n, int i, unsigned *cp);
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
	return 8;			/* CJK glyphs are full width */
}

/* Chrome text may be Chinese: decode UTF-8 and draw with the small font. */
static int ui_clip_x = 128;

static int draw_ui(int x, int y, char const *s, int invert)
{
	unsigned char const *p = (unsigned char const *)s;
	int n = (int)strlen(s), i = 0;
	while (i < n && x < ui_clip_x) {
		unsigned cp = 0;
		int used = utf8_seq(p, n, i, &cp);
		uint16_t cell;
		if (cp < 0x80) {
			cell = (uint16_t)(cp - 0x20);
		} else {
			int g = gb_lookup((unsigned short)cp);
			cell = (g >= 0) ? (uint16_t)(CELL_CJK + g) : (uint16_t)('?' - 0x20);
		}
		draw_cell(x, y, cell, invert);
		x += cell_adv(cell);
		i += used;
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

/*=== window chrome ========================================================*/

/* Width in pixels of a UTF-8 string in the small font. */
static int ui_width(char const *s)
{
	unsigned char const *p = (unsigned char const *)s;
	int n = (int)strlen(s), i = 0, x = 0;
	while (i < n) {
		unsigned cp = 0;
		int used = utf8_seq(p, n, i, &cp);
		uint16_t cell;
		if (cp < 0x80) {
			cell = (uint16_t)(cp - 0x20);
		} else {
			int g = gb_lookup((unsigned short)cp);
			cell = (g >= 0) ? (uint16_t)(CELL_CJK + g) : (uint16_t)('?' - 0x20);
		}
		x += cell_adv(cell);
		i += used;
	}
	return x;
}

/* The bottom bar shows key hints; transient messages override them until the
   next key press. */
static int status_sticky;
static char const *hint_list = "F1新建 F2上次 DEL删 F6帮";
static char const *hint_view = "F2找 F3编辑 F4编码 F6帮助";
static char const *hint_edit_cn = "中 F1存 F3退 F4标 F5中英";
static char const *hint_edit_en = "英 F1存 F3退 F4标 F5中英";

/* Black square with a white letter: A = letter lock (ALPHA), S = capitals (SHIFT). */
static void ui_badge(char ch)
{
	int x = 118, y = STATUS_Y;
	drect(x, y, x + 9, y + 7, C_BLACK);
	draw_cell(x + 1, y, (uint16_t)((unsigned char)ch - 0x20), 1);
}

static void ui_hints(char const *hints, char const *info, int show_lock)
{
	(void)show_lock;
	if (info && info[0]) {
		int x = 96 - ui_width(info);
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

/* Six cups along the bottom edge (F1..F6): no bottom edge, no gaps, and each
   one carries its own short label. */
static void ui_fkeys(void)
{
	drect(0, BOX_Y, 127, BOX_Y, C_BLACK);          /* top edge */
	for (int i = 1; i < 6; i++) {
		int x = i * 21;
		drect(x, BOX_Y, x, 63, C_BLACK);          /* dividers to the bottom */
	}
	for (int i = 0; i < 6; i++) {
		char const *s = box_lab[i];
		if (!s || !s[0])
			continue;
		int w = ui_width(s);
		int x = i * 21 + (21 - w) / 2;
		if (x < i * 21 + 1)
			x = i * 21 + 1;
		draw_ui(x, STATUS_Y, s, 0);
	}
}

/* 9x9 box in the title bar: white letter on black (inverse of the bar). */
static void ui_topbox(int x, char ch)
{
	drect(x, 0, x + 8, 8, C_WHITE);
	draw_cell(x + 1, 1, (uint16_t)((unsigned char)ch - 0x20), 0);
}

/* Right side of the title bar: input mode (中/英) and SHIFT/ALPHA state. */
static void ui_top_state(int show_input)
{
	if (!show_input)
		return;			/* letter state only matters while editing */

	int x = 127;
	if (alpha_lock || alpha_armed) {
		ui_topbox(x - 8, 'A');
		x -= 10;
	}
	if (shift_armed) {
		ui_topbox(x - 8, 'S');
		x -= 10;
	}
	{
		char const *m = cn_mode ? "中" : "英";
		int g = gb_lookup((unsigned short)(cn_mode ? 0x4E2D : 0x82F1));
		int w = (g >= 0) ? 8 : 8;
		x -= (w + 1);
		drect(x, 0, x + w - 1, 8, C_WHITE);
		if (g >= 0)
			draw_cell(x, 1, (uint16_t)(CELL_CJK + g), 0);
		(void)m;
	}
}

static void ui_title_state(char const *text, int show_input)
{
	drect(0, 0, 127, TITLE_H - 1, C_BLACK);
	ui_clip_x = 62;		/* counter and state boxes live to the right */
	draw_ui(3, 1, text, 1);
	ui_clip_x = 128;
	ui_top_state(show_input);
}

static void ui_title(char const *text)
{
	ui_title_state(text, 0);
}

static void ui_status(char const *text)
{
	(void)text;			/* no toasts: the six boxes carry the hints */
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

static void icon_folder(int x, int y)
{
	drect(x + 1, y, x + 4, y, C_BLACK);
	drect(x, y + 1, x + 6, y + 1, C_BLACK);
	drect(x, y + 2, x + 6, y + 6, C_BLACK);
	drect(x + 1, y + 3, x + 5, y + 5, C_WHITE);
	drect(x + 2, y + 2, x + 3, y + 2, C_BLACK);
}

static void icon_file(int x, int y)
{
	drect(x + 1, y, x + 4, y, C_BLACK);
	drect(x, y + 1, x + 6, y + 6, C_BLACK);
	drect(x + 1, y + 1, x + 5, y + 5, C_WHITE);
	drect(x + 5, y + 1, x + 6, y + 1, C_BLACK);
	drect(x + 6, y + 2, x + 6, y + 2, C_BLACK);
	drect(x + 1, y + 3, x + 3, y + 3, C_BLACK);
	drect(x + 1, y + 5, x + 3, y + 5, C_BLACK);
}

/*=== decoding =============================================================*/

static int valid_utf8(unsigned char const *s, int n)
{
	int i = 0, multi = 0;
	while (i < n) {
		unsigned c = s[i];
		if (c < 0x80) { i++; continue; }
		int extra;
		if ((c & 0xE0) == 0xC0) extra = 1;
		else if ((c & 0xF0) == 0xE0) extra = 2;
		else if ((c & 0xF8) == 0xF0) extra = 3;
		else return 0;
		if (i + extra >= n)
			return 0;
		for (int k = 1; k <= extra; k++)
			if ((s[i + k] & 0xC0) != 0x80)
				return 0;
		multi++;
		i += extra + 1;
	}
	return multi ? 1 : 0;
}

static int gb_lookup(unsigned short uni)
{
	int lo = 0, hi = GB_MAP_COUNT - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (gb_uni[mid] == uni) return gb_idx[mid];
		if (gb_uni[mid] < uni) lo = mid + 1;
		else hi = mid - 1;
	}
	return -1;
}

static int utf8_seq(unsigned char const *s, int n, int i, unsigned *cp)
{
	unsigned c = s[i];
	int extra;
	if (c < 0x80) { *cp = c; return 1; }
	if ((c & 0xE0) == 0xC0) { *cp = c & 0x1F; extra = 1; }
	else if ((c & 0xF0) == 0xE0) { *cp = c & 0x0F; extra = 2; }
	else if ((c & 0xF8) == 0xF0) { *cp = c & 0x07; extra = 3; }
	else { *cp = '?'; return 1; }
	if (i + extra >= n) { *cp = '?'; return 1; }
	for (int k = 1; k <= extra; k++)
		*cp = (*cp << 6) | (s[i + k] & 0x3F);
	return extra + 1;
}

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
		if (enc_utf8) {
			unsigned cp = 0;
			int used = utf8_seq(s, n, i, &cp);
			if (cp < 0x80) {
				cells[ncell++] = (uint16_t)(cp - 0x20);
			} else {
				int gi = gb_lookup((unsigned short)cp);
				cells[ncell++] = gi >= 0 ? (uint16_t)(CELL_CJK + gi) : 0;
			}
			i += used;
			continue;
		}
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
		if (enc_utf8) {
			unsigned cp = (g >= 0 && g < FONT_GLYPHS) ? gb_rev[g] : 0;
			if (!cp) { out[n++] = '?'; continue; }
			if (cp < 0x800) {
				if (n + 1 >= max) break;
				out[n++] = 0xC0 | (cp >> 6);
				out[n++] = 0x80 | (cp & 0x3F);
			} else {
				if (n + 2 >= max) break;
				out[n++] = 0xE0 | (cp >> 12);
				out[n++] = 0x80 | ((cp >> 6) & 0x3F);
				out[n++] = 0x80 | (cp & 0x3F);
			}
		} else {
			int qu = g / 94 + 1, wei = g % 94 + 1;
			if (n + 1 >= max) break;
			out[n++] = (unsigned char)(qu + 0xA0);
			out[n++] = (unsigned char)(wei + 0xA0);
		}
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
static void build_paths(void);
static void fs_read_current(void);


#define STATE_PATH "\\\\fls0\\FXTXT.CFG"
#define STATE_MAGIC "FXT1"

typedef struct {
	char magic[4];
	unsigned char version;
	unsigned char big_font;
	unsigned char has_file;
	unsigned char reserved;
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
		return;
	fwrite(&st, 1, sizeof st, f);
	fclose(f);
}

static void fill_state(void)
{
	memcpy(st.magic, STATE_MAGIC, 4);
	st.version = 1;
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

/* Copy a raw path into a byte string (units above 0xFF become '?'). */
static void fc_to_bytes(uint16_t const *fc, char *out, int outsz)
{
	int i = 0;
	for (; fc[i] && i < outsz - 1; i++)
		out[i] = (fc[i] < 0x100) ? (char)fc[i] : '?';
	out[i] = 0;
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
static char const *root_candidates[] = { "/fls0", "fls0", "/", "\\\\fls0", NULL };

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
	root_dev[0] = 0;
	/* The filesystem may only be mounted once it is first touched, so probe
	   several spellings a few times before giving up. */
	for (int attempt = 0; attempt < 3 && !root_dev[0]; attempt++) {
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
static void to_fc_pattern(uint16_t *dst, int max)
{
	int j = 0;
	int i = 0;
	dst[j++] = '\\';
	dst[j++] = '\\';
	if (cur_dir[0] == '/')
		i = 1;
	for (; cur_dir[i] && j < max - 4; i++) {
		unsigned char c = (unsigned char)cur_dir[i];
		dst[j++] = (c == '/') ? '\\' : c;
	}
	dst[j++] = '\\';
	dst[j++] = '*';
	dst[j] = 0;
}

/* List the current folder through the OS file API. This is what the calculator
   itself uses and it works before gint's stdio layer has been touched. */
static void fs_list_dir_depth(int depth);

static void fs_list_dir(void)
{
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
		int k = 0;
		for (int i = last + 1; found[i] && k <= NAME_MAX_L; i++)
			base[k++] = (found[i] < 0x100) ? (char)found[i] : '?';
		base[k] = 0;
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

	/* Drop the bogus volume row, then merge in whatever opendir can see:
	   the search API is the only one reporting non-ASCII names, opendir is
	   the more reliable one for plain files. */
	int real = name_count - first_real;
	int degenerate = 1;		/* always merge opendir results */
	for (int i = first_real; i < name_count; i++)
		if (is_dir[i] && !strcmp(names[i], "fls0"))
			degenerate = 1;

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
	snprintf(status_msg, sizeof status_msg, "%d 个", name_count - first_real);
}

static void fs_enter_dir(void)
{
	char next[64];
	if (strlen(cur_dir) + strlen(names[list_sel]) + 2 > sizeof next) {
		snprintf(status_msg, sizeof status_msg, "路径太长");
		return;
	}
	snprintf(next, sizeof next, "%s/%s", cur_dir, names[list_sel]);
	strncpy(cur_dir, next, sizeof cur_dir - 1);
	cur_dir[sizeof cur_dir - 1] = 0;
	fs_list_dir();
}

static void fs_leave_dir(void)
{
	char *slash = strrchr(cur_dir, '/');
	if (slash && slash != cur_dir)
		*slash = 0;
	if (!cur_dir[0] || !strchr(cur_dir, '/'))
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
static char name_buf[16];
static int name_len;
static int create_ok;

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
	for (int i = 0; name_buf[i] && n < 12; i++) {
		char c = name_buf[i];
		if (!name_char_ok(c))
			continue;
		clean[n++] = c;
	}
	clean[n] = 0;
	if (!n) {
		snprintf(status_msg, sizeof status_msg, "名字不能为空");
		return;
	}
	snprintf(name, sizeof name, "%s.TXT", clean);
	snprintf(path, sizeof path, "%s/%s", cur_dir, name);

	FILE *f = open_any(path, "rb");
	if (f) {
		fclose(f);
		snprintf(status_msg, sizeof status_msg, "已存在同名文件");
		return;
	}
	f = open_any(path, "wb");
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "新建失败");
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
		snprintf(status_msg, sizeof status_msg, "新建失败");
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
	enc_utf8 = 0;
	py_reset();
	snprintf(status_msg, sizeof status_msg, "新建 %s", name);
	status_sticky = 1;
	create_ok = 1;
}

static void draw_name(void)
{
	ui_title("新建文档");
	int y = CONTENT_Y + 2;
	char line[64];
	snprintf(line, sizeof line, "目录 %s", cur_dir);
	draw_ui(2, y, line, 0);
	y += 10;
	draw_ui(2, y, "名称", 0);
	int x = draw_ui(28, y, name_buf, 0);
	for (int k = 0; k < 7; k++)
		px_on(x + 1, y + k);
	y += 10;
	draw_ui(2, y, case_upper ? "大写 F1切换  自动加 .TXT"
	                        : "小写 F1切换  自动加 .TXT", 0);
	ui_hints("ALPH字母 EXE建 EXIT退", "", 1);
	dupdate();
}

static void handle_name_key(key_event_t ev)
{

	if (ev.key == KEY_F1) {
		case_upper = !case_upper;
		snprintf(status_msg, sizeof status_msg,
		         case_upper ? "大写字母" : "小写字母");
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
			snprintf(status_msg, sizeof status_msg, "该符号不能做文件名");
			status_sticky = 1;
			return;
		}
		if (name_len < (int)sizeof name_buf - 1) {
			name_buf[name_len++] = c;
			name_buf[name_len] = 0;
		}
		return;
	}

	switch (ev.key) {
	case KEY_DEL:
	case KEY_F2:
		if (name_len > 0)
			name_buf[--name_len] = 0;
		return;
	case KEY_EXE:
		gint_world_switch(GINT_CALL(name_commit));
		if (create_ok)
			mode = MODE_VIEW;
		return;
	case KEY_EXIT:
		mode = MODE_LIST;
		return;
	default:
		return;
	}
}

static void fs_read_current(void)
{
	ncell = 0;
	FILE *f = open_read();
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "打不开");
		return;
	}
	int n = (int)fread(rawbuf, 1, text_max, f);
	int clipped = (n == text_max) && (fgetc(f) != EOF);
	fclose(f);
	if (n < 0) n = 0;
	enc_utf8 = valid_utf8(rawbuf, n);
	decode_into_cells(rawbuf, n);
	if (clipped)
		snprintf(status_msg, sizeof status_msg, "超出%dK", text_max / 1024);
	else
		snprintf(status_msg, sizeof status_msg, "%d字节 %s", n, enc_utf8 ? "UTF8" : "GB");
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
	if (cur_line > line_count() - 1)
		cur_line = line_count() - 1;
	if (cur_line < 0)
		cur_line = 0;
	snprintf(status_msg, sizeof status_msg, "%d B %s", n, enc_utf8 ? "UTF8" : "GB");
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
	FILE *f = open_write();
	if (!f) {
		snprintf(status_msg, sizeof status_msg, "存不了");
		return;
	}
	fwrite(rawbuf, 1, n, f);
	fclose(f);
	dirty = 0;
	snprintf(status_msg, sizeof status_msg, "已存 %d 字节", n);
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
	snprintf(status_msg, sizeof status_msg, "示例已建好");
	fs_list_dir();
}

/*=== list view ============================================================*/

static void draw_list(void)
{
	char head[24];
	char const *folder = strrchr(cur_dir, '/');
	folder = (folder && folder[1]) ? folder + 1 : "fls0";
	snprintf(head, sizeof head, "FXTXT 文本 - %s", folder);
	ui_title(head);

	static char const *lab_list[6] = { "新建", "上次", "", "", "", "帮助" };
	for (int i = 0; i < 6; i++)
		box_lab[i] = lab_list[i];

	int y = CONTENT_Y;
	if (list_sel < list_top)
		list_top = list_sel;
	if (list_sel >= list_top + 5)
		list_top = list_sel - 4;

	for (int i = 0; i < 5; i++) {
		int idx = list_top + i;
		if (idx >= name_count)
			break;
		int sel = (idx == list_sel);
		if (sel)
			drect(0, y - 1, TEXT_W - 1, y + 7, C_BLACK);
		if (is_dir[idx])
			icon_folder(1, y);
		else
			icon_file(1, y);
		draw_fname(10, y, names[idx], sel ? 1 : 0);
		y += 9;
	}
	if (name_count == 0)
		draw_small(10, CONTENT_Y + 8, "(empty)", 0);

	{
		char info[16];
		int n = name_count - (strcmp(cur_dir, root_dev) ? 1 : 0);
		snprintf(info, sizeof info, "%d 个", n);
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
		ui_status("ALPHA+键打字母");
		return;
	}
	int n = py_candidates();
	if (n <= 0) {
		draw_ui(x + 6, STATUS_Y, "无匹配", 0);
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
	int rows_total = row_count();
	if (view_top > rows_total - 1) view_top = rows_total - 1;
	if (view_top < 0) view_top = 0;

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

	static char const *lab_read[6] = { "", "查找", "编辑", "", "", "帮助" };
	static char const *lab_edit[6] = { "保存", "", "退出", "标点", "中英", "帮助" };
	for (int i = 0; i < 6; i++)
		box_lab[i] = edit_mode ? lab_edit[i] : lab_read[i];

	int caret_x = -1, caret_y = -1;
	int r = rows(), y = CONTENT_Y, row = view_top;
	for (int i = 0; i < r && row < rows_total; i++, row++) {
		int p = row_start(row);
		int q = next_row(p);
		int x = 0;
		int caretx = -1;
		for (int c = p; c < q; c++) {
			if (edit_mode && c == ccell)
				caretx = x;
			draw_cell(x, y, cells[c], 0);
			x += cell_adv(cells[c]);
		}
		if (edit_mode && row == crow && ccell >= q &&
		    (q >= ncell || cells[q] == CELL_NL))
			caretx = x;
		if (caretx >= 0 && caretx < TEXT_W)
			caret_x = caretx, caret_y = y;
		y += lh();
	}

	if (caret_x >= 0) {
		for (int k = 0; k < lh() - 1; k++)
			px_on(caret_x, caret_y + k);
	}

	if (edit_mode && py_len > 0) {
		comp_draw();
	} else {
		char info[16];
		snprintf(info, sizeof info, "%d/%d", view_top + 1, rows_total);
		ui_hints(edit_mode ? (cn_mode ? hint_edit_cn : hint_edit_en) : hint_view, info,
		         edit_mode);
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
		return '"';		/* 引号键 = [x10^x] */
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
	int total = (int)strlen(punct_list);
	int off = punct_offset(n);
	if (off >= total)
		return 0;
	unsigned cp = 0;
	int used = utf8_seq((unsigned char const *)punct_list, total, off, &cp);
	if (!used)
		return 0;
	int g = gb_lookup((unsigned short)cp);
	return (g >= 0) ? (uint16_t)(CELL_CJK + g) : 0;
}

/*=== search prompt ========================================================*/

static void draw_search(void)
{
	ui_title("搜索");
	int y = CONTENT_Y + 2;
	draw_ui(2, y, "查找", 0);
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
	ui_hints(py_len ? "EXE上屏 ←→选" : "EXE搜索 EXIT退", "", 1);
	dupdate();
}

static void handle_search_key(key_event_t ev)
{
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
}

static void text_backspace(void)
{
	int off = line_start(cur_line) + cur_col;
	if (off <= 0 || ncell <= 0)
		return;
	memmove(cells + off - 1, cells + off, (ncell - off) * sizeof(uint16_t));
	ncell--;
	if (cur_col > 0)
		cur_col--;
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

/* ASCII punctuation -> its GB2312 full-width counterpart (qu 1). */
static uint16_t kb_fullwidth(uint16_t cell)
{
	static unsigned char const map[][2] = {
		{',', 0xAC}, {'.', 0xAE}, {':', 0xBA}, {';', 0xBB},
		{'?', 0xBF}, {'!', 0xA1}, {'(', 0xA8}, {')', 0xA9},
		{'[', 0xDB}, {']', 0xDC}, {'{', 0xFB}, {'}', 0xFC},
	};
	if (cell >= CELL_CJK)
		return cell;
	char c = (char)(cell + 0x20);
	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
		if ((char)map[i][0] == c)
			return (uint16_t)(CELL_CJK + ((int)map[i][1] - 0xA1));
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
	int pinyin = (kb_page == PAGE_PINYIN);
	int punct = (kb_page == PAGE_PUNCT);
	ui_title(punct ? "标点符号"
	               : pinyin ? (kb_target == 1 ? "拼音搜索" : "拼音输入")
	                        : (kb_target == 1 ? "搜索文字" : "插入字符"));

	int grid_y;
	if (pinyin) {
		/* input line */
		char line[16];
		if (py_len)
			snprintf(line, sizeof line, "%s_", py_buf);
		else
			snprintf(line, sizeof line, "输入拼音");
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
			draw_ui(2, y, "无匹配", 0);
		}
		grid_y = CONTENT_Y + 22;
	} else {
		grid_y = CONTENT_Y + 2;
	}

	int gc = kb_cols();
	int gr = (pinyin || punct) ? 2 : 5;
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
		ui_status(py_candidates() ? "F3/F4 选字   F5 上屏" : "F1 换页   F6 完成");
	} else if (punct) {
		ui_status("EXE 输入   F1 换页   F6 完成");
	} else {
		ui_status("F1 换页   F6 完成");
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
	for (int i = 0; i < 6; i++)
		box_lab[i] = 0;
	static char const *page1[] = {
		"↑↓   选择文件",
		"EXE  打开 / 进入",
		"      .. 返回上级",
		"F1   新建文档",
		"F2   回到上次位置",
	};
	static char const *page2[] = {
		"↑↓ 滚动   ←→ 翻页",
		"F2   搜索文字",
		"F3   进入编辑",
		"F4   （预留）",
		"F6   帮助   EXIT 返回",
	};
	static char const *page3[] = {
		"F1保存  F3退出编辑",
		"F4标点  F5中/英",
		"EXE换行 DEL删字",
		"ALPHA小写 SHIFT大写",
		"SHIFT+ALPHA 字母锁",
	};
	static char const *titles[] = { "列表页", "阅读页", "编辑页" };
	char const **lines = help_page == 0 ? page1 : (help_page == 1 ? page2 : page3);
	char head[32];
	snprintf(head, sizeof head, "帮助 %d/3", help_page + 1);
	ui_title(head);
	draw_ui(2, CONTENT_Y - 9 + 45, "", 0);	/* no-op: keeps the layout stable */

	/* page name on the first content line */
	draw_ui(2, CONTENT_Y, titles[help_page], 0);

	int y = CONTENT_Y + 9;
	for (int i = 0; i < 4; i++, y += 9)
		draw_ui(2, y, lines[i], 0);

	ui_hints("←→ 翻页  EXIT 关闭", "", 0);
	dupdate();
}

/*=== search ===============================================================*/

static void build_search_cells(void)
{
	search_len = 0;
	for (int i = 0; i < kb_len && search_len < 31; i++)
		search_cells[search_len++] = kb_cells[i];
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
	build_search_cells();
	int hit = find_cells(line_start(cur_line) + cur_col + 1);
	if (hit < 0) hit = find_cells(0);
	if (hit < 0) {
		snprintf(status_msg, sizeof status_msg, "没找到");
		return;
	}
	cur_line = offset_to_line(hit);
	cur_col = hit - line_start(cur_line);
	view_top = cur_line;
	hscroll = cur_col > 4 ? cur_col - 4 : 0;
	snprintf(status_msg, sizeof status_msg, "找到了");
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

	if (edit_mode && ev.key == KEY_F6) {
		caps_on = !caps_on;
		return;
	}

	if (edit_mode && ev.key == KEY_F5) {
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
			c = alpha_letter(ev.key);
			if (c && ev.shift)
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
				text_insert((uint16_t)((unsigned char)c - 0x20));
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
		gint_world_switch(GINT_CALL(fs_save_state));
		mode = MODE_LIST;
		return;
	case KEY_UP:
		if (edit_mode) {
			move_cursor_line(-1);
		} else {
			int total = row_count();
			view_top -= rows();
			if (view_top < 0)
				view_top = 0;
			(void)total;
		}
		return;
	case KEY_DOWN:
		if (edit_mode) {
			move_cursor_line(1);
		} else {
			int total = row_count();
			view_top += rows();
			if (view_top > total - 1)
				view_top = total - 1;
			if (view_top < 0)
				view_top = 0;
		}
		return;
	case KEY_LEFT:
		if (edit_mode) {
			if (cur_col > 0) cur_col--;
			else if (cur_line > 0) {
				move_cursor_line(-1);
				cur_col = line_len(line_start(cur_line));
			}
		}
		return;
	case KEY_RIGHT:
		if (edit_mode) {
			int len = line_len(line_start(cur_line));
			if (cur_col < len) cur_col++;
			else if (cur_line < line_count() - 1) {
				move_cursor_line(1);
				cur_col = 0;
			}
		}
		return;
	case KEY_F2:
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
		if (edit_mode)
			return;			/* handled above */
		help_back = MODE_VIEW;
		mode = MODE_HELP;
		return;
	case KEY_F3:
		if (edit_mode && dirty) {
			confirm_kind = 1;
			mode = MODE_CONFIRM;
			return;
		}
		edit_mode = !edit_mode;
		if (edit_mode) {
			cur_line = view_top;
			if (cur_line > line_count() - 1)
				cur_line = line_count() - 1;
			cur_col = (line_len(line_start(cur_line)) > 0) ? 1 : 0;
		} else {
			py_reset();
		}
		return;
	case KEY_DEL:
		if (edit_mode) {
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
	int gc = kb_cols(), gr = (pinyin || punct) ? 2 : 5;

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
				while (i < 25 && !punct_cell(++i))
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
	snprintf(status_msg, sizeof status_msg, "已删除 %s", name);
	status_sticky = 1;
}

/* Confirmation screen for deleting a file. */
#define DBG_ROWS 5
#define DBG_UNITS 8
static int dbg_rc[DBG_ROWS];
static uint16_t dbg_units[DBG_ROWS][DBG_UNITS];
static int dbg_n;

/* Probe several pattern spellings; each row records its return code. */
static void dbg_collect(void)
{
	static char const *pats[DBG_ROWS] = {
		"\\\\fls0\\*",		/* current form: \fls0\* */
		"\\fls0\\*",		/* single leading slash: \fls0\* */
		"/fls0/*",			/* posix-looking */
		"\\\\fls0\\*.TXT",		/* with extension */
		"\\\\fls0\\",
	};
	uint16_t found[256];
	struct BFile_FileInfo info;

	dbg_n = 0;
	for (int p = 0; p < DBG_ROWS; p++) {
		uint16_t pattern[64];
		int k = 0;
		for (char const *s = pats[p]; *s && k < 63; s++)
			pattern[k++] = (uint16_t)(unsigned char)*s;
		pattern[k] = 0;

		int handle = 0;
		int rc = BFile_FindFirst(pattern, &handle, found, &info);
		dbg_rc[p] = rc;
		for (int i = 0; i < DBG_UNITS; i++)
			dbg_units[p][i] = 0;
		if (rc == 0) {
			for (int i = 0; i < DBG_UNITS && found[i]; i++)
				dbg_units[p][i] = found[i];
			int last = -1;
			for (int i = 0; found[i]; i++)
				last = i;
			int st = last > 5 ? last - 5 : 0;
			for (int i = 0, n = 0; found[st + i] && n < DBG_UNITS; i++, n++)
				dbg_units[p][n] = found[st + i];
			BFile_FindClose(handle);
			dbg_n++;
		}
	}
}

static void draw_debug(void)
{
	ui_title("F2 诊断");
	char line[40];
	snprintf(line, sizeof line, "命中 %d  列表 %d", dbg_n, name_count);
	draw_small(2, CONTENT_Y, line, 0);
	static char const *lab[DBG_ROWS] = { "A \\fls0\*", "B \fls0\*",
	                                     "C /fls0/*", "D ext", "E dir" };
	for (int r = 0; r < DBG_ROWS; r++) {
		char buf[64];
		int k = snprintf(buf, sizeof buf, "%s rc=%d ", lab[r], dbg_rc[r]);
		for (int i = 0; i < DBG_UNITS && dbg_units[r][i]; i++)
			k += snprintf(buf + k, sizeof buf - k, "%X ",
			              (unsigned)dbg_units[r][i]);
		draw_small(2, CONTENT_Y + 10 + r * 9, buf, 0);
	}
	dupdate();
}

static void draw_confirm(void)
{
	if (confirm_kind == 1) {
		ui_title("未保存");
		draw_ui(2, CONTENT_Y + 2, "放弃修改？", 0);
		draw_ui(2, CONTENT_Y + 12, "EXE放弃", 0);
		draw_ui(2, CONTENT_Y + 22, "EXIT继续编辑", 0);
		dupdate();
		return;
	}
	ui_title("删除文件");
	int y = CONTENT_Y + 4;
	draw_ui(2, y, "确定删除？", 0);
	draw_fname(2, y + 12, del_name, 0);
	ui_hints("EXE删除  EXIT取消", "", 0);
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
		snprintf(status_msg, sizeof status_msg, "已取消");
		status_sticky = 1;
		mode = MODE_LIST;
	}
}

static void handle_list_key(key_event_t ev)
{
	switch (ev.key) {
	case KEY_UP:
		if (list_sel > 0) list_sel--;
		return;
	case KEY_DOWN:
		if (list_sel < name_count - 1) list_sel++;
		return;
	case KEY_F1:
		gint_world_switch(GINT_CALL(name_default));
		mode = MODE_NAME;
		return;
	case KEY_F2:
		if (ev.shift) {
			gint_world_switch(GINT_CALL(dbg_collect));
			mode = MODE_DEBUG;
			return;
		}
		/* jump back to the position remembered from the last session */
		if (fs_restore())
			mode = MODE_VIEW;
		else
			snprintf(status_msg, sizeof status_msg, "无上次位置");
		return;
	case KEY_F3:
		if (ev.shift) {
			/* test helper, not advertised */
			gint_world_switch(GINT_CALL(fs_make_demo));
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
			snprintf(status_msg, sizeof status_msg, "不能删文件夹");
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
		cur_line = cur_col = 0;
		view_top = hscroll = 0;
		edit_mode = 0;
		dirty = 0;
		gint_world_switch(GINT_CALL(fs_read_current));
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
				sleep_ms(15);			/* real delay, no busy spin */
			} else {
				rep_key = 0;
				rep_n = 0;
			}
			continue;
		}

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

	int reentry = (cells != NULL);

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
	if (!reentry) {
		gint_world_switch(GINT_CALL(fs_probe_root));
		gint_world_switch(GINT_CALL(fs_list_dir));
		list_sel = list_top = 0;

		/* reopen the file that was being read when the app was last closed */
		if (fs_restore())
			mode = MODE_VIEW;
	}
	/* on re-entry everything is still in memory: show it immediately */

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
		else if (mode == MODE_DEBUG)
			draw_debug();
		else
			draw_keyboard();

		key_event_t ev = next_event();
		status_sticky = 0;
		if (ev.key == KEY_ACON) {
			/* AC/ON switches the calculator off */
			gint_world_switch(GINT_CALL(fs_save_state));
			gint_poweroff(1);
			break;
		}
		if (ev.key == KEY_MENU) {
			/* MENU always leaves the add-in */
			alpha_lock = 0;
			shift_armed = 0;
			alpha_armed = 0;
			py_reset();
			gint_world_switch(GINT_CALL(fs_save_state));
			break;
		}
		if (mode == MODE_LIST) {
			handle_list_key(ev);
		} else if (mode == MODE_VIEW) {
			handle_view_key(ev);
		} else if (mode == MODE_NAME) {
			handle_name_key(ev);
		} else if (mode == MODE_SEARCH) {
			handle_search_key(ev);
		} else if (mode == MODE_DEBUG) {
			if (ev.key == KEY_EXIT || ev.key == KEY_EXE)
				mode = MODE_LIST;
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

	free(cells);
	free(rawbuf);
	free(names);
	free(is_dir);
	cells = NULL;
	rawbuf = NULL;
	names = NULL;
	is_dir = NULL;
	return 0;
}

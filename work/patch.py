"""Apply the paging rework to work/fxtxt/src/main.c (byte exact, ASCII only)."""
import re
import sys

P = r'E:\fxtxtviewer\work\fxtxt\src\main.c'
src = open(P, 'rb').read()
orig_len = len(src)
orig_nonascii = sum(1 for c in src if c > 127)

PATCHES = []


def patch(name, start, end, new, start_occ=1):
    PATCHES.append((name, start, end, new, start_occ))


# ---------------------------------------------------------------- 1: globals
patch('globals',
      b'static int chunk_lines;',
      b'static unsigned char *rawbuf;',
      b'''static int chunk_lines;\t\t/* lines preceding this window */
static int chunk_rows;\t\t/* display rows preceding this window */
static int total_rows;\t\t/* display rows of the whole file */
static int total_lines;\t\t/* lines of the whole file */
static unsigned char *rawbuf;

/* --- window table ---------------------------------------------------------
   The document does not fit into the text buffer, so it is read in windows of
   at most text_max bytes.  Every window but the last ends on a line boundary
   after a whole number of screen pages (see chunk_cut), so a page never
   straddles two windows.  The table records, for each window, its file offset
   and the display rows and lines that precede it; a window-local row plus
   chunk_rows is the document-wide row used by the page counter, the history,
   the bookmarks and the search.  Heap only: static RAM is 6 KiB. */
static int *chunk_off;\t\t/* chunk_off[i] = file offset of window i */
static int *chunk_r0;\t\t/* chunk_r0[i]  = display rows before i */
static int *chunk_l0;\t\t/* chunk_l0[i]  = lines before i */
static int chunk_n;\t\t/* windows in the table */
static int chunk_cap;\t\t/* slots allocated in the three arrays */
static int chunk_idx;\t\t/* window that lives in cells[] now */

static int chunk_of_row(int row);
static void chunk_load(int idx);
static void goto_row(int grow);
static void goto_line(int line);
static int global_row(void);
static int global_line(void);
static void page_step(int dir);
static int hit_chunk(int hit);
static int hit_cell(int hit);''')

# ------------------------------------------- 2: window machinery (the core)
patch('chunks',
      b'/* Read the window that starts at byte `base`.',
      b'\tload_chunk(0);\n}',
      b'''/*=== windows ==============================================================
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
\tint per = rows();
\tint i = 0, x = 0, done = 0;
\tint page = 0, rowb = 0, lineb = 0;

\tif (last || n <= 0)
\t\treturn n;
\twhile (i < n) {
\t\tunsigned char c = s[i];
\t\tuint16_t cell = 0;
\t\tint reps = 1, at = i;

\t\tif (c == '\\n') {
\t\t\ti++;
\t\t\tdone++;\t\t\t/* the newline closes the row */
\t\t\tlineb = i;\t\t/* ... and it is a line end */
\t\t\tif (done % per == 0)
\t\t\t\tpage = rowb = i;
\t\t\tx = 0;
\t\t\tcontinue;
\t\t}
\t\tif (c == '\\r') {
\t\t\ti++;
\t\t\tcontinue;
\t\t}
\t\tif (c == '\\t') {
\t\t\tcell = 0;
\t\t\treps = 4;\t\t/* a tab is four empty cells */
\t\t\ti++;
\t\t} else if (c < 0x20) {
\t\t\ti++;
\t\t\tcontinue;
\t\t} else if (c >= 0x81 && i + 1 < n) {
\t\t\tunsigned char t = s[i + 1];
\t\t\tif (c >= 0xA1 && c <= 0xF7 && t >= 0xA1 && t <= 0xFE) {
\t\t\t\tint qu = c - 0xA0, wei = t - 0xA0;
\t\t\t\tint gi = (qu - 1) * 94 + (wei - 1);
\t\t\t\tcell = (gi >= 0 && gi < FONT_GLYPHS)
\t\t\t\t\t? (uint16_t)(CELL_CJK + gi) : 0;
\t\t\t} else
\t\t\t\tcell = 0;
\t\t\ti += 2;
\t\t} else if (c >= 0x80) {
\t\t\ti++;
\t\t\tcontinue;
\t\t} else {
\t\t\tcell = (uint16_t)(c - 0x20);
\t\t\ti++;
\t\t}
\t\tfor (int k = 0; k < reps; k++) {
\t\t\tint adv = cell_adv(cell);
\t\t\tif (x + adv > TEXT_W && x > 0) {
\t\t\t\t/* soft wrap: this cell opens the next display row */
\t\t\t\tdone++;
\t\t\t\tx = 0;
\t\t\t\tif (done % per == 0)
\t\t\t\t\trowb = at;
\t\t\t}
\t\t\tx += adv;
\t\t}
\t}
\tif (page > 0)
\t\treturn page;
\tif (rowb > 0)
\t\treturn rowb;
\tif (lineb > 0)
\t\treturn lineb;
\treturn n;
}

/* One more slot in the window table; 0 when the heap is exhausted. */
static int chunk_grow(void)
{
\tint cap;
\tint *p;

\tif (chunk_n < chunk_cap)
\t\treturn 1;
\tcap = chunk_cap ? chunk_cap * 2 : 8;
\tp = realloc(chunk_off, (size_t)cap * sizeof *p);
\tif (!p)
\t\treturn 0;
\tchunk_off = p;
\tp = realloc(chunk_r0, (size_t)cap * sizeof *p);
\tif (!p)
\t\treturn 0;
\tchunk_r0 = p;
\tp = realloc(chunk_l0, (size_t)cap * sizeof *p);
\tif (!p)
\t\treturn 0;
\tchunk_l0 = p;
\tchunk_cap = cap;
\treturn 1;
}

/* Map the file: byte offset, display rows and lines before every window.  One
   pass over the document, done once per open (and after a save). */
static void build_chunk_table(void)
{
\tFILE *f = open_read();
\tint base = 0;

\tchunk_n = 0;
\ttotal_rows = 0;
\ttotal_lines = 0;
\tif (!chunk_grow())
\t\treturn;
\tif (!f)
\t\treturn;
\tfor (;;) {
\t\tint n, cut, last;

\t\tif (!chunk_grow())
\t\t\tbreak;\t\t\t/* out of memory: keep what we have */
\t\tif (base > 0 && fseek(f, (long)base, SEEK_SET) != 0)
\t\t\tbreak;
\t\tn = (int)fread(rawbuf, 1, text_max, f);
\t\tif (n < 0)
\t\t\tn = 0;
\t\tlast = (n < text_max) || (fgetc(f) == EOF);
\t\tcut = chunk_cut(rawbuf, n, last);
\t\tif (cut <= 0)
\t\t\tcut = n;\t\t/* cannot happen, but never loop */
\t\tchunk_off[chunk_n] = base;
\t\tchunk_r0[chunk_n] = total_rows;
\t\tchunk_l0[chunk_n] = total_lines;
\t\tdecode_into_cells(rawbuf, cut);
\t\ttotal_rows += row_count();
\t\ttotal_lines += line_count();
\t\tchunk_n++;
\t\tbase += cut;
\t\tif (last || n <= 0)
\t\t\tbreak;
\t}
\tfclose(f);
\tif (chunk_n == 0) {
\t\t/* an empty document still needs one window to point at */
\t\tchunk_off[0] = 0;
\t\tchunk_r0[0] = 0;
\t\tchunk_l0[0] = 0;
\t\tchunk_n = 1;
\t\ttotal_rows = 1;
\t\ttotal_lines = 1;
\t}
}

/* Read window `idx` into cells[] and make chunk_rows/chunk_lines describe it.
   They are never accumulated: the table holds the document-wide offset. */
static void chunk_load(int idx)
{
\tFILE *f;
\tint base, len;

\tncell = 0;
\tchunk_bytes = 0;
\tchunk_more = 0;
\tif (chunk_n <= 0)
\t\treturn;
\tif (idx < 0)
\t\tidx = 0;
\tif (idx > chunk_n - 1)
\t\tidx = chunk_n - 1;
\tf = open_read();
\tif (!f) {
\t\tsnprintf(status_msg, sizeof status_msg, "\\xB4\\xF2\\xB2\\xBB\\xBF\\xAA");
\t\treturn;
\t}
\tbase = chunk_off[idx];
\tif (base > 0 && fseek(f, (long)base, SEEK_SET) != 0) {
\t\tfclose(f);
\t\treturn;
\t}
\tlen = (idx + 1 < chunk_n) ? (chunk_off[idx + 1] - base) : -1;
\tif (len <= 0 || len > text_max)
\t\tlen = (int)fread(rawbuf, 1, text_max, f);
\telse
\t\tlen = (int)fread(rawbuf, 1, len, f);
\tfclose(f);
\tif (len < 0)
\t\tlen = 0;
\tchunk_idx = idx;
\tchunk_base = base;
\tchunk_bytes = len;
\tchunk_more = (idx + 1 < chunk_n);
\tchunk_rows = chunk_r0[idx];
\tchunk_lines = chunk_l0[idx];
\tdecode_into_cells(rawbuf, len);
\tundo_clear();
\tsnprintf(status_msg, sizeof status_msg, "%d\\xD7\\xD6\\xBD\\xDA GB", len);
}

/* Window that holds document row `row` (the table is ordered). */
static int chunk_of_row(int row)
{
\tint lo = 0, hi = chunk_n - 1;
\twhile (lo < hi) {
\t\tint mid = (lo + hi + 1) / 2;
\t\tif (chunk_r0[mid] <= row)
\t\t\tlo = mid;
\t\telse
\t\t\thi = mid - 1;
\t}
\treturn lo;
}

static int chunk_of_line(int line)
{
\tint lo = 0, hi = chunk_n - 1;
\twhile (lo < hi) {
\t\tint mid = (lo + hi + 1) / 2;
\t\tif (chunk_l0[mid] <= line)
\t\t\tlo = mid;
\t\telse
\t\t\thi = mid - 1;
\t}
\treturn lo;
}

/* Document-wide display row / line of the first row on the screen. */
static int global_row(void)
{
\treturn chunk_rows + view_top;
}

static int global_line(void)
{
\tint l = chunk_lines + offset_to_line(row_start(view_top));
\treturn l < 0 ? 0 : l;
}

/* Show document row `grow` at the top of the screen (loads its window). */
static void goto_row(int grow)
{
\tint c;

\tif (total_rows <= 0)
\t\ttotal_rows = 1;
\tif (grow < 0)
\t\tgrow = 0;
\tif (grow > total_rows - 1)
\t\tgrow = total_rows - 1;
\tc = chunk_of_row(grow);
\tif (c != chunk_idx || ncell == 0)
\t\tchunk_load(c);
\tview_top = grow - chunk_rows;
\tif (view_top > row_count() - 1)
\t\tview_top = row_count() - 1;
\tif (view_top < 0)
\t\tview_top = 0;
}

/* Show document line `line` at the top of the screen. */
static void goto_line(int line)
{
\tint c, l;

\tif (total_lines <= 0)
\t\ttotal_lines = 1;
\tif (line < 0)
\t\tline = 0;
\tif (line > total_lines - 1)
\t\tline = total_lines - 1;
\tc = chunk_of_line(line);
\tif (c != chunk_idx || ncell == 0)
\t\tchunk_load(c);
\tl = line - chunk_lines;
\tif (l > line_count() - 1)
\t\tl = line_count() - 1;
\tif (l < 0)
\t\tl = 0;
\tcur_line = l;
\tview_top = row_of_cell(line_start(l));
\tif (view_top > row_count() - 1)
\t\tview_top = row_count() - 1;
\tif (view_top < 0)
\t\tview_top = 0;
}

/* One screen up (dir < 0) or down.  Pages are stepped on the grid of rows()
   rows, so walking down and back up visits exactly the same pages even when a
   search or a bookmark left the screen between two of them. */
static void page_step(int dir)
{
\tint g = chunk_rows + view_top;

\tif (dir < 0) {
\t\tif (g <= 0)
\t\t\treturn;
\t\tgoto_row(((g - 1) / rows()) * rows());
\t} else {
\t\tint next = (g / rows() + 1) * rows();
\t\tif (next > total_rows - 1)
\t\t\treturn;
\t\tgoto_row(next);
\t}
}

static void fs_read_current(void)
{
\tbuild_chunk_table();
\tchunk_load(0);
}''')

# -------------------------------------------------- 3: draw_view bottom clamp
patch('draw_view_lim',
      b'int lim = rows_total - rows();',
      b'int lim = rows_total - rows();',
      b'int lim = rows_total - 1;\t/* the last page may be short */')

# ------------------------------------------------- 4: draw_view search highlight
patch('draw_view_hl',
      b'if (!edit_mode && search_n > 0)\n\t\t\thl = search_hits[search_cur];',
      b'hl = search_hits[search_cur];',
      b'''if (!edit_mode && search_n > 0 &&
\t\t    hit_chunk(search_hits[search_cur]) == chunk_idx)
\t\t\thl = hit_cell(search_hits[search_cur]);''')

# ------------------------------------------------------------- 5: fill_state
patch('fill_state',
      b'st.view_top = view_top;',
      b'if (st.cur_line < 0) st.cur_line = 0;',
      b'''st.view_top = global_row();
\t\tst.hscroll = hscroll;
\t\t/* remember the page, not the caret: scrolling only moves view_top,
\t\t   so saving cur_line kept an old line and the position stopped
\t\t   updating after the first reopen.  Both numbers are document-wide,
\t\t   because a window-local row points at the wrong line as soon as the
\t\t   document is read in more than one window. */
\t\tst.cur_line = global_line();
\t\tif (st.cur_line < 0) st.cur_line = 0;''')

# ----------------------------------------------------------- 6: fs_load_state
patch('fs_load_state',
      b'view_top = s.view_top;',
      b'if (hscroll < 0) hscroll = 0;',
      b'''hscroll = s.hscroll;
\tcur_col = s.cur_col;
\t/* s.view_top is a document-wide display row and s.cur_line a
\t   document-wide line: open the window that holds them, then convert both
\t   back to window-local numbers. */
\tgoto_row(s.view_top);
\tcur_line = s.cur_line - chunk_lines;
\tif (cur_line > line_count() - 1) cur_line = line_count() - 1;
\tif (cur_line < 0) cur_line = 0;
\tint len = line_len(line_start(cur_line));
\tif (cur_col > len) cur_col = len;
\tif (cur_col < 0) cur_col = 0;
\tif (hscroll < 0) hscroll = 0;''')

# --------------------------------------------------------------- 7: pos_store
patch('pos_store',
      b'pos_tab[k].line = offset_to_line',
      b'pos_tab[k].vtop = view_top;',
      b'''pos_tab[k].line = global_line();
\tif (pos_tab[k].line < 0) pos_tab[k].line = 0;
\tpos_tab[k].vtop = global_row();''')

# ----------------------------------------------------------- 8: apply_saved_pos
patch('apply_saved_pos',
      b'if (!pos_lookup(names[list_sel], &line, &vtop))',
      b'hscroll = 0;',
      b'''if (!pos_lookup(names[list_sel], &line, &vtop))
\t\treturn;
\tif (line < 0)
\t\tline = 0;
\t/* the record holds document-wide numbers: open that window, then make
\t   them window-local again */
\tgoto_row(vtop);
\tcur_line = line - chunk_lines;
\tif (cur_line > line_count() - 1) cur_line = line_count() - 1;
\tif (cur_line < 0) cur_line = 0;
\tcur_col = 0;
\thscroll = 0;''')

# --------------------------------------------------------------- 9: resume_line
patch('resume_line',
      b'int total = line_count();',
      b'view_top = row_of_cell(line_start(cur_line));',
      b'''cur_col = 0;
\t/* REC keeps a document-wide 1-based line: goto_line opens the window
\t   that holds it, which may be any window of the document. */
\tgoto_line(line - 1);''')

# ---------------------------------------------------------- 10a: search_collect
patch('search_collect',
      b'/* Store every occurrence of the query, remembering where the caret is. */',
      b'\tif (search_cur >= search_n)\n\t\tsearch_cur = 0;\n}',
      b'''/* A hit is packed as (window << HIT_SHIFT) | cell, so one list can cover the
   whole document while only one window is in memory.  text_max never exceeds
   32768, so a window-local cell index fits into HIT_SHIFT bits. */
#define HIT_SHIFT 15
#define HIT_MASK  0x7FFF

static int hit_chunk(int hit) { return hit >> HIT_SHIFT; }
static int hit_cell(int hit)  { return hit & HIT_MASK; }

/* Append every occurrence found in the window that is loaded now. */
static void search_collect(void)
{
\tif (!search_len || !ncell)
\t\treturn;
\tfor (int i = 0; i + search_len <= ncell && search_n < SEARCH_MAX_HITS; i++) {
\t\tint k = 0;
\t\twhile (k < search_len && cells[i + k] == search_cells[k])
\t\t\tk++;
\t\tif (k != search_len)
\t\t\tcontinue;
\t\tsearch_hits[search_n++] = (chunk_idx << HIT_SHIFT) | (i & HIT_MASK);
\t}
}''')

# ------------------------------------------------------------- 10b: search_show
patch('search_show',
      b'static void search_show(void)\n{',
      b'search_n, search_cur + 1, search_n);',
      b'''static void search_show(void)
{
\tif (!search_n)
\t\treturn;
\tint hit = search_hits[search_cur];
\tint c = hit_chunk(hit);
\tint cell = hit_cell(hit);
\tif (c != chunk_idx || ncell == 0)
\t\tchunk_load(c);
\tif (cell > ncell - 1)
\t\tcell = ncell > 0 ? ncell - 1 : 0;
\tcur_line = offset_to_line(cell);
\tcur_col = cell - line_start(cur_line) + search_len;
\tint llen = line_len(line_start(cur_line));
\tif (cur_col > llen)
\t\tcur_col = llen;
\tview_top = row_of_cell(cell);
\tif (view_top > row_count() - 1)
\t\tview_top = row_count() - 1;
\tif (view_top < 0)
\t\tview_top = 0;
\thscroll = 0;
\tsnprintf(status_msg, sizeof status_msg, "\\xC3\\xFC\\xD6\\xD0%d %d/%d",
\t         search_n, search_cur + 1, search_n);''')

# -------------------------------------------------------------- 10c: run_search
patch('run_search',
      b'static void run_search(void)\n{',
      b'search_show();\n}',
      b'''/* Walk every window of the document, so a match near the end is found even
   when the reader is at the top.  The window the reader was in is put back
   afterwards (search_show then opens the window of the match). */
static void run_search(void)
{
\tint here_chunk = chunk_idx;
\tint here = line_start(cur_line) + cur_col;
\tint from = 0;
\tint save_idx = chunk_idx, save_top = view_top;

\tsearch_n = 0;
\tsearch_cur = 0;
\tif (!search_len)
\t\treturn;
\tfor (int c = 0; c < chunk_n && search_n < SEARCH_MAX_HITS; c++) {
\t\tif (c != chunk_idx)
\t\t\tchunk_load(c);
\t\tsearch_collect();
\t}
\tif (!search_n) {
\t\tif (chunk_idx != save_idx)
\t\t\tchunk_load(save_idx);
\t\tview_top = save_top;
\t\tsnprintf(status_msg, sizeof status_msg, "\\xC3\\xBB\\xD5\\xD2\\xB5\\xBD");
\t\treturn;
\t}
\t/* the first match behind the caret, wrapping to the first one */
\tfor (int i = 0; i < search_n; i++) {
\t\tint hc = hit_chunk(search_hits[i]), hl = hit_cell(search_hits[i]);
\t\tif (hc > here_chunk || (hc == here_chunk && hl > here)) {
\t\t\tfrom = i;
\t\t\tbreak;
\t\t}
\t}
\tsearch_cur = from;
\tsearch_show();
}''')

# ------------------------------------------------------- 11: bookmark page jump
patch('book_jump',
      b'} else if (ev.key == KEY_F2) {\n\t\tint p = book_v[book_sel];',
      b'\t\t\tview_top = rel;\n\t\t\tmode = MODE_VIEW;',
      b'''} else if (ev.key == KEY_F2) {
\t\tint p = book_v[book_sel];
\t\tif (p > 0) {
\t\t\t/* p is a document-wide page: any window can be reached */
\t\t\tgoto_row(p - 1);
\t\t\tmode = MODE_VIEW;''')

# --------------------------------------------- 12: view F5 bookmark + SHIFT jump
patch('view_book_line',
      b'\t\tbook_line = chunk_rows + view_top + 1;',
      b'\t\tbook_line = chunk_rows + view_top + 1;',
      b'\t\tbook_line = global_row() + 1;')

patch('view_book_store',
      b'int page = chunk_rows + view_top + 1;',
      b'int page = chunk_rows + view_top + 1;',
      b'int page = global_row() + 1;\t/* document-wide page */')

patch('view_book_jump',
      b'int p = pos_tab[k].bm[slot - 1];',
      b'\t\t\tview_top = rel;\n\t\t\treturn;',
      b'''int p = pos_tab[k].bm[slot - 1];
\t\t\tif (p <= 0)
\t\t\t\treturn;
\t\t\tgoto_row(p - 1);\t/* document-wide page */
\t\t\treturn;''')

# ------------------------------------------------- 13: main loop bookmark capture
patch('main_book_line',
      b'book_line = chunk_rows + view_top + 1;',
      b'book_line = chunk_rows + view_top + 1;',
      b'book_line = global_row() + 1;')

# ------------------------------------------------------------ 14: KEY_UP/DOWN
patch('keys',
      b'\tcase KEY_UP:\n\t\tif (edit_mode) {\n\t\t\tmove_cursor_row(-1);',
      b'\t\treturn;\n\tcase KEY_LEFT:',
      b'''\tcase KEY_UP:
\t\tif (edit_mode)
\t\t\tmove_cursor_row(-1);
\t\telse
\t\t\tpage_step(-1);
\t\treturn;
\tcase KEY_DOWN:
\t\tif (edit_mode)
\t\t\tmove_cursor_row(1);
\t\telse
\t\t\tpage_step(1);
\t\treturn;
\tcase KEY_LEFT:''')

# ------------------------------------------------------------- 15: app_release
patch('app_release',
      b'free(cells);\n\tfree(rawbuf);',
      b'text_max = 0;\n\tname_count = 0;',
      b'''free(cells);
\tfree(rawbuf);
\tfree(chunk_off);
\tfree(chunk_r0);
\tfree(chunk_l0);
\tcells = NULL;
\trawbuf = NULL;
\tchunk_off = NULL;
\tchunk_r0 = NULL;
\tchunk_l0 = NULL;
\tchunk_n = 0;
\tchunk_cap = 0;
\tchunk_idx = 0;
\tnames = NULL;
\tnames_fc = NULL;
\tis_dir = NULL;
\ttext_max = 0;
\tname_count = 0;''')


def line_start_of(b, i):
    j = b.rfind(b'\n', 0, i)
    return j + 1


def line_end_of(b, i):
    j = b.find(b'\n', i)
    return len(b) if j < 0 else j

fail = 0
out = src
for name, start, end, new, s_occ in PATCHES:
    pos = -1
    idx = 0
    while True:
        p = out.find(start, pos + 1)
        if p < 0:
            break
        idx += 1
        if idx == s_occ:
            pos = p
            break
    if pos < 0:
        print('MISS start', name, start[:50])
        fail += 1
        continue
    a = line_start_of(out, pos)
    e = out.find(end, pos)
    if e < 0:
        print('MISS end', name, end[:50])
        fail += 1
        continue
    b = line_end_of(out, e + len(end) - 1)
    old = out[a:b]
    if old.count(b'\n') > 250:
        print('TOO BIG', name, old.count(b'\n'))
        fail += 1
        continue
    # keep the leading whitespace of the replaced block
    lead = old[:len(old) - len(old.lstrip(b'\t'))]
    if lead and not new[:1] in (b'\t', b' '):
        new = lead + new
    out = out[:a] + new + out[b:]
    print('ok %-16s lines %d -> %d' % (name, old.count(b'\n') + 1, new.count(b'\n') + 1))

if fail:
    print('FAILED', fail)
    sys.exit(1)

left = [m for m in (b'load_chunk(', b'rows_upto(', b'count_total_rows(') if m in out]
print('leftover:', left)
extra = sum(1 for c in out if c > 127)
print('size %d -> %d  nonascii %d -> %d' % (orig_len, len(out), orig_nonascii, extra))
open(P, 'wb').write(out)

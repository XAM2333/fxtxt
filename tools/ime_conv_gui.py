#!/usr/bin/env python3
"""FXTXT 字词库转换工具（图形界面）。

    python tools/ime_conv_gui.py

左边是常用字（最多 24 个），右边是常用词（最多 8 条、每条 12 个字符）。
可以直接在这里编辑，也可以打开电脑上的文本 / 二进制文件，改完存成任意一种格式，
拷到计算器 \\fls0 下即可（文件名 *.IME_* 就行，下划线后随便写）。

命令行版本：tools/ime_conv.py
"""

import os
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ime_conv as core  # noqa: E402

APP_TITLE = "FXTXT 字词库转换工具"


class App:
    def __init__(self, root):
        self.root = root
        self.chars = []          # list[int]  display cells
        self.words = []          # list[list[int]]
        self.path = None

        root.title(APP_TITLE)
        root.minsize(760, 520)
        root.columnconfigure(0, weight=1)
        root.columnconfigure(1, weight=1)
        root.rowconfigure(1, weight=1)

        bar = ttk.Frame(root, padding=(8, 8, 8, 0))
        bar.grid(row=0, column=0, columnspan=2, sticky="ew")
        ttk.Button(bar, text="打开文本…", command=self.open_text).pack(side="left")
        ttk.Button(bar, text="打开二进制…", command=self.open_bin).pack(side="left", padx=(6, 0))
        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=10)
        ttk.Button(bar, text="保存为二进制…", command=self.save_bin).pack(side="left")
        ttk.Button(bar, text="保存为文本…", command=self.save_text).pack(side="left", padx=(6, 0))
        ttk.Button(bar, text="清空", command=self.clear_all).pack(side="right")
        self.sample = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="示例", variable=self.sample,
                        command=self.toggle_sample).pack(side="right", padx=(0, 10))

        self.chars_panel = self._panel(root, 0, "常用字", 24,
                                       self.add_char, self.del_char)
        self.words_panel = self._panel(root, 1, "常用词", 8,
                                       self.add_word, self.del_word)

        foot = ttk.Frame(root, padding=(8, 0, 8, 8))
        foot.grid(row=2, column=0, columnspan=2, sticky="ew")
        self.status = tk.StringVar(value="就绪")
        ttk.Label(foot, textvariable=self.status, anchor="w").pack(side="left")
        self.size = tk.StringVar(value="")
        ttk.Label(foot, textvariable=self.size, anchor="e").pack(side="right")

        self.refresh()

    # ------------------------------------------------------------ widgets
    def _panel(self, root, col, title, cap, on_add, on_del):
        box = ttk.LabelFrame(root, text="%s（最多 %d）" % (title, cap),
                             padding=8)
        box.grid(row=1, column=col, sticky="nsew", padx=(8 if col == 0 else 4, 8),
                 pady=8)
        box.columnconfigure(0, weight=1)
        box.rowconfigure(0, weight=1)

        listbox = tk.Listbox(box, height=12, activestyle="dotbox",
                             exportselection=False)
        listbox.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(box, orient="vertical", command=listbox.yview)
        sb.grid(row=0, column=1, sticky="ns")
        listbox.configure(yscrollcommand=sb.set)

        edit = ttk.Frame(box)
        edit.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        edit.columnconfigure(0, weight=1)
        entry = ttk.Entry(edit)
        entry.grid(row=0, column=0, sticky="ew")
        ttk.Button(edit, text="添加", width=6,
                   command=lambda: on_add(entry.get())).grid(row=0, column=1, padx=(6, 0))
        ttk.Button(edit, text="删除选中", width=9,
                   command=lambda: on_del(listbox.curselection())).grid(row=0, column=2, padx=(6, 0))

        info = tk.StringVar(value="0 / %d" % cap)
        ttk.Label(box, textvariable=info, anchor="w").grid(row=2, column=0,
                                                          columnspan=2, sticky="w",
                                                          pady=(6, 0))
        entry.bind("<Return>", lambda e: on_add(entry.get()))
        listbox.bind("<Double-Button-1>",
                     lambda e: self._fill_entry(listbox, entry))
        if title.endswith("字"):
            self.chars_entry, self.chars_list, self.chars_info = entry, listbox, info
        else:
            self.words_entry, self.words_list, self.words_info = entry, listbox, info
        return box

    # ------------------------------------------------------------ actions
    def _fill_entry(self, listbox, entry):
        """Double-click an entry to load it into the box; 添加 then replaces it."""
        sel = listbox.curselection()
        if not sel:
            return
        entry.delete(0, "end")
        entry.insert(0, listbox.get(sel[0]))
        entry.focus_set()
        self._say("正在修改选中的一条：改好后按 添加")

    def _set_chars(self, chars):
        self.chars = list(chars)[:core.MAX_CHARS]
        self.refresh()

    def _set_words(self, words):
        self.words = [list(w) for w in words][:core.MAX_WORDS]
        self.refresh()

    def set_data(self, chars, words):
        self.chars = list(chars)[:core.MAX_CHARS]
        self.words = [list(w)[:core.MAX_WORD_LEN] for w in words][:core.MAX_WORDS]
        self.refresh()

    def refresh(self):
        self.chars_list.delete(0, "end")
        for c in self.chars:
            self.chars_list.insert("end", core.decode_cells([c]))
        self.words_list.delete(0, "end")
        for w in self.words:
            self.words_list.insert("end", core.decode_cells(w))
        self.chars_info.set("%d / %d" % (len(self.chars), core.MAX_CHARS))
        self.words_info.set("%d / %d" % (len(self.words), core.MAX_WORDS))
        data = len(core.build_bin(self.chars, self.words))
        self.size.set("数据 %d 字节，文件 %d 字节（%s）"
                      % (data, core.ALLOC,
                         os.path.basename(self.path) if self.path else "未命名"))

    def _say(self, text):
        self.status.set(text)

    def add_char(self, text):
        text = text.strip()
        if not text:
            return
        try:
            cells = core.encode_text(text)
        except ValueError as e:
            messagebox.showerror(APP_TITLE, str(e))
            return
        if len(cells) != 1:
            messagebox.showerror(APP_TITLE, "常用字只能是一个字符")
            return
        sel = self.chars_list.curselection()
        if sel:
            # a selected entry is being edited
            self.chars[sel[0]] = cells[0]
            self.chars_entry.delete(0, "end")
            self.refresh()
            self._say("已改为 %s" % text)
            return
        if cells[0] in self.chars:
            self._say("已经在列表里了")
            return
        if len(self.chars) >= core.MAX_CHARS:
            messagebox.showerror(APP_TITLE, "最多 %d 个常用字" % core.MAX_CHARS)
            return
        self.chars.append(cells[0])
        self.chars_entry.delete(0, "end")
        self.refresh()
        self.chars_list.selection_clear(0, "end")
        self.chars_list.selection_set(len(self.chars) - 1)
        self.chars_list.see(len(self.chars) - 1)
        self._say("已添加 %s" % text)

    def del_char(self, sel):
        if not sel:
            self._say("先在列表里选一个")
            return
        i = sel[0]
        if 0 <= i < len(self.chars):
            self._say("已删除 %s" % core.decode_cells([self.chars[i]]))
            del self.chars[i]
            self.refresh()

    def add_word(self, text):
        text = text.strip()
        if not text:
            return
        try:
            cells = core.encode_text(text)
        except ValueError as e:
            messagebox.showerror(APP_TITLE, str(e))
            return
        if not 0 < len(cells) <= core.MAX_WORD_LEN:
            messagebox.showerror(APP_TITLE, "常用词最多 %d 个字符" % core.MAX_WORD_LEN)
            return
        sel = self.words_list.curselection()
        if sel:
            self.words[sel[0]] = cells
            self.words_entry.delete(0, "end")
            self.refresh()
            self._say("已改为 %s" % text)
            return
        key = core.decode_cells(cells)
        if key in [core.decode_cells(w) for w in self.words]:
            self._say("已经在列表里了")
            return
        if len(self.words) >= core.MAX_WORDS:
            messagebox.showerror(APP_TITLE, "最多 %d 条常用词" % core.MAX_WORDS)
            return
        self.words.append(cells)
        self.words_entry.delete(0, "end")
        self.refresh()
        self.words_list.selection_clear(0, "end")
        self.words_list.selection_set(len(self.words) - 1)
        self.words_list.see(len(self.words) - 1)
        self._say("已添加 %s" % text)

    def del_word(self, sel):
        if not sel:
            self._say("先在列表里选一条")
            return
        i = sel[0]
        if 0 <= i < len(self.words):
            self._say("已删除 %s" % core.decode_cells(self.words[i]))
            del self.words[i]
            self.refresh()

    def clear_all(self):
        if not (self.chars or self.words):
            return
        if not messagebox.askyesno(APP_TITLE, "清空全部常用字和常用词？"):
            return
        self.chars, self.words = [], []
        self.path = None
        self.refresh()
        self._say("已清空")

    def toggle_sample(self):
        if not self.sample.get():
            return
        self.set_data(core.encode_text("的一是不了在人有我他这"),
                      [core.encode_text(w) for w in ("您好", "谢谢", "请稍等", "收到")])
        self._say("已载入示例")

    # ------------------------------------------------------------ files
    def open_text(self):
        p = filedialog.askopenfilename(
            title="打开文本字词库", filetypes=[("文本字词库", "*.txt"), ("所有文件", "*.*")])
        if p:
            self.load_path(p)

    def open_bin(self):
        p = filedialog.askopenfilename(
            title="打开二进制字词库",
            filetypes=[("计算器字词库", "*.IME_* *.ime_*"), ("所有文件", "*.*")])
        if p:
            self.load_path(p)

    def load_path(self, path):
        try:
            chars, words = core.load_any(path)
        except (ValueError, OSError) as e:
            messagebox.showerror(APP_TITLE, "打不开 %s\n%s" % (os.path.basename(path), e))
            return
        self.set_data(chars, words)
        self.path = path
        self.refresh()
        self._say("已载入 %s（%d 字 / %d 词）"
                  % (os.path.basename(path), len(self.chars), len(self.words)))

    def save_bin(self):
        p = filedialog.asksaveasfilename(
            title="保存为二进制字词库", defaultextension="",
            initialfile="FXTXT.IME_1",
            filetypes=[("计算器字词库", "*.IME_*"), ("所有文件", "*.*")])
        if p:
            self._save(p, binary=True)

    def save_text(self):
        p = filedialog.asksaveasfilename(
            title="保存为文本字词库", defaultextension=".txt",
            initialfile="words.txt",
            filetypes=[("文本字词库", "*.txt"), ("所有文件", "*.*")])
        if p:
            self._save(p, binary=False)

    def _save(self, path, binary):
        try:
            if binary:
                n = core.save_bin(path, self.chars, self.words)
                what = "二进制"
            else:
                n = core.save_text(path, self.chars, self.words)
                what = "文本"
        except OSError as e:
            messagebox.showerror(APP_TITLE, "写不了 %s\n%s" % (os.path.basename(path), e))
            return
        self.path = path
        self.refresh()
        self._say("已保存%s %s（%d 字节）" % (what, os.path.basename(path), n))


def selftest():
    """Drive the window without a user: load a text file, save binary, reload."""
    root = tk.Tk()
    root.withdraw()
    app = App(root)
    text = ("# sample\n[chars]\n的\n一\n你\n[words]\n您好\n谢谢\n")
    tp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".tmp", "gui_in.txt")
    tp = os.path.abspath(tp)
    with open(tp, "wb") as f:
        f.write(text.encode("cp936"))
    app.load_path(tp)
    assert len(app.chars) == 3 and len(app.words) == 2, (app.chars, app.words)
    app.add_char("好")
    app.add_word("再见")
    assert len(app.chars) == 4 and len(app.words) == 3
    bp = os.path.abspath(os.path.join(os.path.dirname(tp), "gui_out.IME_1"))
    app._save(bp, binary=True)
    assert os.path.getsize(bp) == core.ALLOC, os.path.getsize(bp)
    app.set_data([], [])
    app.load_path(bp)
    assert core.decode_cells([app.chars[3]]) == "好", app.chars
    assert core.decode_cells(app.words[2]) == "再见", app.words
    app.chars_list.selection_set(0)
    app.del_char((0,))
    assert len(app.chars) == 3
    txt = os.path.abspath(os.path.join(os.path.dirname(tp), "gui_out.txt"))
    app._save(txt, binary=False)
    back = open(txt, "rb").read().decode("cp936")
    assert "[words]" in back and "再见" in back, back
    root.destroy()
    print("selftest ok: 4 chars / 3 words round trip through the window")
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "--selftest":
        return selftest()
    root = tk.Tk()
    App(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

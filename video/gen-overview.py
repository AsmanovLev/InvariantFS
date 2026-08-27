#!/usr/bin/env python3
"""
gen-overview.py -- renders the InvariantFS overview slide deck.

20 slides, 1920x1080 PNG, dark theme, Russian captions.
Deterministic: no randomness, fixed fonts, fixed geometry.
Output: frames/slide-01.png .. frames/slide-20.png + frames/filter.txt
(the ffmpeg filtergraph consumed by build.sh).
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "frames")

# ---------------------------------------------------------------- palette ---
BG     = (14, 17, 22)      # 0e1116
PANEL  = (22, 28, 36)
PANEL2 = (28, 36, 48)
LINE   = (44, 55, 70)
ACCENT = (76, 194, 255)    # 4cc2ff
TEXT   = (230, 230, 230)   # e6e6e6
DIM    = (152, 165, 180)
FAINT  = (106, 118, 132)
GREEN  = (126, 231, 135)
ORANGE = (255, 180, 84)
PURPLE = (188, 140, 255)
RED    = (255, 122, 122)
YELLOW = (240, 220, 110)

FDIR   = "/usr/share/fonts"
SANS   = FDIR + "/liberation-sans-fonts/LiberationSans-Regular.ttf"
SANS_B = FDIR + "/liberation-sans-fonts/LiberationSans-Bold.ttf"
MONO   = FDIR + "/liberation-mono-fonts/LiberationMono-Regular.ttf"
MONO_B = FDIR + "/liberation-mono-fonts/LiberationMono-Bold.ttf"

TOTAL = 20
DUR   = 7.0          # seconds per slide
FADE  = 0.45         # fade in/out seconds

_fc = {}
def F(path, size):
    key = (path, size)
    if key not in _fc:
        _fc[key] = ImageFont.truetype(path, size)
    return _fc[key]

def tw(d, s, font):
    return d.textlength(s, font=font)

# ---------------------------------------------------------------- helpers ---
def new_slide(idx, kicker, title):
    """Standard slide scaffold: header + accent rule + footer."""
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((90, 44), kicker, font=F(MONO, 30), fill=ACCENT)
    d.text((90, 86), title, font=F(SANS_B, 62), fill=TEXT)
    d.line((90, 176, W - 90, 176), fill=LINE, width=2)
    d.line((90, 176, 330, 176), fill=ACCENT, width=5)
    d.text((90, 1018), "InvariantFS — обзор архитектуры",
           font=F(MONO, 26), fill=FAINT)
    d.text((W - 90, 1018), "%02d / %d" % (idx, TOTAL),
           font=F(MONO, 26), fill=FAINT, anchor="ra")
    return img, d

def bullets(d, x, y, items, size=38, step=66, color=TEXT, dot=ACCENT):
    fnt = F(SANS, size)
    cy = y
    for it in items:
        c = color
        if isinstance(it, tuple):
            it, c = it
        d.ellipse((x, cy + size // 2 - 5, x + 10, cy + size // 2 + 5), fill=dot)
        d.text((x + 32, cy), it, font=fnt, fill=c)
        cy += step
    return cy

def chip(d, x, y, s, size=34, fg=ACCENT, bg=PANEL2, outline=LINE,
         mono=True, padx=22, h=62):
    fnt = F(MONO if mono else SANS, size)
    w = tw(d, s, fnt) + padx * 2
    d.rounded_rectangle((x, y, x + w, y + h), radius=12,
                        fill=bg, outline=outline, width=2)
    d.text((x + padx, y + h / 2), s, font=fnt, fill=fg, anchor="lm")
    return w

def arrow(d, x1, y1, x2, y2, color=ACCENT, w=6, head=20):
    d.line((x1, y1, x2, y2), fill=color, width=w)
    ang = math.atan2(y2 - y1, x2 - x1)
    for da in (math.radians(152), math.radians(-152)):
        d.line((x2, y2,
                x2 + head * math.cos(ang + da),
                y2 + head * math.sin(ang + da)), fill=color, width=w)

def table(d, x, y, colw, rows, rowh=62, pad=16, fsize=34):
    """rows: list of rows; cell = str or (str, fontpath, size, color).
    Row 0 = header (accent, mono bold). '@' cell -> green dot."""
    total_w = sum(colw)
    yy = y
    for ri, row in enumerate(rows):
        hdr = ri == 0
        fill = PANEL2 if hdr else (PANEL if ri % 2 == 1 else BG)
        d.rectangle((x, yy, x + total_w, yy + rowh), fill=fill)
        xx = x
        for ci, cell in enumerate(row):
            fnt = F(MONO_B, fsize) if hdr else F(MONO, fsize)
            col = ACCENT if hdr else TEXT
            s = cell
            if isinstance(cell, tuple):
                s, fp, fs, col = cell
                fnt = F(fp, fs)
            if s == "@":
                d.ellipse((xx + pad, yy + rowh / 2 - 11,
                           xx + pad + 22, yy + rowh / 2 + 11), fill=GREEN)
            else:
                d.text((xx + pad, yy + rowh / 2), s, font=fnt, fill=col,
                       anchor="lm")
            xx += colw[ci]
        yy += rowh
    # grid
    d.rectangle((x, y, x + total_w, yy), outline=LINE, width=2)
    xx = x
    for wcol in colw[:-1]:
        xx += wcol
        d.line((xx, y, xx, yy), fill=LINE, width=2)
    for ri in range(1, len(rows)):
        d.line((x, y + ri * rowh, x + total_w, y + ri * rowh),
               fill=LINE, width=2)
    return yy

def hbars(d, x, y, w, rows, vmax, barh=64, step=104, label_w=560):
    """rows: (label, value_text, value, color)."""
    fnt = F(SANS, 38)
    vfnt = F(MONO_B, 38)
    cy = y
    for label, vtext, val, color in rows:
        d.text((x, cy + barh / 2), label, font=fnt, fill=TEXT, anchor="lm")
        bw = max(8, int((w - label_w - 200) * val / vmax))
        d.rounded_rectangle((x + label_w, cy, x + label_w + bw, cy + barh),
                            radius=8, fill=color)
        vw = tw(d, vtext, vfnt)
        if vw + 40 <= bw:  # value inside the bar
            d.text((x + label_w + bw - 16, cy + barh / 2), vtext,
                   font=vfnt, fill=BG, anchor="rm")
        else:
            d.text((x + label_w + bw + 18, cy + barh / 2), vtext,
                   font=vfnt, fill=color, anchor="lm")
        cy += step
    return cy

def note(d, x, y, s, size=30, color=DIM, mono=True):
    d.text((x, y), s, font=F(MONO if mono else SANS, size), fill=color)

# ---------------------------------------------------------------- slides ----
def s01():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((W / 2, 250), "InvariantFS", font=F(SANS_B, 132), fill=ACCENT,
           anchor="mm")
    d.text((W / 2, 360), "архивная ФС со sweep-перепаковкой",
           font=F(SANS, 56), fill=TEXT, anchor="mm")
    d.line((W / 2 - 420, 430, W / 2 + 420, 430), fill=LINE, width=2)
    # pipeline
    steps = ["запись", "RAW-зона", "sweep", "перепаковано"]
    widths = [tw(d, s, F(MONO_B, 40)) + 56 for s in steps]
    gap = 90
    total = sum(widths) + gap * (len(steps) - 1)
    x = (W - total) / 2
    y = 520
    for i, (s, w) in enumerate(zip(steps, widths)):
        fg = ACCENT if i in (0, 3) else TEXT
        d.rounded_rectangle((x, y, x + w, y + 86), radius=16,
                            fill=PANEL2, outline=fg, width=3)
        d.text((x + w / 2, y + 44), s, font=F(MONO_B, 40), fill=fg,
               anchor="mm")
        if i < len(steps) - 1:
            arrow(d, x + w + 12, y + 43, x + w + gap - 12, y + 43)
        x += w + gap
    d.text((W / 2, 760), "Каждое сжатие проверено декодом и memcmp",
           font=F(SANS, 42), fill=DIM, anchor="mm")
    d.text((W / 2, 830), "том валиден в любой момент времени",
           font=F(SANS, 42), fill=DIM, anchor="mm")
    d.text((W / 2, 1000), "обзор по doc/16 · impl_docs/AUDIT.md · WP10–WP14",
           font=F(MONO, 26), fill=FAINT, anchor="mm")
    return img

def s02(idx):
    img, d = new_slide(idx, "ЗАЧЕМ", "Главный инвариант")
    d.text((90, 230), "Файл читается байт-в-байт так,",
           font=F(SANS_B, 52), fill=TEXT)
    d.text((90, 300), "как был записан.", font=F(SANS_B, 52), fill=ACCENT)
    bullets(d, 90, 450, [
        "Любое сжатие проверяется: decode + memcmp до коммита",
        "Проверка не прошла — данные остаются как есть",
        "Контейнеры: оригинал verbatim либо рецепт + части",
        "FUSE-демон invf-fuse и офлайн-утилита invf-sweep",
        "Gentoo/OpenRC загружается прямо с тома (vm/)",
    ], size=40, step=76)
    note(d, 90, 940, "инвариант важнее экономии: сомнительное сжатие не применяется")
    return img

def s03(idx):
    img, d = new_slide(idx, "ФОРМАТ", "Разметка тома")
    x0, x1 = 90, W - 90
    total = x1 - x0
    y, h = 400, 150
    # proportions: SB sliver, metadata 8%, RAW 20%, shadow rest-2.5%, inode 2.5%
    w_sb    = 16
    w_meta  = int(total * 0.08)
    w_raw   = int(total * 0.20)
    w_inode = int(total * 0.025)
    w_shad  = total - w_sb - w_meta - w_raw - w_inode
    parts = [
        ("SB", w_sb, PANEL2, DIM),
        ("Metadata ~64 МБ", w_meta, (60, 72, 92), TEXT),
        ("RAW-зона — 1/5 тома", w_raw, (120, 82, 28), ORANGE),
        ("Shadow Space — перепакованные данные", w_shad, (24, 60, 88), ACCENT),
        ("inode", w_inode, (66, 46, 100), PURPLE),
    ]
    x = x0
    for name, w, fill, fg in parts:
        d.rectangle((x, y, x + w, y + h), fill=fill, outline=LINE, width=2)
        if w > 100:
            d.text((x + w / 2, y + h / 2), name, font=F(SANS_B, 36), fill=fg,
                   anchor="mm")
        x += w
    # journal sub-box inside metadata
    jx = x0 + w_sb + 8
    d.rectangle((jx, y + h - 46, jx + w_meta - 16, y + h - 8),
                fill=(38, 46, 60), outline=LINE)
    d.text((jx + (w_meta - 16) / 2, y + h - 27), "журнал 32 МБ",
           font=F(MONO, 24), fill=TEXT, anchor="mm")
    # leader labels
    def leader(bx, ty, s, fg, anchor="ma"):
        d.line((bx, y + h + 6, bx, ty - 8), fill=LINE, width=2)
        d.text((bx, ty), s, font=F(MONO, 26), fill=fg, anchor=anchor)
    d.line((x0 + w_sb / 2, y + h + 6, x0 + w_sb / 2, y + h + 36),
           fill=LINE, width=2)
    d.text((x0, y + h + 44), "superblock 4 КБ", font=F(MONO, 26), fill=DIM,
           anchor="la")
    leader(x0 + w_sb + w_meta + w_raw / 2, y + h + 44,
           "линейная запись · LZ4 · 64 КБ", ORANGE)
    d.line((x0 + w_sb + w_meta + w_raw + w_shad / 2, y + h + 6,
            x0 + w_sb + w_meta + w_raw + w_shad / 2, y + h + 36),
           fill=LINE, width=2)
    d.text((x0 + w_sb + w_meta + w_raw + w_shad, y + h + 44),
           "TEXT-батчи · ZSTD-сегменты · рецепты", font=F(MONO, 26),
           fill=ACCENT, anchor="ra")
    d.text((x1, y - 44), "inode area — в конце тома · append-only",
           font=F(MONO, 26), fill=PURPLE, anchor="ra")
    d.line((x1 - w_inode / 2, y - 8, x1 - w_inode / 2, y - 34),
           fill=LINE, width=2)
    bullets(d, 90, 720, [
        "RAW: новые данные, LZ4-сегменты 64 КБ, ~нулевая задержка write()",
        "Shadow: сюда sweep перепаковывает — TEXT и BINARY формы",
        "Metadata: bitmap 1 бит/блок + L2P-журнал, replay при монтировании",
    ], size=38, step=68)
    return img

def s04(idx):
    img, d = new_slide(idx, "ФОРМАТ", "Зоны: кто за что отвечает")
    cards = [
        ("Superblock · 4 КБ", DIM,
         "размеры зон, состояние CLEAN/DIRTY,",
         "CRC32C всего блока"),
        ("Metadata · ~64 МБ", TEXT,
         "bitmap: 1 бит на блок тома;",
         "L2P-журнал 32 МБ, append-only"),
        ("RAW · 1/5 тома", ORANGE,
         "линейная запись новых файлов;",
         "LZ4, независимые сегменты 64 КБ"),
        ("Shadow · ~4/5 тома", ACCENT,
         "перепакованные данные: TEXT-батчи,",
         "ZSTD-сегменты, контейнерные формы"),
    ]
    cw, ch, gx, gy = 840, 250, 60, 60
    for i, (title, fg, l1, l2) in enumerate(cards):
        cx = 90 + (i % 2) * (cw + gx)
        cy = 230 + (i // 2) * (ch + gy)
        d.rounded_rectangle((cx, cy, cx + cw, cy + ch), radius=18,
                            fill=PANEL, outline=LINE, width=2)
        d.rectangle((cx, cy, cx + 10, cy + ch), fill=fg)
        d.text((cx + 40, cy + 44), title, font=F(SANS_B, 44), fill=fg)
        d.text((cx + 40, cy + 120), l1, font=F(SANS, 36), fill=TEXT)
        d.text((cx + 40, cy + 172), l2, font=F(SANS, 36), fill=TEXT)
    d.rounded_rectangle((90, 830, W - 90, 930), radius=18,
                        fill=PANEL, outline=PURPLE, width=2)
    d.text((130, 880), "inode area — в конце тома", font=F(SANS_B, 40),
           fill=PURPLE, anchor="lm")
    d.text((130 + 640, 880), "append-only INOD/DELT записи; tombstone бьёт по позиции",
           font=F(SANS, 36), fill=TEXT, anchor="lm")
    return img

def s05(idx):
    img, d = new_slide(idx, "АРХИТЕКТУРА", "От RAW к классам хранения")
    y = 330
    w1 = chip(d, 90, y, "файл", size=40, fg=TEXT, h=84)
    arrow(d, 90 + w1 + 16, y + 42, 360, y + 42)
    d.rounded_rectangle((370, y - 10, 780, y + 94), radius=16,
                        fill=(120, 82, 28), outline=ORANGE, width=3)
    d.text((575, y + 42), "RAW: LZ4 · 64 КБ", font=F(MONO_B, 36),
           fill=ORANGE, anchor="mm")
    arrow(d, 796, y + 42, 880, y + 42)
    d.rounded_rectangle((890, y - 10, 1060, y + 94), radius=16,
                        fill=PANEL2, outline=TEXT, width=3)
    d.text((975, y + 42), "sweep", font=F(MONO_B, 40), fill=TEXT, anchor="mm")
    # branches
    arrow(d, 1076, y + 20, 1200, y - 60)
    arrow(d, 1076, y + 64, 1200, y + 150)
    d.rounded_rectangle((1210, y - 110, 1830, y - 10), radius=16,
                        fill=PANEL, outline=ACCENT, width=3)
    d.text((1520, y - 74), "TEXT: PPMd-батчи по 4 МБ", font=F(SANS_B, 36),
           fill=ACCENT, anchor="mm")
    d.text((1520, y - 34), "cross-file контекст сотен файлов",
           font=F(SANS, 28), fill=DIM, anchor="mm")
    d.rounded_rectangle((1210, y + 110, 1830, y + 210), radius=16,
                        fill=PANEL, outline=GREEN, width=3)
    d.text((1520, y + 146), "BINARY: ZSTD-19 · JXL · рецепты",
           font=F(SANS_B, 36), fill=GREEN, anchor="mm")
    d.text((1520, y + 186), "по типу контента", font=F(SANS, 28), fill=DIM,
           anchor="mm")
    # class flag
    d.rounded_rectangle((90, 650, W - 90, 760), radius=16, fill=PANEL2,
                        outline=ACCENT, width=2)
    d.text((960, 692), "invfs.class = { cls u8,  algo u8,  gen u16 }",
           font=F(MONO_B, 40), fill=ACCENT, anchor="mm")
    d.text((960, 738), "xattr-метка: почему файл хранится именно так",
           font=F(SANS, 32), fill=TEXT, anchor="mm")
    bullets(d, 90, 830, [
        "класс пишется только при изменении (check-then-write)",
        "переименование и хардлинки сохраняют метку",
        "генерация кодека в метке → умный retry при обновлении",
    ], size=36, step=58)
    return img

def s06(idx):
    img, d = new_slide(idx, "АРХИТЕКТУРА", "invfs.class: 7+1 классов")
    rows = [
        ["#", "класс", "что это значит"],
        ["1", "UNCOMPRESSIBLE", "выгода сжатия < 0.5% — хранить как есть"],
        ["2", "CODEC", "доменный кодек: JXL · APE · PMP"],
        ["3", "CONTAINER", "TAR · ZIP · GZ · PNG · EXER"],
        ["4", "GENERIC", "ZSTD-19 по сегментам 64 КБ"],
        ["5", "GENERIC_MEMLIMIT", "кодек не прошёл лимит памяти"],
        ["6", "GENERIC_GUARD", "guard отклонил: не bit-exact"],
        ["7", "TEXT", "член PPMd-батча Text Zone"],
        ["—", "(нет метки)", "sweep ещё не видел файл"],
    ]
    table(d, 90, 220, [110, 620, 1010], rows, rowh=82, fsize=36)
    note(d, 90, 950, "on-disk: 4 байта {cls, algo, gen} — xattr invfs.class в INO2 ext-блоке")
    return img

def s07(idx):
    img, d = new_slide(idx, "АРХИТЕКТУРА", "Реестр кодеков")
    hdr = ["algo", "кодек", "seek", "batched", "wholefile", "container", "external"]
    def row(algo, name, caps):
        return [algo, name] + ["@" if c else "—" for c in caps]
    rows = [
        hdr,
        row("NONE", "store",            [1, 0, 0, 0, 0]),
        row("LZ4", "RAW-зона",          [1, 0, 0, 0, 0]),
        row("ZSTD", "generic -19",      [1, 0, 0, 0, 0]),
        row("PPMD", "Text Zone o=8",    [0, 1, 0, 0, 0]),
        row("JXL", "JPEG → JXL",        [0, 0, 1, 0, 1]),
        row("RIMG", "raw_image pack",   [0, 0, 1, 1, 1]),
        row("PMP / APE", "аудио · MP3", [0, 0, 1, 0, 1]),
    ]
    # color the "—" dim
    fixed = []
    for ri, r in enumerate(rows):
        fr = []
        for ci, c in enumerate(r):
            if c == "—":
                fr.append(("—", MONO, 34, FAINT))
            elif ri > 0 and ci == 0:
                fr.append((c, MONO_B, 34, ACCENT))
            else:
                fr.append(c)
        fixed.append(fr)
    table(d, 90, 220, [260, 420, 180, 200, 220, 230, 210], fixed,
          rowh=84, fsize=34)
    note(d, 90, 930, "caps — свойства юнита декодирования;  probe() проверяет наличие внешнего тулза")
    note(d, 90, 972, "sniff: магия > расширение > эвристика;  registry order = приоритет sniff")
    return img

def s08(idx):
    img, d = new_slide(idx, "АРХИТЕКТУРА", "Как sweep выбирает кодек")
    steps = ["sniff", "политика", "transcode", "guard", "stamp"]
    widths = [tw(d, s, F(MONO_B, 38)) + 52 for s in steps]
    gap = 56
    total = sum(widths) + gap * (len(steps) - 1)
    x = (W - total) / 2
    y = 240
    for i, (s, w) in enumerate(zip(steps, widths)):
        fg = ACCENT if i in (0, 4) else TEXT
        d.rounded_rectangle((x, y, x + w, y + 80), radius=14, fill=PANEL2,
                            outline=fg, width=3)
        d.text((x + w / 2, y + 41), s, font=F(MONO_B, 38), fill=fg,
               anchor="mm")
        if i < len(steps) - 1:
            arrow(d, x + w + 8, y + 40, x + w + gap - 8, y + 40, head=16)
        x += w + gap
    bullets(d, 90, 430, [
        "sniff: баллы уверенности — магия > расширение > эвристика",
        "политика: допуск по dec_mem_limit и arc_limit",
        "guard: decode + memcmp; провал → fallback generic",
        "stamp: класс + algo + generation в invfs.class",
    ], size=40, step=72)
    d.rounded_rectangle((90, 790, W - 90, 930), radius=16, fill=PANEL,
                        outline=LINE, width=2)
    d.text((130, 838), "probe внешних тулзов:", font=F(SANS_B, 36),
           fill=TEXT, anchor="lm")
    d.text((130, 892), "$INVFS_TOOLS → /usr/lib/invfs/tools → PATH",
           font=F(MONO, 31), fill=ACCENT, anchor="lm")
    d.text((1060, 838), "нет тулза → файлы остаются", font=F(SANS, 34),
           fill=DIM, anchor="lm")
    d.text((1060, 892), "читаемыми generic, не EIO", font=F(SANS, 34),
           fill=DIM, anchor="lm")
    return img

def s09(idx):
    img, d = new_slide(idx, "TEXT ZONE", "Text Zone: cross-file батчинг")
    files = ["main.c", "util.h", "app.py", "web.js", "data.json", "notes.md"]
    y = 250
    for i, f in enumerate(files):
        chip(d, 90, y + i * 74, f, size=30, fg=TEXT, h=58)
    d.text((240, y + 6 * 74 + 10), "… сотни мелких файлов",
           font=F(SANS, 30), fill=DIM, anchor="lm")
    arrow(d, 470, y + 210, 640, y + 210)
    d.text((555, y + 160), "сортировка", font=F(SANS, 30), fill=DIM,
           anchor="mm")
    d.text((555, y + 196), "(тип, размер)", font=F(SANS, 30), fill=DIM,
           anchor="mm")
    # batch box
    bx, bw = 660, 460
    d.rounded_rectangle((bx, y + 20, bx + bw, y + 400), radius=18,
                        fill=PANEL, outline=ACCENT, width=3)
    d.text((bx + bw / 2, y + 60), "батч 4 МБ", font=F(SANS_B, 44),
           fill=ACCENT, anchor="mm")
    segc = [TEXT, TEXT, DIM, DIM, FAINT]
    for i in range(5):
        d.rectangle((bx + 40, y + 110 + i * 52, bx + 40 + 380 - i * 40,
                     y + 110 + i * 52 + 40), fill=(36, 48, 64),
                    outline=segc[i], width=2)
    arrow(d, bx + bw + 20, y + 210, bx + bw + 130, y + 210)
    d.text((bx + bw + 75, y + 165), "PPMd", font=F(MONO_B, 34), fill=ACCENT,
           anchor="mm")
    d.text((bx + bw + 75, y + 250), "o=8 · 64МБ", font=F(MONO, 28), fill=DIM,
           anchor="mm")
    d.rounded_rectangle((bx + bw + 150, y + 150, bx + bw + 420, y + 270),
                        radius=16, fill=PANEL2, outline=GREEN, width=3)
    d.text((bx + bw + 285, y + 196), "1 сегмент", font=F(SANS_B, 38),
           fill=GREEN, anchor="mm")
    d.text((bx + bw + 285, y + 240), "csize + crc32c", font=F(MONO, 26),
           fill=DIM, anchor="mm")
    bullets(d, 90, 740, [
        "ReiserFS-style: много мелких файлов в одном блоке",
        "сортировка (family, size): +26.6% ratio (B29)",
        "PPMd на батче: +12% плотности над ZSTD-19 (B29.5)",
    ], size=38, step=66)
    return img

def s10(idx):
    img, d = new_slide(idx, "TEXT ZONE", "Анатомия батча")
    # owner
    d.rounded_rectangle((90, 225, 560, 300), radius=14, fill=PANEL2,
                        outline=PURPLE, width=3)
    d.text((325, 262), "владелец: \\x01tzb", font=F(MONO_B, 38), fill=PURPLE,
           anchor="mm")
    arrow(d, 325, 310, 325, 360)
    # batch
    bx, by, bw, bh = 90, 370, 1140, 170
    d.rounded_rectangle((bx, by, bx + bw, by + bh), radius=14, fill=PANEL,
                        outline=ACCENT, width=3)
    d.text((bx + 20, by - 6), "декодированный батч", font=F(SANS, 28),
           fill=DIM, anchor="lb")
    members = [("main.c", 230), ("util.h", 180), ("app.py", 260),
               ("web.js", 300), ("…", 120)]
    x = bx + 14
    for name, w in members:
        d.rectangle((x, by + 30, x + w, by + bh - 30), fill=(36, 48, 64),
                    outline=LINE, width=2)
        d.text((x + w / 2, by + bh / 2), name, font=F(MONO, 30), fill=TEXT,
               anchor="mm")
        x += w + 10
    # offset ticks
    d.line((bx + 14, by + bh + 8, bx + 14, by + bh + 26), fill=DIM, width=3)
    d.text((bx + 14, by + bh + 34), "0", font=F(MONO, 26), fill=DIM,
           anchor="ma")
    offx = bx + 14 + 230 + 10 + 180 + 10
    d.line((offx, by + bh + 8, offx, by + bh + 26), fill=ACCENT, width=3)
    d.text((offx, by + bh + 34), "block_offset", font=F(MONO, 26),
           fill=ACCENT, anchor="ma")
    # member AST
    d.rounded_rectangle((1290, 370, 1830, 540), radius=14, fill=PANEL2,
                        outline=LINE, width=2)
    d.text((1310, 400), "member AST:", font=F(SANS_B, 30), fill=TEXT)
    d.text((1310, 446), "{ zone=TEXT,", font=F(MONO, 30), fill=ACCENT)
    d.text((1310, 486), " block_offset,", font=F(MONO, 30), fill=ACCENT)
    d.text((1310, 526), " length }", font=F(MONO, 30), fill=ACCENT)
    bullets(d, 90, 720, [
        "\\x01tzb — скрытый служебный inode, в его AST — батчи",
        "block_offset — смещение среза в декодированном батче",
        "L2P-дубль (member, batch) → pba; ARC кэширует по pba",
        "удаление члена блоки не трогает — чистит GC mark-and-sweep",
    ], size=36, step=62)
    return img

def s11(idx):
    img, d = new_slide(idx, "BINARY ZONE", "Бинарный батчинг (WP14a)")
    y = 250
    w1 = chip(d, 90, y + 40, "exe-файлы", size=38, fg=TEXT, h=80)
    arrow(d, 90 + w1 + 16, y + 80, 400, y + 80)
    d.rounded_rectangle((410, y + 30, 720, y + 130), radius=14, fill=PANEL,
                        outline=ORANGE, width=3)
    d.text((565, y + 66), "семейство", font=F(SANS_B, 38), fill=ORANGE,
           anchor="mm")
    d.text((565, y + 110), "family sort", font=F(MONO, 28), fill=DIM,
           anchor="mm")
    arrow(d, 736, y + 80, 830, y + 80)
    d.rounded_rectangle((840, y + 30, 1140, y + 130), radius=14, fill=PANEL,
                        outline=ACCENT, width=3)
    d.text((990, y + 66), "BCJ-фильтр", font=F(SANS_B, 38), fill=ACCENT,
           anchor="mm")
    d.text((990, y + 110), "x86 call/jump", font=F(MONO, 28), fill=DIM,
           anchor="mm")
    arrow(d, 1156, y + 80, 1256, y + 80)
    d.rounded_rectangle((1266, y + 30, 1700, y + 130), radius=14, fill=PANEL,
                        outline=GREEN, width=3)
    d.text((1483, y + 66), "ZSTD-батч", font=F(SANS_B, 38), fill=GREEN,
           anchor="mm")
    d.text((1483, y + 110), "общий контекст", font=F(MONO, 28), fill=DIM,
           anchor="mm")
    # family chips
    fams = ["ELF x86", "ELF x86-64", "ELF ARM64", "PE", "Mach-O"]
    x = 90
    for f in fams:
        x += chip(d, x, 480, f, size=34, fg=TEXT) + 24
    bullets(d, 90, 640, [
        "BCJ нормализует адреса x86 call/jump перед ZSTD",
        "батч собирается внутри семейства — общий контекст",
        "Go-бинарники — без BCJ: sniff это проверяет (B9)",
        "algo = ZSTD(BCJ=14), зона TEXT, класс 8",
    ], size=38, step=68)
    return img

def s12(idx):
    img, d = new_slide(idx, "BINARY ZONE", "Эффект на Silesia (данные)")
    rows = [
        ("WP10 · текст",       "75.29 МБ · 2.82x", 75.29, FAINT),
        ("WP12 · dedupe",      "75.08 МБ · 2.82x", 75.08, DIM),
        ("WP14a · бинари",     "74.55 МБ · 2.84x", 74.55, ACCENT),
        ("WP14b · контейнеры", "59.14 МБ · 3.58x", 59.14, GREEN),
    ]
    hbars(d, 90, 250, 1740, rows, vmax=80.0, barh=70, step=120, label_w=620)
    bullets(d, 90, 800, [
        "WP14a: бинарные батчи → 2.84x",
        "WP14b-M1: батчинг членов контейнеров → 3.58x",
        "WP14b-M2: exe-as-container (EXER)",
    ], size=38, step=62)
    return img

def s13(idx):
    img, d = new_slide(idx, "КОНТЕЙНЕРЫ", "Контейнеры: две формы хранения")
    # left: TAR extraction
    d.rounded_rectangle((90, 230, 950, 800), radius=18, fill=PANEL,
                        outline=ACCENT, width=3)
    d.text((130, 280), "Extraction — TAR", font=F(SANS_B, 48), fill=ACCENT)
    y = 370
    w1 = chip(d, 130, y, "file.tar", size=32, fg=TEXT, h=60)
    arrow(d, 130 + w1 + 14, y + 30, 360, y + 30, head=16)
    chip(d, 370, y, "file.tar!part0", size=30, fg=ACCENT, h=60)
    chip(d, 370, y + 74, "file.tar!part1", size=30, fg=ACCENT, h=60)
    chip(d, 370, y + 148, "… + рецепт", size=30, fg=DIM, h=60)
    bullets(d, 130, 620, [
        "члены → сиблинги-иноды + рецепт",
        "члены батчатся (WP14b-M1)",
        "оригинал пересобирается бит-в-бит",
    ], size=34, step=58)
    # right: ZIP windows
    d.rounded_rectangle((990, 230, 1830, 800), radius=18, fill=PANEL,
                        outline=GREEN, width=3)
    d.text((1030, 280), "Windows — ZIP", font=F(SANS_B, 48), fill=GREEN)
    d.rounded_rectangle((1030, 370, 1790, 500), radius=12, fill=(36, 48, 64),
                        outline=LINE, width=2)
    d.text((1410, 400), "оригинал verbatim", font=F(SANS, 30), fill=TEXT,
           anchor="ma")
    for i, (nm, off) in enumerate([("a.txt", 1060), ("b.png", 1330),
                                   ("c.bin", 1600)]):
        d.rectangle((off, 430, off + 160, 480), fill=PANEL2, outline=GREEN,
                    width=2)
        d.text((off + 80, 455), nm, font=F(MONO, 26), fill=GREEN, anchor="mm")
    bullets(d, 1030, 620, [
        "член = (method, offset, len)",
        "seekable: декод лишь своего окна",
        "вложенность: a.zip!inner.zip!x.txt",
    ], size=34, step=58)
    note(d, 90, 880, "выбор формы — по устройству контейнера: у ZIP сжатые члены seekable, у TAR нет")
    return img

def s14(idx):
    img, d = new_slide(idx, "КОНТЕЙНЕРЫ", "EXER и codecpacks")
    d.rounded_rectangle((90, 230, 950, 810), radius=18, fill=PANEL,
                        outline=ORANGE, width=3)
    d.text((130, 280), "EXER — exe-as-container", font=F(SANS_B, 44),
           fill=ORANGE)
    d.text((130, 340), "WP14b-M2", font=F(MONO, 30), fill=DIM)
    bullets(d, 130, 420, [
        "из ELF/PE/Mach-O вырезаются",
        "встроенные JPEG/PNG ≥ 16 КиБ",
        "JPEG → lossless JXL, PNG → ZSTD",
        "блоб EXER: заголовок + таблица",
        "частей + склейка, ZSTD-19",
        "guard: пересборка + memcmp",
    ], size=34, step=62, dot=ORANGE)
    d.rounded_rectangle((990, 230, 1830, 810), radius=18, fill=PANEL,
                        outline=PURPLE, width=3)
    d.text((1030, 280), "raw_image codecpack", font=F(SANS_B, 44),
           fill=PURPLE)
    d.text((1030, 340), "WP13 · плагин-пакеты", font=F(MONO, 30), fill=DIM)
    bullets(d, 1030, 420, [
        "DICOM · PNM · BMP · TIFF",
        "→ lossless JXL (блоб RIMG)",
        "пак = manifest + bin/, argv фикс.",
        "B31: −43% места против generic",
        "2.43x против 1.91x у xz",
        "sniff по магии, probe по PATH",
    ], size=34, step=62, dot=PURPLE)
    note(d, 90, 880, "codecpack: pack_version ≠ generation;  generation++ → retry для GUARD / UNCOMPRESSIBLE")
    return img

def s15(idx):
    img, d = new_slide(idx, "ПОЛИТИКА", "Политика памяти: admission на sweep")
    d.rounded_rectangle((90, 230, 950, 420), radius=18, fill=PANEL,
                        outline=ACCENT, width=3)
    d.text((130, 300), "dec_mem_limit = 512 МБ", font=F(MONO_B, 44),
           fill=ACCENT)
    d.text((130, 366), "потолок памяти декодера на один юнит",
           font=F(SANS, 36), fill=TEXT)
    d.rounded_rectangle((990, 230, 1830, 420), radius=18, fill=PANEL,
                        outline=GREEN, width=3)
    d.text((1030, 300), "arc_limit = 256 МБ", font=F(MONO_B, 44), fill=GREEN)
    d.text((1030, 366), "бюджет ARC; юнит > ½ бюджета не кэшируется",
           font=F(SANS, 36), fill=TEXT)
    bullets(d, 90, 520, [
        "кодек с dec_mem выше лимита пропускается на sweep",
        "отказ → fallback generic + класс GENERIC_MEMLIMIT",
        "размер батча: min(4 МБ, arc_limit / 2)",
        "оценка по заголовкам, без пробного декода:",
        "PNG IHDR · zip CD · xz/zstd frame header · PPMd props",
    ], size=40, step=72)
    note(d, 90, 930, "отклонение — только на sweep; хранимое читается всегда")
    return img

def s16(idx):
    img, d = new_slide(idx, "ПОЛИТИКА", "Гарантии")
    bullets(d, 90, 250, [
        "read-path никогда не отвергает сохранённые данные",
        "guard = decode + memcmp до коммита",
        "PPMD-батч: обязательная проверка при seal",
        "смена лимитов → re-sweep в обе стороны:",
        "upgrade вдохнувшихся и downgrade нарушителей",
        "crash между seal и commit: члены читаются из RAW,",
        "осиротевший батч соберёт GC",
    ], size=42, step=80)
    d.rounded_rectangle((90, 840, W - 90, 950), radius=16, fill=PANEL2,
                        outline=ACCENT, width=2)
    d.text((960, 896), "всё записанное декодируется в рамках политики последнего sweep",
           font=F(SANS, 38), fill=ACCENT, anchor="mm")
    return img

def s17(idx):
    img, d = new_slide(idx, "БЕНЧМАРКИ", "Silesia 202 МБ: плотность (B30–B33)")
    rows = [
        ("InvariantFS WP14b", "3.58x", 3.58, ACCENT),
        ("btrfs zstd:15",     "3.18x", 3.18, DIM),
        ("tar | zstd -19",    "4.01x", 4.01, FAINT),
        ("tar | xz -6",       "4.28x", 4.28, FAINT),
    ]
    hbars(d, 90, 250, 1740, rows, vmax=4.6, barh=78, step=130, label_w=560)
    bullets(d, 90, 830, [
        "xz выигрывает единым словарём 8 МБ+ поверх всего потока",
        "InvariantFS — единственная ФС в списке, остальные — потоки",
    ], size=38, step=64)
    return img

def s18(idx):
    img, d = new_slide(idx, "БЕНЧМАРКИ", "Чтение: InvariantFS vs btrfs (B34)")
    rows = [
        ["фаза", "btrfs", "InvariantFS"],
        ["rand-4K cold", "0.612 мс", ("0.104 мс — в 6x быстрее", MONO_B, 36, GREEN)],
        ["full-seq cold", "672 МБ/с", ("~21 МБ/с", MONO_B, 36, ORANGE)],
        ["rand-4K warm", "0.023 мс", "0.101 мс"],
        ["full-seq warm", "7194 МБ/с", "1316 МБ/с"],
    ]
    table(d, 90, 230, [620, 520, 600], rows, rowh=88, fsize=36)
    bullets(d, 90, 740, [
        ("честно: full-seq cold 21 vs 672 — «PPMd-такс» на тексте", ORANGE),
        "p99 444 мс = декод 4-МБ батча; соседи потом — из ARC",
        "тёплый разрыв — оверхед FUSE, а не кодеки",
    ], size=38, step=66)
    return img

def s19(idx):
    img, d = new_slide(idx, "ДОРОЖНАЯ КАРТА", "Что дальше")
    items = [
        ("WP16", ACCENT, "containerpacks — пакеты контейнерных кодеков"),
        ("WP17", ACCENT, "FUSE-tuning — чтение, кэши, readahead"),
        ("далее", GREEN, "template zone — chunk-store с refcounts"),
        ("далее", GREEN, "fs-image контейнеры: ext4 .img → члены-файлы"),
        ("отложено", ORANGE, "H.264 video (WP15): модель сломана, guard спас"),
    ]
    y = 260
    for tag, fg, text in items:
        w = chip(d, 90, y, tag, size=32, fg=fg, h=64)
        d.text((90 + w + 40, y + 33), text, font=F(SANS, 42), fill=TEXT,
               anchor="lm")
        y += 118
    d.text((90, 900), "template zone консолидирует горячие сэмплы между файлами;",
           font=F(SANS, 34), fill=DIM)
    d.text((90, 950), "fs-image: VM-образы получают cross-file контекст",
           font=F(SANS, 34), fill=DIM)
    return img

def s20():
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((W / 2, 300), "InvariantFS", font=F(SANS_B, 110), fill=ACCENT,
           anchor="mm")
    d.text((W / 2, 420), "инвариант важнее экономии", font=F(SANS, 54),
           fill=TEXT, anchor="mm")
    steps = ["запись", "RAW", "sweep", "перепаковано"]
    widths = [tw(d, s, F(MONO, 34)) + 48 for s in steps]
    gap = 70
    total = sum(widths) + gap * (len(steps) - 1)
    x = (W - total) / 2
    for i, (s, w) in enumerate(zip(steps, widths)):
        d.rounded_rectangle((x, 560, x + w, 560 + 70), radius=12,
                            fill=PANEL2, outline=LINE, width=2)
        d.text((x + w / 2, 596), s, font=F(MONO, 34), fill=TEXT, anchor="mm")
        if i < len(steps) - 1:
            arrow(d, x + w + 10, 595, x + w + gap - 10, 595, w=4, head=14)
        x += w + gap
    d.text((W / 2, 760), "источники чисел: doc/16-benchmarks.md (B30–B34)",
           font=F(MONO, 30), fill=DIM, anchor="mm")
    d.text((W / 2, 810), "impl_docs/AUDIT.md · WP10–WP14 · README-LINUX.md",
           font=F(MONO, 30), fill=DIM, anchor="mm")
    return img

SLIDES = [
    lambda i: s01(),
    s02, s03, s04, s05, s06, s07, s08, s09, s10,
    s11, s12, s13, s14, s15, s16, s17, s18, s19,
    lambda i: s20(),
]

def write_filtergraph(path):
    """Fixed fade-to-background transitions + concat; consumed by build.sh."""
    lines = []
    for i in range(TOTAL):
        lines.append(
            "[%d:v]fps=30,format=yuv420p,setsar=1,"
            "fade=t=in:st=0:d=%.2f:color=0x0e1116,"
            "fade=t=out:st=%.2f:d=%.2f:color=0x0e1116[v%d];"
            % (i, FADE, DUR - FADE, FADE, i))
    lines.append("".join("[v%d]" % i for i in range(TOTAL))
                 + "concat=n=%d:v=1:a=0[v]" % TOTAL)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")

def verify_frames(files):
    """Re-open a few saved frames, confirm real rendered content (not blank,
    not tofu-only): size, palette richness, ink coverage in text zones."""
    import numpy as np
    for idx in (0, 6, 17):
        p = files[idx]
        img = Image.open(p)
        assert img.size == (W, H), (p, img.size)
        a = np.asarray(img)
        colors = len(np.unique(a.reshape(-1, 3), axis=0))
        nonbg = float((np.abs(a.astype(int) - BG).sum(axis=2) > 30).mean())
        print("verify %s: %dx%d, %d colors, non-bg %.1f%%"
              % (os.path.basename(p), img.size[0], img.size[1], colors,
                 nonbg * 100))
        assert colors > 40, "suspiciously few colors — text missing?"
        assert 0.03 < nonbg < 0.95, "frame looks blank or corrupted"
    print("verify: OK (text rendered, Cyrillic fonts loaded:",
          os.path.basename(SANS_B) + ")")

def main():
    os.makedirs(OUT, exist_ok=True)
    # font sanity: Cyrillic must produce ink
    probe = ImageFont.truetype(SANS_B, 48)
    m = probe.getmask("Журнал")
    assert m.getbbox() is not None, "Cyrillic glyphs missing in " + SANS_B

    files = []
    for i, fn in enumerate(SLIDES, start=1):
        img = fn(i)
        p = os.path.join(OUT, "slide-%02d.png" % i)
        img.save(p, optimize=False)
        files.append(p)
        print("rendered slide-%02d.png" % i)
    write_filtergraph(os.path.join(OUT, "filter.txt"))
    print("wrote frames/filter.txt (n=%d, dur=%.1fs each, total %.1fs)"
          % (TOTAL, DUR, TOTAL * DUR))
    verify_frames(files)

if __name__ == "__main__":
    sys.exit(main())

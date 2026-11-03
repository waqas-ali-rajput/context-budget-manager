"""Motion-graphics video for the CS-251 CEP (Pillow frames, numpy SFX, ffmpeg encode).

  python render.py frames <t1> [t2 ...]   save preview PNGs to build/preview/
  python render.py sheet                  save a contact sheet of the whole video to build/preview/sheet.png
  python render.py render                 render the silent video in parallel segments
  python render.py audio                  build the voice + SFX + music mix
  python render.py final                  join the segments and audio -> CEP_video.mp4

voice.wav is not committed: ffmpeg -i voice.mp3 -af atempo=1.07 -ar 48000 -ac 1 voice.wav

Every animation and SFX time comes from the ElevenLabs voice (words.json), looked up by spoken phrase.
All figures shown come from cep/results/ and real cbm.exe runs.
"""
import json
import math
import random
import re
import subprocess
import sys
import wave
from difflib import SequenceMatcher
from functools import lru_cache
from multiprocessing import Pool
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent))
from make_video import W, H, FPS, font, clamp, prog, eo, eio, back, lerp, mix, rgba, txt, rrect, disc, fade, blit, draw, arrow, wrap, para  # noqa: E402

BUILD = ROOT / "build"
ICONS = ROOT / "icons"
TARGET = 120.0  # seconds

BG0, BG1 = (9, 20, 30), (14, 44, 52)
CARD = (19, 38, 50)
CARD2 = (28, 54, 68)
LINE = (52, 82, 96)
TEXT = (240, 246, 248)
MUTED = (150, 172, 184)
DIM = (86, 110, 124)
TEAL = (38, 196, 186)
ORANGE = (240, 138, 52)
RED = (236, 86, 86)
GREEN = (64, 208, 128)
BLUE = (108, 164, 255)
GOLD = (238, 190, 80)
PURPLE = (170, 130, 225)


# ----------------------------------------------------------------------------- voice timing
def norm(w):
    return re.sub(r"[^a-z0-9]", "", w.lower())


def load_timing():
    md = (ROOT / "SCRIPT.md").read_text(encoding="utf-8").split("## Narration", 1)[1]
    sentences = [s.strip() for s in re.split(r"(?<=[.?!:])[\"”]?\s+", md.strip().replace("\n\n", " ")) if s.strip()]
    tokens = [(t, i) for i, s in enumerate(sentences) for t in s.split() if norm(t)]
    heard = [w for w in json.loads((ROOT / "words.json").read_text(encoding="utf-8")) if norm(w[2])]
    a, b = [norm(t[0]) for t in tokens], [norm(w[2]) for w in heard]
    times = [None] * len(tokens)
    for op, i1, i2, j1, j2 in SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if op == "equal":
            for k in range(i2 - i1):
                times[i1 + k] = tuple(heard[j1 + k][:2])
        elif op == "replace":
            s, e = heard[j1][0], heard[j2 - 1][1]
            for k in range(i2 - i1):
                times[i1 + k] = (s + (e - s) * k / (i2 - i1), s + (e - s) * (k + 1) / (i2 - i1))
    for k in range(len(times)):
        if times[k] is None:
            prev = next((times[j] for j in range(k - 1, -1, -1) if times[j]), (0.0, 0.0))
            times[k] = (prev[1], prev[1])
    last = times[-1][1]
    tempo = max(1.0, (last + 1.3) / TARGET)  # squeeze to the target with a final small atempo
    return [(times[i][0] / tempo, times[i][1] / tempo, a[i], tokens[i][0], tokens[i][1]) for i in range(len(tokens))], tempo


WORDS, TEMPO = load_timing()
DUR = min(TARGET, WORDS[-1][1] + 1.3) if TEMPO > 1 else WORDS[-1][1] + 1.3
FRAMES = int(round(DUR * FPS))
_KEYS = [w[2] for w in WORDS]


def at(phrase, occ=1, stop=False):
    """Start (or end) time of the occ-th occurrence of a spoken phrase."""
    keys = [norm(p) for p in phrase.split()]
    seen = 0
    for i in range(len(_KEYS) - len(keys) + 1):
        if _KEYS[i:i + len(keys)] == keys:
            seen += 1
            if seen == occ:
                return WORDS[i + len(keys) - 1][1] if stop else WORDS[i][0]
    raise KeyError(phrase)


T = {
    "q": at("whats the last"), "q_end": at("course registration", stop=True), "tenth": at("the tenth"), "changed": at("but that date"),
    "seventeenth": at("actually the seventeenth"), "wrong": at("so what went"), "broken": at("wasnt broken"), "notice": at("wrong notice"),
    "s2": at("heres the thing"), "tokens": at("in tokens"), "forty": at("around forty"), "fit": at("only about nine"), "which": at("which nine"),
    "s3": at("think of it"), "passport": at("your passport"), "useful": at("actually useful"), "shirt": at("same shirt"), "ticket": at("last years ticket"),
    "s4": at("thats exactly"), "throw": at("first it throws"), "score": at("next it scores"), "newf": at("new facts"), "trust": at("trusted sources"),
    "short": at("short text"), "pick": at("then it keeps"), "full": at("budget is full"), "never": at("never over"),
    "s5": at("to make this"), "heap": at("a binary heap"), "merge": at("merge sort"), "list": at("a linked list"), "hash": at("a hash map"),
    "s6": at("okay lets see"), "qsel": at("we pick a"), "budget": at("set the budget"), "compare": at("compare all four"), "last": at("look at the"),
    "both": at("both facts"), "noold": at("no old notices"), "nocopy": at("no copies"), "back": at("now back to"), "b128": at("with just one"),
    "onem": at("one method still"), "ours": at("ours says"),
    "s7": at("we tested this"), "pct": at("ninetyone percent"), "t29": at("all twentynine"), "runs": at("in over twentyfour"), "notonce": at("not once"),
    "s8": at("is it perfect"), "slower": at("a little slower"), "memory": at("more memory"), "real": at("a real ai"), "learn": at("but heres what"),
    "much": at("isnt just about"), "choose": at("its about choosing"), "thanks": at("thanks for watching"),
}
SCENE_STARTS = [0.0, T["s2"] - 0.1, T["s3"] - 0.1, T["s4"] - 0.1, T["s5"] - 0.1, T["s6"] - 0.1, T["s7"] - 0.1, T["s8"] - 0.1]


# ----------------------------------------------------------------------------- drawing helpers
@lru_cache(None)
def background():
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    r = np.sqrt(((x - W * 0.5) / W) ** 2 + ((y - H * 0.42) / H) ** 2)
    g = np.clip(1 - r * 1.35, 0, 1) ** 1.5
    img = np.stack([b + (c - b) * g for b, c in zip(BG0, BG1)], -1)
    img[(x % 48 == 0) & (y % 48 == 0)] += 14
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


@lru_cache(None)
def icon(name, size, color=TEXT):
    a = Image.open(ICONS / f"{name}.png").convert("RGBA").getchannel("A").resize((size, size), Image.LANCZOS)
    im = Image.new("RGBA", (size, size), rgba(color))
    im.putalpha(a)
    return im


@lru_cache(None)
def disc_icon(name, d, fill, color=TEXT):
    im = disc(d, fill).copy()
    s = int(d * 0.54)
    i = icon(name, s, color)
    im.alpha_composite(i, ((d - s) // 2, (d - s) // 2))
    return im


def t_(cv, s, x, y, size, color=TEXT, kind="reg", a=1.0, anchor="c", sc=1.0):
    blit(cv, txt(s, size, color, kind), x, y, a, sc, anchor)


def box(cv, x, y, w, h, fill=CARD, outline=LINE, r=26, a=1.0, sc=1.0, ow=2, anchor="tl"):
    blit(cv, rrect(int(w), int(h), r, fill, outline, ow), x, y, a, sc, anchor)


def pop(t, t0, d=0.4):
    return back(prog(t, t0, d)) if t >= t0 else 0.0


def appear(t, t0, d=0.3):
    return eo(prog(t, t0, d))


def sweep(cv, x, y, w, h, t, t0, color, r=26):
    """A soft highlight pulse around a box."""
    p = prog(t, t0, 0.8)
    if 0 < p < 1:
        grow = int(26 * eo(p))
        blit(cv, rrect(int(w + grow), int(h + grow), r + grow // 2, None, color, 4), x - grow / 2, y - grow / 2, 1 - p, 1.0, "tl")


def stamp(cv, s, x, y, t, t0, color, size=44, angle=-6):
    if t < t0:
        return
    p = prog(t, t0, 0.28)
    tx = txt(s, size, color, "bold")
    pad = 26
    im = Image.new("RGBA", (tx.width + pad * 2, tx.height + pad), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((0, 0, im.width - 1, im.height - 1), 16, fill=rgba(BG0), outline=rgba(color), width=5)
    im.alpha_composite(tx, (pad, pad // 2))
    im = im.rotate(angle, expand=True, resample=Image.BICUBIC)
    blit(cv, im, x, y, min(1, p * 2), lerp(1.8, 1.0, eo(p)))


def cross(cv, x, y, size, color=RED, a=1.0, p=1.0):
    d = draw(cv)
    c = rgba(color, a)
    s = size / 2
    d.line((x - s, y - s, lerp(x - s, x + s, min(1, p * 2)), lerp(y - s, y + s, min(1, p * 2))), fill=c, width=max(6, int(size / 10)))
    if p > 0.5:
        q = (p - 0.5) * 2
        d.line((x + s, y - s, lerp(x + s, x - s, q), lerp(y - s, y + s, q)), fill=c, width=max(6, int(size / 10)))


def fly(t, t0, d, x0, y0, x1, y1, lift=0):
    """Eased position with an optional arc."""
    p = eio(prog(t, t0, d))
    return lerp(x0, x1, p), lerp(y0, y1, p) - lift * math.sin(math.pi * p), p


def heading(cv, s, t, t0, y=88, color=TEXT, size=56):
    a = appear(t, t0, 0.35)
    t_(cv, s, W / 2, y + 22 * (1 - a), size, color, "bold", a)


# ----------------------------------------------------------------------------- captions (word highlight)
def chunks():
    out, cur = [], []
    for i, w in enumerate(WORDS):
        cur.append(i)
        sentence_end = i + 1 == len(WORDS) or WORDS[i + 1][4] != w[4]
        ends = w[3][-1] in ",:\"”" or w[3].endswith("...")
        if sentence_end or len(cur) >= 10 or (ends and len(cur) >= 3):
            out.append(cur)
            cur = []
    return out


CHUNKS = chunks()


@lru_cache(None)
def caption_word(s):
    f = font("bold", 42)
    return f.getlength(s + " ")


@lru_cache(None)
def word_img(s, color):
    f = font("bold", 42)
    w = int(f.getlength(s)) + 16
    im = Image.new("RGBA", (w, 68), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((8, 4), s, font=f, fill=rgba(color), stroke_width=4, stroke_fill=(0, 0, 0, 255))
    return im


def captions(cv, t):
    for ix, ch in enumerate(CHUNKS):
        t0 = WORDS[ch[0]][0] - 0.05
        t1 = min(WORDS[ch[-1]][1] + 0.2, WORDS[CHUNKS[ix + 1][0]][0] - 0.05) if ix + 1 < len(CHUNKS) else WORDS[ch[-1]][1] + 0.5
        if t0 <= t < t1:
            ws = [WORDS[i][3].strip("\"”“") for i in ch]
            total = sum(caption_word(w) for w in ws)
            x = (W - total) / 2
            for i, w in zip(ch, ws):
                on = WORDS[i][0] <= t < WORDS[i][1] + 0.08
                blit(cv, word_img(w, GOLD if on else TEXT), x - 8, 985, 1, 1.0, "tl")
                x += caption_word(w)
            return


# ----------------------------------------------------------------------------- scene 1: the wrong answer
def bubble(cv, x, y, w, h, fill, a=1.0, sc=1.0):
    blit(cv, rrect(int(w), int(h), 30, fill, None), x + w / 2, y + h / 2, a, sc)


def scene1(cv, t):
    # chat window
    a = appear(t, 0.05, 0.4)
    box(cv, 110, 150, 960, 760, CARD, LINE, 34, a)
    blit(cv, disc_icon("robot", 64, TEAL, BG0), 170, 212, a)
    t_(cv, "University AI assistant", 215, 212, 32, TEXT, "semi", a, "lc")
    # question
    q = "What's the last date for course registration?"
    if t >= T["q"] - 0.3:
        s = pop(t, T["q"] - 0.3, 0.3)
        bubble(cv, 300, 300, 720, 150, BLUE, 1, s)
        n = int(len(q) * prog(t, T["q"], max(0.3, T["q_end"] - T["q"])))
        if n:
            blit(cv, para(q[:n], 34, BG0, "semi", 660), 332, 322, 1, 1.0, "tl")
    # typing dots, then the answer
    if T["q_end"] <= t < T["tenth"]:
        bubble(cv, 150, 490, 180, 90, CARD2)
        for i in range(3):
            yy = 535 - 10 * max(0, math.sin((t * 6) - i * 0.9))
            blit(cv, disc(16, MUTED), 205 + i * 36, yy)
    if t >= T["tenth"]:
        wrong = t >= T["changed"]
        s = pop(t, T["tenth"], 0.35)
        bubble(cv, 150, 490, 720, 170, CARD2, 1, s)
        t_(cv, "Registration closes on", 186, 535, 30, MUTED, "reg", 1, "lc")
        t_(cv, "10 September 2026", 186, 595, 50, RED if wrong else TEXT, "bold", 1, "lc")
        if not wrong:
            t_(cv, "Sounds very sure!", 180, 700, 26, MUTED, "reg", appear(t, T["tenth"] + 0.5), "lc")
        else:
            p = prog(t, T["changed"], 0.3)
            draw(cv).line((186, 597, lerp(186, 640, p), 597), fill=rgba(RED), width=6)
    # notices on the right
    for i, (lab, date, col, t0) in enumerate((("OLD NOTICE", "10 September", DIM, T["changed"]), ("NEW NOTICE", "17 September", GREEN, T["seventeenth"]))):
        if t >= t0:
            s = pop(t, t0, 0.4)
            y = 190 + i * 300
            hot = i == 0 and t >= T["notice"]
            box(cv, 1160 + 325, y + 130, 650, 260, CARD, RED if hot else col, 26, 1, s, 3, "c")
            blit(cv, icon("file", 84, col if not hot else RED), 1240, y + 130, 1, s)
            t_(cv, lab, 1310, y + 90, 26, col if not hot else RED, "bold", 1, "lc")
            t_(cv, date, 1310, y + 150, 46, TEXT, "bold", 1, "lc")
    if t >= T["broken"]:
        s = pop(t, T["broken"], 0.35)
        blit(cv, disc_icon("check", 64, GREEN, BG0), 180, 800, 1, s)
        t_(cv, "The AI is not broken", 228, 800, 34, TEXT, "semi", appear(t, T["broken"]), "lc")
    if t >= T["notice"]:
        p = eo(prog(t, T["notice"], 0.5))
        arrow(cv, 880, 575, 1160, 330, RED, 1, 6, 24, p)
        sweep(cv, 1160, 190, 650, 260, t, T["notice"], RED)
        stamp(cv, "It read the wrong notice", 1480, 855, t, T["notice"] + 0.15, RED, 44)


# ----------------------------------------------------------------------------- scene 2: limited space
def doc_xy(i):
    return 180 + (i % 8) * 92, 270 + (i // 8) * 108


def scene2(cv, t):
    heading(cv, "An AI can only read so much at once", t, T["s2"])
    random.seed(4)
    a = appear(t, T["s2"], 0.3)
    box(cv, 110, 190, 800, 690, CARD, LINE, 30, a)
    if t >= T["forty"]:
        t_(cv, "Search finds about 40 passages", 510, 835, 34, TEXT, "semi", appear(t, T["forty"]))
    # the space window
    box(cv, 1110, 190, 700, 690, CARD, TEAL if t >= T["tokens"] else LINE, 30, a, 1.0, 3)
    t_(cv, "Space the AI can read", 1460, 250, 36, TEXT, "semi", a)
    if t >= T["tokens"]:
        s = pop(t, T["tokens"], 0.4)
        blit(cv, rrect(330, 66, 33, TEAL, None), 1460, 318, 1, s)
        t_(cv, "measured in tokens", 1460, 318, 28, BG0, "bold", 1, "c", s)
    slots = [(1230 + (k % 3) * 230, 450 + (k // 3) * 130) for k in range(9)]
    for i in range(40):
        x, y = doc_xy(i)
        t0 = T["s2"] + 0.1 + i * 0.03
        chosen = i in (1, 5, 8, 12, 17, 22, 26, 31, 36)
        col = TEAL if chosen and t >= T["fit"] else MUTED
        al = 1.0
        if t >= T["fit"] and not chosen:
            al = lerp(1, 0.22, prog(t, T["fit"], 0.5))
        s = pop(t, t0, 0.3)
        if chosen and t >= T["fit"]:
            k = [1, 5, 8, 12, 17, 22, 26, 31, 36].index(i)
            x, y, _ = fly(t, T["fit"] + k * 0.1, 0.55, x, y, slots[k][0], slots[k][1], 90)
        if s > 0:
            blit(cv, icon("file", 66, col), x, y, al, s)
    if t >= T["which"]:
        s = pop(t, T["which"], 0.45)
        blit(cv, rrect(760, 150, 40, ORANGE, None), W / 2, 540, 1, s)
        t_(cv, "Which nine?", W / 2, 540, 88, BG0, "bold", 1, "c", s)


# ----------------------------------------------------------------------------- scene 3: packing a bag
def scene3(cv, t):
    heading(cv, "Think of it like packing a small bag", t, T["s3"])
    a = appear(t, T["s3"], 0.3)
    s = pop(t, T["s3"], 0.5)
    # open bag
    bx, by, bw, bh = 170, 300, 720, 520
    box(cv, bx + bw / 2, by + bh / 2, bw, bh, CARD2, TEAL, 40, a, s, 5, "c")
    d = draw(cv)
    if a > 0.5:
        d.arc((bx + bw / 2 - 110, by - 90, bx + bw / 2 + 110, by + 90), 180, 360, fill=rgba(TEAL), width=14)
    slots = {"passport": (bx + 170, by + 180), "book": (bx + 390, by + 170), "tshirt": (bx + 590, by + 180), "pills": (bx + 260, by + 380), "clock": (bx + 500, by + 380)}
    times = {"passport": T["passport"], "book": T["useful"] - 0.2, "tshirt": T["useful"], "pills": T["useful"] + 0.2, "clock": T["useful"] + 0.4}
    for name, (x, y) in slots.items():
        t0 = times[name]
        if t >= t0:
            xx, yy, _ = fly(t, t0, 0.5, 1500, 170, x, y, 120)
            blit(cv, icon(name, 124 if name == "passport" else 110, GOLD if name == "passport" else TEXT), xx, yy)
    if t >= T["passport"] + 0.5:
        t_(cv, "goes in first", bx + 170, by + 280, 26, GOLD, "semi", appear(t, T["passport"] + 0.5))
    # rejected items bounce away
    for name, t0, label, y0 in (("tshirt", T["shirt"], "Same shirt twice", 420), ("ticket", T["ticket"], "Last year's ticket", 700)):
        if t >= t0:
            xx, yy, p = fly(t, t0, 0.5, 1800, y0 - 100, 1120, y0, 60)
            blit(cv, icon(name, 118, MUTED), xx, yy)
            if p >= 1:
                cross(cv, 1120, y0, 150, RED, 1, prog(t, t0 + 0.5, 0.2))
                t_(cv, label, 1230, y0, 42, TEXT, "semi", appear(t, t0 + 0.55), "lc")
    # the mapping
    if t >= T["passport"]:
        t_(cv, "The bag = your token budget", 1060, 230, 36, TEAL, "semi", appear(t, T["passport"]), "lc")
        t_(cv, "Each item = one passage", 1060, 285, 36, TEXT, "semi", appear(t, T["useful"]), "lc")


# ----------------------------------------------------------------------------- scene 4: the method
def scene4(cv, t):
    # title punches in, then moves up
    up = eio(prog(t, T["throw"] - 0.3, 0.5))
    s = pop(t, T["s4"], 0.5)
    y = lerp(500, 88, up)
    size_sc = lerp(1.0, 0.62, up)
    t_(cv, "Context Budget Manager", W / 2, y, 92, TEAL, "bold", 1, "c", s * size_sc)
    if up < 0.05 and t >= T["s4"] + 0.4:
        t_(cv, "Our project: it chooses what the AI reads", W / 2, 610, 40, MUTED, "reg", appear(t, T["s4"] + 0.4))
    cols = [(110, "1", "Remove old notices", T["throw"]), (710, "2", "Score what is left", T["score"]), (1310, "3", "Pick the best, repeat", T["pick"])]
    for x, n, h, t0 in cols:
        if t < t0:
            continue
        a = pop(t, t0, 0.4)
        box(cv, x + 250, 545, 500, 690, CARD, LINE, 28, 1, a, 2, "c")
        if a > 0.6:
            blit(cv, disc(70, ORANGE), x + 60, 255)
            t_(cv, n, x + 60, 255, 38, BG0, "bold")
            t_(cv, h, x + 110, 255, 34, TEXT, "semi", 1, "lc")
    # 1: old passages slide out
    if t >= T["throw"] + 0.2:
        for i in range(5):
            old = i in (1, 3)
            yy = 370 + i * 105
            xo = 180
            al = 1.0
            if old:
                p = eio(prog(t, T["throw"] + 1.4 + i * 0.1, 0.6))
                xo += 400 * p
                al = 1 - p
            a2 = appear(t, T["throw"] + 0.2 + i * 0.08)
            blit(cv, rrect(360, 84, 18, CARD2, RED if old else LINE), xo + 180, yy, al * a2)
            blit(cv, icon("file", 46, RED if old else TEAL), xo + 44, yy, al * a2)
            t_(cv, "Old notice" if old else f"Passage {i + 1}", xo + 88, yy, 28, RED if old else TEXT, "semi", al * a2, "lc")
    # 2: score bars
    if t >= T["score"] + 0.2:
        for i, (lab, tk, val, col) in enumerate((("New facts", T["newf"], 0.92, GREEN), ("Trusted source", T["trust"], 0.74, BLUE), ("Short text", T["short"], 0.6, GOLD))):
            yy = 390 + i * 165
            al = appear(t, tk - 0.1)
            t_(cv, lab, 780, yy, 32, TEXT, "semi", al, "lc")
            blit(cv, rrect(360, 34, 17, CARD2, None), 780, yy + 40, al, 1.0, "lc")
            p = eo(prog(t, tk, 0.6)) * val
            if p > 0.05:
                blit(cv, rrect(int(360 * p), 34, 17, col, None), 780, yy + 40, 1, 1.0, "lc")
    # 3: budget meter fills, stops under the limit
    if t >= T["pick"]:
        mx, my, mw, mh = 1420, 340, 180, 500
        box(cv, mx, my, mw, mh, CARD2, LINE, 16)
        limit_y = my
        n = 9
        span = max(0.5, T["full"] - T["pick"])
        filled = 0
        for i in range(n):
            t0 = T["pick"] + 0.1 + i * span / n
            if t < t0:
                break
            h = int(mh * 485 / 512 / n) - 4
            yy = my + mh - (i + 1) * (h + 4) + lerp(-30, 0, eo(prog(t, t0, 0.18)))
            blit(cv, rrect(mw - 16, h, 8, TEAL, None), mx + 8, yy, 1, 1.0, "tl")
            filled = i + 1
        draw(cv).line((mx - 20, limit_y, mx + mw + 20, limit_y), fill=rgba(RED), width=5)
        t_(cv, "limit", mx + mw + 30, limit_y, 26, RED, "semi", 1, "lc")
        t_(cv, f"{int(485 * filled / n)} / 512", mx + mw + 30, my + 180, 34, TEXT, "bold", 1, "lc")
        t_(cv, "tokens", mx + mw + 30, my + 225, 26, MUTED, "reg", 1, "lc")
        stamp(cv, "Never over!", 1560, 790, t, T["never"], GREEN, 46)


# ----------------------------------------------------------------------------- scene 5: data structures
PANELS = [(110, 180, "Binary heap", "Gives the best passage next", "heap"), (985, 180, "Merge sort", "Keeps things in order", "merge"),
          (110, 560, "Linked list", "Holds the final package", "list"), (985, 560, "Hash map", "Remembers what we saw", "hash")]


def heap_panel(cv, x, y, t, t0):
    vals = [9, 8, 6, 5, 7, 2, 4]
    pos = [(0, 0), (-130, 90), (130, 90), (-195, 180), (-65, 180), (65, 180), (195, 180)]
    cx, cy = x + 560, y + 60
    d = draw(cv)
    for i in range(1, 7):
        p = (i - 1) // 2
        d.line((cx + pos[p][0], cy + pos[p][1], cx + pos[i][0], cy + pos[i][1]), fill=rgba(LINE), width=4)
    cyc = (t - t0) % 1.7 if t >= t0 else 0
    top = t >= t0 and cyc < 0.8
    for i, (dx, dy) in enumerate(pos):
        yy = cy + dy
        al = 1.0
        if i == 0 and top:
            yy -= 44 * eo(cyc / 0.8)
            al = 1 - cyc / 0.8 * 0.4
        blit(cv, disc(70, TEAL if i == 0 else CARD2, LINE), cx + dx, yy, al)
        t_(cv, str(vals[i]), cx + dx, yy, 30, BG0 if i == 0 else TEXT, "bold", al)


def merge_panel(cv, x, y, t, t0):
    vals = [5, 2, 8, 3, 7, 1, 6, 4]
    n = len(vals)
    left, right = sorted(vals[:4]), sorted(vals[4:])
    a = list(range(n))
    st1 = [vals.index(v) for v in left] + [vals.index(v) for v in right]
    p1 = eio(prog(t, t0 + 0.2, 0.5)) if t >= t0 else 0
    p2 = eio(prog(t, t0 + 1.0, 0.6)) if t >= t0 else 0
    order_mid = {v: i for i, v in enumerate(left + right)}
    order_end = {v: i for i, v in enumerate(sorted(vals))}
    bx, by = x + 355, y + 250
    for i, v in enumerate(vals):
        xx = lerp(lerp(i, order_mid[v], p1), order_end[v], p2) * 56
        gap = 20 * (1 - abs(2 * p1 - 1)) * (1 if xx >= 4 * 56 else -1) * (1 - p2)
        h = v * 22
        blit(cv, rrect(44, h, 8, ORANGE if p2 >= 1 else BLUE, None), bx + xx + gap, by - h, 1, 1.0, "tl")


def list_panel(cv, x, y, t, t0):
    for i in range(4):
        tk = t0 + i * 0.28
        if t < tk:
            continue
        s = pop(t, tk, 0.3)
        xx = x + 345 + i * 118
        blit(cv, rrect(88, 88, 16, TEAL if i == 3 else CARD2, LINE), xx + 44, y + 170, 1, s)
        blit(cv, icon("file", 44, BG0 if i == 3 else TEXT), xx + 44, y + 170, 1, s)
        if i < 3 and t >= tk + 0.2:
            arrow(cv, xx + 90, y + 170, xx + 116, y + 170, MUTED, 1, 5, 12, prog(t, tk + 0.2, 0.15))


def hash_panel(cv, x, y, t, t0):
    words = ["fact: date", "fact: form", "D29-P1"]
    bucket = [1, 3, 0]
    bx, by = x + 560, y + 72
    for b in range(4):
        blit(cv, rrect(170, 46, 10, CARD2, LINE), bx, by + b * 54, 1, 1.0, "tl")
    for i, w in enumerate(words):
        tk = t0 + i * 0.45
        if t < tk:
            continue
        p = eo(prog(t, tk, 0.3))
        blit(cv, rrect(170, 46, 10, GREEN, None), bx, by + bucket[i] * 54, p, 1.0, "tl")
        t_(cv, w, bx + 85, by + bucket[i] * 54 + 23, 22, BG0, "bold", p)
    t_(cv, "seen?", x + 455, y + 150, 34, MUTED, "semi", 1)
    if t >= t0:
        arrow(cv, x + 500, y + 150, x + 552, y + 150, MUTED, 1, 5, 14)


def scene5(cv, t):
    heading(cv, "Four data structures we built ourselves", t, T["s5"])
    draws = {"heap": (heap_panel, T["heap"]), "merge": (merge_panel, T["merge"]), "list": (list_panel, T["list"]), "hash": (hash_panel, T["hash"])}
    for i, (x, y, h, sub, key) in enumerate(PANELS):
        s = pop(t, T["s5"] + 0.2 + i * 0.08, 0.4)
        fn, t0 = draws[key]
        on = t >= t0
        cur = on and all(t < draws[k][1] for k in list(draws)[i + 1:])
        box(cv, x + 412, y + 170, 825, 340, CARD2 if cur else CARD, TEAL if cur else LINE, 28, 1 if on else 0.55, s, 4 if cur else 2, "c")
        if s > 0.9:
            t_(cv, h, x + 34, y + 60, 42, TEAL if on else DIM, "bold", 1, "lc")
            if on:
                blit(cv, para(sub, 28, TEXT, "reg", 300), x + 34, y + 105, appear(t, t0), 1.0, "tl")
                fn(cv, x, y, t, t0)
                sweep(cv, x, y, 825, 340, t, t0, TEAL, 28)


# ----------------------------------------------------------------------------- scene 6: live demo (real cbm.exe output)
Q01 = [("recency-first", "510", "1/2", "0", "0"), ("relevance-first", "512", "1/2", "4", "3"), ("relevance-per-token", "496", "2/2", "3", "4"), ("hybrid-fresh-cover", "485", "2/2", "0", "0")]
M18 = [("recency-first", "insufficient evidence", DIM), ("relevance-first", "17 September 2026 (sources disagree)", GOLD), ("relevance-per-token", "10 September 2026", RED), ("hybrid-fresh-cover", "17 September 2026", GREEN)]
TX, TY = 110, 170
CW = 1700


def mono(cv, s, x, y, size=34, color=TEXT, a=1.0, kind="mono"):
    t_(cv, s, x, y, size, color, kind, a, "lc")


def term(cv, t, s0):
    a = appear(t, s0, 0.3)
    box(cv, TX, TY, CW, 770, (8, 14, 20), LINE, 18, a)
    blit(cv, rrect(CW, 54, 18, (26, 36, 44), None), TX, TY, a, 1.0, "tl")
    for i, c in enumerate((RED, GOLD, GREEN)):
        blit(cv, disc(18, c), TX + 34 + i * 30, TY + 27, a)
    t_(cv, "cbm.exe  |  real results", TX + CW / 2, TY + 27, 24, MUTED, "reg", a)


def typed(s, t, t0, cps=55):
    return s[:int(max(0, t - t0) * cps)]


def scene6(cv, t):
    term(cv, t, T["s6"])
    x = TX + 50
    if t < T["back"] - 0.05:
        if t >= T["qsel"]:
            s = "Q01  What is the last date to drop a course this semester, and which form is needed?"
            mono(cv, "question:", x, 280, 30, TEAL)
            mono(cv, typed(s, t, T["qsel"], 60), x + 190, 280, 30)
        if t >= T["budget"]:
            mono(cv, "budget:", x, 330, 30, TEAL)
            mono(cv, typed("B_CTX = 512 tokens", t, T["budget"], 40), x + 190, 330, 30)
        if t >= T["compare"]:
            cols = [0, 500, 690, 890, 1090]
            for c, h in zip(cols, ("policy", "tokens", "coverage", "stale", "redund")):
                mono(cv, h, x + c, 410, 30, MUTED, appear(t, T["compare"]))
            draw(cv).line((x, 440, x + 1300, 440), fill=rgba(LINE), width=2)
            for i, row in enumerate(Q01):
                t0 = T["compare"] + 0.15 + i * 0.28
                if t < t0:
                    continue
                y = 490 + i * 72
                last = i == 3
                if last and t >= T["last"]:
                    p = eo(prog(t, T["last"], 0.35))
                    blit(cv, rrect(int(1460 * p) + 2, 64, 12, (22, 74, 76), TEAL), x - 18, y - 32, 1, 1.0, "tl")
                a = appear(t, t0, 0.15)
                for c, v in zip(cols, row):
                    col = TEXT
                    if last and ((v == "2/2" and t >= T["both"]) or (c == 890 and t >= T["noold"]) or (c == 1090 and t >= T["nocopy"])):
                        col = GREEN
                    elif not last and c in (890, 1090) and v != "0":
                        col = ORANGE
                    mono(cv, v, x + c, y, 34, col, a, "monob" if last else "mono")
            for lab, tk, c in (("Both facts found", T["both"], 340), ("No old notices", T["noold"], 830), ("No copies", T["nocopy"], 1250)):
                if t >= tk:
                    s = pop(t, tk, 0.3)
                    blit(cv, rrect(370 if c == 340 else 320, 62, 31, GREEN, None), x + c, 860, 1, s)
                    t_(cv, lab, x + c, 860, 30, BG0, "bold", 1, "c", s)
    else:
        s = "M18  last date for course registration fall 2026"
        mono(cv, "question:", x, 280, 30, TEAL)
        mono(cv, typed(s, t, T["back"] + 0.2, 60), x + 190, 280, 30)
        if t >= T["b128"]:
            mono(cv, "budget:", x, 330, 30, TEAL)
            mono(cv, typed("B_CTX = 128 tokens", t, T["b128"], 40), x + 190, 330, 30)
        if t >= T["b128"] + 0.5:
            a = appear(t, T["b128"] + 0.5)
            mono(cv, "policy", x, 420, 30, MUTED, a)
            mono(cv, "answer", x + 500, 420, 30, MUTED, a)
            draw(cv).line((x, 450, x + 1300, 450), fill=rgba(LINE), width=2)
            for i, (pol, ans, col) in enumerate(M18):
                t0 = T["b128"] + 0.6 + i * 0.2
                if t < t0:
                    continue
                y = 505 + i * 82
                hot = (i == 2 and t >= T["onem"]) or (i == 3 and t >= T["ours"])
                if hot:
                    p = eo(prog(t, T["onem"] if i == 2 else T["ours"], 0.3))
                    blit(cv, rrect(int(1460 * p) + 2, 70, 12, (72, 30, 32) if i == 2 else (22, 74, 76), RED if i == 2 else GREEN), x - 18, y - 35, 1, 1.0, "tl")
                a = appear(t, t0, 0.15)
                mono(cv, pol, x, y, 34, TEXT, a)
                mono(cv, ans, x + 500, y, 34, col if hot or i < 2 else TEXT, a, "monob" if hot else "mono")
                if hot:
                    if i == 2:
                        cross(cv, x + 1400, y, 44, RED, 1, prog(t, T["onem"], 0.2))
                    else:
                        blit(cv, disc_icon("check", 56, GREEN, BG0), x + 1400, y, 1, pop(t, T["ours"], 0.3))


# ----------------------------------------------------------------------------- scene 7: results
def scene7(cv, t):
    heading(cv, "The results", t, T["s7"])
    a = appear(t, T["s7"], 0.3)
    box(cv, 110, 170, 1040, 770, CARD, LINE, 28, a)
    t_(cv, "Needed facts found at 512 tokens", 630, 225, 36, TEXT, "semi", a)
    t_(cv, "32 test questions made for the project", 630, 268, 24, MUTED, "reg", a)
    bars = [("Newest first", 29, DIM), ("Best match first", 84, GOLD), ("Best value first", 79, PURPLE), ("Our method", 91, TEAL)]
    base = 840
    grow0 = T["s7"] + 0.4
    for i, (lab, v, col) in enumerate(bars):
        x = 165 + i * 240
        ours = i == 3
        t0 = T["pct"] - 0.1 if ours else grow0 + i * 0.25
        p = eo(prog(t, t0, 0.7))
        h = int(480 * v / 100 * p)
        if h > 4:
            blit(cv, rrect(180, h, 12, col, None), x, base - h, 1, 1.0, "tl")
        if p > 0:
            t_(cv, f"{int(round(v * p))}%", x + 90, base - h - 34, 44 if ours else 36, col if ours else TEXT, "bold")
        t_(cv, lab, x + 90, base + 34, 25, TEAL if ours else TEXT, "semi" if ours else "reg", a)
    # proof cards
    if t >= T["t29"]:
        s = pop(t, T["t29"], 0.4)
        box(cv, 1500, 310, 620, 280, CARD, GREEN, 28, 1, s, 3, "c")
        if s > 0.9:
            blit(cv, disc_icon("check", 90, GREEN, BG0), 1280, 310)
            t_(cv, "29/29", 1350, 275, 84, GREEN, "bold", 1, "lc")
            t_(cv, "automated tests pass", 1350, 355, 34, TEXT, "reg", 1, "lc")
    if t >= T["runs"]:
        s = pop(t, T["runs"], 0.4)
        box(cv, 1500, 690, 620, 400, CARD, TEAL, 28, 1, s, 3, "c")
        if s > 0.9:
            n = int(24704 * eo(prog(t, T["runs"], 1.3)))
            t_(cv, f"{n:,}", 1500, 590, 84, TEAL, "bold")
            t_(cv, "test runs", 1500, 660, 34, TEXT, "reg")
            if t >= T["notonce"] - 0.5:
                stamp(cv, "0 times over budget", 1500, 780, t, T["notonce"] - 0.5, GREEN, 44, -3)


# ----------------------------------------------------------------------------- scene 8: limits and close
def scene8(cv, t):
    if t < T["learn"]:
        heading(cv, "Is it perfect? No.", t, T["s8"], 120, ORANGE, 72)
        cards = [("clock", "A little slower", "about 16 microseconds per choice, against 6 to 7", T["slower"], GOLD),
                 ("memory", "More memory", "7 to 10 times more than the simpler methods", T["memory"], ORANGE),
                 ("robot", "Next step", "test it with a real AI model", T["real"], TEAL)]
        for i, (ic, h, d, t0, col) in enumerate(cards):
            if t < t0:
                continue
            s = pop(t, t0, 0.4)
            x = 370 + i * 590
            box(cv, x, 570, 530, 520, CARD, col, 30, 1, s, 3, "c")
            if s > 0.9:
                blit(cv, disc_icon(ic, 130, col, BG0), x, 410)
                t_(cv, h, x, 530, 42, TEXT, "bold")
                blit(cv, para(d, 32, MUTED, "reg", 440), x, 650, 1, 1.0, "c")
    elif t < T["thanks"]:
        if t < T["much"] - 0.1:
            s_ = pop(t, T["learn"], 0.4)
            t_(cv, "What we learned", W / 2, 500, 84, TEXT, "bold", 1, "c", s_)
        a = appear(t, T["much"] - 0.1, 0.4)
        t_(cv, "A smart assistant isn't just about", W / 2, 370, 54, MUTED, "reg", a)
        t_(cv, "how much it can read.", W / 2, 450, 54, MUTED, "reg", a)
        if t >= T["choose"]:
            s = pop(t, T["choose"], 0.5)
            t_(cv, "It's about choosing", W / 2, 610, 96, TEAL, "bold", 1, "c", s)
            t_(cv, "what to read.", W / 2, 730, 96, TEAL, "bold", 1, "c", s)
    else:
        s = pop(t, T["thanks"], 0.4)
        blit(cv, disc_icon("robot", 150, TEAL, BG0), W / 2, 215, 1, s)
        t_(cv, "Context Budget Manager", W / 2, 370, 78, TEXT, "bold", 1, "c", s)
        t_(cv, "CS-251 Data Structures & Algorithms  ·  Complex Engineering Problem", W / 2, 460, 32, MUTED, "reg", 1, "c", s)
        for i, n in enumerate(("Waqas Ali  25CS061", "Mehran Ali  25CS009", "Gaffar  25CS067", "Basit Azam  25CS113")):
            a = pop(t, T["thanks"] + 0.05 + i * 0.05, 0.3)
            x = 355 + i * 405
            blit(cv, rrect(380, 84, 20, CARD2, LINE), x, 590, 1, a)
            t_(cv, n, x, 590, 30, TEXT, "semi", 1, "c", a)
        t_(cv, "Instructor: Dr. Ali Asghar Manjotho  ·  MUET Jamshoro  ·  2026", W / 2, 710, 34, TEAL, "semi", appear(t, T["thanks"] + 0.15))


SCENES = [scene1, scene2, scene3, scene4, scene5, scene6, scene7, scene8]
# progress bar colours by scene are not needed: a single teal line shows how far along the video is.


def frame(t):
    cv = background().copy()
    idx = 0
    for i, s in enumerate(SCENE_STARTS):
        if t >= s:
            idx = i
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    SCENES[idx](layer, t)
    t0 = SCENE_STARTS[idx]
    t1 = SCENE_STARTS[idx + 1] if idx + 1 < len(SCENES) else DUR
    p_in = eo(prog(t, t0, 0.25)) if idx else 1.0
    p_out = prog(t, t1 - 0.2, 0.2) if idx + 1 < len(SCENES) else 0.0
    alpha = min(p_in, 1 - p_out)
    dx = int(70 * (1 - p_in) - 70 * p_out)
    if dx:
        layer = layer.transform(layer.size, Image.AFFINE, (1, 0, -dx, 0, 1, 0))
    cv.alpha_composite(fade(layer, alpha))
    captions(cv, t)
    d = draw(cv)
    d.rectangle((0, H - 6, int(W * t / DUR), H), fill=rgba(TEAL))
    a = 0
    if t < 0.4:
        a = 1 - t / 0.4
    elif t > DUR - 0.8:
        a = prog(t, DUR - 0.8, 0.8)
    if a > 0:
        cv.alpha_composite(Image.new("RGBA", (W, H), (0, 0, 0, int(255 * a))))
    return cv.convert("RGB")


# ----------------------------------------------------------------------------- SFX and music
SR = 48000
rng = np.random.default_rng(7)


def env(n, attack=0.005, decay=6.0):
    x = np.arange(n) / SR
    return np.minimum(1, x / max(attack, 1e-4)) * np.exp(-x * decay)


def tone(freq, d, decay=6.0, attack=0.005, harm=0.25):
    n = int(d * SR)
    x = np.arange(n) / SR
    return (np.sin(2 * np.pi * freq * x) + harm * np.sin(4 * np.pi * freq * x)) * env(n, attack, decay)


def whoosh(d=0.45):
    n = int(d * SR)
    noise = rng.standard_normal(n)
    x = np.linspace(0, 1, n)
    cut = 0.02 + 0.28 * np.sin(np.pi * x) ** 2
    out = np.empty(n)
    y = 0.0
    for i in range(n):
        y += cut[i] * (noise[i] - y)
        out[i] = y
    return out * np.sin(np.pi * x) ** 1.2 * 2.2


def over(a, b):
    """Mix two sounds of different lengths."""
    out = np.zeros(max(len(a), len(b)))
    out[:len(a)] += a
    out[:len(b)] += b
    return out


def sfx_bank():
    pop_ = over(tone(620, 0.14, 26), 0.5 * tone(930, 0.1, 34))
    click = rng.standard_normal(int(0.022 * SR)) * env(int(0.022 * SR), 0.0005, 190) * 0.8
    ding = over(tone(1046.5, 0.8, 5.5, harm=0.2), 0.7 * tone(1568, 0.6, 7, harm=0.1))
    success = np.concatenate([tone(784, 0.11, 12), tone(1046.5, 0.11, 12), tone(1318.5, 0.55, 6)])
    n = int(0.42 * SR)
    x = np.arange(n) / SR
    error = (np.sign(np.sin(2 * np.pi * 150 * x)) * 0.35 + np.sin(2 * np.pi * 112 * x) * 0.6) * env(n, 0.004, 7)
    thump = over(tone(62, 0.42, 9, 0.002, 0.1) * 1.8, tone(120, 0.2, 18, 0.001, 0) * 0.5)
    stamp_ = thump * 0.8
    stamp_[:int(0.03 * SR)] += rng.standard_normal(int(0.03 * SR)) * env(int(0.03 * SR), 0.0005, 90) * 0.6
    return {"pop": pop_, "click": click, "ding": ding, "success": success, "error": error, "thump": thump, "stamp": stamp_, "whoosh": whoosh(), "whoosh_s": whoosh(0.28)}


def cues():
    c = []
    add = lambda t, name, g=1.0: c.append((t, name, g))
    for s in SCENE_STARTS[1:]:
        add(s - 0.12, "whoosh", 0.8)
    add(T["q"] - 0.3, "pop", 0.7)
    q_len = len("What's the last date for course registration?")
    for i in range(0, q_len, 2):
        add(T["q"] + i / q_len * (T["q_end"] - T["q"]), "click", 0.5)
    add(T["tenth"], "ding", 0.55)
    add(T["changed"], "error", 0.5)
    add(T["seventeenth"], "pop", 0.8)
    add(T["broken"], "pop", 0.6)
    add(T["notice"] + 0.15, "stamp", 0.8)
    for i in range(0, 40, 2):
        add(T["s2"] + 0.1 + i * 0.03, "click", 0.4)
    add(T["tokens"], "pop", 0.6)
    for k in range(9):
        add(T["fit"] + k * 0.1, "whoosh_s", 0.22)
    add(T["which"], "thump", 0.9)
    add(T["s3"], "pop", 0.5)
    add(T["passport"] + 0.45, "thump", 0.5)
    for d in (-0.2, 0, 0.2, 0.4):
        add(T["useful"] + d + 0.45, "pop", 0.45)
    add(T["shirt"] + 0.5, "error", 0.42)
    add(T["ticket"] + 0.5, "error", 0.42)
    add(T["s4"], "thump", 0.9)
    add(T["s4"], "ding", 0.3)
    add(T["throw"], "pop", 0.6)
    for i in (1, 3):
        add(T["throw"] + 1.4 + i * 0.1, "whoosh_s", 0.4)
    add(T["score"], "pop", 0.6)
    for k in ("newf", "trust", "short"):
        add(T[k], "pop", 0.45)
    add(T["pick"], "pop", 0.6)
    span = max(0.5, T["full"] - T["pick"])
    for i in range(9):
        add(T["pick"] + 0.1 + i * span / 9, "click", 0.7)
    add(T["never"], "stamp", 0.9)
    add(T["never"] + 0.05, "success", 0.5)
    for k in ("heap", "merge", "list", "hash"):
        add(T[k], "ding", 0.28)
        add(T[k], "whoosh_s", 0.3)
    s = "Q01  What is the last date to drop a course this semester, and which form is needed?"
    for i in range(0, len(s), 2):
        add(T["qsel"] + i / 60, "click", 0.35)
    for i in range(0, 18, 2):
        add(T["budget"] + i / 40, "click", 0.35)
    for i in range(4):
        add(T["compare"] + 0.15 + i * 0.28, "pop", 0.4)
    add(T["last"], "whoosh_s", 0.5)
    for k in ("both", "noold", "nocopy"):
        add(T[k], "pop", 0.65)
    add(T["nocopy"] + 0.25, "success", 0.45)
    s = "M18  last date for course registration fall 2026"
    for i in range(0, len(s), 2):
        add(T["back"] + 0.2 + i / 60, "click", 0.35)
    add(T["onem"], "error", 0.5)
    add(T["ours"], "success", 0.6)
    for i in range(3):
        add(T["s7"] + 0.4 + i * 0.25, "pop", 0.4)
    add(T["pct"], "ding", 0.5)
    add(T["t29"], "success", 0.5)
    add(T["runs"], "whoosh", 0.5)
    add(T["notonce"] - 0.5, "stamp", 0.9)
    for k in ("slower", "memory", "real"):
        add(T[k], "pop", 0.6)
    add(T["choose"], "thump", 0.8)
    add(T["choose"], "ding", 0.35)
    add(T["thanks"], "success", 0.45)
    return c


def music(n):
    """A soft, slowly moving pad so the mix never sits in dead silence."""
    x = np.arange(n) / SR
    chords = [(220.0, 277.18, 329.63), (174.61, 220.0, 261.63), (130.81, 196.0, 261.63), (196.0, 246.94, 293.66)]
    out = np.zeros(n)
    step = 6.0
    for i in range(int(n / SR / step) + 2):
        t0 = i * step
        s0, s1 = int(t0 * SR), min(n, int((t0 + step * 1.5) * SR))
        if s0 >= n:
            break
        seg = x[s0:s1] - t0
        e = np.sin(np.pi * np.clip(seg / (step * 1.5), 0, 1)) ** 2
        for f in chords[i % 4]:
            out[s0:s1] += e * (np.sin(2 * np.pi * f * seg) + 0.3 * np.sin(2 * np.pi * f * 2.003 * seg))
    return out / np.max(np.abs(out))


def build_audio():
    BUILD.mkdir(parents=True, exist_ok=True)
    final_wav = BUILD / "voice_final.wav"
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", str(ROOT / "voice.wav"), "-af", f"atempo={TEMPO:.5f}", "-ar", str(SR), "-ac", "1", str(final_wav)], check=True)
    with wave.open(str(final_wav)) as wv:
        voice = np.frombuffer(wv.readframes(wv.getnframes()), np.int16).astype(np.float64) / 32768
    n = int(DUR * SR)
    v = np.zeros(n)
    v[:min(n, len(voice))] = voice[:n]
    v *= 0.5 / max(1e-6, np.sqrt(np.mean(v[v != 0] ** 2)) * 3.2)
    bank = sfx_bank()
    fx = np.zeros(n)
    for tc, name, g in cues():
        s = bank[name] * g
        i = int(tc * SR)
        if 0 <= i < n:
            e = min(n, i + len(s))
            fx[i:e] += s[:e - i]
    fx *= 0.17
    # duck the music under speech
    win = int(0.25 * SR)
    cum = np.concatenate([[0.0], np.cumsum(np.abs(v))])
    level = (cum[np.minimum(n, np.arange(n) + win // 2)] - cum[np.maximum(0, np.arange(n) - win // 2)]) / win
    duck = 1 - 0.55 * np.clip(level / (np.percentile(level, 90) + 1e-9), 0, 1)
    pad = music(n) * 0.055 * duck
    fade_i = np.minimum(1, np.arange(n) / (0.8 * SR)) * np.minimum(1, (n - np.arange(n)) / (1.2 * SR))
    mix_ = v + fx + pad * fade_i
    mix_ = np.clip(mix_ / max(1.0, np.max(np.abs(mix_)) / 0.97), -1, 1)
    out = BUILD / "mix.wav"
    with wave.open(str(out), "wb") as wv:
        wv.setnchannels(1)
        wv.setsampwidth(2)
        wv.setframerate(SR)
        wv.writeframes((mix_ * 32767).astype(np.int16).tobytes())
    print("mix", out, f"{n / SR:.2f}s, {len(cues())} SFX cues, tempo {TEMPO:.4f}")


# ----------------------------------------------------------------------------- render
def render_seg(job):
    i, f0, f1 = job
    out = BUILD / f"seg_{i:02d}.mp4"
    proc = subprocess.Popen(["ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
                             "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p", "-threads", "2", str(out)], stdin=subprocess.PIPE)
    for f in range(f0, f1):
        proc.stdin.write(frame(f / FPS).tobytes())
    proc.stdin.close()
    proc.wait()
    return f"seg {i}: frames {f0}-{f1}"


def main():
    cmd = sys.argv[1]
    BUILD.mkdir(parents=True, exist_ok=True)
    pv = BUILD / "preview"
    pv.mkdir(exist_ok=True)
    if cmd == "frames":
        for ts in sys.argv[2:]:
            frame(float(ts)).save(pv / f"f_{float(ts):06.2f}.png")
    elif cmd == "sheet":
        cols, rows = 6, 8
        step = DUR / (cols * rows)
        sheet = Image.new("RGB", (cols * 480, rows * 270))
        for k in range(cols * rows):
            im = frame(k * step + 0.5).resize((480, 270), Image.BILINEAR)
            sheet.paste(im, ((k % cols) * 480, (k // cols) * 270))
        sheet.save(pv / "sheet.png")
        print("DUR", DUR, "TEMPO", TEMPO, "scenes", [round(s, 1) for s in SCENE_STARTS])
    elif cmd == "render":
        parts = 12
        jobs = [(i, FRAMES * i // parts, FRAMES * (i + 1) // parts) for i in range(parts)]
        with Pool(6) as pool:
            for r in pool.imap_unordered(render_seg, jobs):
                print(r, flush=True)
    elif cmd == "audio":
        build_audio()
    elif cmd == "final":
        lst = BUILD / "segs.txt"
        lst.write_text("\n".join(f"file '{(BUILD / f'seg_{i:02d}.mp4').as_posix()}'" for i in range(12)), encoding="utf-8")
        out = ROOT / "CEP_video.mp4"
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", str(lst), "-i", str(BUILD / "mix.wav"), "-map", "0:v", "-map", "1:a",
                        "-c:v", "copy", "-af", "loudnorm=I=-16:TP=-1.5:LRA=11", "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-shortest", "-movflags", "+faststart", str(out)], check=True)
        print(out)


if __name__ == "__main__":
    main()

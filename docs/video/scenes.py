# InvariantFS overview — manim scenes (draft S1..S6+S5b).
# Style: bg #0d1117, amber #f0a500 (invfs), teal #39c5cf (good),
# red #e5484d (corruption). Inter preferred, DejaVu fallback. No LaTeX.
# Render draft:  manim -qm --fps 30 scenes.py -a
# Render final:  manim -qh --fps 120 scenes.py -a
from manim import *
import numpy as np

config.background_color = "#0d1117"
AMBER, TEAL, RED, INK = "#f0a500", "#39c5cf", "#e5484d", "#e6edf3"

try:
    FONT = "Inter" if "Inter" in [f.name for f in Text("x").font_list()] else "DejaVu Sans"
except Exception:
    FONT = "DejaVu Sans"


def title(text, size=54):
    return Text(text, font=FONT, font_size=size, color=INK, weight=BOLD)


def kicker(text, size=30, color=AMBER):
    return Text(text, font=FONT, font_size=size, color=color, weight=BOLD)


def fit(obj, max_w=12.0):
    # Kinetic text must never leave the frame: shrink wide groups in.
    if obj.width > max_w:
        obj.scale(max_w / obj.width)
    return obj


class S1Hook(Scene):
    def construct(self):
        t = title("WHAT YOU WRITE", 56)
        t2 = title("COMES BACK BYTE-IDENTICAL.", 56)
        g = fit(VGroup(t, t2).arrange(DOWN, buff=0.25))
        self.play(LaggedStart(FadeIn(t, shift=UP * 0.4), FadeIn(t2, shift=DOWN * 0.4),
                              lag_ratio=0.5, run_time=2))
        box = SurroundingRectangle(g, color=TEAL, buff=0.4)
        self.play(Create(box, run_time=1.2))
        self.play(box.animate.scale(1.02).set_color(AMBER), run_time=0.4)
        self.wait(3)


class S2Problem(Scene):
    def construct(self):
        left = VGroup(Rectangle(width=3.2, height=2.2, color=TEAL), title("saved", 28)).arrange(DOWN)
        left.move_to(LEFT * 3.4)
        right = VGroup(Rectangle(width=3.2, height=2.2, color=TEAL), title("read back", 28)).arrange(DOWN)
        right.move_to(RIGHT * 3.4)
        self.play(LaggedStart(Create(left), Create(right), lag_ratio=0.4, run_time=2))
        cell = Square(0.5, color=RED, fill_opacity=0.9).move_to(right[0].get_center())
        badge = kicker("1 BIT FLIPPED", 30, RED).next_to(right, DOWN)
        self.play(FadeIn(cell, scale=0.3), run_time=0.8)
        self.play(cell.animate.scale(1.4), rate_func=there_and_back, run_time=0.8)
        self.play(FadeIn(badge, shift=UP * 0.3), run_time=0.8)
        verdict = fit(kicker("ALMOST RIGHT IS WRONG.", 40, RED)).to_edge(DOWN)
        self.play(FadeIn(verdict, shift=UP * 0.4), run_time=1)
        self.wait(4)


class S3Recipe(Scene):
    def construct(self):
        bar = RoundedRectangle(corner_radius=0.15, width=9, height=0.9, color=AMBER, fill_opacity=0.25).to_edge(UP, buff=1.2)
        fname = kicker("photo.jpg — 2.4 MB", 26).next_to(bar, UP)
        self.play(FadeIn(bar), FadeIn(fname), run_time=1)
        segs, labels = [], []
        cols = [AMBER, TEAL, "#7aa2f7", "#9ece6a", "#bb9af7"]
        hashes = ["a3f9…", "77c1…", "e04b…", "9d22…", "c5aa…"]
        for i in range(5):
            s = Rectangle(width=1.7, height=0.9, color=cols[i], fill_opacity=0.7)
            s.move_to(bar.get_left() + RIGHT * (0.95 + i * 1.72))
            h = Text(hashes[i], font="DejaVu Sans Mono", font_size=20, color=INK)
            h.next_to(s, DOWN, buff=0.15)
            segs.append(s)
            labels.append(h)
        self.play(LaggedStart(*[FadeIn(s, shift=DOWN * 0.5) for s in segs],
                              lag_ratio=0.25, run_time=2.5))
        self.play(LaggedStart(*[FadeIn(h) for h in labels], lag_ratio=0.2, run_time=1.5))
        recipe = VGroup(
            kicker("RECIPE  photo.jpg", 26, TEAL),
            Text("0.0–0.5MB → a3f9 (zstd)", font="DejaVu Sans Mono", font_size=22, color=INK),
            Text("0.5–1.1MB → 77c1 (verbatim)", font="DejaVu Sans Mono", font_size=22, color=INK),
            Text("… 5 segments, all hashed", font="DejaVu Sans Mono", font_size=22, color=INK),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.18).to_edge(DOWN, buff=0.8)
        self.play(LaggedStart(*[FadeIn(l, shift=RIGHT * 0.4) for l in recipe],
                              lag_ratio=0.3, run_time=2.5))
        pulse = Dot(color=TEAL).move_to(recipe[1].get_right() + RIGHT * 0.3)
        self.play(FadeIn(pulse),
                  pulse.animate.move_to(segs[1].get_center()).set_color(AMBER),
                  run_time=1.5)
        tag = fit(kicker("CONTENT + RECIPE. PROVEN, NOT TRUSTED.", 32)).to_edge(DOWN, buff=0.2)
        self.play(FadeIn(tag), run_time=1)
        self.wait(4)


class S4WritePath(Scene):
    def construct(self):
        names = ["RAW  · stage fast", "Delta  · log", "Shadow  · consolidate"]
        rails = VGroup(*[
            RoundedRectangle(corner_radius=0.15, width=9.5, height=0.9, color=AMBER if i == 0 else (TEAL if i == 2 else INK),
                             fill_opacity=0.15) for i in range(3)
        ]).arrange(DOWN, buff=0.5)
        labels = VGroup(*[kicker(n, 24, AMBER if i == 0 else INK).move_to(r.get_center())
                           for i, (n, r) in enumerate(zip(names, rails))])
        self.play(LaggedStart(*[Create(r) for r in rails], lag_ratio=0.3, run_time=2))
        self.play(LaggedStart(*[FadeIn(l) for l in labels], lag_ratio=0.3, run_time=1.5))
        dots = VGroup(*[Dot(color=AMBER, radius=0.09) for _ in range(6)])
        dots.arrange(RIGHT, buff=0.4).move_to(rails[0].get_left() + RIGHT * 1.4 + DOWN * 0.22)
        self.play(LaggedStart(*[FadeIn(d) for d in dots], lag_ratio=0.15, run_time=1.5))
        self.play(dots.animate.shift(RIGHT * 6.5), run_time=2.5, rate_func=smooth)
        wave = Rectangle(width=9.5, height=1.0, color=TEAL, fill_opacity=0.3)
        wave.move_to(rails[2].get_center())
        wave.stretch_to_fit_width(0.6)
        self.play(FadeIn(wave, run_time=0.5),
                  wave.animate.stretch_to_fit_width(9.5).move_to(rails[2].get_center()),
                  run_time=3)
        self.play(FadeOut(wave), run_time=0.5)
        flag = Triangle(color=TEAL, fill_opacity=1).scale(0.25).next_to(rails[2], UP, buff=0.1)
        flag.shift(LEFT * 3)
        tag = fit(kicker("STAGE FAST. CONSOLIDATE OFFLINE. ROLL BACK ANY SWEEP.", 30))
        tag.to_edge(DOWN, buff=0.4)
        self.play(FadeIn(flag, shift=DOWN * 0.4), FadeIn(tag), run_time=1.2)
        self.wait(4)


class S5Packs(Scene):
    def construct(self):
        rack = RoundedRectangle(corner_radius=0.2, width=10, height=2.6, color=INK, fill_opacity=0.08).shift(UP * 0.4)
        self.play(Create(rack), run_time=1)
        cards = VGroup(*[
            VGroup(RoundedRectangle(corner_radius=0.12, width=2.1, height=1.4, color=AMBER, fill_opacity=0.25),
                   kicker(n, 26)).arrange(DOWN, buff=0.1)
            for n in ["FLAC", "TAR", "PNG", "qcow2"]
        ]).arrange(RIGHT, buff=0.35).move_to(rack.get_center())
        self.play(LaggedStart(*[FadeIn(c, shift=UP * 0.6) for c in cards],
                              lag_ratio=0.35, run_time=2.5))
        shield = Circle(1.1, color=TEAL).to_edge(DOWN, buff=0.9).shift(LEFT * 3)
        grid = VGroup(*[Square(0.3, color=INK, fill_opacity=0.3)
                        for _ in range(12)]).arrange_in_grid(3, 4, buff=0.12)
        grid.next_to(shield, RIGHT, buff=0.6)
        self.play(Create(shield, run_time=1.2), FadeIn(grid, run_time=1))
        tag = fit(kicker("SIGNED PACKS. SEALED BYTES.", 34)).to_edge(DOWN, buff=0.25)
        self.play(FadeIn(tag), run_time=1)
        self.wait(4)


class S5bBenchmarks(Scene):
    def construct(self):
        head = kicker("MEASURED, NOT PROMISED.", 36).to_edge(UP, buff=0.6)
        self.play(FadeIn(head, shift=DOWN * 0.3), run_time=1)
        rows = [
            ("mixed 30 MB corpus", "0.95× data", 0.95, "tar|zstd 1.14×"),
            ("text batches", "2.8×", 2.8, "PPMd/ZSTD"),
            ("4× duplicated → stored once", "3.76×", 3.76, "98→26 MB"),
        ]
        table = VGroup()
        bars = VGroup()
        for i, (a, b, val, c) in enumerate(rows):
            y = 1.0 - i * 1.5
            lab = Text(a, font=FONT, font_size=26, color=INK)
            lab.move_to(LEFT * 5 + UP * y).align_to(LEFT * 6.2, LEFT)
            num = kicker(b, 32, TEAL)
            num.move_to(RIGHT * 0.6 + UP * y)
            note = Text(c, font="DejaVu Sans Mono", font_size=20, color=INK)
            note.move_to(RIGHT * 4.2 + UP * y)
            w = min(val / 4.0, 1.0) * 3.4
            bar = Rectangle(width=w, height=0.3,
                            color=TEAL if i < 2 else AMBER, fill_opacity=0.7)
            bar.move_to(LEFT * 5 + UP * (y - 0.45)).align_to(LEFT * 6.2, LEFT)
            table.add(VGroup(lab, num, note))
            bars.add(bar)
        self.play(LaggedStart(*[FadeIn(r, shift=RIGHT * 0.4) for r in table],
                              lag_ratio=0.4, run_time=3))
        self.play(LaggedStart(*[GrowFromEdge(b, LEFT) for b in bars],
                              lag_ratio=0.35, run_time=2.5))
        self.wait(4)


class S6CTA(Scene):
    def construct(self):
        cmd = Text("curl …/releases/latest/download/bootstrap.sh | sudo sh",
                   font="DejaVu Sans Mono", font_size=30, color=TEAL)
        cmd.to_edge(UP, buff=1.5)
        self.play(AddTextLetterByLetter(cmd, run_time=3))
        links = VGroup(*[kicker(t, 28) for t in
                         ["releases · install guides · pack registry"]])
        links.next_to(cmd, DOWN, buff=0.8)
        self.play(FadeIn(links, shift=UP * 0.3), run_time=1.2)
        end = fit(title("PROVE IT, DON'T PROMISE IT.", 48)).to_edge(DOWN, buff=1.0)
        box = SurroundingRectangle(end, color=AMBER, buff=0.35)
        self.play(FadeIn(end, shift=UP * 0.4), Create(box), run_time=1.5)
        self.wait(4)

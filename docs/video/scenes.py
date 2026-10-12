# InvariantFS overview v2 — manim scenes. Thesis: complexity demands proof.
# Dark slate bg, amber/teal/red, Inter/DejaVu, no LaTeX. Hard cuts, 3-8 s/scene.
# Draft: manim -qm --fps 30 scenes.py -a | Final: manim -qh --fps 120 scenes.py -a
from manim import *

config.background_color = "#0d1117"
AMBER, TEAL, RED, INK = "#f0a500", "#39c5cf", "#e5484d", "#e6edf3"
FONT = "Inter"
MONO = "DejaVu Sans Mono"


def title(text, size=52):
    return Text(text, font=FONT, font_size=size, color=INK, weight=BOLD)


def kicker(text, size=30, color=AMBER):
    return Text(text, font=FONT, font_size=size, color=color, weight=BOLD)


def fit(obj, max_w=12.0):
    if obj.width > max_w:
        obj.scale(max_w / obj.width)
    return obj


def stamp(text="PROVEN", color=TEAL):
    return kicker("✓ " + text, 28, color)


class S1Hook(Scene):
    def construct(self):
        bits = VGroup(*[Square(0.55, color=INK, fill_opacity=0.25) for _ in range(8)])
        bits.arrange(RIGHT, buff=0.12).shift(UP * 1.2)
        self.play(LaggedStart(*[FadeIn(b) for b in bits], lag_ratio=0.08, run_time=1.2))
        self.play(bits[4].animate.set_color(RED).set_fill(opacity=0.95), run_time=0.4)
        self.play(bits[4].animate.scale(1.5), rate_func=there_and_back, run_time=0.5)
        v = fit(kicker("ALMOST RIGHT IS WRONG.", 44, RED))
        v.next_to(bits, DOWN, buff=0.8)
        self.play(FadeIn(v, shift=UP * 0.3), run_time=0.7)
        self.wait(2)


class S2TrustProof(Scene):
    def construct(self):
        div = Line(UP * 3.2, DOWN * 3.2, color=INK, stroke_opacity=0.4)
        left = VGroup(kicker("TRUST", 40, INK),
                      Text("codec? disk? luck?", font=FONT, font_size=26, color=INK))
        left.arrange(DOWN).move_to(LEFT * 3.5)
        right = VGroup(kicker("PROOF", 40, TEAL),
                       Text("every step stamped", font=FONT, font_size=26, color=INK))
        right.arrange(DOWN).move_to(RIGHT * 3.5)
        self.play(Create(div, run_time=0.6),
                  LaggedStart(FadeIn(left), FadeIn(right), lag_ratio=0.3, run_time=1.5))
        tag = fit(kicker("COMPLEXITY DEMANDS PROOF.", 36))
        tag.to_edge(DOWN, buff=0.6)
        self.play(FadeIn(tag, shift=UP * 0.3), run_time=0.8)
        self.wait(3)


class S3Decompose(Scene):
    def construct(self):
        box = RoundedRectangle(corner_radius=0.15, width=4.4, height=1.4,
                               color=AMBER, fill_opacity=0.25)
        box.shift(UP * 2.2)
        name = kicker("backup.qcow2 — 1.1 GB", 28)
        name.next_to(box, UP, buff=0.2)
        self.play(FadeIn(box), FadeIn(name), run_time=0.8)
        parts, hashes = [], ["9f02…", "41bc…", "d771…", "03e9…"]
        cols = [AMBER, TEAL, "#7aa2f7", "#9ece6a"]
        for i in range(4):
            p = RoundedRectangle(corner_radius=0.1, width=2.4, height=1.0,
                                 color=cols[i], fill_opacity=0.6)
            p.move_to(DOWN * 0.6 + LEFT * 4.2 + RIGHT * i * 2.8)
            h = Text(hashes[i], font=MONO, font_size=20, color=INK)
            h.next_to(p, DOWN, buff=0.12)
            parts.append(VGroup(p, h))
        self.play(box.animate.scale(0.6).set_opacity(0.15), run_time=0.8)
        self.play(LaggedStart(*[FadeIn(p, shift=DOWN * 0.8) for p in parts],
                              lag_ratio=0.25, run_time=2))
        tag = fit(kicker("SPLIT APART. EACH PIECE ON ITS OWN. BACK BIT-EXACT.", 30))
        tag.to_edge(DOWN, buff=0.4)
        self.play(FadeIn(tag), run_time=0.8)
        self.wait(3)


class S4Gauntlet(Scene):
    def construct(self):
        gates = ["CODEC", "SPLIT", "DEDUP", "RE-ENCODE", "MIGRATE", "SEAL"]
        row = VGroup()
        for i, gname in enumerate(gates):
            gate = VGroup(Rectangle(width=1.7, height=2.2, color=AMBER, fill_opacity=0.12),
                          kicker(gname, 18))
            gate.arrange(DOWN, buff=0.15)
            gate.shift(LEFT * 5.2 + RIGHT * i * 2.05 + UP * 0.4)
            row.add(gate)
        self.play(LaggedStart(*[Create(g[0]) for g in row],
                              lag_ratio=0.15, run_time=1.8))
        self.play(LaggedStart(*[FadeIn(g[1]) for g in row], lag_ratio=0.15, run_time=1.5))
        pkt = Dot(color=TEAL, radius=0.16).move_to(row[0].get_left() + LEFT * 0.8)
        self.play(FadeIn(pkt), run_time=0.3)
        for g in row:
            self.play(pkt.animate.move_to(g.get_center()), run_time=0.45,
                      rate_func=smooth)
            s = stamp()
            s.scale(0.75).next_to(g, DOWN, buff=0.1)
            self.play(FadeIn(s, scale=0.5), run_time=0.3)
            self.play(FadeOut(s), run_time=0.2)
        self.play(pkt.animate.move_to(row[-1].get_right() + RIGHT * 0.8), run_time=0.4)
        tag = fit(kicker("NO STAMP, NO TRANSFORM.", 34))
        tag.to_edge(DOWN, buff=0.4)
        self.play(FadeIn(tag), run_time=0.8)
        self.wait(2)


class S5Numbers(Scene):
    def construct(self):
        head = fit(kicker("MEASURED, NOT PROMISED.", 40))
        head.to_edge(UP, buff=0.7)
        self.play(FadeIn(head, shift=DOWN * 0.3), run_time=0.8)
        data = [("mixed 30 MB", "0.95×", "honest"), ("text batches", "2.8×", "PPMd"),
                ("4× dupes → once", "3.76×", "98→26 MB")]
        for i, (a, b, c) in enumerate(data):
            y = 0.8 - i * 1.5
            lab = Text(a, font=FONT, font_size=26, color=INK)
            lab.move_to(LEFT * 5 + UP * y).align_to(LEFT * 6.2, LEFT)
            num = kicker(b, 34, TEAL if i else AMBER)
            num.move_to(RIGHT * 1.2 + UP * y)
            note = Text(c, font=MONO, font_size=20, color=INK)
            note.move_to(RIGHT * 4.4 + UP * y)
            w = min(float(b.replace("×", "")) / 4.0, 1.0) * 3.0
            bar = Rectangle(width=w, height=0.28,
                            color=TEAL if i else AMBER, fill_opacity=0.7)
            bar.move_to(LEFT * 5 + UP * (y - 0.45)).align_to(LEFT * 6.2, LEFT)
            self.play(FadeIn(lab, shift=RIGHT * 0.3), FadeIn(num, scale=0.6),
                      GrowFromEdge(bar, LEFT), FadeIn(note), run_time=1.2)
        self.wait(3)


class S6SaveSeal(Scene):
    def construct(self):
        flag = Triangle(color=TEAL, fill_opacity=1).scale(0.3).shift(LEFT * 2 + UP * 0.6)
        vol = RoundedRectangle(corner_radius=0.15, width=3.4, height=1.6,
                               color=INK, fill_opacity=0.1).shift(LEFT * 2 + DOWN * 0.8)
        self.play(FadeIn(vol), FadeIn(flag, shift=DOWN * 0.4), run_time=0.8)
        back = kicker("⟲ ROLL BACK ANY SWEEP", 26, TEAL).shift(LEFT * 2 + DOWN * 2.2)
        self.play(FadeIn(back), run_time=0.6)
        shield = Circle(1.0, color=TEAL).shift(RIGHT * 3.2)
        self.play(Create(shield, run_time=1.0))
        s = stamp("SEALED")
        s.move_to(shield.get_center())
        self.play(FadeIn(s, scale=0.5), run_time=0.5)
        self.wait(3)


class S7CTA(Scene):
    def construct(self):
        cmd = Text("curl …/releases/latest/download/bootstrap.sh | sudo sh",
                   font=MONO, font_size=30, color=TEAL)
        cmd.to_edge(UP, buff=1.6)
        self.play(AddTextLetterByLetter(cmd, run_time=2.5))
        end = fit(title("PROVE IT, DON'T PROMISE IT.", 46))
        end.next_to(cmd, DOWN, buff=1.0)
        box = SurroundingRectangle(end, color=AMBER, buff=0.35)
        self.play(FadeIn(end, shift=UP * 0.3), Create(box), run_time=1.2)
        self.wait(3)

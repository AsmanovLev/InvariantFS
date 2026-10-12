#!/usr/bin/env python3
# docs/video/sfx.py — synthesize the whole SFX kit with ffmpeg lavfi.
# Zero samples, zero licenses: pink-noise whooshes, sine-decay pops,
# harmonic dings, layered-sine ambient pad. 48k mono wav into sfx/.
import os
import subprocess

SR = 48000
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sfx")
os.makedirs(OUT, exist_ok=True)


def run(args):
    subprocess.run(["ffmpeg", "-hide_banner", "-y", *args],
                   check=True, capture_output=True)


def synth(name, lavfi, dur):
    out = os.path.join(OUT, name + ".wav")
    if os.path.exists(out):
        print("keep", name)
        return
    run(["-f", "lavfi", "-i", lavfi, "-t", str(dur),
         "-ar", str(SR), "-ac", "1", "-c:a", "pcm_s16le", out])
    print("made", name)


# whoosh: pink noise, band sweep feel via high/lowpass + symmetric fade
synth("whoosh",
      f"anoisesrc=color=pink:r={SR}:d=1.2,"
      "highpass=f=300,lowpass=f=2400,"
      "afade=t=in:st=0:d=0.7:curve=qsin,"
      "afade=t=out:st=0.7:d=0.5:curve=qsin,volume=0.9", 1.2)
# pop: bright sine spike, fast decay (titles, cards)
synth("pop",
      f"aevalsrc=0.9*sin(2*PI*820*t)*exp(-22*t)"
      f"+0.3*sin(2*PI*1640*t)*exp(-30*t):s={SR}:d=0.25", 0.25)
# ding: harmonic bell (proof moments), E6+B6+E7-ish stack
synth("ding",
      f"aevalsrc=0.55*sin(2*PI*1318.5*t)*exp(-5*t)"
      f"+0.28*sin(2*PI*2637*t)*exp(-7*t)"
      f"+0.12*sin(2*PI*3951*t)*exp(-9*t):s={SR}:d=1.0", 1.0)
# stamp: low thump for verdict lines
synth("stamp",
      f"aevalsrc=sin(2*PI*110*t)*exp(-9*t)"
      f"+0.4*sin(2*PI*220*t)*exp(-12*t):s={SR}:d=0.5", 0.5)
# tick: typewriter tick for terminal typing
synth("tick",
      f"aevalsrc=0.7*sin(2*PI*2400*t)*exp(-60*t):s={SR}:d=0.06", 0.06)
# pad: 8 s warm bed (A1+E2+A2-ish stack, slow attack), looped under all
synth("pad",
      f"aevalsrc=0.30*sin(2*PI*110*t)+0.22*sin(2*PI*164.8*t)"
      f"+0.18*sin(2*PI*220*t):s={SR}:d=8,"
      "afade=t=in:st=0:d=3,afade=t=out:st=5:d=3,volume=0.5", 8)
print("kit ready:", OUT)

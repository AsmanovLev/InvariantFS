# Storyboard + motion spec (draft for review)

Style: dark slate background (#0d1117), accent amber (#f0a500) for
InvariantFS elements, teal (#39c5cf) for verified/good states, red for
corruption. Font: Inter, bold titles. 1920×1080, 30 fps draft / 120 fps
final. All text also shipped as `subs_en.srt` + `subs_ru.srt`.

| # | t | visual | motion | text overlay |
|---|---|--------|--------|--------------|
| S1 | 0:00–0:30 | photo → pixelate (scale blocks up) → snap back | pixelation ramps 0→12px over 8 s, hold 2 s, snap to 0 in 6 frames + flash | WHAT YOU WRITE COMES BACK BYTE-IDENTICAL. |
| S2 | 0:30–1:30 | doc card left; clone right, one byte cell flips red, checksum badge FAIL | byte cell pulses; FAIL badge stamps in with shake | ALMOST RIGHT IS WRONG. |
| S3 | 1:30–3:00 | file bar splits into 5 colored segments; hash labels count up; recipe card builds line by line; read head jumps to segment 2 only | segments slide apart (stagger 0.2 s); hash labels typewriter; read pulse travels recipe→segment→file | CONTENT + RECIPE. PROVEN, NOT TRUSTED. |
| S4 | 3:00–4:30 | three zone rails (RAW / Delta / Shadow); packet dots flow write→RAW, fold arrows merge log→tree, sweep wave drains RAW→Shadow; flag plants, rewind arrow | dots 60/s; sweep wave left→right 4 s; flag drops with bounce; rewind plays flow backwards 2× | STAGE FAST. CONSOLIDATE OFFLINE. ROLL BACK ANY SWEEP. |
| S5 | 4:30–5:30 | pack cards (FLAC/TAR/PNG/qcow2) slot into a rack; parity shield draws over block grid; terminal types install + verify OK | cards fly in stagger; shield draws clockwise; terminal types at 30 cps | SIGNED PACKS. SEALED BYTES. |
| S6 | 5:30–6:00 | install one-liner types itself; link cards fade in; end card | typing 25 cps; links stagger fade; hold 4 s | PROVE IT, DON'T PROMISE IT. |

## Build

- `scenes.py` — manim scenes S1..S6 (Text + Rectangle + VGroup only, no
  LaTeX; DejaVu fallback if Inter missing).
- Draft: `manim -qm --fps 30` per scene → `ffmpeg` concat.
- Final: `manim -qh --fps 120` → concat → single svtav1 pass:
  `ffmpeg -i concat.mp4 -c:v libsvtav1 -preset 8 -crf 40 -r 120
   -s 1920x1080 -g 600 -pix_fmt yuv420p invfs-overview.mp4`
  (gop = 5 s × 120 fps = 600).
- Subtitles muxed from SCRIPT.md lines (`subs_en.srt`, `subs_ru.srt`).

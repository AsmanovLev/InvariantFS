# avrecode three-profile bench (synthetic H.264 corpus)

Date: 2026-10-13. Host: Debian 13 (system `ffmpeg` 7.1.5-0+deb13u1,
gcc 14.2.0, nasm present, `protoc` libprotoc 3.21.12).

Question: avrecode (Dropbox, BSD) losslessly re-compresses H.264 back
to exact source bytes — the video analogue of the JXL-reconstruction /
PNGR pattern. But per the 2015 Solveig analysis it wins ONLY on
CAVLC/ultrafast encodes and ERRORS on CABAC+B-frame content. Bench
three profiles before any lane talk. Upstream warns "trusted inputs
only, does not validate input" — sandboxed exec is the assumed shape
for anything downstream, not this WP's question.

## Tool build

- avrecode @ `0749548` ("fix buffer overrun in unzigzag lookup..."),
  ffmpeg-fork submodule @ `48e5c63`. Benched binary `recode` built
  from that tree (10.6 MB, ASan-instrumented per the upstream
  Makefile default `-fsanitize=address`).
- Fork configure (known-good flags from the task doc; `config.log`
  records one addition, `--disable-inline-asm`, needed by the
  2016-era fork under the gcc-14 toolchain):
  `./configure --disable-everything --disable-network
  --enable-decoder=h264 --enable-parser=h264 --enable-demuxer=mov
  --enable-protocol=file --disable-doc --disable-inline-asm`
- One flag-level fix to `recode.cpp` (6 lines, build-only, does not
  touch the model): the `av_unique_ptr` deleter wrappers took
  `std::function<void(T*&)>`, which gcc-14 no longer accepts;
  changed to `void(T*)` with the lambda taking `T*` by value. No
  upstream fork, no other patches (ffmpeg submodule tree clean).
- Bench ran in `/tmp` (no binaries in tree). Build tree and logs
  kept out of tree; only this text ships.

## Corpus

Three synthetic MP4s, `ffmpeg -f lavfi` `testsrc2`, 1280x720, 30 fps,
10 s each, then `-c copy -movflags faststart` remux (moov-before-mdat
verified per file: `moov@36 < mdat@~2-4k in all three). Canonical
recipe (system ffmpeg + libx264; exact bytes vary by x264 build, the
verified properties below are what the legs depend on):

```
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30:duration=10 \
  -c:v libx264 -preset ultrafast -x264-params cabac=0 -pix_fmt yuv420p a-ultrafast-cavlc.mp4
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30:duration=10 \
  -c:v libx264 -preset veryfast -bf 0 -pix_fmt yuv420p b-phone-cabac-nobf.mp4
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30:duration=10 \
  -c:v libx264 -preset medium -pix_fmt yuv420p c-youtube-cabac-bf.mp4
```

Profile verification per benched file (ffprobe stream profile +
`has_b_frames`, cross-checked against the x264 settings SEI
`cabac=`/`bframes=` strings embedded in each file — an unlabeled
profile would void the leg):

| File (faststart) | Bytes | ffprobe profile | `cabac=` | `bframes=` | Class |
|---|---|---|---|---|---|
| `a-ultrafast-cavlc-fs.mp4` | 9,364,931 | Constrained Baseline, `has_b_frames=0` | 0 | 0 | CAVLC, no B |
| `b-phone-cabac-nobf-fs.mp4` | 3,750,723 | High, `has_b_frames=0` | 1 | 0 | CABAC, no B |
| `c-youtube-cabac-bf-fs.mp4` | 3,901,473 | High, `has_b_frames=2` | 1 | 3 | CABAC + B |

## Legs

Per profile: `recode compress IN OUT.avrc` (ratio), `recode roundtrip
IN` (tool's own check + reported ratio), and where a non-empty
`.avrc` resulted, `recode decompress` + `cmp` against the original.
Exit codes recorded explicitly, no piped suites.

| Profile | compress exit | .avrc bytes | roundtrip exit | decompress + cmp | Verdict |
|---|---|---|---|---|---|
| A CAVLC/no-B | 0 | 9,364,941 (**+10 B** over input; tool reports ratio 100%, protobuf overhead 0.000106781%) | 0 "roundtrip succeeded" | exit 0, **byte-identical** | NOT lane-worthy: correct, but zero savings |
| B CABAC/no-B | **134 (SIGABRT)**: `recode.cpp:946: h264_model::end_coding_type: Assertion num_nonzeros... failed`, 0-byte output | n/a | **134**, same assertion | n/a | NOT lane-worthy: hard crash |
| C CABAC+B | **134 (SIGABRT)**: `recode.cpp:1033: h264_model::update_state_for_model_key: Assertion symbol == (num_nonzeros == nonzeros_observed) failed`, 0-byte output | n/a | **134**, same assertion | n/a | NOT lane-worthy: hard crash |

## Reading

- The Solveig shape reproduces exactly: CAVLC roundtrips bit-exact,
  both CABAC profiles abort in model assertions. The failures are
  `assert()`-driven SIGABRTs, not clean error returns — contained
  under the assumed sandboxed-exec shape, but a lane could never link
  this in-process and would have to treat non-zero exit as refuse.
- Even the "winning" profile earns nothing here: on synthetic
  `testsrc2` CAVLC the re-encode is 10 bytes LARGER than the input.
  (Synthetic caveat cuts both ways — real-world ultrafast/CAVLC
  captures might show the small wins Solveig reported — but a lane
  needs a measured win on a real corpus, not a 2015 citation.)
- Overall: **decline by default.** An avrecode lane earns existence
  only if ALL of these hold: (1) inputs are pre-sniffed CAVLC-only
  (cheap gate: Baseline profile / `cabac=0` in SPS or settings SEI —
  anything else refuses before exec); (2) a measured saving on a real
  CAVLC corpus that pays for the lane's complexity (this bench: 0 B);
  (3) exec remains sandboxed with abort-tolerant refusal, given the
  tool aborts rather than errors. None holds today — no lane.

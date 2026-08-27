#!/usr/bin/env bash
# build.sh -- assemble frames/slide-*.png into invfs-overview.mp4
# H.264 / yuv420p / 30 fps, fade-to-background transitions (filtergraph
# generated deterministically by gen-overview.py into frames/filter.txt).
set -euo pipefail
cd "$(dirname "$0")"

ffmpeg -y -hide_banner -loglevel warning \
  -loop 1 -t 7 -i frames/slide-01.png \
  -loop 1 -t 7 -i frames/slide-02.png \
  -loop 1 -t 7 -i frames/slide-03.png \
  -loop 1 -t 7 -i frames/slide-04.png \
  -loop 1 -t 7 -i frames/slide-05.png \
  -loop 1 -t 7 -i frames/slide-06.png \
  -loop 1 -t 7 -i frames/slide-07.png \
  -loop 1 -t 7 -i frames/slide-08.png \
  -loop 1 -t 7 -i frames/slide-09.png \
  -loop 1 -t 7 -i frames/slide-10.png \
  -loop 1 -t 7 -i frames/slide-11.png \
  -loop 1 -t 7 -i frames/slide-12.png \
  -loop 1 -t 7 -i frames/slide-13.png \
  -loop 1 -t 7 -i frames/slide-14.png \
  -loop 1 -t 7 -i frames/slide-15.png \
  -loop 1 -t 7 -i frames/slide-16.png \
  -loop 1 -t 7 -i frames/slide-17.png \
  -loop 1 -t 7 -i frames/slide-18.png \
  -loop 1 -t 7 -i frames/slide-19.png \
  -loop 1 -t 7 -i frames/slide-20.png \
  -/filter_complex frames/filter.txt \
  -map "[v]" \
  -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -r 30 \
  -movflags +faststart \
  invfs-overview.mp4

ffprobe -v error \
  -show_entries stream=codec_name,width,height,avg_frame_rate,nb_frames \
  -show_entries format=duration,size \
  -of default=noprint_wrappers=1 \
  invfs-overview.mp4

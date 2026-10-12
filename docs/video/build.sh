#!/bin/sh
# docs/video/build.sh — assemble the overview v2.
#   sh build.sh draft   720p30 concat + sound, for review (Telegram)
#   sh build.sh final   1080p120 + svtav1 p6 crf30 + opus 64k vbr + faststart
set -eu
cd "$(dirname "$0")"
MODE="${1:-draft}"

mix_sound() { # $1 = silent concat, $2 = out
    IN="$1"; OUT="$2"
    CUES="riser|0.2 stamp|2.2 riser|5.5 pop|8.0 riser|11.5 stamp|15.0 riser|20.0 pop|26.0 riser|33.0 ding|42.0 riser|50.0 stamp|55.0 riser|62.0 ding|66.0 riser|70.0 stamp|73.0"
    INPUTS="-i $IN"
    FILT="[0:a]volume=0[m0];"
    MIXINS="[m0]"
    N=1
    INPUTS="$INPUTS -stream_loop -1 -i sfx/pad.wav"
    DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$IN")
    FILT="${FILT}[$N:a]atrim=end=$DUR,asetpts=PTS-STARTPTS,volume=-22dB[bed];"
    MIXINS="$MIXINS[bed]"
    N=$((N + 1))
    COUNT=2
    for c in $CUES; do
        sfx="${c%%|*}"; at="${c##*|}"
        if awk "BEGIN{exit !( $at < $DUR )}"; then
            INPUTS="$INPUTS -i sfx/$sfx.wav"
            ms=$(awk "BEGIN{printf \"%d\", $at*1000}")
            FILT="${FILT}[$N:a]adelay=${ms}|${ms},volume=-8dB[s$N];"
            MIXINS="$MIXINS[s$N]"
            COUNT=$((COUNT + 1))
            N=$((N + 1))
        fi
    done
    FILT="${FILT}${MIXINS}amix=inputs=$COUNT:duration=first:normalize=0,alimiter=limit=0.98[mix]"
    # shellcheck disable=SC2086
    ffmpeg -hide_banner -y $INPUTS -filter_complex "$FILT" \
        -map 0:v -c:v copy -map "[mix]" -c:a aac -b:a 160k -ar 48000 \
        -movflags +faststart "$OUT"
    echo "mixed -> $OUT"
}

join() { # $1 = scene dir, $2 = out
    ls "$PWD/$1"/S*.mp4 | sort > /tmp/vjoin.txt
    sed -i "s/^/file '/;s/$/'/" /tmp/vjoin.txt
    ffmpeg -hide_banner -y -f concat -safe 0 -i /tmp/vjoin.txt -c copy "$2"
}

if [ "$MODE" = draft ]; then
    join media/videos/scenes/720p30 draft_silent.mp4
    ffmpeg -hide_banner -y -i draft_silent.mp4 -f lavfi -i anullsrc=r=48000:cl=stereo \
        -shortest -c:v copy -c:a aac draft_base.mp4
    mix_sound draft_base.mp4 draft.mp4
elif [ "$MODE" = final ]; then
    join media/videos/scenes/1080p120 final_silent.mp4
    ffmpeg -hide_banner -y -i final_silent.mp4 -f lavfi -i anullsrc=r=48000:cl=stereo \
        -shortest -c:v copy -c:a aac final_base.mp4
    mix_sound final_base.mp4 final_mixed.mp4
    ffmpeg -hide_banner -y -i final_mixed.mp4 \
        -c:v libsvtav1 -preset 6 -crf 30 -r 120 -s 1920x1080 -g 600 \
        -pix_fmt yuv420p -c:a libopus -b:a 64k -vbr on -ar 48000 \
        -movflags +faststart invfs-overview.mp4
    echo "FINAL -> invfs-overview.mp4"
else
    echo "usage: build.sh [draft|final]"; exit 2
fi

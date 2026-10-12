#!/bin/sh
# docs/video/build.sh — assemble the overview video.
#   sh build.sh draft   720p30 concat + sound, for review
#   sh build.sh final   1080p120 scenes + sound + svtav1 (preset 8, crf 40, gop 600)
set -eu
cd "$(dirname "$0")"
MODE="${1:-draft}"

# Scene order with SFX cues (seconds into the ASSEMBLED timeline;
# measured after concat with ffprobe; first draft values, re-pin on change).
# whoosh on every scene cut, pop on card entries, ding on proof lines,
# stamp on verdict lines, tick under terminal typing, pad bed throughout.
sfx_at() { # <sfx> <at-sec> <inputs-so-far... (appended by caller)>
    echo "$1|$2"
}

mix_sound() { # $1 = silent concat, $2 = out
    IN="$1"; OUT="$2"
    # cue plan: (sfx, at)
    CUES="whoosh|0.2 pop|1.5 pop|8.0 whoosh|16.0 ding|24.0 pop|30.0 whoosh|44.0 stamp|52.0 ding|60.0 whoosh|68.0 pop|74.0 stamp|82.0 whoosh|92.0 ding|100.0 whoosh|112.0 stamp|118.0"
    INPUTS="-i $IN"
    FILT="[0:a]volume=0[m0];"
    MIXINS="[m0]"
    N=1
    # bed: looped pad at low level
    INPUTS="$INPUTS -stream_loop -1 -i sfx/pad.wav"
    DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$IN")
    FILT="${FILT}[$N:a]atrim=end=$DUR,asetpts=PTS-STARTPTS,volume=-22dB[bed];"
    MIXINS="$MIXINS[bed]"
    N=$((N + 1))
    COUNT=2
    for c in $CUES; do
        sfx="${c%%|*}"; at="${c##*|}"
        # skip cues past the end (draft vs final timing drift)
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

if [ "$MODE" = draft ]; then
    D="$PWD/media/videos/scenes/720p30"
    ls "$D"/S*.mp4 | sort > /tmp/vjoin.txt
    sed -i "s/^/file '/;s/$/'/" /tmp/vjoin.txt
    ffmpeg -hide_banner -y -f concat -safe 0 -i /tmp/vjoin.txt -c copy draft_silent.mp4
    # silent track so the mix graph has [0:a]
    ffmpeg -hide_banner -y -i draft_silent.mp4 -f lavfi -i anullsrc=r=48000:cl=stereo \
        -shortest -c:v copy -c:a aac draft_base.mp4
    mix_sound draft_base.mp4 draft.mp4
elif [ "$MODE" = final ]; then
    D="$PWD/media/videos/scenes/1080p120"
    ls "$D"/S*.mp4 | sort > /tmp/vjoin.txt
    sed -i "s/^/file '/;s/$/'/" /tmp/vjoin.txt
    ffmpeg -hide_banner -y -f concat -safe 0 -i /tmp/vjoin.txt -c copy final_silent.mp4
    ffmpeg -hide_banner -y -i final_silent.mp4 -f lavfi -i anullsrc=r=48000:cl=stereo \
        -shortest -c:v copy -c:a aac final_base.mp4
    mix_sound final_base.mp4 final_mixed.mp4
    DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 final_mixed.mp4)
    echo "final duration: $DUR s"
    ffmpeg -hide_banner -y -i final_mixed.mp4 \
        -c:v libsvtav1 -preset 8 -crf 40 -r 120 -s 1920x1080 -g 600 \
        -pix_fmt yuv420p -c:a aac -b:a 160k -ar 48000 \
        -movflags +faststart invfs-overview.mp4
    echo "FINAL -> invfs-overview.mp4"
else
    echo "usage: build.sh [draft|final]"; exit 2
fi

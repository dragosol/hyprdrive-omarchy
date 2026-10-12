#!/usr/bin/env bash
# Rotate the quickshell wallpaper. Writes background.wallpaperPath and lets
# quickshell's file watch pick it up (verified: 76% of pixels change ~4s later).
#
# Deliberately NOT switchwall.sh: that runs matugen + a python venv + kde
# -material-you and re-derives the Material palette, which drives
# general:col.active_border -- the exact value X2/X3/X4 measure against. A
# wallpaper rotation must not move the goalposts of the test suite. Verified
# 2026-08-02: a rotation leaves col.active_border and material_colors.scss
# byte-identical, so quickshell does not re-theme on its own either.
#
# THEME=1 turns that off for one call, and only for one call. Phase 10's theme
# adoption pipeline begins at the palette, so a gate that wants to watch a
# wallpaper swap re-theme the desktop has to be allowed to move the goalposts on
# purpose; every other caller, including the timer, still must not. The matugen
# invocation is ii's own -- the arguments switchwall.sh builds, minus the venv,
# the kde bridge and the Gemini categoriser, none of which touch the palette.
#
# Driven by a systemd user timer in the guest (hyprwall.timer, OnUnitActiveSec
# =3min). x1_fixture.sh stops it for the duration of a stress run.
#
# VIDEO (Phase 12). A wallpaper can be an mp4. mpvpaper draws it, ii's config
# still records the path so the shell and every profile snapshot see one
# wallpaper setting rather than two, and the palette -- if THEME=1 -- is derived
# from a frame ffmpeg pulls out of it, because matugen reads images. Switching
# back to a still kills mpvpaper; that is what "turn the video wallpaper off"
# means and there is no separate switch for it.
#
#   hyprwall-rotate.sh                 pick a random other wallpaper
#   WALL=/path/to.jpg hyprwall-rotate.sh   pick that one (gates: be deterministic)
#   WALL=/path/to.mp4 hyprwall-rotate.sh   ...including a video, via mpvpaper
#   THEME=1 hyprwall-rotate.sh         ...and re-derive the palette from it
set -u
DIR=${DIR:-/mnt/host/wallpapers}
CFG=$HOME/.config/illogical-impulse/config.json
[ -d "$DIR" ] || exit 0

CUR=$(jq -r '.background.wallpaperPath' "$CFG")

# A live wallpaper is a video path plus an mpvpaper process, and rotating a
# still image over it would kill the wallpaper every 3 minutes. Stand down and
# leave it alone -- switching back to stills is what turns the slideshow on.
#
# ONLY WHEN NOBODY NAMED A WALLPAPER. This guard is for the timer: an unattended
# rotation must not stamp on a video somebody chose. A caller that passed WALL
# meant it, and refusing them was the old behaviour refusing the settings app the
# ability to switch away from a video at all.
if [ -z "${WALL:-}" ]; then
    case "${CUR,,}" in
        *.mp4|*.webm|*.mkv|*.avi|*.mov) exit 0 ;;
    esac
    pgrep -x mpvpaper >/dev/null && exit 0
fi

NEXT=${WALL:-$(find "$DIR" -maxdepth 1 -name '*.jpg' ! -wholename "$CUR" | shuf -n1)}
[ -n "$NEXT" ] && [ -f "$NEXT" ] || exit 0

# mktemp creates the temp file 0600 and chmod --reference gives it the config's
# own mode, so a private config is never replaced by a umask-default 0644 copy
# (marketplace review, #11012).
TMP=$(mktemp "$CFG.XXXXXX") || exit 1
if jq --arg p "$NEXT" '.background.wallpaperPath = $p' "$CFG" > "$TMP" \
    && chmod --reference="$CFG" "$TMP" && mv "$TMP" "$CFG"; then :; else rm -f "$TMP"; exit 1; fi
echo "wallpaper -> $NEXT"

# The image matugen reads. For a still it is the wallpaper; for a video it is one
# frame of it, which is the only thing a palette generator can be given.
SOURCE=$NEXT
case "${NEXT,,}" in
    *.mp4|*.webm|*.mkv|*.avi|*.mov)
        pkill -x mpvpaper >/dev/null 2>&1
        sleep 0.3
        SOURCE=${XDG_RUNTIME_DIR:-/tmp}/hyprdrive-wallframe.png
        if command -v ffmpeg >/dev/null 2>&1; then
            # One frame, a second in: the very first frame of a video is very
            # often black, and a palette derived from black is a grey desktop.
            ffmpeg -y -loglevel error -ss 1 -i "$NEXT" -frames:v 1 "$SOURCE" </dev/null \
                || SOURCE=
        else
            echo "no ffmpeg -- the palette cannot follow a video wallpaper" >&2
            SOURCE=
        fi
        if command -v mpvpaper >/dev/null 2>&1; then
            OUTPUT=$(hyprctl monitors -j 2>/dev/null | jq -r '.[0].name' 2>/dev/null)
            setsid mpvpaper -o "no-audio --loop-file=inf --panscan=1.0" \
                "${OUTPUT:-*}" "$NEXT" >/dev/null 2>&1 </dev/null &
            echo "mpvpaper -> ${OUTPUT:-*}"
        else
            echo "no mpvpaper -- the video is recorded as the wallpaper but nothing is drawing it" >&2
        fi
        ;;
    *)
        # A still replaces a video: whatever was drawing the old one has to go,
        # or it keeps drawing over the shell's wallpaper layer forever.
        pkill -x mpvpaper >/dev/null 2>&1
        ;;
esac

if [ "${THEME:-0}" = 1 ]; then
    # ii's own colorgen arguments: scheme type out of its config, dark/light out
    # of the GNOME setting switchwall.sh itself writes.
    TYPE=$(jq -r '.appearance.palette.type // "scheme-tonal-spot"' "$CFG")
    case "$TYPE" in scheme-*) ;; *) TYPE=scheme-tonal-spot ;; esac
    MODE=dark
    [ "$(gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null)" = "'prefer-light'" ] && MODE=light
    [ -n "$SOURCE" ] && [ -f "$SOURCE" ] || { echo "no image to derive a palette from" >&2; exit 1; }
    matugen --source-color-index 0 image "$SOURCE" --mode "$MODE" --type "$TYPE" >/dev/null || exit 1
    echo "palette -> $(jq -r .primary "${XDG_STATE_HOME:-$HOME/.local/state}/quickshell/user/generated/colors.json")"
fi

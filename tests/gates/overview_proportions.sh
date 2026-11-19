#!/usr/bin/env bash
# overview_proportions -- the overview's search row, results and strip keep their share of a short screen (ui_scale auto), never overlap, and 3840x2160 is unchanged.
# Protects the proportions (src/core/UiScale.hpp and every reader of
# overviewUi(): layoutSearch, the results band maths in SearchBand.hpp, the
# strip band and pads in OverviewInternal.hpp, the strip label in
# OverviewStrip.cpp). The chrome was sized for a 2160 px tall screen and kept
# those logical sizes everywhere: on 1280x800 the search row and the strip took
# 34% of the screen open and 62% with a query.
# State from `hyprdrive:debug search` (search.lib: rowL, bandH, stripPlateL,
# pill, container, ui, tip), `debug stripclean` (the strip plate) and `debug
# drawn` (the grid's tiles), all rendered px except the *L logical heights.
# Fixtures: HEADLESS-1 at each geometry below with a one-line kitty panel on the
# top edge; gsalpha and gsecho on desktop 2, gsbeta, gsgamma and gsdelta on
# desktop 1 (focused); overview scale 0.84, horizontal, the mosaic on.
# Geometries (mode@scale -> logical): 1280x800, 1920x1080, 1920x1200@1.0666667
# (1800x1125), 2880x1800@1.6 (1800x1125), 2560x1440, 3840x2160, 1080x1920.
# The bounds come from the design (owner, 2026-11-19, "option B"): with
# uL = clamp(H / 2160, 0.5, 1) the search row's margins (16) and the strip
# plate (223.2 at 4K) scale by uL and the 48 px pill by at most 1, so
#   open share  <= (48 + (16 + 223.2) x uL) / H                    + 1 point
#   query share <= open bound + (min(360, 0.25 H) + 20 x uL) / H    (results
#                  budget, its two 10 px gaps)
# Asserts, per geometry:
#   G open:  the factors are the design's (ui=uL,uT); search row + strip within
#            the open bound; pill above the strip plate, plate above the grid
#   G hover: hovering desktop 1's strip cell, its label clears the pill, the
#            plate and the grid
#   G query: typing "gs", search row + results + strip within the query bound;
#            pill, results, plate and grid in that order, none overlapping; the
#            inspector shows from 1200 logical px of width and not below
#   4K: every number above equals HEAD's (4549a658) -- the reference look
# Control (--control): ui_scale = 1, the old fixed sizes on every screen. The
# short screens break the share bounds. Recorded 2026-11-19: control FAILS as
# it must (1280x800 open 33.9% > 21.9%).
source "$(dirname "$0")/lib.sh"
gate_begin sp
source "$GATES_DIR/search.lib"
GEOMS="1280x800:1 1920x1080:1 1920x1200:1.0666667 2880x1800:1.6 2560x1440:1 3840x2160:1 1080x1920:1"
M=HEADLESS-1
UIS=; [ "$CONTROL" = 1 ] && UIS=',ui_scale="1"'
plate() { dbg stripclean | grep -m1 -o 'plate=[0-9-]*,[0-9-]* [0-9]*x[0-9]*' | tr '=,x' '   ' | awk '{print $2, $3, $4, $5}'; }  # x y w h
cell0() { set -- $(plate); echo $(( $1 + $3 / 6 )) $(( $2 + $4 / 2 )); }
gridtop() { local c t m=999999; for c in gsbeta gsgamma gsdelta; do t=$(drawn_y $c); [ -n "$t" ] && [ "$t" -lt "$m" ] && m=$t; done; echo $m; }
calc() { awk "BEGIN { $1 }"; }
bx() { echo "$1" | cut -d, -f"$2"; }   # bx "x,y,w,h" N
search_start
cfg "{plugin={hyprdrive={scale=0.84,layout=\"horizontal\",workspace_gap=40,anim_scale=1.053,mosaic={infinite_canvas=true,zoom_labels=\"hover\",default=true}$UIS}}}" >/dev/null
$R eval "hl.monitor({ output = '$M', mode = '1920x1080@60', position = '0x0', scale = 1 })" >/dev/null; sleep 1
$R win kitty +kitten panel --edge=top --lines=1 sleep 9999 >/dev/null; sleep 2
go 2; kw gsalpha 700 450; kw gsecho 600 400; go 1; kw gsbeta 700 450; kw gsgamma 600 420; kw gsdelta 500 380; sleep 0.5
focus "$(addr_of gsbeta)"; settle 3 geom

for G in $GEOMS; do
    MODE=${G%%:*}; SCL=${G#*:}
    $R eval "hl.monitor({ output = '$M', mode = '${MODE}@60', position = '0x0', scale = ${SCL} })" >/dev/null; sleep 1.5
    go 1; focus "$(addr_of gsbeta)"; $R ptr move 5 5 >/dev/null; settle 4 geom
    MON=$(J monitors | jq -c '.[0] | {width,height,scale}')
    WL=$(calc "printf \"%.0f\", $(echo "$MON" | jq .width) / $(echo "$MON" | jq .scale)")
    HL=$(calc "printf \"%.0f\", $(echo "$MON" | jq .height) / $(echo "$MON" | jq .scale)")
    UL=$(calc "u = $HL / 2160; u = u < 0.5 ? 0.5 : (u > 1 ? 1 : u); printf \"%.4f\", u")
    if [ "$CONTROL" = 1 ]; then WANTUI="1.000,1.000"; else WANTUI=$(calc "t = sqrt($UL); t = t < 0.9 ? 0.9 : (t > 1 ? 1 : t); printf \"%.3f,%.3f\", $UL, t"); fi
    OPENMAX=$(calc "printf \"%.4f\", (48 + (16 + 223.2) * $UL) / $HL + 0.01")
    QUERYMAX=$(calc "b = 0.25 * $HL; b = b < 360 ? b : 360; printf \"%.4f\", $OPENMAX + (b + 20 * $UL) / $HL")

    # ---- open
    $R overview >/dev/null
    until_s 5 '[ "$(sk overviews)" = 1 ] && [ "$(mk $M drawable)" = 1 ]'; settle_s $M 5; sleep 0.8; snap
    P=$(mk $M pill); PL=($(plate)); GT=$(gridtop)
    SHARE=$(calc "printf \"%.4f\", ($(mk $M rowL) + $(mk $M stripPlateL)) / $HL")
    check "$MODE open: the factors are the design's" "$(is "$(mk $M ui)" "$WANTUI")" "ui $(mk $M ui), want $WANTUI (logical ${WL}x$HL)"
    check "$MODE open: search row + strip within the bound" "$(flt "$SHARE" "$OPENMAX")" \
        "share $SHARE > $OPENMAX (rowL $(mk $M rowL) stripPlateL $(mk $M stripPlateL), H $HL)"
    check "$MODE open: pill, plate, grid in order" \
        "$( [ $(( $(bx "$P" 2) + $(bx "$P" 4) )) -le "${PL[1]}" ] && [ $(( PL[1] + PL[3] )) -le "$GT" ] && echo 1 || echo 0)" \
        "pill $P plate ${PL[*]} grid top $GT"
    if [ "$MODE" = 3840x2160 ]; then
        check "4K open: unchanged" "$(is "$(mk $M rowL) $(mk $M stripPlateL) $P ${PL[*]}" "64.0 223.2 1783,137,274,40 1535 214 770 157")" \
            "rowL $(mk $M rowL) stripPlateL $(mk $M stripPlateL) pill $P plate ${PL[*]}"
    fi

    # ---- hover desktop 1's strip cell
    set -- $(cell0); $R ptr path 20 $(( $1 - 40 )) $(( $2 + 30 )) "$1" "$2" >/dev/null; sleep 1.4; snap
    P=$(mk $M pill); T=$(mk $M tip)
    if [ "$T" = none ] || [ -z "$T" ]; then
        check "$MODE hover: the strip label is up" 0 "no tip (pill $P)"
    else
        TT=$(bx "$T" 2); TB=$(( TT + $(bx "$T" 4) ))
        check "$MODE hover: the label clears the pill, the plate and the grid" \
            "$( [ $(( $(bx "$P" 2) + $(bx "$P" 4) )) -le "$TT" ] && { [ "$TB" -le "${PL[1]}" ] || { [ "$TT" -ge $(( PL[1] + PL[3] )) ] && [ "$TB" -le "$GT" ]; }; } && echo 1 || echo 0)" \
            "pill $P tip $T plate ${PL[*]} grid top $GT"
        if [ "$MODE" = 3840x2160 ]; then
            check "4K hover: unchanged" "$(is "$(mk $M pillNudgeWant) $P $T" "20.0 1783,117,274,40 1462,165,412,41")" "pillNudgeWant $(mk $M pillNudgeWant) pill $P tip $T"
        fi
    fi

    # ---- query
    W=$(echo "$MON" | jq .width); H=$(echo "$MON" | jq .height)
    $R ptr path 20 $(( W / 2 )) $(( H * 3 / 4 )) $(( W / 2 + 10 )) $(( H * 3 / 4 + 10 )) >/dev/null; sleep 0.8
    $R type gs >/dev/null; until_s 4 '[ "$(mk $M hits)" -ge 1 ] 2>/dev/null'; settle_s $M 5; sleep 0.8; snap
    P=$(mk $M pill); C=$(mk $M container); PL=($(plate)); GT=$(gridtop)
    SHARE=$(calc "printf \"%.4f\", ($(mk $M rowL) + $(mk $M bandH) + $(mk $M stripPlateL)) / $HL")
    check "$MODE query: search row + results + strip within the bound" "$(flt "$SHARE" "$QUERYMAX")" \
        "share $SHARE > $QUERYMAX (rowL $(mk $M rowL) bandH $(mk $M bandH) stripPlateL $(mk $M stripPlateL), H $HL)"
    check "$MODE query: pill, results, plate, grid in order" \
        "$( [ "$(bx "$C" 3)" -gt 0 ] && [ $(( $(bx "$P" 2) + $(bx "$P" 4) )) -le "$(bx "$C" 2)" ] && [ $(( $(bx "$C" 2) + $(bx "$C" 4) )) -le "${PL[1]}" ] && [ $(( PL[1] + PL[3] )) -le "$GT" ] && echo 1 || echo 0)" \
        "pill $P results $C plate ${PL[*]} grid top $GT"
    WANTINSP=1; [ "$CONTROL" != 1 ] && [ "$WL" -lt 1200 ] && WANTINSP=0
    check "$MODE query: inspector $WANTINSP at ${WL} logical px wide" "$(is "$(mk $M inspector)" "$WANTINSP")" "inspector $(mk $M inspector)"
    if [ "$MODE" = 3840x2160 ]; then
        check "4K query: unchanged" "$(is "$(mk $M rowL) $(mk $M bandH) $P $C ${PL[*]}" "0.0 292.0 1800,58,240,40 565,138,2710,229 1535 406 770 157")" \
            "rowL $(mk $M rowL) bandH $(mk $M bandH) pill $P results $C plate ${PL[*]}"
    fi

    $R tap Escape >/dev/null; sleep 0.3; $R tap Escape >/dev/null; sleep 0.6
    until_s 4 '[ "$(sk overviews)" = 0 ]' || { $R overview >/dev/null; sleep 1; }
done
gate_end

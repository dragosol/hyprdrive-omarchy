.pragma library

function keyOf(state, key, fallback) {
    if (!state) return fallback
    var live = state.keys ? state.keys[key] : undefined
    if (live === undefined || live === null)
        live = state.owned ? state.owned[key] : undefined
    return live === undefined || live === null ? fallback : live
}

function asBool(v) {
    if (v === true || v === false) return v
    var s = String(v).toLowerCase()
    return s === "1" || s === "true" || s === "yes" || s === "on"
}

function asNumber(v, fallback) {
    var n = Number(v)
    return isFinite(n) ? n : fallback
}

function dockRounded(state) {
    return asNumber(keyOf(state, "plugin:hyprdrive:dock:corner_radius", 24), 24) > 0
}

function normalizeDirection(direction) {
    if (direction === "pinch_in" || direction === "pinch-in") return "pinchin"
    if (direction === "pinch_out" || direction === "pinch-out") return "pinchout"
    if (direction === "tap") return "click"
    return direction || "up"
}

function cloneGestures(list) {
    var out = []
    if (!list) return out
    for (var i = 0; i < list.length; i++) {
        var g = list[i] || {}
        out.push({
            fingers: asNumber(g.fingers, 3),
            direction: normalizeDirection(g.direction),
            action: g.action || "overview",
            mods: g.mods || "",
            scale: asNumber(g.scale, 1),
            disable_inhibit: !!g.disable_inhibit,
            arg: g.arg || ""
        })
    }
    return out
}

function gestureLabel(g, actions) {
    var action = g.action || "overview"
    if (actions) {
        for (var i = 0; i < actions.length; i++) {
            if (actions[i].value === action) {
                action = actions[i].label
                break
            }
        }
    }
    var mods = g.mods ? (g.mods + " + ") : ""
    var arg = g.arg ? ("  ·  " + g.arg) : ""
    return mods + g.fingers + "F " + g.direction + " → " + action + arg
}

function emptyGesture() {
    return {
        fingers: 3,
        direction: "up",
        action: "overview",
        mods: "",
        scale: 1,
        disable_inhibit: false,
        arg: ""
    }
}

function actionNeedsArg(action) {
    return action === "dispatcher" || action === "special" || action === "zoom" || action === "float" || action === "fullscreen" || action === "keys"
}

function argPlaceholder(action) {
    if (action === "dispatcher") return "exec,notify-send hi   or   hyprdrive:overview toggle"
    if (action === "special") return "special workspace name"
    if (action === "zoom") return "live, toggle, or 2 live"
    if (action === "float") return "toggle, float, or tile"
    if (action === "fullscreen") return "fullscreen or maximize"
    if (action === "keys") return "CTRL+T   or   Print   or   SUPER+SHIFT+S"
    return ""
}

// A Keybinds row's live chords (hdpanel dump `keybinds[].chords`, hyprctl's
// spelling: "SUPER + SHIFT + mouse_down") the way the panel shows them:
// "Super + Shift + Scroll". The wheel's two directions, the four arrows and
// the ten number keys each read as one chord.
var MOD_NAMES = { SUPER: "Super", SHIFT: "Shift", CTRL: "Ctrl", ALT: "Alt" }

function prettyChords(chords) {
    if (!chords || !chords.length) return ""
    var out = []
    for (var i = 0; i < chords.length; i++) {
        var parts = String(chords[i]).split(" + ")
        var key = parts.pop()
        var mods = []
        for (var j = 0; j < parts.length; j++) mods.push(MOD_NAMES[parts[j]] || parts[j])
        var lower = key.toLowerCase()
        if (MOD_NAMES[key]) key = MOD_NAMES[key] + " tap" // the overview's lone Super
        else if (lower === "mouse_down" || lower === "mouse_up") key = "Scroll"
        else if (lower === "left" || lower === "right" || lower === "up" || lower === "down") key = "Arrows"
        else if (/^[0-9]$/.test(key) && chords.length > 1) key = "1…0"
        else if (key.length === 1) key = key.toUpperCase()
        var text = mods.concat([key]).join(" + ")
        if (out.indexOf(text) < 0) out.push(text)
    }
    return out.join(", ")
}

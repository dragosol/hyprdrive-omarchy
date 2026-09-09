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

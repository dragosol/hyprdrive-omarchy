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

// ------------------------------------------------------------- the chord editor
// A Keybinds chip in capture mode turns the next key event into a chord in
// hl.bind's spelling ("SUPER + SHIFT + K"; hdpanel's parse_chord() is the
// other half). Qt's modifier and key numbers are spelled out, so this file
// needs no QML engine and node can test it (scripts/keybind_editor_test.py).
var QT_SHIFT = 0x02000000, QT_CTRL = 0x04000000, QT_ALT = 0x08000000, QT_META = 0x10000000
var CHORD_MODS = ["SUPER", "CTRL", "ALT", "SHIFT"]
// Qt keys that ARE modifiers, and the modifier each one holds.
var MODIFIER_KEYS = {}
MODIFIER_KEYS[0x01000020] = "SHIFT"; MODIFIER_KEYS[0x01000021] = "CTRL"
MODIFIER_KEYS[0x01000022] = "SUPER"; MODIFIER_KEYS[0x01000023] = "ALT"
MODIFIER_KEYS[0x01000053] = "SUPER"; MODIFIER_KEYS[0x01000054] = "SUPER"   // Super_L / Super_R
MODIFIER_KEYS[0x01000056] = "SUPER"; MODIFIER_KEYS[0x01000057] = "SUPER"   // Hyper_L / Hyper_R
MODIFIER_KEYS[0x01001103] = ""; MODIFIER_KEYS[0x01000024] = ""; MODIFIER_KEYS[0x01000025] = ""   // AltGr, Caps, NumLock
var QT_ESCAPE = 0x01000000, QT_BACKSPACE = 0x01000003, QT_DELETE = 0x01000007
// Named keys, Qt -> xkb (Hyprland matches key names case-insensitively).
var QT_NAMES = {}
;(function() {
    var t = [[0x01000000, "ESCAPE"], [0x01000003, "BACKSPACE"], [0x01000007, "DELETE"], [0x01000001, "TAB"], [0x01000002, "TAB"], [0x01000004, "RETURN"], [0x01000005, "KP_ENTER"],
             [0x01000006, "INSERT"], [0x01000008, "PAUSE"], [0x01000009, "PRINT"], [0x0100000a, "PRINT"],
             [0x01000010, "HOME"], [0x01000011, "END"], [0x01000012, "LEFT"], [0x01000013, "UP"],
             [0x01000014, "RIGHT"], [0x01000015, "DOWN"], [0x01000016, "PAGE_UP"], [0x01000017, "PAGE_DOWN"],
             [0x01000026, "SCROLL_LOCK"], [0x01000055, "MENU"], [0x20, "SPACE"],
             [0x2c, "COMMA"], [0x2e, "PERIOD"], [0x2f, "SLASH"], [0x3b, "SEMICOLON"], [0x27, "APOSTROPHE"],
             [0x5b, "BRACKETLEFT"], [0x5d, "BRACKETRIGHT"], [0x5c, "BACKSLASH"], [0x2d, "MINUS"],
             [0x3d, "EQUAL"], [0x60, "GRAVE"]]
    for (var i = 0; i < t.length; i++) QT_NAMES[t[i][0]] = t[i][1]
    for (var f = 1; f <= 35; f++) QT_NAMES[0x01000030 + f - 1] = "F" + f
})()
// The US number row and punctuation by xkb keycode (Qt's nativeScanCode on
// Wayland): Shift+1 arrives as Qt's "!", but Hyprland binds SUPER + SHIFT + 1.
var SCAN_NAMES = { 20: "MINUS", 21: "EQUAL", 34: "BRACKETLEFT", 35: "BRACKETRIGHT", 47: "SEMICOLON",
                   48: "APOSTROPHE", 49: "GRAVE", 51: "BACKSLASH", 59: "COMMA", 60: "PERIOD", 61: "SLASH" }
for (var _d = 1; _d <= 10; _d++) SCAN_NAMES[9 + _d] = String(_d % 10)

function modsFromQt(modifiers) {
    var out = []
    if (modifiers & QT_META) out.push("SUPER")
    if (modifiers & QT_CTRL) out.push("CTRL")
    if (modifiers & QT_ALT) out.push("ALT")
    if (modifiers & QT_SHIFT) out.push("SHIFT")
    return out
}

// The modifiers held right after this event: Qt's report, plus the key's own
// modifier when it went down, minus it when it came up (Qt is not consistent
// about including it either way).
function heldMods(modifiers, key, pressed) {
    var mods = modsFromQt(modifiers)
    var own = MODIFIER_KEYS[key]
    if (own) {
        var i = mods.indexOf(own)
        if (pressed && i < 0) mods.push(own)
        if (!pressed && i >= 0) mods.splice(i, 1)
    }
    return CHORD_MODS.filter(function(m) { return mods.indexOf(m) >= 0 })
}

function isModifierKey(key) { return MODIFIER_KEYS[key] !== undefined }

// A key event's key in xkb's spelling, or "" when it has none worth binding.
function keyName(key, scanCode) {
    if (SCAN_NAMES[scanCode]) return SCAN_NAMES[scanCode]
    if (key >= 0x41 && key <= 0x5a) return String.fromCharCode(key)
    if (key >= 0x30 && key <= 0x39) return String.fromCharCode(key)
    if (QT_NAMES[key]) return QT_NAMES[key]
    if (scanCode > 8) return "code:" + scanCode
    return ""
}

// A captured press -> {chord, error}. `chord` is "" when the press is only a
// modifier (keep listening); `error` says why a chord cannot be used.
var SOLO_RE = /^(F\d{1,2}|PRINT|PAUSE|MENU|SCROLL_LOCK|INSERT)$/
function chordFromEvent(modifiers, key, scanCode) {
    if (isModifierKey(key)) return { chord: "", error: "" }
    var mods = heldMods(modifiers, key, true)
    var name = keyName(key, scanCode)
    if (name === "") return { chord: "", error: "That key cannot be bound" }
    var strong = mods.filter(function(m) { return m !== "SHIFT" })
    if (strong.length === 0 && !SOLO_RE.test(name))
        return { chord: "", error: "Hold Super, Ctrl or Alt with it, or it would stop that key typing" }
    return { chord: mods.concat([name]).join(" + "), error: "" }
}

// "SUPER + SHIFT + K" -> ["Super", "Shift", "K"]: one keycap per part.
var KEY_WORDS = { SUPER: "Super", CTRL: "Ctrl", ALT: "Alt", SHIFT: "Shift", RETURN: "Enter", KP_ENTER: "Enter",
                  PAGE_UP: "Page Up", PAGE_DOWN: "Page Down", BRACKETLEFT: "[", BRACKETRIGHT: "]",
                  SEMICOLON: ";", APOSTROPHE: "'", GRAVE: "`", BACKSLASH: "\\", MINUS: "-", EQUAL: "=",
                  COMMA: "Comma", PERIOD: "Period", SLASH: "/", LEFT: "←", RIGHT: "→", UP: "↑", DOWN: "↓",
                  SPACE: "Space", TAB: "Tab", PRINT: "Print", mouse_down: "Scroll", mouse_up: "Scroll" }
function chordParts(chord) {
    var s = String(chord || "").trim()
    if (s === "") return []
    return s.split(/\s*\+\s*/).map(function(p) {
        if (KEY_WORDS[p] !== undefined) return KEY_WORDS[p]
        var up = p.toUpperCase()
        if (KEY_WORDS[up] !== undefined) return KEY_WORDS[up]
        var code = /^code:(\d+)$/i.exec(p)
        if (code) return +code[1] >= 10 && +code[1] <= 19 ? String((+code[1] - 9) % 10) : "Key " + code[1]
        if (p.length === 1) return p.toUpperCase()
        return up.charAt(0) + up.slice(1).toLowerCase().replace(/_/g, " ")
    })
}
function chordPretty(chord) { return chordParts(chord).join(" + ") }

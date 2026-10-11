import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import Quickshell
import Quickshell.Io
import Quickshell.Services.UPower
import qs.Commons
import qs.Ui
import "Model.js" as Model
import "Schema.js" as Schema

// THE HYPRDRIVE PANEL (user, 2026-09-30: "a bespoke high-quality hyprdrive
// settings panel ... re-arrange all settings, a new Animation section ... an
// entire motion blur section ... each section should have a restore defaults
// button ... easy to navigate, animated and cool. No memory consumed when
// unloaded.")
//
// Loaded by Panel.qml only while the panel is open (and for its exit), so
// everything here -- the window, the tree, the hdpanel processes, the state --
// is destroyed on close. What it draws is Schema.js; this file is the
// renderer, the controls and the talk to bin/hdpanel.
Item {
  id: root

  // Injected by Panel.qml.
  property var host: null
  property Item anchorItem: null
  readonly property var bar: host ? host.bar : null

  // Mapped one tick after creation, so the card's own fade runs on the way in
  // (created already open, it would just appear).
  property bool shown: false
  readonly property bool isOpen: !!host && host.opened && shown
  Component.onCompleted: Qt.callLater(function() { root.shown = true })

  // ------------------------------------------------------------------ theme
  readonly property color fg: Color.popups.text
  readonly property color bg: Color.popups.background
  readonly property color accent: Color.accent
  readonly property color urgent: Color.urgent
  readonly property color dim: tint(fg, 0.62)
  readonly property color faint: tint(fg, 0.38)
  readonly property color hairline: tint(fg, 0.09)
  readonly property color raised: tint(fg, 0.045)
  readonly property string fontFamily: Style.font.family
  readonly property int radius: Math.max(Style.space(6), Math.min(Style.space(14), Style.cornerRadius + Style.space(4)))

  function tint(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
  function mix(a, b, t) { return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t) }

  // One motion vocabulary for the panel itself.
  readonly property int quick: 140
  readonly property int calm: 240
  readonly property int slow: 380

  // ------------------------------------------------------------------ state
  property string ctlPath: ""
  property var hd: ({})
  property var pending: ({})
  property var defaults: ({})
  property var metaOptions: ({})
  property bool dumping: false
  property bool dumpWanted: false
  property var ctlQueue: []
  property bool ctlBusy: false

  property string section: "overview"
  property string shownSection: "overview"
  property string query: ""
  property int navDirection: 1
  property string toastText: ""
  property int gestureIndex: 0
  property var gestureDraft: null
  property bool gestureDirty: false
  property real lastScale: 1
  property int lastMinimizeMs: 400

  readonly property bool onBattery: UPower.onBattery === true
  readonly property var shownSectionData: Schema.sectionById(shownSection)
  readonly property var pageCards: query !== "" ? searchCards : shownSectionData.cards
  readonly property var searchCards: {
    var groups = Schema.search(root.query)
    var out = []
    for (var i = 0; i < groups.length; i++) out.push({ title: groups[i].title, rows: groups[i].rows, section: groups[i].section })
    return out
  }

  function k(key, fallback) {
    if (pending[key] !== undefined) return pending[key]
    var fb = fallback !== undefined ? fallback : defaults[key]
    return Model.keyOf(hd, key, fb)
  }
  function kb(key) { return Model.asBool(k(key)) }
  function kn(key) { return Model.asNumber(k(key), Model.asNumber(defaults[key], 0)) }

  function remember(key, value) {
    var next = Object.assign({}, pending)
    next[key] = value
    pending = next
  }

  // --------------------------------------------------------------- hdpanel
  function resolveCtl() {
    if (ctlPath !== "" || resolveProc.running) return
    resolveProc.running = true
  }

  function dump() {
    if (ctlPath === "") return
    if (dumping) { dumpWanted = true; return }
    dumpWanted = false
    dumping = true
    dumpProc.running = true
  }

  // A drag previews on every step; only the newest preview of a key is worth
  // running, so a queued one for the same key is replaced rather than stacked.
  function runCtl(args, refresh, previewKeyName) {
    if (ctlPath === "") return
    var q = ctlQueue.slice()
    if (previewKeyName)
      q = q.filter(function(job) { return job.previewKey !== previewKeyName })
    q.push({ args: args, refresh: refresh !== false, previewKey: previewKeyName || "" })
    ctlQueue = q
    pumpCtl()
  }

  function pumpCtl() {
    if (ctlPath === "" || ctlProc.running || ctlQueue.length === 0) return
    var q = ctlQueue.slice()
    var job = q.shift()
    ctlQueue = q
    ctlProc.refresh = job.refresh
    ctlProc.command = [ctlPath].concat(job.args)
    ctlBusy = true
    ctlProc.running = true
  }

  function setKey(key, value) {
    remember(key, value)
    runCtl(["set", key, String(value)], true)
  }

  function previewKey(key, value) {
    remember(key, value)
    runCtl(["preview", key, String(value)], false, key)
  }

  function setMany(pairs) {
    var args = ["set"]
    for (var i = 0; i < pairs.length; i++) {
      remember(pairs[i][0], pairs[i][1])
      args.push(pairs[i][0], String(pairs[i][1]))
    }
    runCtl(args, true)
  }

  function restoreSection(sec) {
    runCtl(["reset", sec.group], true)
    var next = Object.assign({}, pending)
    for (var key in defaults) {
      var m = metaFor(key)
      if (m && m.group === sec.group) next[key] = defaults[key]
    }
    pending = next
    if (sec.group === "gestures") { gestureDraft = null; gestureDirty = false; gestureIndex = 0 }
    showToast(sec.title + " restored to defaults")
  }

  function metaFor(key) {
    var list = hd.meta || []
    for (var i = 0; i < list.length; i++) if (list[i].key === key) return list[i]
    return null
  }

  function showToast(text) {
    toastText = text
    toastTimer.restart()
  }

  function askConfirm(spec, onAllow) {
    if (menuLayer.owner) menuLayer.close()
    // Off the search field, so Enter and Escape reach the confirm.
    keys.forceActiveFocus()
    confirmLayer.ask(spec, onAllow)
  }

  // ------------------------------------------------------------- conditions
  function rowEnabled(row) {
    switch (row.when) {
    case "dockOn": return kb("plugin:hyprdrive:dock:enabled")
    case "mountsOn": return kb("plugin:hyprdrive:dock:enabled") && kb("plugin:hyprdrive:dock:show_mounts")
    case "springOn": return kb("plugin:hyprdrive:drag:spring_dock") || kb("plugin:hyprdrive:drag:spring_edges")
    case "isolateOvApplies": return !kb("plugin:hyprdrive:dock:isolate_workspaces")
    case "roomApplies": return kb("plugin:hyprdrive:dock:enabled") && !kb("plugin:hyprdrive:dock:autohide")
    case "motionOn": return kn("plugin:hyprdrive:anim_scale") > 0.001
    case "blurOn": return kb("plugin:hyprdrive:motion_blur:enabled")
    case "mosaicOn": return kb("plugin:hyprdrive:mosaic:enabled")
    default: return true
    }
  }

  function formatValue(row, v) {
    var n = Number(v)
    switch (row.fmt) {
    case "percent": return Math.round(n * 100) + "%"
    case "follow": return n < 0 ? "Follow" : String(Math.round(n)) + (row.unit ? " " + row.unit : "")
    case "deciseconds": return Math.round(n * 100) + " ms"
    case "samples": return n < 2 ? "Off" : String(Math.round(n))
    case "fraction": return n.toFixed(2)
    }
    var decimals = row.step && row.step < 1 ? (row.step < 0.1 ? 2 : 1) : 0
    return n.toFixed(decimals) + (row.unit ? " " + row.unit : "")
  }

  function goTo(id) {
    if (id === section && query === "") return
    var from = sectionIndex(section), to = sectionIndex(id)
    navDirection = to >= from ? 1 : -1
    query = ""
    searchField.text = ""
    section = id
  }

  function sectionIndex(id) {
    for (var i = 0; i < Schema.sections.length; i++) if (Schema.sections[i].id === id) return i
    return 0
  }

  onSectionChanged: pageSwap.restart()
  onQueryChanged: pageSwap.restart()

  // ----------------------------------------------------------------- gestures
  readonly property var gestureList: gestureDraft !== null ? gestureDraft : Model.cloneGestures(hd.gestures)
  function editGestures(mutator) {
    var list = Model.cloneGestures(gestureList)
    mutator(list)
    gestureDraft = list
    gestureDirty = true
  }
  function applyGestures() {
    runCtl(["gestures-set", JSON.stringify(gestureList)], true)
    gestureDirty = false
    showToast("Gesture map applied")
  }

  // --------------------------------------------------------------- processes
  Process {
    id: resolveProc
    command: ["bash", "-lc",
      'c="$HOME/.config/omarchy/plugins/dragos.hyprdrive/bin/hdpanel"; ' +
      'if [ -x "$c" ]; then echo "$c"; ' +
      'elif [ -x "$HOME/Projects/hyprdrive-omarchy/omarchy-plugin/bin/hdpanel" ]; then echo "$HOME/Projects/hyprdrive-omarchy/omarchy-plugin/bin/hdpanel"; ' +
      'else echo "$HOME/.local/lib/hyprdrive/hdpanel"; fi']
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        root.ctlPath = String(text || "").trim()
        root.dump()
        root.pumpCtl()
      }
    }
  }

  Process {
    id: dumpProc
    command: [root.ctlPath, "dump"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        var parsed = null
        try { parsed = JSON.parse(String(text || "{}")) } catch (e) {}
        if (parsed) {
          var defs = {}, opts = {}
          var meta = parsed.meta || []
          for (var i = 0; i < meta.length; i++) {
            defs[meta[i].key] = meta[i]["default"]
            if (meta[i].options) opts[meta[i].key] = meta[i].options
          }
          root.defaults = defs
          root.metaOptions = opts
          root.hd = parsed
          var s = Model.asNumber(Model.keyOf(parsed, "plugin:hyprdrive:anim_scale", 1), 1)
          if (s > 0.001) root.lastScale = s
          var m = Model.asNumber(Model.keyOf(parsed, "plugin:hyprdrive:dock:minimize_ms", 400), 400)
          if (m > 0) root.lastMinimizeMs = m
        }
        if (!root.ctlBusy) root.pending = ({})
        root.dumping = false
        if (root.dumpWanted) root.dump()
      }
    }
  }

  Process {
    id: ctlProc
    property bool refresh: true
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        root.ctlBusy = false
        if (ctlProc.refresh && root.ctlQueue.length === 0) root.dump()
        root.pumpCtl()
      }
    }
  }

  Timer { id: toastTimer; interval: 2200; onTriggered: root.toastText = "" }

  onIsOpenChanged: if (isOpen) resolveCtl()

  // ============================================================== controls

  // A switch: the track fills with the accent, the knob springs across.
  component Switch: Item {
    id: sw
    property bool checked: false
    property bool live: true
    signal toggled()
    implicitWidth: Style.space(40)
    implicitHeight: Style.space(22)
    opacity: live ? 1 : 0.4

    Rectangle {
      anchors.fill: parent
      radius: height / 2
      color: sw.checked ? root.accent : root.tint(root.fg, swArea.containsMouse ? 0.16 : 0.11)
      border.width: 1
      border.color: sw.checked ? root.tint(root.accent, 0.9) : root.tint(root.fg, 0.12)
      Behavior on color { ColorAnimation { duration: root.quick } }
    }
    Rectangle {
      id: knob
      width: sw.height - Style.space(6)
      height: width
      radius: width / 2
      y: Style.space(3)
      x: sw.checked ? sw.width - width - Style.space(3) : Style.space(3)
      color: sw.checked ? root.bg : root.fg
      scale: swArea.pressed ? 0.86 : 1
      Behavior on x { NumberAnimation { duration: root.calm; easing.type: Easing.OutBack; easing.overshoot: 1.6 } }
      Behavior on scale { NumberAnimation { duration: root.quick; easing.type: Easing.OutCubic } }
      Behavior on color { ColorAnimation { duration: root.quick } }
    }
    MouseArea {
      id: swArea
      anchors.fill: parent
      anchors.margins: -Style.space(6)
      hoverEnabled: true
      enabled: sw.live
      cursorShape: Qt.PointingHandCursor
      onClicked: sw.toggled()
    }
  }

  // A track with a value chip. `ticks` draws a detent per step (the motion
  // blur strengths' eighths). moved() while dragging, released() on let go.
  component ValueSlider: Item {
    id: sl
    property real from: 0
    property real to: 1
    property real stepSize: 0.01
    property real value: 0
    property bool ticks: false
    property bool live: true
    property string valueText: ""
    signal moved(real v)
    signal released(real v)
    implicitWidth: Style.space(280)
    implicitHeight: Style.space(26)
    opacity: live ? 1 : 0.4

    property real dragValue: value
    property bool dragging: slArea.pressed
    readonly property real current: dragging ? dragValue : value
    readonly property real frac: to > from ? Math.max(0, Math.min(1, (current - from) / (to - from))) : 0
    readonly property real trackW: width - chip.width - Style.space(14)

    function snap(v) {
      var s = stepSize > 0 ? Math.round((v - from) / stepSize) * stepSize + from : v
      return Math.max(from, Math.min(to, Number(s.toFixed(4))))
    }
    function valueAt(x) { return snap(from + Math.max(0, Math.min(1, x / trackW)) * (to - from)) }

    Item {
      id: track
      width: sl.trackW
      height: parent.height
      Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: Style.space(4)
        radius: height / 2
        color: root.tint(root.fg, 0.12)
      }
      Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(height, parent.width * sl.frac)
        height: Style.space(4)
        radius: height / 2
        color: root.accent
        Behavior on width { enabled: !sl.dragging; NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
      }
      Repeater {
        model: sl.ticks && sl.stepSize > 0 ? Math.round((sl.to - sl.from) / sl.stepSize) + 1 : 0
        Rectangle {
          required property int index
          readonly property real at: index / Math.max(1, Math.round((sl.to - sl.from) / sl.stepSize))
          x: track.width * at - width / 2
          anchors.verticalCenter: parent.verticalCenter
          width: Style.space(2)
          height: Style.space(8)
          radius: 1
          color: at <= sl.frac + 0.0001 ? root.tint(root.bg, 0.55) : root.tint(root.fg, 0.22)
        }
      }
      Rectangle {
        id: thumb
        width: Style.space(16)
        height: width
        radius: width / 2
        x: track.width * sl.frac - width / 2
        anchors.verticalCenter: parent.verticalCenter
        color: root.fg
        border.width: Style.space(3)
        border.color: root.accent
        scale: sl.dragging ? 1.18 : (slArea.containsMouse ? 1.06 : 1)
        Behavior on x { enabled: !sl.dragging; NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: root.quick; easing.type: Easing.OutBack } }
      }
      MouseArea {
        id: slArea
        anchors.fill: parent
        anchors.leftMargin: -Style.space(8)
        anchors.rightMargin: -Style.space(8)
        hoverEnabled: true
        enabled: sl.live
        cursorShape: Qt.PointingHandCursor
        preventStealing: true
        function at(mx) { return sl.valueAt(mx - Style.space(8)) }
        onPressed: function(m) { sl.dragValue = at(m.x); sl.moved(sl.dragValue) }
        onPositionChanged: function(m) {
          if (!pressed) return
          var v = at(m.x)
          if (v !== sl.dragValue) { sl.dragValue = v; sl.moved(v) }
        }
        onReleased: sl.released(sl.dragValue)
        onWheel: function(w) {
          var v = sl.snap(sl.value + (w.angleDelta.y > 0 ? sl.stepSize : -sl.stepSize))
          sl.moved(v); sl.released(v)
        }
      }
    }
    Rectangle {
      id: chip
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      width: Math.max(Style.space(58), chipText.implicitWidth + Style.space(16))
      height: Style.space(24)
      radius: Style.space(7)
      color: sl.dragging ? root.tint(root.accent, 0.22) : root.raised
      border.width: 1
      border.color: sl.dragging ? root.tint(root.accent, 0.6) : root.hairline
      Behavior on color { ColorAnimation { duration: root.quick } }
      Text {
        id: chipText
        anchors.centerIn: parent
        text: sl.valueText
        color: root.fg
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
      }
    }
  }

  // Segmented choice: the selection slides between the segments.
  component Segmented: Item {
    id: seg
    property var options: []
    property string value: ""
    property bool live: true
    signal picked(string v)
    implicitWidth: row.implicitWidth + Style.space(6)
    implicitHeight: Style.space(28)
    opacity: live ? 1 : 0.4
    readonly property int currentIndex: {
      for (var i = 0; i < options.length; i++) if (options[i].value === value) return i
      return -1
    }
    Rectangle { anchors.fill: parent; radius: Style.space(8); color: root.raised; border.width: 1; border.color: root.hairline }
    Rectangle {
      id: segSel
      readonly property Item target: rep.count > 0 && seg.currentIndex >= 0 ? rep.itemAt(seg.currentIndex) : null
      visible: target !== null
      x: target ? row.x + target.x : 0
      y: Style.space(3)
      width: target ? target.width : 0
      height: seg.height - Style.space(6)
      radius: Style.space(6)
      color: root.accent
      Behavior on x { NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
      Behavior on width { NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
    }
    Row {
      id: row
      x: Style.space(3)
      anchors.verticalCenter: parent.verticalCenter
      Repeater {
        id: rep
        model: seg.options
        Item {
          required property var modelData
          required property int index
          width: segLabel.implicitWidth + Style.space(22)
          height: seg.height - Style.space(6)
          Text {
            id: segLabel
            anchors.centerIn: parent
            text: modelData.label
            color: seg.currentIndex === index ? root.bg : (segHover.containsMouse ? root.fg : root.dim)
            font.family: root.fontFamily
            font.pixelSize: Style.font.bodySmall
            font.weight: seg.currentIndex === index ? Font.DemiBold : Font.Normal
            Behavior on color { ColorAnimation { duration: root.quick } }
          }
          MouseArea {
            id: segHover
            anchors.fill: parent
            hoverEnabled: true
            enabled: seg.live
            cursorShape: Qt.PointingHandCursor
            onClicked: seg.picked(modelData.value)
          }
        }
      }
    }
  }

  // A curve, drawn: the CSS cubic-bezier through (0,0) and (1,1), with room
  // above for an overshoot. `progress` >= 0 rides a dot along it.
  component CurveGlyph: Canvas {
    id: cg
    property var points: [0, 0, 1, 1]
    property color stroke: root.fg
    property real progress: -1
    property real lineWidth: Style.space(2)
    onPointsChanged: requestPaint()
    onStrokeChanged: requestPaint()
    onProgressChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    function bez(t, p1, p2) { var u = 1 - t; return 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t }
    onPaint: {
      var ctx = getContext("2d")
      ctx.reset()
      var pad = lineWidth + 2
      var w = width - pad * 2, h = height - pad * 2
      var top = 1.35
      function X(v) { return pad + v * w }
      function Y(v) { return pad + h - (v / top) * h }
      ctx.strokeStyle = Qt.rgba(stroke.r, stroke.g, stroke.b, 0.18)
      ctx.lineWidth = 1
      ctx.beginPath(); ctx.moveTo(X(0), Y(1)); ctx.lineTo(X(1), Y(1)); ctx.stroke()
      ctx.strokeStyle = stroke
      ctx.lineWidth = lineWidth
      ctx.lineCap = "round"
      ctx.beginPath()
      ctx.moveTo(X(0), Y(0))
      ctx.bezierCurveTo(X(points[0]), Y(points[1]), X(points[2]), Y(points[3]), X(1), Y(1))
      ctx.stroke()
      if (progress >= 0) {
        // Solve x(t) = progress for the dot's height.
        var lo = 0, hi = 1, t = progress
        for (var i = 0; i < 18; i++) {
          t = (lo + hi) / 2
          if (bez(t, points[0], points[2]) < progress) lo = t; else hi = t
        }
        ctx.fillStyle = root.accent
        ctx.beginPath()
        ctx.arc(X(progress), Y(bez(t, points[1], points[3])), lineWidth * 2.2, 0, Math.PI * 2)
        ctx.fill()
      }
    }
  }

  // A button that reads as text until hovered.
  component GhostButton: Rectangle {
    id: gb
    property string label: ""
    property string glyph: ""
    property bool emphasis: false
    property bool danger: false
    signal activated()
    implicitWidth: gbRow.implicitWidth + Style.space(22)
    implicitHeight: Style.space(30)
    radius: Style.space(8)
    readonly property color hue: danger ? root.urgent : root.accent
    color: emphasis ? hue : (gbArea.containsMouse ? root.tint(root.fg, 0.08) : "transparent")
    border.width: 1
    border.color: emphasis ? hue : (gbArea.containsMouse ? root.tint(root.fg, 0.18) : root.hairline)
    scale: gbArea.pressed ? 0.96 : 1
    Behavior on color { ColorAnimation { duration: root.quick } }
    Behavior on scale { NumberAnimation { duration: root.quick; easing.type: Easing.OutCubic } }
    Row {
      id: gbRow
      anchors.centerIn: parent
      spacing: Style.space(7)
      Text {
        visible: gb.glyph !== ""
        text: gb.glyph
        color: gb.emphasis ? root.bg : root.dim
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
        anchors.verticalCenter: parent.verticalCenter
      }
      Text {
        text: gb.label
        color: gb.emphasis ? root.bg : root.fg
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
        font.weight: gb.emphasis ? Font.DemiBold : Font.Normal
        anchors.verticalCenter: parent.verticalCenter
      }
    }
    MouseArea {
      id: gbArea
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onClicked: gb.activated()
    }
  }

  // A dropdown button; the list opens in the panel-wide overlay (menuLayer),
  // so a card's clip never cuts it off. `kind` "curve" draws each option's
  // curve beside its name.
  component DropButton: Rectangle {
    id: db
    property var options: []
    property string value: ""
    property string kind: "plain"
    property bool live: true
    signal picked(string v)
    implicitWidth: Style.space(190)
    implicitHeight: Style.space(30)
    radius: Style.space(8)
    opacity: live ? 1 : 0.4
    color: dbArea.containsMouse || (menuLayer.owner === db) ? root.tint(root.fg, 0.08) : root.raised
    border.width: 1
    border.color: menuLayer.owner === db ? root.tint(root.accent, 0.8) : root.hairline
    Behavior on color { ColorAnimation { duration: root.quick } }
    readonly property var current: {
      for (var i = 0; i < options.length; i++) if (options[i].value === value) return options[i]
      return options.length ? options[0] : ({ label: value })
    }
    Row {
      anchors.left: parent.left
      anchors.leftMargin: Style.space(10)
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.space(8)
      CurveGlyph {
        visible: db.kind === "curve"
        width: Style.space(22)
        height: Style.space(18)
        anchors.verticalCenter: parent.verticalCenter
        points: db.current.decel || [0, 0, 1, 1]
        stroke: root.accent
        lineWidth: Style.space(1.5)
      }
      Text {
        text: db.current.label || db.value
        color: root.fg
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
        anchors.verticalCenter: parent.verticalCenter
      }
    }
    Text {
      anchors.right: parent.right
      anchors.rightMargin: Style.space(10)
      anchors.verticalCenter: parent.verticalCenter
      text: ""
      color: root.dim
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
      rotation: menuLayer.owner === db ? 180 : 0
      Behavior on rotation { NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
    }
    MouseArea {
      id: dbArea
      anchors.fill: parent
      hoverEnabled: true
      enabled: db.live
      cursorShape: Qt.PointingHandCursor
      onClicked: menuLayer.openFor(db, db.options, db.value, db.kind, function(v) { db.picked(v) })
    }
  }

  // One settings row: label and description on the left, its control on the
  // right (or below, for the wide ones).
  component SettingRow: Item {
    id: sr
    property var row: ({})
    property bool first: false
    readonly property bool live: root.rowEnabled(row)
    readonly property bool wide: row.type === "gestures" || row.type === "power" || row.type === "curve"
    width: parent ? parent.width : 0
    implicitHeight: wide ? wideCol.implicitHeight + Style.space(24) : Math.max(labelCol.implicitHeight, controlHolder.implicitHeight) + Style.space(26)

    Rectangle {
      anchors.fill: parent
      anchors.leftMargin: -Style.space(10)
      anchors.rightMargin: -Style.space(10)
      radius: Style.space(8)
      color: rowHover.hovered && !sr.wide ? root.tint(root.fg, 0.035) : "transparent"
      Behavior on color { ColorAnimation { duration: root.quick } }
    }
    HoverHandler { id: rowHover }
    Rectangle {
      visible: !sr.first
      width: parent.width
      height: 1
      color: root.hairline
    }

    Column {
      id: labelCol
      visible: !sr.wide
      anchors.left: parent.left
      anchors.right: controlHolder.left
      anchors.rightMargin: Style.space(20)
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.space(3)
      opacity: sr.live ? 1 : 0.45
      Behavior on opacity { NumberAnimation { duration: root.calm } }
      Text {
        width: parent.width
        text: sr.row.label || ""
        color: root.fg
        font.family: root.fontFamily
        font.pixelSize: Style.font.body
        elide: Text.ElideRight
      }
      Text {
        width: parent.width
        visible: !!sr.row.desc
        text: sr.row.desc || ""
        color: root.dim
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        wrapMode: Text.WordWrap
        lineHeight: 1.15
      }
    }

    Loader {
      id: controlHolder
      visible: !sr.wide
      active: !sr.wide
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      sourceComponent: {
        switch (sr.row.type) {
        case "toggle": return toggleControl
        case "slider": return sliderControl
        case "choice": return choiceControl
        case "speed": return speedControl
        case "motion": return motionControl
        case "blur": return blurControl
        }
        return null
      }
      property var row: sr.row
      property bool live: sr.live
    }

    Column {
      id: wideCol
      visible: sr.wide
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.top: parent.top
      anchors.topMargin: Style.space(12)
      spacing: Style.space(10)
      Loader {
        width: parent.width
        active: sr.wide
        sourceComponent: sr.row.type === "curve" ? curveControl : sr.row.type === "gestures" ? gestureEditor : sr.row.type === "power" ? powerControl : null
        property var row: sr.row
        property bool live: sr.live
      }
    }
  }

  Component {
    id: toggleControl
    Switch {
      property var row: parent ? parent.row : ({})
      checked: root.kb(row.key)
      live: parent ? parent.live : true
      // A row with `confirm` (Schema.js) reaches outside hyprdrive when it
      // turns on, so on asks first and only Allow sets it. Off never asks.
      onToggled: {
        var key = row.key, next = !root.kb(key)
        if (next && row.confirm) root.askConfirm(row.confirm, function() { root.setKey(key, true) })
        else root.setKey(key, next)
      }
    }
  }

  Component {
    id: sliderControl
    ValueSlider {
      property var row: parent ? parent.row : ({})
      from: row.min; to: row.max; stepSize: row.step || 1
      value: root.kn(row.key)
      live: parent ? parent.live : true
      valueText: root.formatValue(row, current)
      onMoved: function(v) { root.previewKey(row.key, v) }
      onReleased: function(v) { root.setKey(row.key, v) }
    }
  }

  Component {
    id: choiceControl
    Segmented {
      property var row: parent ? parent.row : ({})
      options: root.metaOptions[row.key] || []
      value: String(root.k(row.key))
      live: parent ? parent.live : true
      onPicked: function(v) { root.setKey(row.key, v) }
    }
  }

  // anim_scale is a SLOWDOWN (2 = half speed); shown the right way up.
  Component {
    id: speedControl
    ValueSlider {
      from: 0.25; to: 2.0; stepSize: 0.05
      readonly property real scaleNow: root.kn("plugin:hyprdrive:anim_scale")
      value: scaleNow > 0.001 ? Math.max(0.25, Math.min(2, 1 / scaleNow)) : 1 / Math.max(0.001, root.lastScale)
      live: parent ? parent.live : true
      valueText: current.toFixed(2) + "×"
      onMoved: function(v) { root.previewKey("plugin:hyprdrive:anim_scale", Number((1 / v).toFixed(3))) }
      onReleased: function(v) { root.setKey("plugin:hyprdrive:anim_scale", Number((1 / v).toFixed(3))) }
    }
  }

  // Off is anim_scale 0 with the flight snapped too; on restores both.
  Component {
    id: motionControl
    Switch {
      checked: root.kn("plugin:hyprdrive:anim_scale") > 0.001
      onToggled: {
        var on = !checked
        if (!on) {
          var cur = root.kn("plugin:hyprdrive:anim_scale")
          if (cur > 0.001) root.lastScale = cur
          var ms = root.kn("plugin:hyprdrive:dock:minimize_ms")
          if (ms > 0) root.lastMinimizeMs = ms
        }
        root.setMany([["plugin:hyprdrive:anim_scale", on ? (root.lastScale > 0.001 ? root.lastScale : 1) : 0],
                      ["plugin:hyprdrive:dock:minimize_ms", on ? (root.lastMinimizeMs > 0 ? root.lastMinimizeMs : 400) : 0]])
      }
    }
  }

  // One motion-blur site: strength in eighths (0 = off) and, for the sites
  // that run on a clock, how the smear fades.
  Component {
    id: blurControl
    Row {
      id: blurRow
      spacing: Style.space(10)
      property var row: parent ? parent.row : ({})
      property bool rowLive: parent ? parent.live : true
      readonly property string sKey: "plugin:hyprdrive:motion_blur:" + row.site + ":strength"
      readonly property string fKey: "plugin:hyprdrive:motion_blur:" + row.site + ":fade"
      DropButton {
        visible: !!blurRow.row.clock
        implicitWidth: Style.space(112)
        anchors.verticalCenter: parent.verticalCenter
        options: Schema.fades
        value: String(root.k(blurRow.fKey))
        live: blurRow.rowLive && root.kn(blurRow.sKey) > 0
        onPicked: function(v) { root.setKey(blurRow.fKey, v) }
      }
      ValueSlider {
        implicitWidth: Style.space(250)
        anchors.verticalCenter: parent.verticalCenter
        from: 0; to: 1; stepSize: 0.125; ticks: true
        value: root.kn(blurRow.sKey)
        live: blurRow.rowLive
        valueText: current <= 0 ? "Off" : current.toFixed(3).replace(/0+$/, "").replace(/\.$/, "")
        onMoved: function(v) { root.previewKey(blurRow.sKey, v) }
        onReleased: function(v) { root.setKey(blurRow.sKey, v) }
      }
    }
  }

  // The curve picker: the choice on the left, a live demonstration on the
  // right -- a puck crossing a track on the chosen curve, and the curve drawn
  // with a dot riding it.
  Component {
    id: curveControl
    Item {
      id: cc
      property var row: parent ? parent.row : ({})
      property bool rowLive: parent ? parent.live : true
      implicitHeight: Style.space(118)
      readonly property string value: String(root.k(row.key))
      readonly property var preset: Schema.curveByValue(value)
      opacity: rowLive ? 1 : 0.45
      Behavior on opacity { NumberAnimation { duration: root.calm } }
      onValueChanged: demo.restart()

      Column {
        anchors.left: parent.left
        anchors.top: parent.top
        width: parent.width * 0.44
        spacing: Style.space(6)
        Text { text: cc.row.label || ""; color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.body }
        Text { width: parent.width; text: cc.row.desc || ""; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.caption; wrapMode: Text.WordWrap }
        Item { width: 1; height: Style.space(4) }
        DropButton {
          implicitWidth: Math.min(parent.width, Style.space(220))
          options: Schema.curves
          value: cc.value
          kind: "curve"
          live: cc.rowLive
          onPicked: function(v) { root.setKey(cc.row.key, v) }
        }
        Text { text: cc.preset.note; color: root.faint; font.family: root.fontFamily; font.pixelSize: Style.font.caption }
      }

      Rectangle {
        id: stage
        anchors.right: parent.right
        anchors.top: parent.top
        width: parent.width * 0.52
        height: parent.height
        radius: Style.space(10)
        color: root.raised
        border.width: 1
        border.color: root.hairline
        property real t: 0

        CurveGlyph {
          id: bigCurve
          anchors.left: parent.left
          anchors.top: parent.top
          anchors.bottom: parent.bottom
          anchors.margins: Style.space(10)
          width: height * 1.1
          points: cc.preset.decel
          stroke: root.tint(root.fg, 0.85)
          progress: stage.t
        }
        Item {
          anchors.left: bigCurve.right
          anchors.leftMargin: Style.space(14)
          anchors.right: parent.right
          anchors.rightMargin: Style.space(14)
          anchors.verticalCenter: parent.verticalCenter
          height: Style.space(30)
          Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: Style.space(2); radius: 1; color: root.tint(root.fg, 0.12) }
          Rectangle {
            id: puck
            width: Style.space(24); height: Style.space(24); radius: Style.space(7)
            anchors.verticalCenter: parent.verticalCenter
            color: root.accent
            x: 0
          }
        }
        MouseArea { anchors.fill: parent; hoverEnabled: true; onEntered: demo.restart(); onClicked: demo.restart() }

        SequentialAnimation {
          id: demo
          running: root.isOpen
          loops: 1
          PauseAnimation { duration: 180 }
          ParallelAnimation {
            NumberAnimation {
              target: puck; property: "x"; from: 0; to: puck.parent.width - puck.width; duration: 900
              easing.type: Easing.BezierSpline; easing.bezierCurve: [cc.preset.decel[0], cc.preset.decel[1], cc.preset.decel[2], cc.preset.decel[3], 1, 1]
            }
            NumberAnimation { target: stage; property: "t"; from: 0; to: 1; duration: 900 }
          }
          PauseAnimation { duration: 500 }
          ScriptAction { script: { stage.t = -1 } }
        }
      }
    }
  }

  // The trackpad map: the gestures as chips, the selected one's editor below.
  Component {
    id: gestureEditor
    Column {
      spacing: Style.space(12)
      readonly property var g: root.gestureList[root.gestureIndex] || null

      Flow {
        width: parent.width
        spacing: Style.space(8)
        Repeater {
          model: root.gestureList
          Rectangle {
            required property var modelData
            required property int index
            readonly property bool sel: index === root.gestureIndex
            width: chipLabel.implicitWidth + Style.space(22)
            height: Style.space(30)
            radius: Style.space(15)
            color: sel ? root.accent : (chipArea.containsMouse ? root.tint(root.fg, 0.1) : root.raised)
            border.width: 1
            border.color: sel ? root.accent : root.hairline
            Behavior on color { ColorAnimation { duration: root.quick } }
            Text {
              id: chipLabel
              anchors.centerIn: parent
              text: Model.gestureLabel(modelData, root.hd.actions)
              color: parent.sel ? root.bg : root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.bodySmall
            }
            MouseArea { id: chipArea; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.gestureIndex = index }
          }
        }
        GhostButton {
          label: "Add"; glyph: ""
          onActivated: { root.editGestures(function(l) { l.push(Model.emptyGesture()) }); root.gestureIndex = root.gestureList.length - 1 }
        }
      }

      Rectangle {
        visible: g !== null
        width: parent.width
        height: editorCol.implicitHeight + Style.space(28)
        radius: Style.space(10)
        color: root.raised
        border.width: 1
        border.color: root.hairline
        Column {
          id: editorCol
          anchors.fill: parent
          anchors.margins: Style.space(14)
          spacing: Style.space(12)
          Row {
            spacing: Style.space(12)
            Text { text: "Fingers"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter; width: Style.space(72) }
            Segmented {
              options: [{ value: "2", label: "2" }, { value: "3", label: "3" }, { value: "4", label: "4" }, { value: "5", label: "5" }]
              value: g ? String(g.fingers) : "3"
              onPicked: function(v) { root.editGestures(function(l) { l[root.gestureIndex].fingers = Number(v) }) }
            }
          }
          Row {
            spacing: Style.space(12)
            Text { text: "Direction"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter; width: Style.space(72) }
            DropButton {
              options: (root.hd.directions || []).map(function(d) { return { value: d, label: d } })
              value: g ? g.direction : "up"
              onPicked: function(v) { root.editGestures(function(l) { l[root.gestureIndex].direction = v }) }
            }
            Text { text: "Action"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter; width: Style.space(52) }
            DropButton {
              implicitWidth: Style.space(260)
              options: root.hd.actions || []
              value: g ? g.action : "overview"
              onPicked: function(v) { root.editGestures(function(l) { l[root.gestureIndex].action = v }) }
            }
          }
          Row {
            spacing: Style.space(12)
            Text { text: "Keys held"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter; width: Style.space(72) }
            Rectangle {
              width: Style.space(200); height: Style.space(30); radius: Style.space(8)
              color: root.tint(root.fg, 0.03); border.width: 1; border.color: modsInput.activeFocus ? root.accent : root.hairline
              TextInput {
                id: modsInput
                anchors.fill: parent; anchors.leftMargin: Style.space(10); anchors.rightMargin: Style.space(10)
                verticalAlignment: TextInput.AlignVCenter
                text: g ? (g.mods || "") : ""
                color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall
                selectByMouse: true
                onEditingFinished: { var v = text; root.editGestures(function(l) { l[root.gestureIndex].mods = v }) }
                Text { visible: !parent.text && !parent.activeFocus; text: "SUPER, ALT, CTRL"; color: root.faint; font: parent.font; anchors.verticalCenter: parent.verticalCenter }
              }
            }
            Text {
              visible: g && Model.actionNeedsArg(g.action)
              text: "Argument"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
              visible: g && Model.actionNeedsArg(g.action)
              width: Style.space(250); height: Style.space(30); radius: Style.space(8)
              color: root.tint(root.fg, 0.03); border.width: 1; border.color: argInput.activeFocus ? root.accent : root.hairline
              TextInput {
                id: argInput
                anchors.fill: parent; anchors.leftMargin: Style.space(10); anchors.rightMargin: Style.space(10)
                verticalAlignment: TextInput.AlignVCenter
                text: g ? (g.arg || "") : ""
                color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall
                selectByMouse: true
                clip: true
                onEditingFinished: { var v = text; root.editGestures(function(l) { l[root.gestureIndex].arg = v }) }
                Text { visible: !parent.text && !parent.activeFocus; text: g ? Model.argPlaceholder(g.action) : ""; color: root.faint; font: parent.font; anchors.verticalCenter: parent.verticalCenter }
              }
            }
          }
          Row {
            spacing: Style.space(12)
            Text { text: "Distance"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter; width: Style.space(72) }
            ValueSlider {
              implicitWidth: Style.space(300)
              from: 0.2; to: 3; stepSize: 0.05
              value: g ? g.scale : 1
              valueText: current.toFixed(2) + "×"
              onReleased: function(v) { root.editGestures(function(l) { l[root.gestureIndex].scale = v }) }
            }
            Text { text: "Ignore inhibit"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter }
            Switch {
              anchors.verticalCenter: parent.verticalCenter
              checked: !!(g && g.disable_inhibit)
              onToggled: root.editGestures(function(l) { l[root.gestureIndex].disable_inhibit = !l[root.gestureIndex].disable_inhibit })
            }
          }
        }
      }

      Row {
        spacing: Style.space(8)
        GhostButton { label: "Apply map"; glyph: ""; emphasis: root.gestureDirty; onActivated: root.applyGestures() }
        GhostButton {
          label: "Remove gesture"; glyph: ""; danger: true
          onActivated: {
            var i = root.gestureIndex
            root.editGestures(function(l) { l.splice(i, 1) })
            root.gestureIndex = Math.max(0, Math.min(i, root.gestureList.length - 1))
          }
        }
        GhostButton { visible: root.gestureDirty; label: "Discard changes"; onActivated: { root.gestureDraft = null; root.gestureDirty = false } }
      }
    }
  }

  Component {
    id: powerControl
    Column {
      spacing: Style.space(12)
      readonly property string mode: root.hd.profileMode || "auto"
      Row {
        spacing: Style.space(12)
        Rectangle {
          width: Style.space(10); height: width; radius: width / 2
          anchors.verticalCenter: parent.verticalCenter
          color: root.onBattery ? root.urgent : root.accent
        }
        Text {
          text: (root.onBattery ? "On battery" : "On AC power") + " · " + (parent.parent.mode === "auto" ? "profiles follow the charger" : "profile forced")
          color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.body
          anchors.verticalCenter: parent.verticalCenter
        }
      }
      Segmented {
        options: [{ value: "auto", label: "Auto" }, { value: "ac", label: "Always AC" }, { value: "battery", label: "Always battery" }]
        value: parent.mode
        onPicked: function(v) { root.runCtl(["profile-mode", v], true) }
      }
      Text {
        width: parent.width
        wrapMode: Text.WordWrap
        text: "Save what the panel is set to now as the AC or battery profile. In Auto, the matching one is applied when the charger is plugged in or pulled."
        color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.caption
      }
      Row {
        spacing: Style.space(8)
        GhostButton { label: "Save as AC"; glyph: ""; onActivated: { root.runCtl(["profile-save", "ac"], true); root.showToast("Saved as the AC profile") } }
        GhostButton { label: "Save as battery"; glyph: ""; onActivated: { root.runCtl(["profile-save", "battery"], true); root.showToast("Saved as the battery profile") } }
        GhostButton { label: "Apply now"; glyph: ""; onActivated: { root.runCtl(["profile-apply", "auto"], true); root.showToast("Profile applied") } }
      }
    }
  }

  // ================================================================ window

  KeyboardPanel {
    id: panel
    anchorItem: root.anchorItem
    owner: root.host
    bar: root.bar
    open: root.isOpen
    padding: 0
    focusTarget: keys
    contentWidth: panel.fittedContentWidth(Style.space(1080))
    contentHeight: panel.fittedContentHeight(Style.space(900))

    Item {
      id: keys
      anchors.fill: parent
      focus: true
      Keys.priority: Keys.BeforeItem
      Keys.onPressed: function(e) {
        var typing = searchField.activeFocus
        // The confirm owns the keyboard while it is up: Escape cancels,
        // Enter allows, and nothing else reaches the page behind it.
        if (confirmLayer.open) {
          if (e.key === Qt.Key_Escape) confirmLayer.cancel()
          else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) confirmLayer.allow()
          e.accepted = true; return
        }
        if (e.key === Qt.Key_Escape) {
          if (menuLayer.owner) menuLayer.close()
          else if (root.query !== "" || typing) { searchField.text = ""; keys.forceActiveFocus() }
          else if (root.host) root.host.close()
          e.accepted = true; return
        }
        if (e.key === Qt.Key_Tab || e.key === Qt.Key_Backtab) {
          if (root.host) root.host.switchPanel((e.modifiers & Qt.ShiftModifier) || e.key === Qt.Key_Backtab ? -1 : 1)
          e.accepted = true; return
        }
        if ((e.modifiers & Qt.ControlModifier) && e.key === Qt.Key_F) { searchField.forceActiveFocus(); e.accepted = true; return }
        if (typing) return
        if ((e.modifiers & Qt.AltModifier) && e.key >= Qt.Key_1 && e.key <= Qt.Key_9) {
          var n = e.key - Qt.Key_1
          if (n < Schema.sections.length) root.goTo(Schema.sections[n].id)
          e.accepted = true; return
        }
        if (e.key === Qt.Key_Up || e.key === Qt.Key_Down) {
          var i = root.sectionIndex(root.section) + (e.key === Qt.Key_Down ? 1 : -1)
          root.goTo(Schema.sections[(i + Schema.sections.length) % Schema.sections.length].id)
          e.accepted = true; return
        }
        // Typing anywhere starts a search.
        if (e.text && e.text.length === 1 && e.text >= " " && !(e.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
          searchField.forceActiveFocus()
          searchField.text = searchField.text + e.text
          e.accepted = true
        }
      }

      // --------------------------------------------------------- the rail
      Rectangle {
        id: rail
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Style.space(232)
        radius: root.radius
        color: root.tint(root.fg, 0.03)
        // Square the rail's right edge against the page.
        Rectangle { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: root.radius; color: parent.color }
        Rectangle { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: 1; color: root.hairline }

        // The brand: the mark, the name, the version.
        Item {
          id: brand
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.top: parent.top
          anchors.margins: Style.space(20)
          height: Style.space(56)

          Rectangle {
            id: markTile
            width: Style.space(44); height: width
            radius: Style.space(12)
            anchors.verticalCenter: parent.verticalCenter
            gradient: Gradient {
              orientation: Gradient.Vertical
              GradientStop { position: 0; color: root.mix(root.accent, root.fg, 0.18) }
              GradientStop { position: 1; color: root.mix(root.accent, root.bg, 0.28) }
            }
            scale: root.isOpen ? 1 : 0.6
            rotation: root.isOpen ? 0 : -12
            opacity: root.isOpen ? 1 : 0
            Behavior on scale { NumberAnimation { duration: root.slow; easing.type: Easing.OutBack; easing.overshoot: 1.8 } }
            Behavior on rotation { NumberAnimation { duration: root.slow; easing.type: Easing.OutCubic } }
            Behavior on opacity { NumberAnimation { duration: root.calm } }
            Image {
              id: markImg
              anchors.centerIn: parent
              width: parent.width * 0.62; height: width
              source: Qt.resolvedUrl("hyprdrive-icon.png")
              sourceSize: Qt.size(128, 128)
              smooth: true; mipmap: true
              visible: false
            }
            MultiEffect { anchors.fill: markImg; source: markImg; colorization: 1.0; colorizationColor: root.bg }
          }
          Column {
            anchors.left: markTile.right
            anchors.leftMargin: Style.space(12)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)
            opacity: root.isOpen ? 1 : 0
            x: root.isOpen ? 0 : -Style.space(8)
            Behavior on opacity { NumberAnimation { duration: root.slow; easing.type: Easing.OutCubic } }
            Text {
              text: "Hyprdrive"
              color: root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.heading + Style.space(3)
              font.weight: Font.Bold
              font.letterSpacing: -0.3
            }
            Rectangle {
              width: verText.implicitWidth + Style.space(12)
              height: Style.space(18)
              radius: Style.space(9)
              color: root.tint(root.accent, 0.16)
              visible: verText.text !== ""
              Text {
                id: verText
                anchors.centerIn: parent
                text: root.hd.version ? "v" + root.hd.version : ""
                color: root.accent
                font.family: root.fontFamily
                font.pixelSize: Style.font.caption
              }
            }
          }
        }

        // Find a setting.
        Rectangle {
          id: searchBox
          anchors.top: brand.bottom
          anchors.topMargin: Style.space(14)
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.leftMargin: Style.space(14)
          anchors.rightMargin: Style.space(14)
          height: Style.space(34)
          radius: Style.space(9)
          color: searchField.activeFocus ? root.tint(root.fg, 0.07) : root.tint(root.fg, 0.045)
          border.width: 1
          border.color: searchField.activeFocus ? root.tint(root.accent, 0.85) : root.hairline
          Behavior on border.color { ColorAnimation { duration: root.quick } }
          Text {
            id: searchGlyph
            anchors.left: parent.left
            anchors.leftMargin: Style.space(11)
            anchors.verticalCenter: parent.verticalCenter
            text: ""
            color: searchField.activeFocus ? root.accent : root.faint
            font.family: root.fontFamily
            font.pixelSize: Style.font.bodySmall
          }
          TextInput {
            id: searchField
            anchors.left: searchGlyph.right
            anchors.leftMargin: Style.space(9)
            anchors.right: parent.right
            anchors.rightMargin: Style.space(10)
            anchors.verticalCenter: parent.verticalCenter
            color: root.fg
            font.family: root.fontFamily
            font.pixelSize: Style.font.body
            clip: true
            selectByMouse: true
            onTextChanged: root.query = text
            Keys.onDownPressed: keys.forceActiveFocus()
            Text {
              visible: !parent.text
              text: "Find a setting"
              color: root.faint
              font: parent.font
              anchors.verticalCenter: parent.verticalCenter
            }
          }
        }

        // The sections. The highlight glides between them.
        Item {
          id: nav
          anchors.top: searchBox.bottom
          anchors.topMargin: Style.space(14)
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.leftMargin: Style.space(10)
          anchors.rightMargin: Style.space(10)
          height: navCol.implicitHeight

          Rectangle {
            id: navHighlight
            readonly property Item target: navRep.count > 0 && root.query === "" ? navRep.itemAt(root.sectionIndex(root.section)) : null
            visible: target !== null
            x: 0
            width: nav.width
            y: target ? target.y : 0
            height: target ? target.height : 0
            radius: Style.space(9)
            color: root.tint(root.accent, 0.16)
            Behavior on y { NumberAnimation { duration: root.calm + 40; easing.type: Easing.OutBack; easing.overshoot: 0.9 } }
            Rectangle {
              width: Style.space(3)
              height: parent.height * 0.5
              radius: width / 2
              anchors.left: parent.left
              anchors.leftMargin: Style.space(3)
              anchors.verticalCenter: parent.verticalCenter
              color: root.accent
            }
          }

          Column {
            id: navCol
            width: parent.width
            spacing: Style.space(2)
            Repeater {
              id: navRep
              model: Schema.sections
              Item {
                id: navItem
                required property var modelData
                required property int index
                readonly property bool active: root.query === "" && root.section === modelData.id
                width: navCol.width
                height: Style.space(38)
                opacity: root.isOpen ? 1 : 0
                transform: Translate {
                  x: root.isOpen ? 0 : -Style.space(14)
                  Behavior on x { SequentialAnimation { PauseAnimation { duration: 40 + navItem.index * 35 } NumberAnimation { duration: root.slow; easing.type: Easing.OutCubic } } }
                }
                Behavior on opacity { SequentialAnimation { PauseAnimation { duration: 40 + navItem.index * 35 } NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } } }

                Rectangle {
                  anchors.fill: parent
                  radius: Style.space(9)
                  color: navArea.containsMouse && !navItem.active ? root.tint(root.fg, 0.05) : "transparent"
                  Behavior on color { ColorAnimation { duration: root.quick } }
                }
                Text {
                  id: navGlyph
                  anchors.left: parent.left
                  anchors.leftMargin: Style.space(16)
                  anchors.verticalCenter: parent.verticalCenter
                  width: Style.space(18)
                  horizontalAlignment: Text.AlignHCenter
                  text: navItem.modelData.icon
                  color: navItem.active ? root.accent : root.dim
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.title
                  scale: navItem.active ? 1.08 : 1
                  Behavior on color { ColorAnimation { duration: root.quick } }
                  Behavior on scale { NumberAnimation { duration: root.calm; easing.type: Easing.OutBack } }
                }
                Text {
                  anchors.left: navGlyph.right
                  anchors.leftMargin: Style.space(12)
                  anchors.verticalCenter: parent.verticalCenter
                  text: navItem.modelData.title
                  color: navItem.active ? root.fg : root.dim
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.body
                  font.weight: navItem.active ? Font.DemiBold : Font.Normal
                  Behavior on color { ColorAnimation { duration: root.quick } }
                }
                Text {
                  anchors.right: parent.right
                  anchors.rightMargin: Style.space(12)
                  anchors.verticalCenter: parent.verticalCenter
                  text: "Alt " + (navItem.index + 1)
                  color: root.faint
                  font.family: root.fontFamily
                  font.pixelSize: Style.font.caption
                  opacity: navArea.containsMouse ? 1 : 0
                  Behavior on opacity { NumberAnimation { duration: root.quick } }
                }
                MouseArea {
                  id: navArea
                  anchors.fill: parent
                  hoverEnabled: true
                  cursorShape: Qt.PointingHandCursor
                  onClicked: root.goTo(navItem.modelData.id)
                }
              }
            }
          }
        }

        // By dragosol, with the links behind it.
        Item {
          id: signature
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.bottom: parent.bottom
          anchors.margins: Style.space(14)
          height: Style.space(40)
          readonly property bool hot: sigArea.containsMouse || linksHover.hovered
          onHotChanged: if (hot) linksGrace.stop(); else linksGrace.restart()
          property bool revealed: false
          Timer { id: linksGrace; interval: 260; onTriggered: signature.revealed = false }
          onRevealedChanged: if (!revealed) linksGrace.stop()

          Rectangle {
            anchors.fill: parent
            radius: Style.space(10)
            color: signature.revealed ? root.tint(root.fg, 0.06) : "transparent"
            Behavior on color { ColorAnimation { duration: root.quick } }
          }
          Rectangle {
            id: avatar
            width: Style.space(26); height: width; radius: width / 2
            anchors.left: parent.left
            anchors.leftMargin: Style.space(8)
            anchors.verticalCenter: parent.verticalCenter
            gradient: Gradient {
              GradientStop { position: 0; color: root.mix(root.accent, root.fg, 0.25) }
              GradientStop { position: 1; color: root.mix(root.accent, root.bg, 0.35) }
            }
            Text { anchors.centerIn: parent; text: "d"; color: root.bg; font.family: root.fontFamily; font.pixelSize: Style.font.body; font.weight: Font.Bold }
          }
          Row {
            anchors.left: avatar.right
            anchors.leftMargin: Style.space(10)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(5)
            Text { text: "by"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.body }
            Text { text: "dragosol"; color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.body; font.weight: Font.DemiBold }
          }
          Text {
            anchors.right: parent.right
            anchors.rightMargin: Style.space(10)
            anchors.verticalCenter: parent.verticalCenter
            text: ""
            color: root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.bodySmall
            rotation: signature.revealed ? 0 : 180
            Behavior on rotation { NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
          }
          MouseArea {
            id: sigArea
            anchors.fill: parent
            hoverEnabled: true
            onEntered: signature.revealed = true
            onClicked: signature.revealed = !signature.revealed
          }

          // The links card rises out of the signature.
          Rectangle {
            id: linksCard
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.top
            anchors.bottomMargin: Style.space(8)
            height: linksCol.implicitHeight + Style.space(12)
            radius: Style.space(12)
            color: root.mix(root.bg, root.fg, 0.06)
            border.width: 1
            border.color: root.tint(root.fg, 0.12)
            visible: opacity > 0.01
            opacity: signature.revealed ? 1 : 0
            transform: [
              Translate { y: signature.revealed ? 0 : Style.space(10) ; Behavior on y { NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } } },
              Scale { origin.x: linksCard.width / 2; origin.y: linksCard.height; xScale: signature.revealed ? 1 : 0.94; yScale: xScale
                      Behavior on xScale { NumberAnimation { duration: root.calm; easing.type: Easing.OutBack } } }
            ]
            Behavior on opacity { NumberAnimation { duration: root.quick; easing.type: Easing.OutCubic } }
            HoverHandler { id: linksHover }

            Column {
              id: linksCol
              anchors.left: parent.left
              anchors.right: parent.right
              anchors.top: parent.top
              anchors.margins: Style.space(6)
              spacing: Style.space(2)
              Repeater {
                model: Schema.links
                Rectangle {
                  id: linkRow
                  required property var modelData
                  required property int index
                  width: linksCol.width
                  height: Style.space(40)
                  radius: Style.space(8)
                  color: linkArea.containsMouse ? root.tint(root.accent, 0.16) : "transparent"
                  Behavior on color { ColorAnimation { duration: root.quick } }
                  opacity: signature.revealed ? 1 : 0
                  Behavior on opacity { SequentialAnimation { PauseAnimation { duration: signature.revealed ? 30 + linkRow.index * 45 : 0 } NumberAnimation { duration: root.quick } } }
                  Text {
                    id: linkGlyph
                    anchors.left: parent.left
                    anchors.leftMargin: Style.space(10)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Style.space(18)
                    horizontalAlignment: Text.AlignHCenter
                    text: linkRow.modelData.icon
                    color: linkArea.containsMouse ? root.accent : root.fg
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.title
                  }
                  Column {
                    anchors.left: linkGlyph.right
                    anchors.leftMargin: Style.space(10)
                    anchors.right: linkArrow.left
                    anchors.rightMargin: Style.space(6)
                    anchors.verticalCenter: parent.verticalCenter
                    // Bounded and elided: a long handle must never run out of the card.
                    Text { width: parent.width; elide: Text.ElideRight; text: linkRow.modelData.label; color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall }
                    Text { width: parent.width; elide: Text.ElideRight; text: linkRow.modelData.detail; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.caption }
                  }
                  Text {
                    id: linkArrow
                    anchors.right: parent.right
                    anchors.rightMargin: Style.space(10)
                    anchors.verticalCenter: parent.verticalCenter
                    text: ""
                    color: root.faint
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.caption
                    opacity: linkArea.containsMouse ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: root.quick } }
                  }
                  MouseArea {
                    id: linkArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                      Qt.openUrlExternally(linkRow.modelData.url)
                      if (root.host) root.host.close()
                    }
                  }
                }
              }
            }
          }
        }
      }

      // --------------------------------------------------------- the page
      Item {
        id: page
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        clip: true

        property real slide: 0
        opacity: 1

        SequentialAnimation {
          id: pageSwap
          NumberAnimation { target: page; property: "opacity"; to: 0; duration: 90; easing.type: Easing.InCubic }
          ScriptAction {
            script: {
              root.shownSection = root.section
              flick.contentY = 0
              page.slide = Style.space(18) * root.navDirection
            }
          }
          ParallelAnimation {
            NumberAnimation { target: page; property: "opacity"; to: 1; duration: root.calm; easing.type: Easing.OutCubic }
            NumberAnimation { target: page; property: "slide"; to: 0; duration: root.slow; easing.type: Easing.OutCubic }
          }
        }

        // Title, subtitle, restore defaults.
        Item {
          id: pageHead
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.top: parent.top
          anchors.leftMargin: Style.space(30)
          anchors.rightMargin: Style.space(26)
          anchors.topMargin: Style.space(24)
          height: Style.space(58)
          transform: Translate { y: page.slide * 0.5 }

          Column {
            anchors.left: parent.left
            anchors.right: restore.left
            anchors.rightMargin: Style.space(16)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(4)
            Text {
              text: root.query !== "" ? "Results" : root.shownSectionData.title
              color: root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.display
              font.weight: Font.Bold
              font.letterSpacing: -0.4
            }
            Text {
              width: parent.width
              elide: Text.ElideRight
              text: root.query !== ""
                ? (root.searchCards.length ? "Settings matching “" + root.query + "”" : "Nothing matches “" + root.query + "”")
                : root.shownSectionData.subtitle
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
            }
          }

          // Restore defaults: one click arms it, the second restores.
          GhostButton {
            id: restore
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: root.query === ""
            property bool armed: false
            glyph: ""
            label: armed ? "Click again to restore" : "Restore defaults"
            emphasis: armed
            onActivated: {
              if (!armed) { armed = true; disarm.restart(); return }
              armed = false
              root.restoreSection(root.shownSectionData)
            }
            Timer { id: disarm; interval: 4000; onTriggered: restore.armed = false }
            Connections { target: root; function onShownSectionChanged() { restore.armed = false } }
          }
        }

        Flickable {
          id: flick
          anchors.left: parent.left
          anchors.right: parent.right
          anchors.top: pageHead.bottom
          anchors.bottom: parent.bottom
          anchors.topMargin: Style.space(12)
          contentWidth: width
          contentHeight: cardsCol.implicitHeight + Style.space(40)
          boundsBehavior: Flickable.StopAtBounds
          clip: true
          ScrollBar.vertical: ScrollBar {
            policy: flick.contentHeight > flick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            contentItem: Rectangle { implicitWidth: Style.space(4); radius: 2; color: root.tint(root.fg, 0.25) }
          }

          Column {
            id: cardsCol
            x: Style.space(30)
            width: flick.width - Style.space(56)
            spacing: Style.space(16)
            transform: Translate { y: page.slide }

            Repeater {
              model: root.pageCards
              Item {
                id: cardItem
                required property var modelData
                required property int index
                width: cardsCol.width
                height: cardBody.height + cardTitle.height + Style.space(8)

                // Staggered entrance each time the page is rebuilt.
                opacity: 0
                property real rise: Style.space(10)
                Component.onCompleted: cardIn.start()
                ParallelAnimation {
                  id: cardIn
                  SequentialAnimation {
                    PauseAnimation { duration: 30 + cardItem.index * 45 }
                    ParallelAnimation {
                      NumberAnimation { target: cardItem; property: "opacity"; to: 1; duration: root.calm; easing.type: Easing.OutCubic }
                      NumberAnimation { target: cardItem; property: "rise"; to: 0; duration: root.slow; easing.type: Easing.OutCubic }
                    }
                  }
                }
                transform: Translate { y: cardItem.rise }

                Row {
                  id: cardTitle
                  spacing: Style.space(8)
                  height: Style.space(22)
                  Text {
                    text: cardItem.modelData.title.toUpperCase()
                    color: root.faint
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.caption
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.2
                    anchors.verticalCenter: parent.verticalCenter
                  }
                  // In search results, a way back to where the setting lives.
                  Text {
                    visible: root.query !== "" && !!cardItem.modelData.section
                    text: "Open "
                    color: root.accent
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.caption
                    anchors.verticalCenter: parent.verticalCenter
                    MouseArea { anchors.fill: parent; anchors.margins: -Style.space(4); cursorShape: Qt.PointingHandCursor; onClicked: root.goTo(cardItem.modelData.section) }
                  }
                }

                Rectangle {
                  id: cardBody
                  anchors.top: cardTitle.bottom
                  anchors.topMargin: Style.space(8)
                  width: parent.width
                  height: rowsCol.implicitHeight + Style.space(8)
                  radius: root.radius
                  color: root.raised
                  border.width: 1
                  border.color: root.hairline

                  Column {
                    id: rowsCol
                    x: Style.space(18)
                    y: Style.space(4)
                    width: parent.width - Style.space(36)
                    Repeater {
                      model: cardItem.modelData.rows
                      SettingRow {
                        required property var modelData
                        required property int index
                        row: modelData
                        first: index === 0
                      }
                    }
                  }
                }
              }
            }

            // Search found nothing: say so, softly.
            Item {
              visible: root.query !== "" && root.searchCards.length === 0
              width: parent.width
              height: Style.space(160)
              Column {
                anchors.centerIn: parent
                spacing: Style.space(8)
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: ""; color: root.faint; font.family: root.fontFamily; font.pixelSize: Style.font.display }
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Try “blur”, “dock” or “speed”"; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.body }
              }
            }
          }
        }

        // Toast.
        Rectangle {
          anchors.horizontalCenter: parent.horizontalCenter
          anchors.bottom: parent.bottom
          anchors.bottomMargin: root.toastText !== "" ? Style.space(18) : Style.space(4)
          width: toastRow.implicitWidth + Style.space(28)
          height: Style.space(34)
          radius: height / 2
          color: root.mix(root.bg, root.fg, 0.12)
          border.width: 1
          border.color: root.tint(root.accent, 0.5)
          opacity: root.toastText !== "" ? 1 : 0
          Behavior on opacity { NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
          Behavior on anchors.bottomMargin { NumberAnimation { duration: root.calm; easing.type: Easing.OutBack } }
          Row {
            id: toastRow
            anchors.centerIn: parent
            spacing: Style.space(8)
            Text { text: ""; color: root.accent; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter }
            Text { text: root.toastText; color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; anchors.verticalCenter: parent.verticalCenter }
          }
        }

        // Working: a thin line along the page's top edge while hdpanel runs.
        Rectangle {
          anchors.top: parent.top
          height: Style.space(2)
          width: parent.width * 0.25
          color: root.accent
          radius: 1
          visible: root.ctlBusy
          SequentialAnimation on x {
            running: root.ctlBusy
            loops: Animation.Infinite
            NumberAnimation { from: -page.width * 0.25; to: page.width; duration: 900; easing.type: Easing.InOutCubic }
          }
        }
      }

      // ------------------------------------------------------ the dropdowns
      Item {
        id: menuLayer
        anchors.fill: parent
        z: 50
        visible: owner !== null
        property Item owner: null
        property var options: []
        property string value: ""
        property string kind: "plain"
        property var onPick: null

        function openFor(item, options, value, kind, onPick) {
          if (owner === item) { close(); return }
          owner = item
          menuLayer.options = options
          menuLayer.value = value
          menuLayer.kind = kind
          menuLayer.onPick = onPick
          menu.width = Math.max(item.width, kind === "curve" ? Style.space(260) : Style.space(180))
          var p = item.mapToItem(menuLayer, 0, item.height + Style.space(6))
          var h = Math.min(menuList.implicitHeight + Style.space(12), menuLayer.height - Style.space(20))
          menu.x = Math.max(Style.space(10), Math.min(p.x, menuLayer.width - menu.width - Style.space(10)))
          menu.y = p.y + h > menuLayer.height - Style.space(10) ? Math.max(Style.space(10), item.mapToItem(menuLayer, 0, 0).y - h - Style.space(6)) : p.y
          menuIn.restart()
        }
        function close() { owner = null }

        MouseArea { anchors.fill: parent; onClicked: menuLayer.close() }

        Rectangle {
          id: menu
          height: Math.min(menuList.implicitHeight + Style.space(12), menuLayer.height - Style.space(20))
          radius: Style.space(10)
          color: root.mix(root.bg, root.fg, 0.07)
          border.width: 1
          border.color: root.tint(root.fg, 0.14)
          clip: true
          transformOrigin: Item.Top
          ParallelAnimation {
            id: menuIn
            NumberAnimation { target: menu; property: "opacity"; from: 0; to: 1; duration: root.quick; easing.type: Easing.OutCubic }
            NumberAnimation { target: menu; property: "scale"; from: 0.94; to: 1; duration: root.calm; easing.type: Easing.OutBack }
          }
          Flickable {
            anchors.fill: parent
            anchors.margins: Style.space(6)
            contentHeight: menuList.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Column {
              id: menuList
              width: parent.width
              spacing: Style.space(1)
              Repeater {
                model: menuLayer.options
                Rectangle {
                  id: opt
                  required property var modelData
                  readonly property bool sel: modelData.value === menuLayer.value
                  width: menuList.width
                  height: menuLayer.kind === "curve" ? Style.space(46) : Style.space(32)
                  radius: Style.space(7)
                  color: optArea.containsMouse ? root.tint(root.accent, 0.18) : (sel ? root.tint(root.fg, 0.06) : "transparent")
                  Behavior on color { ColorAnimation { duration: root.quick } }
                  CurveGlyph {
                    id: optCurve
                    visible: menuLayer.kind === "curve"
                    anchors.left: parent.left
                    anchors.leftMargin: Style.space(10)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Style.space(38); height: Style.space(32)
                    points: opt.modelData.decel || [0, 0, 1, 1]
                    stroke: opt.sel || optArea.containsMouse ? root.accent : root.dim
                    lineWidth: Style.space(1.6)
                  }
                  Column {
                    anchors.left: menuLayer.kind === "curve" ? optCurve.right : parent.left
                    anchors.leftMargin: Style.space(10)
                    anchors.verticalCenter: parent.verticalCenter
                    Text { text: opt.modelData.label || opt.modelData.value; color: root.fg; font.family: root.fontFamily; font.pixelSize: Style.font.bodySmall; font.weight: opt.sel ? Font.DemiBold : Font.Normal }
                    Text { visible: !!opt.modelData.note; text: opt.modelData.note || ""; color: root.dim; font.family: root.fontFamily; font.pixelSize: Style.font.caption }
                  }
                  Text {
                    anchors.right: parent.right
                    anchors.rightMargin: Style.space(10)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: opt.sel
                    text: ""
                    color: root.accent
                    font.family: root.fontFamily
                    font.pixelSize: Style.font.caption
                  }
                  MouseArea {
                    id: optArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                      var cb = menuLayer.onPick
                      var v = opt.modelData.value
                      menuLayer.close()
                      if (cb) cb(v)
                    }
                  }
                }
              }
            }
          }
        }
      }

      // ------------------------------------------------------- the confirm
      // A question before a row changes something outside hyprdrive (a row's
      // `confirm` in Schema.js). Only Allow runs the change; Cancel, Escape
      // or a click outside the card leave everything as it was. Drawn like
      // the dropdowns, over a dimmed panel.
      Item {
        id: confirmLayer
        anchors.fill: parent
        z: 60
        property var spec: null
        property var onAllow: null
        // What the card shows. Kept after a close so the text does not
        // blank out while the card fades.
        property var shown: ({})
        readonly property bool open: spec !== null
        visible: open || card.opacity > 0.01

        function ask(s, cb) {
          shown = s
          onAllow = cb
          spec = s
        }
        function cancel() { spec = null; onAllow = null }
        function allow() {
          var cb = onAllow
          cancel()
          if (cb) cb()
        }

        Rectangle {
          anchors.fill: parent
          radius: root.radius
          color: root.tint(root.bg, 0.6)
          opacity: confirmLayer.open ? 1 : 0
          Behavior on opacity { NumberAnimation { duration: root.calm; easing.type: Easing.OutCubic } }
        }
        MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: confirmLayer.cancel() }

        Rectangle {
          id: card
          anchors.centerIn: parent
          width: Math.min(Style.space(440), parent.width - Style.space(60))
          height: cardCol.implicitHeight + Style.space(44)
          radius: Style.space(14)
          color: root.mix(root.bg, root.fg, 0.07)
          border.width: 1
          border.color: root.tint(root.fg, 0.14)
          opacity: confirmLayer.open ? 1 : 0
          scale: confirmLayer.open ? 1 : 0.94
          Behavior on opacity { NumberAnimation { duration: root.quick; easing.type: Easing.OutCubic } }
          Behavior on scale { NumberAnimation { duration: root.calm; easing.type: Easing.OutBack } }
          // Clicks on the card itself are not a click outside it.
          MouseArea { anchors.fill: parent }

          Column {
            id: cardCol
            x: Style.space(22)
            y: Style.space(22)
            width: parent.width - Style.space(44)
            spacing: Style.space(10)

            Rectangle {
              visible: !!confirmLayer.shown.glyph
              width: Style.space(40); height: width
              radius: Style.space(11)
              color: root.tint(root.accent, 0.16)
              Text {
                anchors.centerIn: parent
                text: confirmLayer.shown.glyph || ""
                color: root.accent
                font.family: root.fontFamily
                font.pixelSize: Style.font.body
              }
            }
            Text {
              width: parent.width
              text: confirmLayer.shown.title || ""
              color: root.fg
              font.family: root.fontFamily
              font.pixelSize: Style.font.body
              font.weight: Font.DemiBold
              wrapMode: Text.WordWrap
            }
            Text {
              width: parent.width
              visible: !!confirmLayer.shown.body
              text: confirmLayer.shown.body || ""
              color: root.dim
              font.family: root.fontFamily
              font.pixelSize: Style.font.bodySmall
              wrapMode: Text.WordWrap
              lineHeight: 1.2
            }
            Item { width: 1; height: Style.space(4) }
            Row {
              anchors.right: parent.right
              spacing: Style.space(8)
              GhostButton { label: confirmLayer.shown.cancel || "Cancel"; onActivated: confirmLayer.cancel() }
              GhostButton { label: confirmLayer.shown.allow || "Allow"; emphasis: true; onActivated: confirmLayer.allow() }
            }
          }
        }
      }
    }
  }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import Quickshell
import Quickshell.Io
import Quickshell.Services.UPower
import qs.Commons
import qs.Ui
import "Model.js" as Model

Panel {
  id: root
  moduleName: "dragos.hyprdrive"
  ipcTarget: "dragos.hyprdrive"
  manageIpc: false

  property string ctlPath: ""
  property var state: ({})
  property var pending: ({})
  property string section: "overview"
  property int gestureIndex: 0
  property bool dumping: false
  property bool dumpWanted: false
  property bool applying: false
  property bool ctlRefresh: true
  property var ctlQueue: []

  readonly property color foreground: bar ? bar.foreground : Color.foreground
  readonly property color dim: Qt.darker(foreground, 1.5)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property var gestures: Model.cloneGestures(state.gestures)
  readonly property var actions: state.actions || []
  readonly property var directions: state.directions || ["up", "down", "left", "right", "horizontal"]
  readonly property bool onBattery: UPower.onBattery === true
  readonly property string profileMode: state.profileMode || "auto"
  readonly property bool dockIsRounded: {
    var _p = root.pending
    var _s = root.state
    return Model.asNumber(root.k("plugin:hyprdrive:dock:corner_radius", 24), 24) > 0
  }
  readonly property bool compositorAnims: {
    var _p = root.pending
    var _s = root.state
    return Model.asBool(root.k("animations:enabled", true))
  }
  readonly property bool hyprdriveAnims: {
    var _p = root.pending
    var _s = root.state
    return Model.asNumber(root.k("plugin:hyprdrive:anim_scale", 1), 1) > 0.001
  }
  property real lastHyprdriveScale: 1
  property int lastMinimizeMs: 400

  function k(key, fallback) {
    if (root.pending && root.pending[key] !== undefined)
      return root.pending[key]
    return Model.keyOf(state, key, fallback)
  }

  function remember(key, value) {
    var next = Object.assign({}, root.pending)
    next[key] = value
    root.pending = next
  }

  function dump() {
    if (ctlPath === "") return
    if (dumping) {
      dumpWanted = true
      return
    }
    dumpWanted = false
    dumping = true
    dumpProc.running = true
  }

  function pumpCtl() {
    if (ctlPath === "" || ctlProc.running || ctlQueue.length === 0)
      return
    var job = ctlQueue.shift()
    ctlRefresh = job.refresh !== false
    applying = true
    ctlProc.command = [ctlPath].concat(job.args)
    ctlProc.running = true
  }

  function runCtl(args, refresh) {
    if (ctlPath === "") return
    ctlQueue = ctlQueue.concat([{ args: args, refresh: refresh !== false }])
    pumpCtl()
  }

  function setKey(key, value) {
    remember(key, value)
    runCtl(["set", key, String(value)], true)
  }

  function previewKey(key, value) {
    remember(key, value)
    runCtl(["preview", key, String(value)], false)
  }

  function toggleKey(key) {
    setKey(key, !Model.asBool(k(key, false)))
  }

  function setDockRounded(on) {
    var radius = on ? (state.roundedRadius || 24) : 0
    setKey("plugin:hyprdrive:dock:corner_radius", radius)
  }

  function setHyprdriveAnims(on) {
    var curScale = Model.asNumber(root.k("plugin:hyprdrive:anim_scale", 1), 1)
    var curMin = Model.asNumber(root.k("plugin:hyprdrive:dock:minimize_ms", 400), 400)
    if (!on) {
      if (curScale > 0.001)
        lastHyprdriveScale = curScale
      if (curMin > 0)
        lastMinimizeMs = curMin
    }
    var scale = on ? (lastHyprdriveScale > 0.001 ? lastHyprdriveScale : 1) : 0
    var minms = on ? (lastMinimizeMs > 0 ? lastMinimizeMs : 400) : 0
    remember("plugin:hyprdrive:anim_scale", scale)
    remember("plugin:hyprdrive:dock:minimize_ms", minms)
    runCtl(["set",
            "plugin:hyprdrive:anim_scale", String(scale),
            "plugin:hyprdrive:dock:minimize_ms", String(minms)], true)
  }

  function saveGestures() {
    runCtl(["gestures-set", JSON.stringify(root.gestures)])
  }

  function addGesture() {
    var list = root.gestures
    list.push(Model.emptyGesture())
    state = Object.assign({}, state, { gestures: list })
    gestureIndex = list.length - 1
  }

  function removeGesture() {
    var list = root.gestures
    if (list.length === 0) return
    list.splice(Math.max(0, gestureIndex), 1)
    state = Object.assign({}, state, { gestures: list })
    gestureIndex = Math.max(0, Math.min(gestureIndex, list.length - 1))
    saveGestures()
  }

  function patchGesture(field, value) {
    var list = root.gestures
    if (!list[gestureIndex]) return
    list[gestureIndex][field] = value
    state = Object.assign({}, state, { gestures: list })
  }

  function saveCurrentProfile() {
    runCtl(["profile-save", root.onBattery ? "battery" : "ac"])
  }

  onOpenedChanged: if (opened) dump()
  // A flaky USB-C source flips onBattery several times a second, and every
  // applied profile rewrites 40-panel.lua (a full Hyprland reload). Apply once
  // the power state has held still; hdpanel reads the settled state itself.
  onOnBatteryChanged: if (profileMode === "auto") profileSettle.restart()

  Timer {
    id: profileSettle
    interval: 4000
    onTriggered: if (root.profileMode === "auto" && root.ctlPath !== "")
      root.runCtl(["profile-apply", "auto"])
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  IpcHandler {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): void { root.dump() }
  }

  Process {
    id: ctlResolve
    running: true
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
      }
    }
  }

  Process {
    id: dumpProc
    command: [root.ctlPath, "dump"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        try { root.state = JSON.parse(String(text || "{}")) } catch (e) {}
        root.pending = ({})
        var s = Model.asNumber(Model.keyOf(root.state, "plugin:hyprdrive:anim_scale", 1), 1)
        if (s > 0.001)
          root.lastHyprdriveScale = s
        var m = Model.asNumber(Model.keyOf(root.state, "plugin:hyprdrive:dock:minimize_ms", 400), 400)
        if (m > 0)
          root.lastMinimizeMs = m
        root.dumping = false
        if (root.gestures.length && root.gestureIndex >= root.gestures.length)
          root.gestureIndex = 0
        if (root.dumpWanted)
          root.dump()
      }
    }
  }

  Process {
    id: ctlProc
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: {
        root.applying = false
        if (root.ctlRefresh)
          root.dump()
        root.pumpCtl()
      }
    }
  }

  // The bar icon is hyprdrive's own mark rather than a font glyph (user,
  // 2026-09-12). It ships as an alpha mask -- white shape, transparent
  // everywhere else, the triangle notch included -- and is TINTED to the bar's
  // colour the way the tray tints symbolic icons (Tray.qml), with the exact
  // colour expression BarIconButton gives its own glyph, so light and dark
  // themes and the opened state all behave as the glyph they replace did.
  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    tooltipText: "Hyprdrive"
    active: root.opened
    onPressed: function(b) { root.toggle() }

    iconComponent: Component {
      Item {
        Image {
          id: mark
          anchors.fill: parent
          source: Qt.resolvedUrl("hyprdrive-icon.png")
          sourceSize: Qt.size(128, 128)
          fillMode: Image.PreserveAspectFit
          smooth: true
          mipmap: true
          visible: false
        }

        MultiEffect {
          anchors.fill: mark
          source: mark
          colorization: 1.0
          colorizationColor: button.active && button.useActiveColor ? button.activeColor : button.foreground
        }
      }
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(980))
    contentHeight: panel.fittedContentHeight(Style.space(740))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Row {
        id: body
        anchors.fill: parent
        anchors.margins: Style.spacing.popupPadding
        spacing: Style.space(16)

        Column {
          id: nav
          width: Style.space(168)
          spacing: Style.space(6)

          Text {
            textFormat: Text.PlainText
            text: "Hyprdrive"
            color: root.foreground
            font.family: root.fontFamily
            font.pixelSize: Style.font.title
            font.bold: true
            width: parent.width
          }
          Text {
            textFormat: Text.PlainText
            text: root.onBattery ? "On battery" : "On AC"
            color: root.dim
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
            width: parent.width
          }

          Repeater {
            model: [
              { id: "overview", label: "Overview" },
              { id: "dock", label: "Dock" },
              { id: "gestures", label: "Gestures" },
              { id: "textures", label: "Textures" },
              { id: "power", label: "Power" }
            ]
            Button {
              required property var modelData
              width: nav.width
              text: modelData.label
              selected: root.section === modelData.id
              bordered: true
              foreground: root.foreground
              onClicked: root.section = modelData.id
            }
          }
        }

        Rectangle {
          width: 1
          height: parent.height
          color: Qt.rgba(root.foreground.r, root.foreground.g, root.foreground.b, 0.12)
        }

        Flickable {
          id: pane
          width: parent.width - nav.width - 1 - parent.spacing * 2
          height: parent.height
          clip: true
          contentWidth: width
          contentHeight: paneColumn.height
          boundsBehavior: Flickable.StopAtBounds
          ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

          Column {
            id: paneColumn
            width: pane.width - Style.space(8)
            spacing: Style.space(10)

            // -------------------------------------------------------- overview
            Column {
              width: parent.width
              spacing: Style.space(8)
              visible: root.section === "overview"

              PanelSectionHeader { text: "MOTION"; foreground: root.foreground }
              Toggle {
                width: parent.width
                label: "Compositor animations"
                description: "Window open, close, and move. Off disables every Hyprland animation leaf."
                checked: root.compositorAnims
                foreground: root.foreground
                onClicked: root.setKey("animations:enabled", !root.compositorAnims)
              }
              Toggle {
                width: parent.width
                label: "Hyprdrive motion"
                description: "Overview, dock flights, and hover eases. Off snaps those; the autohide slide still eases in from the bottom."
                checked: root.hyprdriveAnims
                foreground: root.foreground
                onClicked: root.setHyprdriveAnims(!root.hyprdriveAnims)
              }
              Toggle {
                width: parent.width
                label: "Tap Super for overview"
                description: "A Super tap (no other key) toggles Mission Control."
                checked: Model.asBool(root.k("plugin:hyprdrive:tap_super", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:tap_super")
              }
              Toggle {
                width: parent.width
                label: "Mission Control grid"
                checked: Model.asBool(root.k("plugin:hyprdrive:mission_control", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:mission_control")
              }

              PanelSectionHeader { text: "SPEED"; foreground: root.foreground }
              Text { textFormat: Text.PlainText; text: "Animation scale  " + Number(root.k("plugin:hyprdrive:anim_scale", 1)).toFixed(2); color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                minimum: 0; maximum: 4; step: 0.05
                value: Model.asNumber(root.k("plugin:hyprdrive:anim_scale", 1), 1)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:anim_scale", v) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:anim_scale", v) }
              }
              Text { textFormat: Text.PlainText; text: "Overview scale  " + Number(root.k("plugin:hyprdrive:scale", 0.82)).toFixed(2); color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                minimum: 0.4; maximum: 0.9; step: 0.01
                value: Model.asNumber(root.k("plugin:hyprdrive:scale", 0.82), 0.82)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:scale", v) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:scale", v) }
              }
              Text { textFormat: Text.PlainText; text: "Open speed  " + Number(root.k("plugin:hyprdrive:open_speed", 2.6)).toFixed(1); color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                minimum: 0.1; maximum: 8; step: 0.1
                value: Model.asNumber(root.k("plugin:hyprdrive:open_speed", 2.6), 2.6)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:open_speed", v) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:open_speed", v) }
              }
              Text { textFormat: Text.PlainText; text: "Close speed  " + Number(root.k("plugin:hyprdrive:close_speed", 1.9)).toFixed(1); color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                minimum: 0.1; maximum: 8; step: 0.1
                value: Model.asNumber(root.k("plugin:hyprdrive:close_speed", 1.9), 1.9)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:close_speed", v) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:close_speed", v) }
              }
            }

            // ------------------------------------------------------------ dock
            Column {
              width: parent.width
              spacing: Style.space(8)
              visible: root.section === "dock"

              PanelSectionHeader { text: "SHAPE"; foreground: root.foreground }
              Toggle {
                width: parent.width
                label: "Rounded dock"
                description: "Off is a squared pill (corner radius 0). On restores the last rounded radius."
                checked: root.dockIsRounded
                foreground: root.foreground
                onClicked: root.setDockRounded(!root.dockIsRounded)
              }
              Toggle {
                width: parent.width
                label: "Show dock"
                checked: Model.asBool(root.k("plugin:hyprdrive:dock:enabled", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:dock:enabled")
              }
              Toggle {
                width: parent.width
                label: "Autohide"
                description: "Hide until the pointer presses the bottom edge."
                checked: Model.asBool(root.k("plugin:hyprdrive:dock:autohide", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:dock:autohide")
              }
              Toggle {
                width: parent.width
                label: "Show Applications"
                checked: Model.asBool(root.k("plugin:hyprdrive:dock:show_apps_button", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:dock:show_apps_button")
              }
              Toggle {
                width: parent.width
                label: "Pinned favorites"
                checked: Model.asBool(root.k("plugin:hyprdrive:dock:show_favorites", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:dock:show_favorites")
              }

              PanelSectionHeader { text: "TILT"; foreground: root.foreground }
              Toggle {
                width: parent.width
                label: "Hover tilt"
                description: "Icons lean toward the pointer in 3D while hovered, and spring flat when it leaves."
                checked: Model.asBool(root.k("plugin:hyprdrive:dock:hover_tilt", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:dock:hover_tilt")
              }
              Toggle {
                width: parent.width
                label: "Press tilt"
                description: "A pressed icon leans in toward where you click, with light on its face. Off keeps the plain squish."
                checked: Model.asBool(root.k("plugin:hyprdrive:dock:press_tilt", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:dock:press_tilt")
              }

              PanelSectionHeader { text: "SIZE"; foreground: root.foreground }
              Text { textFormat: Text.PlainText; text: "Icon size  " + root.k("plugin:hyprdrive:dock:icon_size", 54); color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                integer: true
                minimum: 24; maximum: 96; step: 1
                value: Model.asNumber(root.k("plugin:hyprdrive:dock:icon_size", 54), 54)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:dock:icon_size", Math.round(v)) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:dock:icon_size", Math.round(v)) }
              }
              Text { textFormat: Text.PlainText; text: "Corner radius  " + root.k("plugin:hyprdrive:dock:corner_radius", 24); color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                integer: true
                minimum: 0; maximum: 40; step: 1
                value: Model.asNumber(root.k("plugin:hyprdrive:dock:corner_radius", 24), 24)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:dock:corner_radius", Math.round(v)) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:dock:corner_radius", Math.round(v)) }
              }
              Text { textFormat: Text.PlainText; text: "Minimize flight  " + root.k("plugin:hyprdrive:dock:minimize_ms", 400) + " ms"; color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                integer: true
                minimum: 0; maximum: 1200; step: 10
                value: Model.asNumber(root.k("plugin:hyprdrive:dock:minimize_ms", 400), 400)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:dock:minimize_ms", Math.round(v)) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:dock:minimize_ms", Math.round(v)) }
              }
              Text { textFormat: Text.PlainText; text: "Autohide slide  " + root.k("plugin:hyprdrive:dock:autohide_ms", 200) + " ms"; color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                integer: true
                minimum: 0; maximum: 800; step: 10
                value: Model.asNumber(root.k("plugin:hyprdrive:dock:autohide_ms", 200), 200)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:dock:autohide_ms", Math.round(v)) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:dock:autohide_ms", Math.round(v)) }
              }
            }

            // -------------------------------------------------------- gestures
            Column {
              width: parent.width
              spacing: Style.space(8)
              visible: root.section === "gestures"

              PanelSectionHeader { text: "TRACKPAD MAP"; foreground: root.foreground }
              Text {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: "This map is live in Hyprland. Swipes, pinches, and 3/4-finger clicks. Plus pinch zoom, move/resize, special workspaces, modifier keys, and arbitrary dispatchers."
                color: root.dim
                font.pixelSize: Style.font.caption
              }

              Repeater {
                model: root.gestures
                Button {
                  required property int index
                  required property var modelData
                  width: paneColumn.width
                  text: Model.gestureLabel(modelData, root.actions)
                  selected: root.gestureIndex === index
                  bordered: true
                  foreground: root.foreground
                  onClicked: root.gestureIndex = index
                }
              }

              Row {
                spacing: Style.space(8)
                Button { text: "Add"; bordered: true; onClicked: root.addGesture() }
                Button { text: "Remove"; bordered: true; onClicked: root.removeGesture() }
                Button { text: "Apply map"; bordered: true; active: true; onClicked: root.saveGestures() }
              }

              PanelSectionHeader { text: "SELECTED GESTURE"; foreground: root.foreground; visible: root.gestures.length > 0 }

              Column {
                width: parent.width
                spacing: Style.space(8)
                visible: root.gestures.length > 0 && root.gestures[root.gestureIndex]

                NumberField {
                  label: "Fingers"
                  value: root.gestures[root.gestureIndex] ? root.gestures[root.gestureIndex].fingers : 3
                  from: 2; to: 5
                  foreground: root.foreground
                  onModified: function(v) { root.patchGesture("fingers", v) }
                }
                Dropdown {
                  width: parent.width
                  label: "Direction"
                  value: root.gestures[root.gestureIndex] ? root.gestures[root.gestureIndex].direction : "up"
                  options: root.directions
                  foreground: root.foreground
                  onChanged: function(v) { root.patchGesture("direction", v) }
                }
                Dropdown {
                  width: parent.width
                  label: "Action"
                  value: root.gestures[root.gestureIndex] ? root.gestures[root.gestureIndex].action : "overview"
                  options: root.actions
                  foreground: root.foreground
                  onChanged: function(v) { root.patchGesture("action", v) }
                }
                TextField {
                  width: parent.width
                  placeholderText: "Require these mods while gesturing (SUPER, ALT, CTRL)"
                  text: root.gestures[root.gestureIndex] ? (root.gestures[root.gestureIndex].mods || "") : ""
                  onEditingFinished: root.patchGesture("mods", text)
                }
                TextField {
                  width: parent.width
                  visible: Model.actionNeedsArg(root.gestures[root.gestureIndex] ? root.gestures[root.gestureIndex].action : "")
                  placeholderText: Model.argPlaceholder(root.gestures[root.gestureIndex] ? root.gestures[root.gestureIndex].action : "")
                  text: root.gestures[root.gestureIndex] ? (root.gestures[root.gestureIndex].arg || "") : ""
                  onEditingFinished: root.patchGesture("arg", text)
                }
                Toggle {
                  width: parent.width
                  label: "Ignore inhibit"
                  description: "Fire even when a fullscreen game or inhibitor is holding gestures."
                  checked: !!(root.gestures[root.gestureIndex] && root.gestures[root.gestureIndex].disable_inhibit)
                  foreground: root.foreground
                  onClicked: root.patchGesture("disable_inhibit", !(root.gestures[root.gestureIndex] && root.gestures[root.gestureIndex].disable_inhibit))
                }
                Text { textFormat: Text.PlainText; text: "Delta scale  " + Number(root.gestures[root.gestureIndex] ? root.gestures[root.gestureIndex].scale : 1).toFixed(2); color: root.dim; font.pixelSize: Style.font.caption }
                PanelSlider {
                  width: parent.width
                  bar: root.bar
                  minimum: 0.2; maximum: 3; step: 0.05
                  value: root.gestures[root.gestureIndex] ? root.gestures[root.gestureIndex].scale : 1
                  onMoved: function(v) { root.patchGesture("scale", v) }
                }
              }
            }

            // -------------------------------------------------------- textures
            Column {
              width: parent.width
              spacing: Style.space(8)
              visible: root.section === "textures"

              PanelSectionHeader { text: "THUMBNAILS"; foreground: root.foreground }
              Text {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: "How live window textures are sampled in the overview and dock previews."
                color: root.dim
                font.pixelSize: Style.font.caption
              }
              Toggle {
                width: parent.width
                label: "Live layer frost"
                description: "Evaluate dock/bar frost against the live backdrop instead of a stale blur buffer."
                checked: Model.asBool(root.k("plugin:hyprdrive:live_layer_frost", true))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:live_layer_frost")
              }
              Toggle {
                width: parent.width
                label: "Smooth frames"
                description: "Let Hyprland render ahead when the GPU runs late, so animations drop fewer frames. Can add up to one frame of latency on late frames."
                checked: Model.asBool(root.k("plugin:hyprdrive:smooth_frames", false))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:smooth_frames")
              }
              Toggle {
                width: parent.width
                label: "Overview wallpaper blur"
                checked: Model.asBool(root.k("plugin:hyprdrive:blur", false))
                foreground: root.foreground
                onClicked: root.toggleKey("plugin:hyprdrive:blur")
              }
              Text { textFormat: Text.PlainText; text: "Mipmap prefilter below  " + Number(root.k("plugin:hyprdrive:prefilter_below", 0.55)).toFixed(2); color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                minimum: 0; maximum: 1; step: 0.05
                value: Model.asNumber(root.k("plugin:hyprdrive:prefilter_below", 0.55), 0.55)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:prefilter_below", v) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:prefilter_below", v) }
              }
              Text { textFormat: Text.PlainText; text: "Dock blur size  " + root.k("plugin:hyprdrive:dock:blur_size", -1) + "  (−1 follows Hyprland)"; color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                integer: true
                minimum: -1; maximum: 32; step: 1
                value: Model.asNumber(root.k("plugin:hyprdrive:dock:blur_size", -1), -1)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:dock:blur_size", Math.round(v)) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:dock:blur_size", Math.round(v)) }
              }
              Text { textFormat: Text.PlainText; text: "Dock blur passes  " + root.k("plugin:hyprdrive:dock:blur_passes", -1) + "  (−1 follows Hyprland)"; color: root.dim; font.pixelSize: Style.font.caption }
              PanelSlider {
                width: parent.width
                bar: root.bar
                integer: true
                minimum: -1; maximum: 8; step: 1
                value: Model.asNumber(root.k("plugin:hyprdrive:dock:blur_passes", -1), -1)
                onMoved: function(v) { root.previewKey("plugin:hyprdrive:dock:blur_passes", Math.round(v)) }
                onReleased: function(v) { root.setKey("plugin:hyprdrive:dock:blur_passes", Math.round(v)) }
              }
            }

            // ----------------------------------------------------------- power
            Column {
              width: parent.width
              spacing: Style.space(8)
              visible: root.section === "power"

              PanelSectionHeader { text: "PROFILES"; foreground: root.foreground }
              Text {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: "Save the current panel values as the AC or battery profile. Auto applies the matching one when the charger is plugged or unplugged."
                color: root.dim
                font.pixelSize: Style.font.caption
              }
              Text {
                textFormat: Text.PlainText
                text: root.onBattery ? "Currently discharging" : "Currently on AC"
                color: root.foreground
                font.bold: true
              }
              Row {
                spacing: Style.space(8)
                Button {
                  text: "Auto"
                  selected: root.profileMode === "auto"
                  bordered: true
                  onClicked: root.runCtl(["profile-mode", "auto"])
                }
                Button {
                  text: "Force AC"
                  selected: root.profileMode === "ac"
                  bordered: true
                  onClicked: root.runCtl(["profile-mode", "ac"])
                }
                Button {
                  text: "Force battery"
                  selected: root.profileMode === "battery"
                  bordered: true
                  onClicked: root.runCtl(["profile-mode", "battery"])
                }
              }
              Row {
                spacing: Style.space(8)
                Button { text: "Save as AC"; bordered: true; onClicked: root.runCtl(["profile-save", "ac"]) }
                Button { text: "Save as battery"; bordered: true; onClicked: root.runCtl(["profile-save", "battery"]) }
                Button { text: "Apply now"; bordered: true; onClicked: root.runCtl(["profile-apply", "auto"]) }
              }
              Text {
                width: parent.width
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: "Typical battery profile: compositor animations off, Hyprdrive motion off, dock flights instant. Save that while unplugged, then restore a full-motion AC profile while charging."
                color: root.dim
                font.pixelSize: Style.font.caption
              }
            }
          }
        }
      }
    }
  }
}

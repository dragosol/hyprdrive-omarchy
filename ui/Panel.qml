import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Io
import Quickshell.Services.UPower
import qs.Commons
import qs.Ui

// The bar button, and nothing else that lives while the panel is closed.
//
// "activation": "on-demand" in the manifest is not read by the shell (a
// bar-widget's entry QML lives as long as it sits in the bar, one copy per
// monitor), so the settings panel -- window, content tree, state, the
// processes that talk to hdpanel -- is HyprdrivePanel.qml behind a Loader that
// is active only while the panel is open and for its closing animation. Closed,
// all of it is destroyed; what stays is this icon, the IPC target and the
// battery-profile watcher.
Panel {
  id: root
  moduleName: "dragos.hyprdrive"
  ipcTarget: "dragos.hyprdrive"
  manageIpc: false

  readonly property bool onBattery: UPower.onBattery === true
  // Kept loaded after a close long enough for the exit animation to finish;
  // KeyboardPanel's card fade is 140 ms, the content's own exit a little less.
  property bool lingering: false
  // Set by the `icons` IPC call; HyprdrivePanel takes it (and clears it).
  property string pendingIconApp: ""

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onOpenedChanged: {
    if (opened) {
      lingerTimer.stop()
      lingering = true
    } else {
      lingerTimer.restart()
    }
  }

  Timer {
    id: lingerTimer
    interval: 260
    onTriggered: if (!root.opened) root.lingering = false
  }

  // Battery / AC profile auto-apply works with the panel closed. A flaky USB-C
  // source flips onBattery several times a second, and every applied profile
  // rewrites 40-panel.lua (a full Hyprland reload): apply once the power state
  // has held still. hdpanel reads the settled state and the mode itself, and
  // does nothing when the mode is forced or no profile is saved.
  onOnBatteryChanged: profileSettle.restart()

  Timer {
    id: profileSettle
    interval: 4000
    onTriggered: Quickshell.execDetached(["bash", "-lc",
      'c="$HOME/.config/omarchy/plugins/dragos.hyprdrive/bin/hdpanel"; ' +
      '[ -x "$c" ] || c="$HOME/.local/lib/hyprdrive/hdpanel"; ' +
      '[ -x "$c" ] && exec "$c" profile-auto'])
  }

  IpcHandler {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function show(): void { root.open() }
    function hide(): void { root.close() }
    function toggle(): void { root.toggle() }
    // The dock's Get Info -> App Icon: open on Icons with this app chosen.
    function icons(appId: string): void { root.pendingIconApp = appId; root.open() }
  }

  // The bar icon is hyprdrive's own mark rather than a font glyph (user,
  // 2026-09-12). It ships as an alpha mask and is TINTED to the bar's colour
  // the way the tray tints symbolic icons, so light and dark themes and the
  // opened state all behave as the glyph they replace did.
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

  Loader {
    id: panelLoader
    active: root.opened || root.lingering
    source: Qt.resolvedUrl("HyprdrivePanel.qml")
    visible: false
    onLoaded: {
      item.host = root
      item.anchorItem = button
    }
  }
}

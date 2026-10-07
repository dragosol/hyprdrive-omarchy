.pragma library

// THE PANEL, AS DATA. Every section, card and row the Hyprdrive panel draws,
// in the order it draws them. HyprdrivePanel.qml renders this and nothing
// else, so re-arranging the panel is editing this file; search indexes it.
//
// A row's `type` picks its control:
//   toggle   -- a switch                        (key)
//   slider   -- a value on a track              (key, min, max, step, unit, fmt)
//   choice   -- a segmented control / dropdown  (key, options from hdpanel meta)
//   curve    -- the animation-curve picker      (key)
//   speed    -- anim_scale shown as a speed     (key)
//   motion   -- Hyprdrive motion on/off (anim_scale 0 <-> remembered value)
//   blur     -- one motion-blur site: strength (0..1 in 1/8ths) + fade (site, clock)
//   gestures -- the trackpad map editor
//   power    -- AC / battery profiles
//
// `when` names a condition (HyprdrivePanel.qml, rowEnabled) under which the
// row applies; otherwise it is drawn dimmed and does not take input.
//
// `group` on a section is the hdpanel group its "Restore defaults" resets.

var P = "plugin:hyprdrive:"

var sections = [
  {
    id: "overview", title: "Overview", icon: "", group: "overview",
    subtitle: "Mission Control, the desktop strip and search.",
    cards: [
      { title: "Opening", rows: [
        { type: "toggle", key: P + "tap_super", label: "Tap Super to open",
          desc: "A quick Super tap, with no other key, toggles the overview." },
        { type: "toggle", key: P + "mission_control", label: "Mission Control grid",
          desc: "Lay the current desktop out as a grid. Off shows scrolling columns." },
        { type: "toggle", key: P + "strip", label: "Desktop strip",
          desc: "Live thumbnails of every desktop along the top." }
      ]},
      { title: "Layout", rows: [
        { type: "slider", key: P + "scale", label: "Overview size", min: 0.4, max: 0.9, step: 0.01, fmt: "percent",
          desc: "How large the desktop cards are, as a share of the screen." },
        { type: "slider", key: P + "mission_control_gap", label: "Grid spacing", min: 0, max: 120, step: 2, unit: "px",
          desc: "Room between windows in the grid." }
      ]},
      { title: "Backdrop", rows: [
        { type: "toggle", key: P + "blur", label: "Blur the wallpaper",
          desc: "Frost the wallpaper behind the cards, like the lock screen." }
      ]},
      { title: "Search", rows: [
        { type: "toggle", key: P + "search:dictionary", label: "Dictionary",
          desc: "Type “define” and a word to look it up on Wiktionary." },
        { type: "toggle", key: P + "search:translate", label: "Translate",
          desc: "Type “tr” and some text to translate it (needs a LibreTranslate endpoint)." }
      ]}
    ]
  },
  {
    id: "dock", title: "Dock", icon: "", group: "dock",
    subtitle: "What the dock shows, how it hides and how it looks.",
    cards: [
      { title: "Visibility", rows: [
        { type: "toggle", key: P + "dock:enabled", label: "Show the dock" },
        { type: "toggle", key: P + "dock:autohide", label: "Autohide", when: "dockOn",
          desc: "Stay out of sight until the pointer presses the bottom edge." },
        { type: "toggle", key: P + "dock:windows_make_room", label: "Windows make room", when: "roomApplies",
          desc: "With autohide off, tiled windows under the dock shorten to clear it." },
        { type: "choice", key: P + "dock:monitor", label: "Show on", when: "dockOn",
          desc: "Put the dock on one screen only. While that screen is unplugged, the dock is on every screen." }
      ]},
      { title: "Contents", rows: [
        { type: "toggle", key: P + "dock:show_favorites", label: "Pinned favorites" },
        { type: "toggle", key: P + "dock:show_apps_button", label: "Applications button" },
        { type: "toggle", key: P + "dock:show_trash", label: "Trash" },
        { type: "toggle", key: P + "dock:show_mounts", label: "Mounted drives" },
        { type: "toggle", key: P + "dock:running_indicator", label: "Running indicators" },
        { type: "toggle", key: P + "dock:notification_counter", label: "Notification badges" },
        { type: "toggle", key: P + "dock:isolate_in_overview", label: "Current desktop only in the overview", when: "isolateOvApplies",
          desc: "In the overview, the dock lists only the apps and windows on the desktop you are viewing." },
        { type: "toggle", key: P + "dock:isolate_workspaces", label: "Current desktop only, always",
          desc: "The dock lists only the current desktop's apps and windows, outside the overview too." }
      ]},
      { title: "Appearance", rows: [
        { type: "slider", key: P + "dock:icon_size", label: "Icon size", min: 24, max: 96, step: 1, unit: "px" },
        { type: "slider", key: P + "dock:corner_radius", label: "Corner radius", min: 0, max: 40, step: 1, unit: "px",
          desc: "0 is a square-cornered pill." },
        { type: "toggle", key: P + "dock:hover_tilt", label: "Hover tilt",
          desc: "Icons lean toward the pointer in 3D, and spring flat when it leaves." },
        { type: "toggle", key: P + "dock:press_tilt", label: "Press tilt",
          desc: "A pressed icon leans in toward the click, with light on its face." },
        { type: "toggle", key: P + "dock:dance_urgent", label: "Wiggle urgent apps" }
      ]},
      { title: "Frost", rows: [
        { type: "slider", key: P + "dock:blur_size", label: "Blur size", min: -1, max: 32, step: 1, fmt: "follow",
          desc: "Leftmost follows Hyprland's decoration blur." },
        { type: "slider", key: P + "dock:blur_passes", label: "Blur passes", min: -1, max: 8, step: 1, fmt: "follow" }
      ]}
    ]
  },
  {
    id: "animation", title: "Animation", icon: "", group: "animation",
    subtitle: "One curve and one speed for everything hyprdrive moves.",
    cards: [
      { title: "Motion", rows: [
        { type: "motion", key: P + "anim_scale", label: "Hyprdrive motion",
          desc: "Off snaps the overview, flights and hover eases into place." },
        { type: "toggle", key: "animations:enabled", label: "Compositor animations",
          desc: "Hyprland's own window and workspace animations." }
      ]},
      { title: "Curve", rows: [
        { type: "curve", key: P + "animation:curve", label: "Animation curve", when: "motionOn",
          desc: "Shapes every hyprdrive animation except the dock stacks." }
      ]},
      { title: "Speed", rows: [
        { type: "speed", key: P + "anim_scale", label: "Speed", when: "motionOn",
          desc: "Scales every hyprdrive animation at once." }
      ]},
      { title: "Timing", rows: [
        { type: "slider", key: P + "open_speed", label: "Overview open", min: 0.5, max: 8, step: 0.1, fmt: "deciseconds" },
        { type: "slider", key: P + "close_speed", label: "Overview close", min: 0.5, max: 8, step: 0.1, fmt: "deciseconds" },
        { type: "slider", key: P + "dock:minimize_ms", label: "Minimize flight", min: 0, max: 1200, step: 10, unit: "ms" },
        { type: "slider", key: P + "dock:autohide_ms", label: "Dock autohide slide", min: 0, max: 800, step: 10, unit: "ms" },
        { type: "slider", key: P + "mission_control_reflow_ms", label: "Grid reflow", min: 0, max: 1000, step: 10, unit: "ms",
          desc: "Tiles gliding to new slots as a search filters them." }
      ]}
    ]
  },
  {
    id: "blur", title: "Motion blur", icon: "", group: "blur",
    subtitle: "How much each animation smears, and how the smear fades.",
    cards: [
      { title: "Motion blur", rows: [
        { type: "toggle", key: P + "motion_blur:enabled", label: "Motion blur",
          desc: "Smear fast motion the way a camera shutter does." }
      ]},
      { title: "Windows", rows: [
        { type: "blur", when: "blurOn", site: "minimize", label: "Minimize & restore", clock: true },
        { type: "blur", when: "blurOn", site: "layout", label: "Retile & open", clock: true,
          desc: "Dwindle and scrolling layouts rearranging." },
        { type: "blur", when: "blurOn", site: "flick", label: "Scrolling flick", clock: true,
          desc: "Only fast flicks of the scrolling tape." },
        { type: "blur", when: "blurOn", site: "scratchpad", label: "Scratchpad slide", clock: true }
      ]},
      { title: "Desktops", rows: [
        { type: "blur", when: "blurOn", site: "swipe", label: "Trackpad swipe", clock: false },
        { type: "blur", when: "blurOn", site: "desktop_switch", label: "Keyboard switch", clock: false,
          desc: "Switching desktops by keyboard in the overview." }
      ]},
      { title: "Overview", rows: [
        { type: "blur", when: "blurOn", site: "overview", label: "Open & close", clock: true,
          desc: "When it plays from a keypress (a swipe follows the fingers)." },
        { type: "blur", when: "blurOn", site: "search_pane", label: "Search results pane", clock: true },
        { type: "blur", when: "blurOn", site: "search_tiles", label: "Search tile motion", clock: false },
        { type: "blur", when: "blurOn", site: "drag", label: "Drag resize", clock: false }
      ]},
      { title: "Dock", rows: [
        { type: "blur", when: "blurOn", site: "stack_pane", label: "Stack grid pane", clock: true }
      ]},
      { title: "Quality", rows: [
        { type: "slider", key: P + "layout_motion_blur", label: "Layout & overview samples", min: 0, max: 32, step: 1, fmt: "samples", when: "blurOn",
          desc: "More samples, smoother smear, more GPU." },
        { type: "slider", key: P + "dock:minimize_motion_blur", label: "Flight samples", min: 0, max: 32, step: 1, fmt: "samples" },
        { type: "slider", key: P + "dock:stack_motion_blur", label: "Stack fan samples", min: 0, max: 32, step: 1, fmt: "samples" }
      ]}
    ]
  },
  {
    id: "gestures", title: "Gestures", icon: "", group: "gestures",
    subtitle: "Swipes, pinches and clicks on the trackpad.",
    cards: [
      { title: "Trackpad map", rows: [ { type: "gestures", label: "Gestures" } ] }
    ]
  },
  {
    id: "performance", title: "Performance", icon: "", group: "performance",
    subtitle: "Frame pacing, texture quality and power profiles.",
    cards: [
      { title: "Rendering", rows: [
        { type: "toggle", key: P + "smooth_frames", label: "Smooth frames",
          desc: "Let Hyprland render ahead when the GPU runs late, so a late frame costs one refresh instead of half the frame rate." },
        { type: "toggle", key: P + "live_layer_frost", label: "Live frost",
          desc: "Frost the dock and bar against the live backdrop, not a cached blur." },
        { type: "slider", key: P + "prefilter_below", label: "Preview smoothing", min: 0, max: 1, step: 0.05, fmt: "fraction",
          desc: "Smooth-shrink window previews drawn smaller than this share of their size." }
      ]},
      { title: "Power profiles", rows: [ { type: "power", label: "Power profiles" } ] }
    ]
  }
]

var links = [
  { label: "GitHub", detail: "github.com/dragosol", icon: "", url: "https://github.com/dragosol" },
  { label: "Website", detail: "dragos.shastasia.com", icon: "", url: "https://dragos.shastasia.com" },
  { label: "YouTube", detail: "@dragos-the-dragon", icon: "", url: "https://www.youtube.com/@dragos-the-dragon" },
  { label: "LinkedIn", detail: "linkedin.com/in/dragos-robu", icon: "", url: "https://www.linkedin.com/in/dragos-robu/" }
]

// The curve presets, as CSS cubic-bezier control points (Motion.cpp owns the
// numbers; these draw the previews). Only decel is shown: it is the curve
// most of hyprdrive's motion travels on.
var curves = [
  { value: "hyprdrive", label: "Hyprdrive", note: "The house curves", decel: [0.16, 1, 0.3, 1] },
  // Serpantinum's family (ilyamiro/serpantinum): each with its InCubic exit and OutBack pop.
  { value: "serpantinum", label: "Serpantinum \u00b7 Windows", note: "myBezier \u2014 its Hyprland windows", decel: [0.05, 0.9, 0.1, 1.05] },
  { value: "serpantinum_glide", label: "Serpantinum \u00b7 Glide", note: "OutQuint \u2014 long travel", decel: [0.22, 1.09, 0.46, 1] },
  { value: "serpantinum_soft", label: "Serpantinum \u00b7 Soft", note: "OutCubic \u2014 panels and widgets", decel: [0.33, 1, 0.66, 1] },
  { value: "serpantinum_intro", label: "Serpantinum \u00b7 Intro", note: "OutExpo \u2014 popups arriving", decel: [0.15, 1.04, 0.35, 0.99] },
  { value: "serpantinum_pop", label: "Serpantinum \u00b7 Pop", note: "OutBack 1.2 \u2014 things popping in", decel: [0.33, 1.39, 0.66, 1] },
  { value: "swift", label: "Swift", note: "Quick off the mark", decel: [0, 0.55, 0.45, 1] },
  { value: "springy", label: "Springy", note: "A little overshoot", decel: [0.34, 1.3, 0.64, 1] },
  { value: "linear", label: "Linear", note: "Constant speed", decel: [0, 0, 1, 1] }
]

var fades = [
  { value: "early", label: "Early", note: "Gone by halfway" },
  { value: "full", label: "Full", note: "Fades the whole way" },
  { value: "late", label: "Late", note: "Holds, then fades" },
  { value: "linear", label: "Linear", note: "Even fade" },
  { value: "constant", label: "Constant", note: "Never fades" }
]

function sectionById(id) {
  for (var i = 0; i < sections.length; i++)
    if (sections[i].id === id) return sections[i]
  return sections[0]
}

function curveByValue(v) {
  for (var i = 0; i < curves.length; i++)
    if (curves[i].value === v) return curves[i]
  return curves[0]
}

// Rows matching a query, grouped as cards titled by their section, for the
// search results page. Matches label, description and card title.
function search(query) {
  var q = String(query || "").toLowerCase().trim()
  if (q === "") return []
  var out = []
  for (var s = 0; s < sections.length; s++) {
    var sec = sections[s]
    var rows = []
    for (var c = 0; c < sec.cards.length; c++) {
      var card = sec.cards[c]
      for (var r = 0; r < card.rows.length; r++) {
        var row = card.rows[r]
        var hay = (row.label + " " + (row.desc || "") + " " + card.title + " " + sec.title).toLowerCase()
        if (hay.indexOf(q) >= 0) rows.push(row)
      }
    }
    if (rows.length) out.push({ title: sec.title, section: sec.id, rows: rows })
  }
  return out
}

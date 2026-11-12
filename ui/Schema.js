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
//   icons    -- per-app dock icons (bin/hdicons)
//   keybind  -- one hyprdrive shortcut: its live chord (hdpanel dump's
//               `keybinds`, read from hyprctl binds), an on/off -- a plugin
//               `key`, or one of hdpanel's own `bind` switches -- and, with
//               `modsKey`, a modifier picker (its last entry, Custom..., captures
//               the modifiers held while the wheel turns). `chordEdit`: the
//               chord chip is a button -- click it, press the new keys
//               (hdpanel bind-set ID CHORD); rows without it are fixed
//
// `when` names a condition (HyprdrivePanel.qml, rowEnabled) under which the
// row applies; otherwise it is drawn dimmed and does not take input.
//
// `group` on a section is the hdpanel group its "Restore defaults" resets.

var P = "plugin:hyprdrive:"

var sections = [
  {
    id: "general", title: "General", icon: "\uf013", group: "general",
    subtitle: "What Hyprdrive opens things with.",
    cards: [
      { title: "Fullscreen", rows: [
        { type: "choice", key: P + "general:fullscreen_desktop", label: "Fullscreen apps",
          desc: "Its own desktop: a fullscreen app gets the desktop right after yours, the others move along a place, and it comes back when you leave fullscreen. An app alone on its desktop keeps it." }
      ]},
      { title: "Windows", rows: [
        { type: "toggle", key: P + "general:open_tiled", label: "Windows float only when you float them",
          desc: "A window an app opens floating, such as a dialog or a fixed-size window (Wine and Proton windows included), is tiled instead, on every desktop. Mosaic desktops always do this, with the option on or off. Float rules in your config still float a window, and so do splash screens, tool palettes and the hyprdrive-float tag. A window you float yourself stays floating." }
      ]},
      { title: "Apps", rows: [
        { type: "choice", dropdown: true, key: P + "general:file_manager", label: "File manager",
          desc: "Where the dock's folders and stacks, Open in Files and Show in Files open. System default follows your desktop's default for folders." }
      ]}
    ]
  },
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
          desc: "Live thumbnails of every desktop along the top." },
        { type: "toggle", key: P + "strip_cleanup_button", label: "Clean Up button", when: "stripOn",
          desc: "Hold Super over the desktop strip and a Clean Up Empty Desktops button appears under it: the desktops with windows close ranks, the empty ones between them go." },
        { type: "toggle", key: P + "hot_corner:enabled", label: "Hot corner",
          desc: "Rest the pointer in a screen corner to open the overview, and again to close it, without touching the keyboard." },
        { type: "choice", key: P + "hot_corner:corner", label: "Corner", when: "hotCornerOn",
          desc: "Only a corner at the outside edge of your screens counts. The top left is also Omarchy's menu button: a click there still opens the menu." },
        { type: "slider", key: P + "hot_corner:delay_ms", label: "Rest time", min: 0, max: 1000, step: 50, unit: "ms", when: "hotCornerOn",
          desc: "How long the pointer rests in the corner before the overview opens." },
        { type: "toggle", key: P + "hot_corner:ripple", label: "Ripple", when: "hotCornerOn",
          desc: "A ripple runs out from the corner across the screen, bending what is under it like water." }
      ]},
      { title: "Rearranging", rows: [
        { type: "toggle", key: P + "drag:hold_to_swap", label: "Hold to swap",
          desc: "Drag a window over another one, in the grid or the desktop strip, and hold it there. Both dim and Swap appears: let go and they trade places." },
        { type: "slider", key: P + "drag:hold_to_swap_ms", label: "Hold time", min: 500, max: 4000, step: 100, unit: "ms", when: "swapOn",
          desc: "How long to hold before letting go swaps the two." }
      ]},
      { title: "Multiple monitors", rows: [
        { type: "toggle", key: P + "focus_follows_pick", label: "Focus stays where you pick",
          desc: "Picking a desktop or window in one monitor's overview keeps focus and the pointer there, instead of jumping to the window another monitor's overview had selected." }
      ]},
      { title: "Layout", rows: [
        { type: "slider", key: P + "scale", label: "Overview size", min: 0.4, max: 0.9, step: 0.01, fmt: "percent",
          desc: "How large the desktop cards are, as a share of the screen." },
        { type: "slider", key: P + "mission_control_gap", label: "Grid spacing", min: 0, max: 120, step: 2, unit: "px",
          desc: "Room between windows in the grid." },
        { type: "choice", key: P + "overview_tile_order", label: "Window order",
          desc: "How the windows are ordered in the grid, from the top left. Last used puts the window you used most recently first; Order opened, the oldest first; Match desktop position lays them out as they sit on the desktop. Taken as the overview opens, so nothing moves while it is up." }
      ]},
      { title: "Backdrop", rows: [
        { type: "choice", key: P + "backdrop", label: "Behind the cards",
          desc: "What is blurred and darkened behind the desktop cards. Snapshot freezes a live wallpaper as the overview opens; Live keeps blurring it as it moves (more GPU); Auto is Live while it changes rarely, Snapshot while it animates. The cards always show the live desktop." }
      ]},
      { title: "Search", rows: [
        { type: "toggle", key: P + "search:browse_on_click", label: "Browse on click",
          desc: "Clicking the search bar opens the results before you type: the menu, your folders and the AI tools." },
        { type: "toggle", key: P + "search:dictionary", label: "Dictionary",
          desc: "Type “define” and a word to look it up on Wiktionary." },
        { type: "toggle", key: P + "search:translate", label: "Translate",
          desc: "Type “tr” and some text to translate it (the free MyMemory service, or your own LibreTranslate server)." }
      ]},
      { title: "Easter egg", rows: [
        { type: "toggle", key: P + "search:easter_egg", label: "Easter egg",
          desc: "Indecision has its rewards." }
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
        { type: "toggle", key: P + "dock:folders_anywhere", label: "Folders anywhere",
          desc: "Off, your folders sit together after the apps. On, drag a folder anywhere among the apps. Drives, the Trash and Show Overview stay at the end either way." },
        { type: "toggle", key: P + "dock:show_trash", label: "Trash" },
        { type: "toggle", key: P + "dock:show_mounts", label: "Mounted drives" },
        { type: "toggle", key: P + "dock:show_mounts_network", label: "Network drives", when: "mountsOn",
          desc: "Shares on other machines: SMB, SFTP, NFS and WebDAV, including the ones you connect to in Files." },
        { type: "choice", key: P + "dock:mount_sort", label: "Drive order", when: "mountsOn",
          desc: "By name sorts alphabetically with numbers as numbers. Smart also reads spelled-out and Roman numbers, so KleThree comes after KleTwo. As in fstab follows /etc/fstab's order; drives it does not list come after." },
        { type: "toggle", key: P + "dock:running_indicator", label: "Running indicators" },
        { type: "toggle", key: P + "dock:notification_counter", label: "Notification badges" },
        { type: "toggle", key: P + "dock:isolate_in_overview", label: "Current desktop only in the overview", when: "isolateOvApplies",
          desc: "In the overview, the dock lists only the apps and windows on the desktop you are viewing." },
        { type: "toggle", key: P + "dock:isolate_workspaces", label: "Current desktop only, always",
          desc: "The dock lists only the current desktop's apps and windows, outside the overview too." }
      ]},
      { title: "Clicking", rows: [
        { type: "toggle", key: P + "dock:pointer_to_window", label: "Pointer follows to the window",
          desc: "Clicking a window preview, or Ctrl+clicking an app, moves the pointer to the middle of the window it brings up. A plain click on an app leaves the pointer on the dock, so a second click can minimize it again." }
      ]},
      { title: "Drag and drop", rows: [
        { type: "toggle", key: P + "drag:spring_dock", label: "Spring-loaded apps",
          desc: "Hold a dragged file over a running app or one of its previews: the icon flashes twice and the app comes forward for the drop." },
        { type: "toggle", key: P + "drag:spring_edges", label: "Switch desktops at the edges",
          desc: "Hold a dragged file, folder or window at the left or right edge to move to the next desktop. Keep holding to keep going." },
        { type: "slider", key: P + "drag:spring_delay_ms", label: "Hold time", min: 500, max: 4000, step: 100, unit: "ms", when: "springOn",
          desc: "How long a drag waits over an app or at an edge before it springs." }
      ]},
      { title: "Appearance", rows: [
        { type: "slider", key: P + "dock:icon_size", label: "Icon size", min: 24, max: 96, step: 1, unit: "px" },
        { type: "slider", key: P + "dock:corner_radius", label: "Corner radius", min: 0, max: 40, step: 1, unit: "px",
          desc: "0 is a square-cornered pill." },
        { type: "toggle", key: P + "dock:hover_tilt", label: "Hover tilt",
          desc: "Icons lean toward the pointer in 3D, and spring flat when it leaves." },
        { type: "toggle", key: P + "dock:press_tilt", label: "Press tilt",
          desc: "A pressed icon leans in toward the click, with light on its face." },
        { type: "toggle", key: P + "dock:dance_urgent", label: "Wiggle urgent apps" },
        { type: "toggle", key: P + "dock:menu_animation", label: "Menu expands",
          desc: "The right-click menu grows out of its icon and folds back into it, like the search results." },
        { type: "slider", key: P + "dock:menu_anim_ms", label: "Menu expand time", min: 0, max: 800, step: 10, unit: "ms", when: "dockMenuAnimOn",
          desc: "Closing takes about a quarter less. Scaled by the Animation speed." }
      ]},
      { title: "Frost", rows: [
        { type: "slider", key: P + "dock:blur_size", label: "Blur size", min: -1, max: 32, step: 1, fmt: "follow",
          desc: "Leftmost follows Hyprland's decoration blur." },
        { type: "slider", key: P + "dock:blur_passes", label: "Blur passes", min: -1, max: 8, step: 1, fmt: "follow" }
      ]}
    ]
  },
  {
    id: "icons", title: "Icons", icon: "\uf03e", group: "icons",
    subtitle: "Give any app or folder on the dock a different icon.",
    cards: [
      { title: "Dock icons", rows: [
        { type: "icons", label: "Change an app's icon",
          desc: "Pick an app, or one of the Trash's four states (empty or full, on a dark or light dock), then a new icon from your icon theme, your own folders or selfh.st. Reset puts the original back." }
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
      { title: "Desktops", rows: [
        { type: "toggle", key: "animations:workspaces", label: "Slide between desktops", when: "compositorAnimOn",
          desc: "Hyprland's own desktop-change animation. Omarchy ships it off; the two switches below only act while it is on." },
        { type: "toggle", key: P + "animation:desktop_keyboard", label: "Animate desktop changes from keys", when: "desktopSlideOn",
          desc: "Super+1…0, Super+Tab, Super+scroll and moving a window to a desktop slide there. Off, they switch at once." },
        { type: "toggle", key: P + "animation:desktop_focus", label: "Animate desktop changes when focus moves", when: "desktopSlideOn",
          desc: "An app, a dock icon or a notification bringing up a window on another desktop slides there. Off, it switches at once." },
        { type: "toggle", key: P + "animation:desktop_row_bounce", label: "Bounce at the first and last desktop", when: "desktopSlideOn",
          desc: "Scrolling past either end of the row stretches it a little and springs back, so you can feel where it stops." },
        { type: "toggle", key: P + "animation:desktop_jump_through", label: "Slide through desktops on a jump", when: "desktopSlideOn",
          desc: "A key or dock jump of several desktops, like Super+5 from desktop 1, slides past the desktops between instead of straight to the new one." }
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
      ]},
      // Mosaic's infinite canvas (Mosaic.cpp canvasEase): its own curve, so a
      // pan can glide on Serpantinum whatever the rest of hyprdrive uses.
      { title: "Infinite canvas", rows: [
        { type: "curve", key: P + "animation:canvas_curve", label: "Canvas pan curve", when: "canvasMotionOn",
          desc: "How the canvas glides when focus brings a window into view, and on the Super zoom." },
        { type: "slider", key: P + "animation:canvas_pan_ms", label: "Canvas pan", min: 0, max: 2000, step: 10, unit: "ms", when: "canvasMotionOn",
          desc: "0 jumps straight there." }
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
        { type: "blur", when: "blurOn", site: "desktop_slide", label: "Desktop switch", clock: true,
          desc: "Switching desktops outside the overview: a key, Super+scroll, a dock click." },
        { type: "blur", when: "blurOn", site: "desktop_switch", label: "Keyboard switch", clock: false,
          desc: "Switching desktops by keyboard in the overview." },
        { type: "blur", when: "blurOn", site: "wheel_pan", label: "Wheel switch", clock: false,
          desc: "Switching desktops with the mouse wheel or Super+scroll in the overview." }
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
      { title: "Fingers", rows: [
        { type: "toggle", key: P + "gestures:keep_swipe", label: "Lift fingers mid-swipe",
          desc: "A swipe keeps going while any finger that started it is still down." },
        { type: "toggle", key: P + "gestures:swipe_while_dragging", label: "Swipe while dragging",
          desc: "Hold a drag with one finger and swipe with three or four others to switch desktops. A dragged window comes along." }
      ]},
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
  },
  {
    // Last, so Alt+1..6 keep reaching the sections they always have.
    // `experimental`: an accent "Experimental" pill beside the title, in the
    // nav and on the page -- it works, and it is still finding its shape.
    id: "mosaic", title: "Mosaic", icon: "", group: "mosaic", experimental: true,
    subtitle: "Windows at their own size, centred. Best on large displays.",
    cards: [
      { title: "Desktop mode", rows: [
        // `confirm`: switching it on changes Super+L, so the panel asks first.
        { type: "toggle", key: P + "mosaic:enabled", label: "Mosaic",
          desc: "Windows keep their natural size and gather in the middle of the desktop. When they no longer fit, the desktop tiles them, and goes back to the mosaic once they fit again. Drag a window's corner to resize it. Super+L cycles Dwindle → Scrolling → Mosaic. Recommended for large displays: on a laptop screen only a couple of windows fit before it tiles.",
          confirm: {
            title: "Add Mosaic as a third desktop mode?",
            body: "Super+L will cycle Dwindle → Scrolling → Mosaic instead of switching between two. Turning Mosaic off puts Super+L back the way it was.",
            allow: "Allow", glyph: ""
          } },
        { type: "toggle", key: P + "mosaic:default", label: "Make Mosaic the default", when: "mosaicOn",
          desc: "New desktops, and any you have not set with Super+L, start in Mosaic. Off restores the previous default." },
        { type: "toggle", key: P + "mosaic:spawn_at_cursor", label: "Open windows at the pointer", when: "mosaicOn",
          desc: "A new window lands where the pointer is, and the others make room around it. Off, new windows join the cluster wherever it packs them." },
        { type: "toggle", key: P + "mosaic:quiet_new_windows", label: "Windows you didn't open stay quiet", when: "mosaicOn",
          desc: "A loading screen or an app reopening itself opens without taking your focus, and your pointer stays on your window. Windows you launch (keys, the dock, a launcher) still come forward." },
        { type: "toggle", key: P + "mosaic:infinite_canvas", label: "Infinite canvas", when: "mosaicOn",
          desc: "When windows no longer fit the screen, they keep their size and spill onto a canvas you pan across, instead of tiling. Hold Super and move the pointer to see every window on it." },
        { type: "choice", key: P + "mosaic:canvas_directions", label: "Canvas directions", when: "canvasOn",
          desc: "Horizontal: one strip of windows. All directions: a grid of them." },
        { type: "toggle", key: P + "mosaic:edge_push", label: "Push past the edge", when: "canvasOn",
          desc: "Keep pushing the pointer against a screen edge to slide the canvas toward the windows hidden past it. Moving back onto your window brings it fully into view again." },
        { type: "slider", key: P + "mosaic:edge_push_resistance", label: "Edge resistance", min: 50, max: 1500, step: 50, unit: "px", when: "edgePushOn",
          desc: "How far you have to keep pushing before the canvas gives way. Higher, and a light push only shows the dock or bar. The bottom edge, where the dock is, takes half again as much." },
        { type: "note", label: "Hold Super and move the pointer", when: "canvasOn",
          desc: "Every window on the canvas zooms out onto the screen. Hover one to focus it, then let go of Super to zoom back in on it. Drag a window while zoomed out to move it; hold it at the screen's edge to take it to the next desktop. A Super+click or Super+drag does not zoom." },
        { type: "choice", key: P + "mosaic:zoom_labels", label: "Window titles while holding Super",
          desc: "Hold Super and move the pointer, on any desktop: no titles, the window under the pointer, or every window." }
      ]}
    ]
  },
  {
    // After Mosaic, so Alt+1..8 keep their sections; this one is Alt+9.
    id: "keybinds", title: "Keybinds", icon: "\uf11c", group: "keybinds",
    subtitle: "Hyprdrive's shortcuts, as they are bound right now. Click a key with a pencil to change it.",
    cards: [
      { title: "Overview and windows", rows: [
        { type: "keybind", id: "overview", key: P + "tap_super", label: "Overview",
          desc: "Tap Super on its own, with no other key, to open or close the overview." },
        { type: "keybind", id: "minimize", bind: "minimize", label: "Minimize", chordEdit: true,
          desc: "Minimizes the focused window into the dock. Takes its key (Super+H unless you change it) from anything else bound to it." },
        { type: "keybind", id: "show_desktop", bind: "show_desktop", label: "Show desktop", chordEdit: true,
          desc: "Slides every window on the current desktop off the screen, each by its nearest edge, so the desktop shows. Press it again at any point, even halfway, and they turn round and come back to their places; so does switching desktops, opening the overview or a new window, or picking one of them in the dock. Takes its key (Super+D unless you change it) from anything else bound to it." }
      ]},
      { title: "Desktops", rows: [
        { type: "keybind", id: "scroll_desktops", key: P + "binds:scroll_desktops", modsKey: P + "binds:scroll_desktops_mods", label: "Scroll through desktops",
          desc: "Hold the modifier and turn the wheel: the row of desktops slides with it, notch after notch, and turns round the moment you scroll back. Replaces Omarchy's Super+scroll." },
        { type: "keybind", id: "dock_hotkeys", key: P + "dock:hot_keys", label: "Dock hotkeys",
          desc: "Super+1…0 open or focus the dock's first ten apps instead of switching to desktops 1…10.",
          confirm: {
            title: "Use Super+1…0 for the dock?",
            body: "Super+1…0 will open the dock's apps instead of switching desktops. Turning this off gives them back to desktop switching.",
            allow: "Allow", glyph: "\uf11c"
          } }
      ]},
      { title: "Scrolling through desktops", rows: [
        { type: "toggle", key: P + "binds:scroll_desktops_smooth", label: "Smooth scrolling", when: "scrollDesktopsOn",
          desc: "The desktops follow the wheel continuously, two notches to a desktop, and settle on the nearest one when you stop. Off, they move a whole desktop at a time." },
        { type: "slider", key: P + "binds:scroll_desktops_ticks", label: "Notches per desktop", min: 1, max: 10, step: 1, when: "scrollSteppedOn",
          desc: "How many wheel notches move one desktop. Notches short of a desktop show as a peek of the next one." },
        { type: "toggle", key: P + "binds:scroll_desktops_sticky", label: "Sticky desktops", when: "scrollDesktopsOn",
          desc: "After the first desktop of a quick scroll, each further one takes a little more push, so a fast spin is easier to stop where you want." },
        { type: "toggle", key: P + "binds:scroll_desktops_stop_at_empty", label: "Stop at the first empty desktop", when: "scrollDesktopsOn",
          desc: "The wheel goes no further than one empty desktop past your last desktop with windows, as the overview does. Off, it goes on to 10 or your highest desktop." },
        { type: "toggle", key: P + "binds:scroll_desktops_populated_only", label: "Skip empty desktops", when: "scrollDesktopsOn",
          desc: "Only desktops with windows, plus that first empty one and the desktop you are on. Off, every desktop number in between is visited too." }
      ]},
      { title: "Mosaic", rows: [
        { type: "keybind", id: "mosaic_cycle", bind: "mosaic_cycle", label: "Layout cycle", when: "mosaicOn", chordEdit: true,
          desc: "Its key (Super+L unless you change it) cycles Dwindle → Scrolling → Mosaic. Off, Super+L stays Omarchy's two-way toggle. Needs Mosaic on." },
        { type: "keybind", id: "mosaic_focus", bind: "mosaic_focus", label: "Mosaic-aware focus and swap", when: "mosaicOn",
          desc: "Super+arrows move focus and Super+Shift+arrows swap windows by position on a Mosaic desktop, and work as before everywhere else. Needs Mosaic on." }
      ]}
    ]
  }
]

// "Send feedback", above the signature: GitHub's issue form (.github/
// ISSUE_TEMPLATE/feedback.yml), opened with the installed version filled in.
var feedbackUrl = "https://github.com/dragosol/hyprdrive-omarchy/issues/new?template=feedback.yml"

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

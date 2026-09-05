-- hyprdrive fragment 25-rules -- window rules the house presentation needs.
--
-- MACHINE-OWNED above the raw-snippet marker; USER-OWNED below it. Everything
-- above BEGIN is rewritten wholesale when this fragment is regenerated;
-- everything between BEGIN and END is carried across verbatim.
--
-- These are rules for windows the plugin CAUSES but does not own. A preview
-- the dock opened is still an ordinary toplevel belonging to another process.

-- GNOME Sushi / Nautilus Previewer -- SPACE over a dock stack item, and
-- Nautilus's own spacebar, both open this window. Float and centre it; leave
-- opacity and blur to Omarchy's window rules so a preview is not the one
-- surface that ignores the theme.
o.window({ class = [[^(org\.gnome\.NautilusPreviewer)$]] }, {
    float = true,
    center = true,
})

-- Restate default-opacity after plugin load so mapped windows keep a rule.
-- Values match ~/.config/hypr/windows.lua (0.985 focused / 0.85 unfocused).
-- Apps that opt out (`-default-opacity`) are untouched.
o.window({ tag = "default-opacity" }, { opacity = "0.985 override 0.85 override" })

# Search refactor: map, design, gates (stage 1)

Branch `p3/search` from `hyprland-native` 8c358b3 (1.9.68.1). Goal of the whole
refactor: search becomes ONE module with an explicit session object passed in,
instead of being spread over src/overview with the `g_search` global. No
behaviour or visual change. Stage 1 = this map, the design, and the gates; no
code moved yet. Line numbers are at 8c358b3.

## 1. Map

### 1.1 The state and its three lifetimes (search/SearchSession.hpp)

`inline SSearchSession g_search` -- ONE instance, plugin-wide (one keyboard,
one cursor):

| group | lifetime | contents |
| --- | --- | --- |
| `query` | session (reset()) | text, active, windowsOnly, providerTotals, the window-gate memo (memoFrame/memoQuery/memo, keyed on `g_overviewFrame` + query) |
| `nav` | session | selection, colTop[3] (column scroll), selectFirstPending, explicitSelection, intent, hoverIndex, zone (Tile/Result/Desktop/Search), keyboardTile, pendingFocus |
| `pointer` | session; also endInteraction() | drag (G12 result drag), uiPress |
| `layouts[MONITORID]` | PER MONITOR, per frame | SSearchLayout: bandH, rowL, stripPlateL, gridTiles, gridNudgeL, gridShiftL, hits, and the frame's boxes (pill, badge, container, cards, inspector, tool pane, label, selection) |
| `fields[MONITORID]` | PER MONITOR, spans frames | SSearchFieldMotion: the live field's width/scroll springs |

reset() = query + nav + endInteraction(); resetLayout(mon) erases that
monitor's layout + field (COverview constructor only); endInteraction() =
pointer + every layout's hits.

Other search state outside g_search (all plugin-wide):
- `overviewSearchRows()` memo: function-local static of an inline function in
  OverviewInternal.hpp (one instance; keyed on query, menu route, browse flag,
  SearchBackend inputs generation, Membership generation, AppSearch inventory
  generation; root browse refreshed at most once a second).
- `SearchLetters::g_searchLetters` (query letter pop), and in OverviewState.cpp
  (namespace OverviewDetail): `g_searchLit` (row hover lift), `g_pillHovered`,
  `g_pillPress`, `g_placeholderLetters`, `g_pillReleasedAt`.
- core/PluginParts.hpp externs: `g_heldGuardKeys`, `g_consumedPresses`,
  `g_letterTickTimer`, `g_searchRepeatTimer`, `g_searchRepeatKey`.
- `static s_lastPress` in COverview::onKeyboardKey (one press, one overview).
- Per COverview (one per monitor), Overview.hpp: `resultsBand`, `resultsWidth`,
  `searchDock`, `pillNudge`, `gridNudge` (PHLANIMVARs, overview's anim config),
  `pillNudgeWant`, `searchPaneLastBox`, the matchingWindowCount memo.

### 1.2 Two monitors today (preserve exactly)

Measured in the rig (search_monitors gate): one query, one nav, one selection
drawn by both outputs; each output lays out and publishes its own layout (same
rows, its own band/pill/hits, its own gridTiles: the match is in the grid of
the output whose desktop has it); the keyboard is with one overview
(`activeScrollOverview`), which is what lights the pill/results border; Tab
walks search -> grid 1 -> grid 2 -> search (tabAcrossMonitors). Escape clears
both and the second Escape closes both. Details that are shared on purpose
or by accident and must stay as they are:
- `nav.selection` is clamped and written back by EVERY output's publish
  (renderSearch: `g_search.nav.selection = LAYOUT.selection`).
- `selectFirstPending` is spent by whichever output renders first.
- `nav.colTop` is session state but written by each output's
  overviewTrimSectionsToFit with ITS row cap: with outputs of different
  heights the last one drawn wins (see 4, quirk 4).
- close(): the last overview to close resets the session; an earlier one only
  ends the interaction (a second output keeps the query).
- COverview's constructor resets only its own monitor's layout and field (a
  second output opening must not wipe the query).

### 1.3 Who reads / writes what (by file and function)

g_search uses: 358 in 16 files. Writers of `query`: SearchInput.cpp
(handleSearchPress, afterQueryEdit), OverviewOpen.cpp
(openScrollOverviewWithQuery, overviewAdmitWindow, resetSearchSession),
OverviewInternal.hpp (activateSearchResult rewrite/drill,
overviewSettleMenuStep, overviewSearchMatches memo), OverviewInput.cpp
(onMouseButton: pill click -> active), DebugCommands.cpp (`type`). Readers:
GridLayout.cpp (query text for the mission-control grid, gridShiftL),
OverviewCards.cpp (empty query -> promoted/visibility/motion), OverviewLifecycle
(empty edge -> rebuild), Overview.cpp (prof line, drag icon), KeyRouter.cpp
(Backspace repeat).

Writers of `nav`: SearchInput.cpp (afterQueryEdit, Esc, Backspace, typing),
OverviewKeys.cpp (moveSelection 47, tabAcrossMonitors 20, stepTileDesktop 4),
OverviewInput.cpp (onKeyboardKey Down/Tab/Return 43, onMouseMove hover 20,
onMouseButton press 15), OverviewHit.cpp (updateTileHover, tile select),
OverviewSearchView.cpp (renderSearch spend + clamp, Ctrl tap reorder),
OverviewInternal.hpp (settleMenuStep, activate, trim colTop), OverviewOpen.cpp.
Readers: OverviewCards.cpp (tile ring), OverviewStrip.cpp (strip plate lit),
OverviewHit.cpp (hoverSignature), layoutSearch (zone, lines 289/311).

`layouts`: written ONLY by renderSearch's publish (whole struct), plus
OverviewStrip.cpp (stripPlateL, gridNudgeL each frame), OverviewStripCleanup.cpp
(gridNudgeL bump), the resultsBand/gridNudge update callbacks
(OverviewLifecycle.cpp: bandH, gridShiftL between frames). Read by
overviewStripLogicalHeight/StackL, overviewSearchLogicalHeight,
overviewResultsLogicalHeight (-> GridLayout, OverviewDrag, OverviewStrip),
GridLayout (gridShiftL), moveSelection/currentNavBox/Down (hits, pill),
searchRowAt (pointer), OverviewStrip label (containerBox, pillRest, scale).
`fields`: renderSearch (stepTo), drawSearch (contentL, measured),
letterTickCb, OverviewFrames.cpp (two frame predicates; fieldOn creates the slot).

### 1.4 Where each concern lives

| concern | where |
| --- | --- |
| row building | OverviewInternal.hpp 486-985: SSearchRow, overviewSearchRows (memo) / RowsLive / ToolRows, SSearchSection + overviewSearchSections, overviewWindowForApp, overviewFirstAppIndex |
| scoring | AppSearch.cpp (fuzzy app scorer, strictMatch for windows), SearchBackend.cpp (menu, files, calc, agents), SearchProviders.cpp (D-Bus providers), ToolSearch(+Parse) |
| window filter | OverviewInternal.hpp 1115 overviewSearchMatches (+ memo in g_search.query), 1176 overviewMembershipMatchesLive (Y9 minimized rule), Membership cache |
| intent parsing | SearchIntent.hpp (Typing/Browsing/Hovering + overviewSpacePreviews), ToolSearchParse.hpp (define/tr), SearchBackend::isAgentQuery, OVERVIEW_QUERY_REWRITE |
| activation | OverviewInternal.hpp activateSearchResult, overviewSettleMenuStep, overviewSearchPaneUp; OverviewSearchView.cpp Space/Shift/Ctrl taps, overviewMenuBack |
| keyboard (text) | input/KeyRouter.cpp raw hook -> search/SearchInput.cpp handleSearchPress (text, Esc, Backspace, repeat, compose) |
| keyboard (nav) | OverviewInput.cpp onKeyboardKey (Down/Tab/Return), OverviewKeys.cpp moveSelection, tabAcrossMonitors, currentNavBox, collectNavTargets |
| pointer hit-test | SearchSession.hpp searchRowAt; OverviewInput.cpp onMouseMove (hover), onMouseButton (G12 press/hold/drag/click, pill press); OverviewHit.cpp |
| band/pill layout maths | OverviewInternal.hpp 1379-1870 (strip band ceilings that read the published layout, results budget/row cap/trim/band/width targets, column widths, card metrics, tool row heights); SearchSession.hpp pill constants |
| layout | OverviewSearchView.cpp layoutSearch (92-515; reads g_search.nav.zone and Hyprland: top-chrome layers, window list, text rasterising for tool rows) |
| draw | OverviewSearchView.cpp drawSearch (572-1295), renderSearch (1306-1521: ANIMATE -> LAYOUT -> PUBLISH -> DRAW), KeyboardCat::render |
| frames | SearchInput.cpp letterTickCb (8 ms timer), OverviewFrames.cpp predicates, pillPlaying/searchHoverEasing |

### 1.5 Threads (core/Workers) and timers

| worker | owner | started | stopped |
| --- | --- | --- | --- |
| hd-search-dbus | SearchProviders.cpp | first query(); one worker, generation per keystroke | SearchProviders::shutdown (main.cpp PLUGIN_EXIT) |
| hd-file-search | SearchBackend.cpp | SearchBackend::init (overview open, gesture open) | SearchBackend::shutdown |
| hd-tool-search | ToolSearch.cpp | first define/tr lookup | ToolSearch::shutdown |
| hd-icon-index, hd-icon-theme | AppSearch.cpp | inventory / icon theme change | AppSearch::shutdownIconIndex |
| hd-keyboard-cat | KeyboardCat.cpp | the easter egg | KeyboardCat::shutdown |

All plugin-wide, none per overview or per session. Results return under
mutexes + generation atomics and are pulled by the main thread in
overviewSearchRows (memo keyed on the generations). Main-loop sources: the
letter tick timer, the key-repeat timer, the file worker's eventfd
(onFileResultsReady -> damage every overview).

### 1.6 Hyprland coupling

None: SearchBackend, SearchProviders, ToolSearch(+Parse), NativeSearch,
SearchLetters, SearchIntent, AppSearch.hpp. Light: AppSearch.cpp (Executor for
launch), SearchSession.hpp (PHLWINDOWREF, CWindow* memo key, CBox, Time,
MONITORID). Heavy: SearchInput.cpp (seat, xkb, compositor event loop),
KeyboardCat.cpp (render pass), and everything on the overview side
(OverviewInternal.hpp search block: window list, workspaces, layer surfaces,
Membership; OverviewSearchView.cpp: renderer, PHLANIMVAR, FlightBlur).

## 2. Design

### 2.1 Target files

src/search/ (new or renamed; every include stays bare):

| file | content | from |
| --- | --- | --- |
| SearchSession.hpp/.cpp | `Search::CSession` = today's SSearchSession (query, nav, pointer, per-monitor views), reset/endInteraction/resetLayout unchanged; the instance defined in the .cpp | SearchSession.hpp |
| SearchNav.hpp | EOverviewNavZone, ESearchIntent (+ SearchIntent.hpp content) | SearchSession.hpp, SearchIntent.hpp |
| SearchField.hpp | pill constants, qtSpringStep, SSearchFieldMotion (pure) | SearchSession.hpp |
| SearchLayout.hpp | SSearchLayout, SSearchRowHit, SResultDrag, searchRowAt(view, point) | SearchSession.hpp |
| SearchRows.hpp/.cpp | SSearchRow/SSearchSection, rows (+ the ONE memo), tool rows, sections, windowForApp, firstAppIndex | OverviewInternal.hpp 486-856, 1871 |
| SearchFilter.hpp | the window gate's query half: overviewSearchMatches + memo (takes the frame key) | OverviewInternal.hpp 1115-1158 |
| SearchBand.hpp | results budget/row cap/trim/band & width targets, column widths, card + tool metrics (stripStackL passed IN) | OverviewInternal.hpp 1552-1870 |
| SearchActions.cpp | activate, settleMenuStep, paneUp, menuBack, Space/Shift/Ctrl taps, afterQueryEdit, resetSearchSession, openWithQuery/admitWindow session halves | OverviewInternal.hpp, OverviewSearchView.cpp, OverviewOpen.cpp, SearchInput.cpp |
| SearchKeys.cpp | text keys (today's SearchInput.cpp) + the Result/Search half of the arrows, Tab and Return (returns an outcome) | SearchInput.cpp, OverviewInput.cpp, OverviewKeys.cpp |
| SearchPointer.cpp | hover, press, hold/drag, click-release (returns an outcome) | OverviewInput.cpp |
| SearchView.cpp | layoutSearch + SSearchLayoutInputs (zone becomes an input), drawSearch, the pill press/lit/placeholder animators, frame predicates (pillPlaying, searchHoverEasing) | OverviewSearchView.cpp, OverviewState.cpp |

What stays in src/overview/ and why: `COverview::renderSearch` as a thin frame
step (it owns the five PHLANIMVARs created with the overview's animation
config, and the PUBLISH point is load-bearing in the frame order), the strip
label's two nudges (they are the strip's geometry), and everything about tiles
and strip cells (collectNavTargets, pickNavTarget, closeOnWindow, stepTileDesktop,
tabAcrossMonitors' monitor order). Drawing with Hyprland's renderer is NOT a
reason to stay: KeyboardCat.cpp already draws from search/, and src/README says a
file moves between folders without an edit. overviewStripLogicalHeight/StackL
stay in overview (strip geometry) and read the band through
`session.view(mon).bandH/rowL`.

### 2.2 Session ownership and the interface

- One `Search::CSession`, plugin-wide, owned by the overview registry
  (OverviewManager.cpp, next to scrollOverviews()) -- not by a COverview: two
  overviews share it exactly as they share g_search today. Each COverview gets
  `Search::CSession& m_search` at construction; KeyRouter/SearchKeys and
  DebugCommands get it from the same owner. Per-monitor `views` (layout +
  field) stay a map inside the session (2.4 #6).
- In (overview -> search, per frame): `SFrameIn { monitor, renderScale,
  layout axis, fade, sampled anim values (dock, nudge, band, width), gridTiles,
  matchingWindowCount, stripStackL, keys (is this the keyboard's overview),
  pointer-on-chrome tests }`.
- Out: ANIMATE targets `{ band, width (+ warp-from-pill), dockGoal, pillNudge,
  gridNudge goal }` that renderSearch applies to its PHLANIMVARs (same
  thresholds: 0.5 / 0.01), `fieldMoving` (-> requestInputFrame), then the
  SSearchLayout it publishes with the same carries (stripPlateL, gridTiles,
  gridNudgeL, gridShiftL) and the selection write-back.
- Keyboard: the raw hook keeps calling `Search::onTextKey(session, keycode)`
  first (Esc/Backspace/text, unchanged order: Space/Quick Look and the
  Shift/Ctrl taps stay ahead of it in KeyRouter). onKeyboardKey keeps its
  guards (mods, Super+W, s_lastPress) and asks `Search::onNavKey(session,
  key, view, rows) -> { Handled | ActivateIndex(i, newInstance) | ToTiles |
  ToDesktops | NotMine }` for Result/Search zones; Tile/Desktop stay in the
  overview. Return's PICKABLE/launchIdx decision moves into search;
  close()/selectAndClose stay in the overview.
- Pointer: `Search::onPointerMove/Press/Release(session, view, point, …) ->
  outcome` (hover change, consumed press, activate i, pin/unpin desktopId).
- The grid reads the band/pill through the existing free functions
  (overviewSearchLogicalHeight, overviewResultsLogicalHeight,
  overviewStripStackL, GridLayout gridShiftL), which read
  `session.view(mon)`; no grid code learns about rows or nav.

### 2.3 Ordered steps (each builds, passes `make all`, the search gates and the existing gates)

1. PURE MOTION (script, anchor-asserting): split SearchSession.hpp into
   SearchNav.hpp / SearchField.hpp / SearchLayout.hpp, SearchSession.hpp
   including them at the same points.
2. PURE MOTION: cut OverviewInternal.hpp's search blocks into SearchRows.hpp,
   SearchFilter.hpp, SearchBand.hpp, SearchActions.hpp -- still `inline` in
   headers, included by OverviewInternal.hpp at the exact cut points (the
   forward declarations overviewFrameOn / g_overviewFrame / overviewStripStackL
   stay where they are). Inline stays inline, so the row memo is still one
   function-local static.
3. PURE MOTION: OverviewSearchView.cpp -> search/SearchView.cpp (layoutSearch,
   drawSearch, report) and the COverview members (renderSearch, Space/Shift/
   Ctrl taps, menuBack) into overview/OverviewSearchStep.cpp. The OverviewDetail
   pill/lit globals stay in OverviewState.cpp for now (moving them changes
   static-init TU order; do it only once they are session members).
4. SHAPE: `Search::CSession& searchSession()` accessor; every `g_search.` use
   becomes `S.` with `auto& S = searchSession();` at the top of each function
   (scripted, per function). g_search becomes a static in SearchSession.cpp.
5. SHAPE: layoutSearch takes `zone` as an input (its two g_search reads);
   overviewSearchRows/activate/settle/paneUp/afterQueryEdit take `CSession&`.
6. SHAPE: COverview gets `m_search` from the registry; the accessor is used
   only by the registry, KeyRouter, DebugCommands; then removed.
7. SHAPE: SearchKeys -- move the Result/Search branches of onKeyboardKey /
   moveSelection / tabAcrossMonitors' ENTERSEARCH into Search::onNavKey with
   an outcome; branch order verbatim.
8. SHAPE: SearchPointer -- onMouseMove hover + onMouseButton G12 into
   Search::onPointer*; release order (result drag first, then uiPress, then the
   pill ladder) verbatim.
9. SHAPE: renderSearch -> Search::frameTargets + apply + layout + publish +
   draw; frame order and every threshold verbatim.
10. SHAPE (optional, last): row building out of line (SearchRows.cpp, memo a
    TU static: same single instance); band maths take stripStackL as a
    parameter. 11. README/COVERAGE.

Pure motion = 1, 2, 3 (plus scripted renames in 4). Code shape = 4-10.

### 2.4 What a "clean" version would change, and how it is avoided

1. Publish one frame late on purpose (grid/strip read last frame's
   bandH/rowL; X50) -- keep the publish point and the update callbacks that
   write bandH/gridShiftL between frames.
2. Selection clamp written back by every output; first-app spend by the first
   output drawn -- keep in the shared nav.
3. colTop is per session but trimmed per output -- keep it in nav (a
   per-output colTop would change scrolling on mixed-height outputs).
4. Tab Result -> Tile keeps nav.selection (observed sel=0 with zone tile): Tab
   back to the results returns to the same row. Do not reset it.
5. Reset sites stay separate: the dispatcher resets before every action but
   `tab` (also before CLOSE, so the close animation unfilters); close() resets
   only as the last overview; openOverview only when none exists; the gesture
   open; session lock; the dock's Show Applications. Do not fold them into
   the COverview constructor (a second output would wipe the query).
6. The per-monitor layout outlives its overview (erased at the NEXT
   construction), and layoutOn()/fieldOn() create a default slot when read
   (overviewStripLogicalHeight, frame predicates). Moving views into COverview
   would hand any reader with no overview defaults instead of the last values;
   readers today: overview, strip, grid, drag -- verify none runs without an
   overview before even considering it. Kept as a map in the session.
7. The row memo, s_lastPress and the letter/pill animators must stay single
   instances.
8. layoutSearch reads g_search.nav.zone and Hyprland (top chrome layers,
   window list, text rasterising) despite its "pure" comment -- pass zone in;
   keep the Hyprland reads where they are evaluated today (same frame).

### 2.5 Bugs and quirks found (not fixed)

1. Strip label stays up after ONE pointer motion off the strip to empty space
   (rig: a single absolute move from desktop 1's cell to 300,600 leaves
   gridNudgeL 24.7 and the label drawn for >3 s; the next motion clears it).
   Unconfirmed on hardware; the gate leaves the strip along a path.
2. X20's "no matches" (`emptyResult`) is unreachable for a typed query: the
   rows always end with Search Google (plus Define/Translate), so RESULTS is
   never empty; only Show Windows (windowsOnly) can reach it. Typing nonsense
   docks the pill and opens the band for the web row alone.
3. With no app match the auto-selection falls back to row 0 of ANY column
   (overviewFirstAppIndex): "gsbeta" (a window's title, no desktop entry)
   selects "Ask Grok" in the rig, so Return would run the agent with the
   query instead of going to the window shown in the grid. Possibly intended
   ("the first result is selected"); worth a ruling.
4. nav.colTop is shared by outputs that trim with different row caps (2.4 #3).
5. Comment rot: layoutSearch "reads no global" (it reads two);
   `void overviewQueryEdited(); // main.cpp` is defined in OverviewOpen.cpp;
   KeyboardCat.cpp includes SearchSession.hpp and does not use it.

## 3. Gates (tests/gates/search_*.sh, search.lib) -- commit e17a1e9

State from `hyprdrive:debug search` (commit ba365d7, the read-only dump: session
line, ranked rows, per-overview layout and hit boxes), not pixels. Desktop-entry
fixtures (Gsalpha/Gslaunch) reach AppSearch through XDG_DATA_DIRS.

| gate | checks | covers |
| --- | --- | --- |
| search_keys | 32 | 0 fresh open; 1 typing: query, running-app row auto-selected, pane laid out, band >= 100, pill docked, filter (1 match, promoted into the grid); 2 Up/Down/Right/Left; 3 Tab results -> tiles -> desktops -> results; 4 Esc clears (band 0, placeholder, tile back on its card), Esc closes; 5 Backspace to empty and once more; 6 no match; 7 long-query field/pill growth; 8 strip label under the plate with results (grid nudge) / pill lift without; 9 close + reopen fresh |
| search_activate | 9 | Return on a running app focuses it (desktop 2), hover + click on the row does the same, Return launches a not-running app, Return in the tile zone picks the tile |
| search_monitors | 10 | two outputs: keys with one, one query/selection, both panes, per-output grid filter, selection drawn by both, Esc clears both, Esc closes both |

Baseline (1.9.68.1 + dump): each gate 4/4 PASS (3 direct runs + 1 via run.sh).

Controls: 14 one-line patches in tests/gates/search_controls/ (build, run with
`run.sh --control --control-so`). Every run failed its named check; later
checks fail by cascade where the broken state carries on:

| patch (line disabled) | gate: checks failed |
| --- | --- |
| esc_clear (Esc's text.clear) | keys 4 (+5,6,7); monitors 5 |
| backspace_active (active=false at empty) | keys 5 |
| band_target (*resultsBand = TARGET) | keys 1 (+2,3,8); monitors 2 (+4) |
| up_to_search (Up off the column top) | keys 2 (+3,4) |
| tab_to_tiles (Tab Result -> Tile) | keys 3 (+4); activate 4 |
| first_select (selectFirstPending spend) | keys 1 |
| filter_off (overviewSearchMatches always true) | keys 1 (+3,6) |
| field_fixed (field spring target fixed at rest) | keys 7 (+8) |
| strip_nudge (strip label's gridNudgeL) | keys 8 |
| session_reset (g_search.reset in resetSearchSession) | keys 9 |
| pending_focus (activate's pendingFocus) | activate 1, 2 |
| launch_off (AppSearch::launch in activate) | activate 3 |
| click_index (release on the pressed row) | activate 2 (+3) |
| other_monitor_blank (non-keyboard overview skips its search step) | monitors 2 (+3,4) |

Not covered (next): Show Windows (windowsOnly/emptyResult), the menu tree
(drill/back with Esc/Backspace/Left), define/tr tool pane, Space/Quick Look,
Shift/Ctrl double taps, result drag to the dock (G12 pin/unpin), Tab across
two monitors, pixels of the pane.

## 4. Stage 2 status (work log, kept current as each step lands)

Tools (not committed): line-range mover `cut.py` (anchor-asserting: first/last
line text + exact length), `conserve.py` (multiset of lines before/after over
the touched files: every removed line must reappear), `nmdata.sh` (every data
object in the built .so with its instance count). nm inventories and gate runs
are under /tmp/hdg-search/.

| step | commit | evidence |
| --- | --- | --- |
| 1 split SearchSession.hpp | c3f7b57 | conserve: 0 removed, 40 added (pragmas, includes, header comments); nm data objects identical to the base build |
| 2 OverviewInternal.hpp search blocks -> SearchRows/Actions/Filter/Band.hpp | 2510c65 | conserve: 0 removed, 54 added (namespace close/reopen around each include, pragmas, header comments); nm identical; `overviewSearchRows()::MEMO` still 1 |
| 3 OverviewSearchView.cpp -> search/SearchView.cpp + overview/OverviewSearchStep.cpp + SearchView.hpp | 8363d40 | conserve: 4 lines changed (the two `static` signatures, drawSearch's default arg moved to the declaration; the 2-line file header); nm: only compiler constants and Hyprland's own per-TU `Render::ASSET_PATHS` (+1 TU) differ |
| 4 accessor `searchSession()`, `Search::CSession`, instance a static in SearchSession.cpp | 4f3e539 | scripted per function (code tokens only); nm: g_search V (inline) -> b (file static), still 1. Gates search_keys 32, search_activate 9, search_monitors 10, overview 29, overview_hover_still 79, overview_rest 20: all PASS |
| 5 layoutSearch takes the zone; rows/paneUp/settle/activate/afterQueryEdit take CSession& | b05565d | nm: only the memo's mangled name changes (`overviewSearchRows(Search::CSession&)::MEMO`, still 1). Six gates PASS (same counts as step 4) |
| 6 COverview::m_search; instance in OverviewManager.cpp | d25a4e0 | nm: g_search b x1 (now in the registry TU); drawSearch's statics renamed with its signature, still 1 each. Six gates PASS. searchSession() NOT removed: 19 uses left at entry points outside any one overview (KeyRouter's hook, DebugCommands + overviewSearchReport, OverviewOpen's 3 open/admit/reset paths, GridLayout x2, the window gate + membership, the strip/band/search height helpers, getOverviewWindowBox) -- threading the session through the window gate's ~40 callers is out of scope |
| 7 SearchKeys: onArrow, downIntoResults, tabIntoResults, tabEnterSearch, returnPick | fe6c676 | onArrow is moveSelection's 145 search lines cut verbatim and rewritten mechanically (7 damage+return -> {handled, damage}, 6 bare returns -> {handled}). Six gates PASS. Deviation: the text keys stay in SearchInput.cpp (it is a PluginParts TU; the nav half needs OverviewParts), so SearchKeys.cpp holds only the nav half |
| 8 SearchPointer: onPointerMove, onPointerDragMove, takeDrag, clickedRow, takeUiPress, onPointerPress | dd17c05 | release order kept (drag, then uiPress, then the pill ladder). Six gates PASS |
| 9 SearchFrame: frameRows, bandTarget/widthTarget/dockGoal, stepField, publish | 7f52bd0 | the PHLANIMVAR applies, thresholds, width warp and the draw stay in renderSearch at the same points; publish point unchanged. Six gates PASS |
| 10 rows out of line | skipped | not pure motion: every inline function in SearchRows.hpp would need its `inline` dropped, a declaration added and the OvInternal context made explicit, and the band maths' stripStackL parameter is a signature change at every caller |
| 11 README / COVERAGE / controls | (see log) | src/README.md describes the module; the 14 control patches regenerated against the moved lines (mkpatches2.py); search.lib and COVERAGE.md paths; the two comments still naming g_search |

Step 11 commit: 81d5005. Final full suite (tests/gates, 39 gates + own_size_tile,
each through run.sh with HL_NAME=gs<tag>): new build 40/40 PASS, 749 checks;
baseline e17a1e9 (= 1.9.68.1 + the read-only dump; built from git archive in
/tmp/hdg-search/base) 40/40 PASS, 749 checks; identical gate by gate, no
flakes, no regression.

Counts: src/search 16 files / 8512 lines -> 31 / 11420; src/overview 53 / 28891
-> 53 / 26465 (OverviewSearchView.cpp gone, OverviewSearchStep.cpp new);
`g_search` 365 mentions in 16 files -> 2 (the registry's static and its return).

Controls re-verified after step 11 (14 builds from the regenerated patches,
17 control runs, /tmp/hdg-search/runctl2.sh): every control fails its named
check, and the failing check sets match stage 1's logs exactly in 16 of 17.
search_monitors/band_target failed 2 only (stage 1: 2 + 4 by cascade) on its
first run; three reruns failed 2 + 4 like stage 1 -- a cascade that depends on
the timing of the broken state, the named check failed in all four.
search_keys/esc_clear still ends without a RESULT line (an unbound variable in
search_keys.sh once the cascade empties a value), exactly as in stage 1; its
FAIL lines are the stage-1 set.

## 5. Stage 3 status: the accessor removed

| commit | what | evidence |
| --- | --- | --- |
| ca80df7 | the registry is an object: `SOverviewRegistry g_overviewRegistry` (one member, `search`) in OverviewManager.cpp where g_search was; KeyRouter's hook, onDebugDispatcher, overviewSearchReport, openScrollOverviewWithQuery, overviewAdmitWindow, resetSearchSession name `g_overviewRegistry.search`; COverview binds m_search in the constructor's init list (same declaration slot, so same init order) | make all; six gates PASS |
| 322a145 | grid family takes `Search::CSession&` first: promoteCtx, promotable, releasing (peek/step), promoted, gridSlot, mainGridWindows(Into), gridLayout, cardGridPush, getOverviewWindowBox, getOverviewDragWindowBox, tileShown | make all; six gates PASS |
| 41dc6ac | the window gate: overviewSearchMatches, membershipMatches(Live), shouldShowOverviewWindow, gridWindows(Into), gridPromotableIn, stripMiniShown, searchHides, overviewShowsWindow, the two blur tests, the fullscreen visibility/path windows; renderOverviewWindow takes the session (its blur test asks the gate) | make all; six gates PASS |
| a4de3e7 | heights: stripLogicalHeight, stripPadL, chromeGapL, stripStackL, searchLogicalHeight, resultsLogicalHeight, resultsBudgetL, resultsRowCap, resultsBandTarget, Search::bandTarget | make all; six gates PASS |
| a7a20b0 | searchSession() removed; README + the registry comment; band_target, filter_off, session_reset, strip_nudge control patches re-aimed (context lines only; the disabled line is unchanged; all 14 `git apply --check` clean, controls not re-run) | make all; full suite below |

Scripted (/tmp/hdg-search: thread.py-style rewrite, session first parameter;
calls in a COverview member pass `m_search`, in a threaded free function `S`),
then compiler-checked. Not a pure pass, by hand: ~COverview (the script read it
as a free function; it passes m_search); the dock's renderLivePreview
(DockPreview.cpp) is a fourth entry point outside any overview and names the
registry; overviewTrimSectionsToFit already took the session (the script
doubled it; undone). Every parameter is non-const: the gate writes the filter
memo in `S.query` and layoutOn() creates a slot, so no reader is const. There
is still exactly one CSession object in the tree (the registry's member), so
every reference passed is that object. nm: `g_overviewRegistry` B x1 (was
g_search b x1: now a global, exported, same TU); `overviewSearchRows(Search::
CSession&)::MEMO` still 1. `git grep searchSession HEAD` and `git grep -w
g_search HEAD`: no hits.

Full suite on a7a20b0 (each gate through run.sh, HL_NAME=gs<tag>): 40/40 PASS,
749 checks, identical gate by gate (status and check count) to stage 2's run.
Baseline e17a1e9 rebuilt (/tmp/hdg-search/base), run right after: 40/40 PASS,
749 checks; identical gate by gate. No reruns needed, no flakes. Runs:
/tmp/hdg-search/runs/s3c{1..4}.txt (six gates per commit), s3-full-new.txt,
s3-full-base.txt.

## 6. Proportions (ui_scale, 2026-11-19)

The search row, the results and the strip were drawn for 3840x2160 at scale 1
(2160 logical px tall) and kept those logical sizes on every screen. Owner's
design ("option B"), in `src/core/UiScale.hpp`, read through `overviewUi()`
(OverviewInternal.hpp):

| factor | value | applies to |
| --- | --- | --- |
| space `uL` | clamp(H / 2160, 0.5, 1), H = the monitor's logical height (not the work area: a bar coming and going must not resize the overview, and 4K with a bar is still exactly 1) | strip band (`strip_height`, under `strip_max_screen_pct`), `OVERVIEW_STRIP_PANELPADL`, `strip_gap`, `mission_control_gap` (grid inset and tile gaps, the results span), the search row's margins and dock pad, `OVERVIEW_RESULT_PANELGAPL`, the label/plate gaps (strip, scratchpad, other monitors, Clean Up button) |
| type `uT` | clamp(sqrt(uL), 0.9, 1) | the pill (laid out uT smaller; everything inside follows the measured transform), result and tool rows, headers, paddings and their fonts, the strip labels' text and padding |
| budget | min(360 x uT, 0.25 x H); pinned: the old 0.42 x H | the results band |
| fixed | | 1 px dividers and rings, hit and drag slop, tile close button and window-pill buttons, the dock |

`SSearchLayout::scale` is the card transform times uT (what text is drawn at);
`cardScale` is the transform alone, for turning a rendered distance back into
logical px. The inspector is dropped below 1200 logical px of width; on a
portrait or narrower-than-1600 screen it and the columns' 160 px floor shrink by
uT. `ui_scale` = a number pins uL to it (uT follows) and turns the two
screen-relative rules off, so 1 is the old look on every screen. The Omarchy
text scale still stops at 14/12 (TextScale.hpp): the rows follow uT, not the
text size.

Gate: `overview_proportions` (control `ui_scale = 1`). 3840x2160 at scale 1 is
pixel-identical to the build before.

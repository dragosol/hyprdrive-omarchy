# Dock Stacks B — dropping into the dock — Implementation Plan

> Executed natively in the session that wrote it (user: "make and execute plans B and C").

**Goal:** Files dropped on a stack go into its folder, files dropped on Trash are trashed, and a folder dropped between cells pins as a stack there, with the dock parting around a placeholder.

**Spec:** `docs/superpowers/specs/2026-09-24-dock-stacks-b-design.md`

## Global Constraints

- Every pass element box through `Stacks::drawableBox`; textures `->ok()`.
- No file I/O on the compositor thread; moves/copies/trash on `StackReader`'s worker.
- Everything armed is removed in PLUGIN_EXIT.
- New behaviour runs in the nested Hyprland (`scratchpad/nested/nested.sh`) before the live desktop.
- The drag source must never be told the drop succeeded as a MOVE (it could delete its copy): the plugin always aborts, and does the move itself.

## Review Focus

1. A drag that ends somewhere else (a window, the desktop, Esc) after hovering the dock: the pending read is dropped, the placeholder disappears, nothing is moved. → Task 3/4 nested check.
2. A drag source that never answers the uri-list request (or answers slowly): release over the dock does nothing harmful (no files known → no action) and the pipe is closed. → Task 3 (release with no data).
3. URIs with spaces/UTF-8/`#` comment lines/`\r\n`: paths decoded exactly. → Task 1 tests.
4. A name conflict in the destination: never overwrite. → Task 2 test.
5. Dropping a folder onto itself / into its own subfolder / onto the stack it already is: refused with a shake, not recursed. → Task 2 test + Task 3 check.

---

### Task 1: DockDropModel — pure parsing and decisions (host-tested)

**Files:** Create `DockDropModel.hpp` (header-only), `scripts/dock_drop_test.cpp`; modify `scripts/host_tests.sh`.

**Produces:**
```cpp
namespace ScrollOverview::DockDropModel {
    std::vector<std::string> parseUriList(const std::string& data);          // file:// → decoded absolute paths
    std::string              uniqueName(const std::string& dir, const std::string& base,
                                        const std::function<bool(const std::string&)>& exists); // "a (2).txt"
    enum class ECell : uint8_t { None, Stack, Trash, App, Gap };
    enum class EAction : uint8_t { None, IntoStack, ToTrash, PinAt };
    EAction resolve(ECell cell, size_t files, size_t dirs);
    bool    isInside(const std::string& path, const std::string& dir);        // path == dir or under it
}
```
- `resolve`: Stack + (files+dirs>0) → IntoStack; Trash + any → ToTrash; Gap + (dirs==1 && files==0) → PinAt; else None.
- Tests: CRLF and LF lists, `#` comments, `%20` and `%C3%A9`, `http://` skipped, empty lines; `uniqueName` for `a.txt`, `a`, `.bashrc`, `a.tar.gz`, and when `a (2).txt` also exists; `resolve` table; `isInside` (`/a/b` in `/a`, `/ab` not in `/a`).

### Task 2: StackReader move-or-copy job

**Files:** `StackReader.hpp/.cpp`, `scripts/stack_reader_test.cpp`.

**Produces:** `void moveOrCopyInto(const std::string& dir, std::vector<std::string> paths);` → result via `lastTrashResult()` renamed to a shared `lastJobResult()` (`STrashResult` → `SJobResult{seq, moved, error, dest}`), plus `SListing`-free sync hook `moveOrCopyNow(dir, paths)` for the host test.
- Same filesystem (`st_dev` equal) → `g_file_move`; else `g_file_copy` (recursive for a directory: `g_file_copy` does not recurse, so a directory crossing filesystems is refused with an error rather than half-copied).
- Destination name through `DockDropModel::uniqueName`.
- A source that `isInside` the destination, or equals it, is refused.
- Test: move two files into a temp dir, one conflicting → `a (2).txt`; moving a dir into itself refused.

### Task 3: DockDrop — prefetch, target, takeover (replaces the spike)

**Files:** Create `DockDrop.hpp/.cpp`; delete `DockDropSpike.cpp`; modify `main.cpp` (hover + button hooks, PLUGIN_EXIT), `Dock.hpp/.cpp` (a `cellAt(monitor, point)` query returning the item and its `ECell`, and a gap index).

**Produces:**
```cpp
namespace ScrollOverview::DockDrop {
    void onPointerMove(PHLMONITOR monitor, const Vector2D& local); // prefetch + target tracking
    bool onButton(PHLMONITOR monitor, const Vector2D& local, bool pressed); // true = took the drop
    int  placeholderIndex(MONITORID monitor);   // stack-slot index for the Moses gap, -1 none
    std::string placeholderPath();              // the folder being offered
    std::string hoveredStackTarget();           // stack path under the drag, for the ring
    bool hoveringTrash();
    void shutdown();
}
```
- Prefetch once per drag source (as in the spike); a new source resets state; `dndActive()` false resets state and removes the read source.
- On release over the pill with a known list: `resolve(...)` → IntoStack: `StackReader::moveOrCopyInto(stack, paths)`; ToTrash: `StackReader::trash(paths)`; PinAt: `Stacks::pinAt(dir, index)`; then `abortDndIfPresent()`, cancel the event. With no list yet (source silent): abort anyway (the drop was aimed at the dock, which cannot give it to the window below) and do nothing else.
- Results bounce/shake through the existing `onReaderReady` path (job owner remembered as with `trashFrom`).

### Task 4: The Moses gap and hover feedback

**Files:** `Dock.cpp` (`buildDockItems` inserts a placeholder `SItem{.stack=true, .placeholder=true, .icon=folder}` at `placeholderIndex` among the stacks; the memo key includes it; the icon loop draws a placeholder at 45% alpha; the ring+lift for `hoveredStackTarget()` / Trash), `Dock.hpp` (`SItem::placeholder`), `DockStacks.hpp/.cpp` (`pinAt(path, index)` = pin + moveTo).
- The arrangement animation already slides cells when the item list changes; the placeholder rides it in and out.
- Reveal: while `PROTO::data->dndActive()` and the pointer is at the bottom edge, `Dock` treats it as a hover (same path as the pointer).

### Task 5: Finish

- Delete the spike, correct `DockStacks.hpp`'s drag paragraph, README ("drop files on a stack or on Trash; drag a folder onto the dock to pin it").
- Full `make all`; nested checks for all four spec scenarios; unload/reload twice in nested mid-drag-hover; commit.

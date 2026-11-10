# Contributing to Hyprdrive

This is a compositor plugin. A mistake here does not throw an exception, it
takes the user's session down mid-sentence, so the rules below are not style
preferences — every one of them is a bug that already happened.

## Layout

| folder | what is in it |
| --- | --- |
| `src/<area>/` | the plugin, one folder per subsystem (`src/README.md` is the map) |
| `ui/` | the settings page (QML), its schema, and the command-line back ends in `ui/bin/` |
| `config/` | the Lua fragments `make install` puts in `~/.config/hypr/hyprdrive/` |
| `scripts/` | what the build and the installer run, and nothing else |
| `tests/` | `make check`: host unit tests (`tests/unit/`), tool tests, and the gates that need a compositor (`tests/gates/`, driven by `tests/rig/`) |
| `docs/` | `CONFIG.md` (generated) and `DEBUG.md` |

Every `#include` is a bare file name: each `src/<area>/` folder is on the include
path and no two files may share a name (the Makefile stops if they do).

## Build

```bash
make all
```

- `all` is the build **and** `make check`: `scripts/symcheck.sh`,
  `scripts/keycheck.sh`, `gen_keys_table.sh --check`, `tests/host_tests.sh`
  and the tool tests in `tests/`, in that order. The symcheck line must read
  `symcheck: OK -- every undefined symbol has a provider (~563 checked)`, and
  the run ends on the host tests rather than on it.
  A `-shared` link never complains about an undefined symbol and `dlopen` binds
  lazily, so a missing symbol is a time bomb that goes off at the first call
  that reaches it — once, that was a mid-close tile ease, minutes after login,
  with no crash report because `ld.so`'s error path is `_exit(127)`. Never ship
  a build whose symcheck failed.
- **`rm -f hyprdrive.so` before `make` while the compositor has it mapped.**
  The link otherwise truncates the inode that is currently loaded.
- After any unclean stop — power loss, a paused VM, a full disk — run
  `find .build -size 0 -delete` before trusting a build. Truncated objects keep
  fresh timestamps, so `make` skips them and the link happily consumes them.
- `-O2` is the shipped artifact and what every pixel gate grades. `OPT=-O0` for
  a debugging session, never for a measurement.

## Gates

`tests/gates/<name>/run.sh` runs one gate against a headless Hyprland started by
`tests/rig/headless/hl.sh` (its own user, its own socket; never the session you
are sitting in). A gate states what it defends in its header.

A change is clean when it moves **zero** gates, in either direction: a red
turning green without you intending it means you changed observed behaviour and
did not notice.

### Pixel doctrine

A screenshot is evidence only with its controls. Non-negotiable:

- Control frames on **both** sides of the measured one. Two identical frames are
  an explicit FAIL, not a pass — a frozen output hands back the same picture
  forever, and "nothing changed" then means nothing.
- Dilate the noise mask; measure the **largest connected blob**, never a
  bounding box and never a raw pixel count. A bbox spans the whole panel as soon
  as two far-apart pixels move.
- A new or changed gate must FAIL on the old build before its pass means
  anything.
- Film motion with `wf-recorder`. A one-shot `grim` on an output nobody is
  watching returns a stale frame.
- Hold the wallpaper rotation for the whole run, not per phase. Starting the
  timer is itself a rotation trigger, so a per-phase resume changes the
  wallpaper under the next phase — measured, and it cost a whole A/B pass.
- Fixtures are **kitty**, never foot: an opaque application defeats compositor
  blur and makes every frost measurement ambiguous.

### When gates cannot see it

Some behaviour has no green gate — drag and drop is the standing example. Then
the evidence is an A/B against the parent commit:

1. Instrument both builds with the same dump (temporary, never committed).
2. Drive the **same** script on both. Coordinates are measured once and reused;
   a per-run measurement lets the two builds be driven differently, which is the
   one thing an A/B may not do.
3. Run each build **twice**. The same-build pair is the noise floor; a
   cross-build residual only means something below it.
4. Report the numbers, not the verdict.

`tests/unit/drop_table_test.cpp` is the other half of the same idea: pull the
decision out as a pure function and check it exhaustively off the compositor.

## Changing behaviour-preserving code

Refactors here have a specific failure mode. One previous extraction had every
moved function verified byte-identical and still regressed a gate, because
grouping the mutations together moved them relative to the draws between them.

So: **pure code motion and grouping, yes; execution-order changes, no.** If a
decomposition would force a mutation to happen before or after a draw it
previously followed, take the smaller decomposition and say so in the commit.
The same applies to reads that sit inside short-circuited conditions — a
refactor that quietly starts calling `layoutTarget()` on windows the old code
never asked about is not a refactor.

### Instruments are off by default, and the switch goes at the call site

Two of these exist, both read from the environment exactly once:

| variable | what it turns on |
| --- | --- |
| `HYPRDRIVE_PROFILE` | `Prof::` — keystroke-to-paint timing for the search path |
| `HYPRDRIVE_MEMBERSHIP_SHADOW` | `Membership::` — recomputes the cached half of the overview membership gate on every call and logs any disagreement to `~/.local/state/hyprdrive/membership-shadow.log`. This is how you re-prove the bump set after adding a mutator. |

Both are consulted from **inside the render pass**, so "off" has to mean a
predictable branch and not a cheaper function: the flag is checked at the CALL
SITE, in a header, so the out-of-line instrument is never entered. Measured, the
membership shadow costs a full recomputation plus a synchronous `fopen`/
`fprintf`/`fclose` per divergence — enough, when it was still unconditional, to
move three pixel gates from green to red purely by perturbing frame timing.
Anything new that instruments a hot path follows the same shape.

## Configuration keys

`docs/CONFIG.md` is generated. Add the key to `Config.cpp` — one `addKey(...)`
line with `KEY("tail")`, never a spelled-out `plugin:hyprdrive:` literal, because
the registration body is run **twice**: once under `plugin:hyprdrive:` and once
under the deprecated `plugin:scrolloverview:` prefix. Registering the alias from
the same expression is the point; a hand-copied twin drifts, and a default that
drifts between the two names is a compat layer that lies. Then:

```bash
scripts/gen_keys_table.sh          # rewrite docs/CONFIG.md
scripts/gen_keys_table.sh --check  # fails if it is stale
```

A key the settings page shows is written in two more places, and `make check`
holds them together:

- `ui/bin/hdpanel`, `KEY_META`: its type, the section whose "Restore defaults"
  owns it, and the default again. `tests/config_defaults_test.py` fails when
  that default differs from Config.cpp's.
- `ui/Schema.js`: the row (label, control, range). `tests/schema_keys_test.py`
  fails when a row names a key `KEY_META` does not have.

The hand-maintained table this fork inherited had two wrong defaults and covered
six of them. Do not reintroduce one.

## Config fragments and the raw-snippet block

`~/.config/hypr/hyprdrive/*.lua` is the surface the settings app writes through.
Every fragment is split by two marker lines:

```
-- BEGIN raw-snippet (hand edits below survive regeneration)
-- END raw-snippet
```

The contract, and it has exactly two clauses:

- **Everything outside the block is machine-owned** and is regenerated wholesale.
  Editing it is legal and pointless — the next write erases it.
- **Everything inside the block is user-owned** and is carried across verbatim,
  every regeneration, forever. Nothing generated ever lands inside it.

The block is **last** in the file because Lua's last assignment wins: a hand
edit that survives but is then overwritten by a generated line above it has not
survived in any sense the user cares about.

Keep the parse and the emit trivial: two line-anchored markers, no nesting, no
escaping, no format of its own. `scripts/fragment.py` is the entire
implementation and it should stay short enough to read in one sitting — a
richer format is a format that can lose somebody's config.

New fragments get a numeric prefix that puts them in dependency order, a header
comment naming which half is which, and a raw block even when it is empty. A
fragment without the markers is not a fragment; `fragment.py check` says so.

## The compat aliases expire

Phase 9 renamed every user-facing surface from `scrolloverview` to `hyprdrive`
and kept the old names working **for one release**. `Compat.hpp` carries the list; read it rather than
this paragraph, because the old name is deliberately spelled in several places
outside it — `OVERVIEW_SUBMAP_LEGACY` in `OverviewInternal.hpp`, the
`$SCROLLOVERVIEW_PROFILE` env var in `globals.hpp` and `SearchProviders.cpp`,
and the legacy thunks, the `scrolloverview-gesture` keyword and the second
registration pass in `Config.cpp`. When the release expires, delete
`Compat.{hpp,cpp}` and every one of those, plus the second IPC event,
`adoptLegacyFavorites()`, and `tests/gates/x53_compat_alias` — that gate is
the checklist that proves you got them all.

## Commits

- Small, one coherent change each.
- The message explains **why**, names what was measured, and names the evidence.
  "Fixes the drag" is not a commit message; "the proxy box is the
  mission-control slot, so descaling it by the overview scale alone commits a
  shrunken window" is.
- Deviations, scope-downs and things you deliberately did not do belong in the
  message. A decision that is not written down gets re-litigated.
- **No AI attribution anywhere** — not in commits, not in code, not in docs. No
  `Co-Authored-By`, no "generated with".
- Code comments and commit messages use `--` rather than an em dash. That is the
  existing convention throughout the source, and it survives a terminal.

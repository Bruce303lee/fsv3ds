# CLAUDE.md

Guidance for Claude Code (or any coding agent) working in this repo. See
[`README.md`](README.md) for what the project is; this file is about how
to work on it.

## The build/deploy loop

There is no local devkitARM toolchain in this environment. The loop is:

1. **Build remotely over SSH** on a devkitPro Ubuntu box. `DEVKITPRO`/
   `DEVKITARM` are not sourced by non-interactive SSH shells (`.bashrc`
   early-returns for non-interactive sessions) — export them explicitly
   in every SSH command:
   ```bash
   ssh user@host "export DEVKITPRO=/opt/devkitpro; export DEVKITARM=\$DEVKITPRO/devkitARM; \
     export PATH=\$DEVKITARM/bin:\$DEVKITPRO/tools/bin:\$PATH; cd ~/fsv3ds && make 2>&1"
   ```
   Sync the working tree there first (`rsync -az --delete`), then `scp`
   the resulting `fsv3ds.3dsx` back.
2. **Deploy over FTP** to the 3DS. The console can only run one homebrew
   app at a time, so the user has to manually switch it to an FTP server
   app before a new build can be pushed, then switch back to launch
   fsv3ds. Wait for the user to say the FTP app is up before pushing;
   don't assume it's running.
3. **Verify over RPC before asking the user to look at anything.**
   fsv3ds runs its own dev-only TCP control service (`source/rpc.c`,
   port 5151, documented in `RPC.md`) whenever it's running — no
   FTP-app-swap needed to use it, just the app itself running. Use it to
   scan, screenshot, read logs, and simulate button presses (`KEY A`,
   `KEY SELECT`, etc.) to confirm a change actually works before handing
   back to the user. A reference client lives outside this repo at
   `/home/kali/Projects/3ds/rpc_client.py`. This has repeatedly caught
   real bugs before they reached hardware — use it proactively, not just
   when asked to "test" something.

   One important gap: RPC's `KEY <NAME>` only simulates digital button
   presses, not circle-pad analog input or the physical 3D slider, and
   screenshots only capture the left eye as flat 2D (no stereo depth
   perception). Camera orbit feel, stereo pop-out direction/intensity,
   and animation smoothness need the user's actual hands/eyes on
   hardware — say so rather than guessing.

## Architecture landmarks

- `source/compat/` — a from-scratch minimal GLib subset (`GNode`,
  `GList`, `GMemChunk`, `GStringChunk`, plus `scandir()`/`alphasort()`
  since devkitARM/newlib doesn't guarantee them). devkitARM has no GLib;
  this exists so upstream fsv's tree/layout logic could port with light
  changes instead of a rewrite.
- `source/scanfs.c` — SD-card scanner, close to a 1:1 port of upstream's
  `scanfs.c`.
- `source/mapv.c` — the MapV (treemap) layout engine (ported from
  upstream `geometry.c`) plus Phase 3 navigation state (view root,
  selection, camera animation state machine) that upstream has no
  equivalent for (that logic lived in GTK event handlers there).
- `source/render.c` — citro3d rendering from scratch. Upstream's
  `ogl.c` is OpenGL; there is no OpenGL on 3DS, only the low-level
  PICA200 GPU API via citro3d.
- `source/dirtree_stub.c` — stubs upstream's GTK directory-tree widget
  query (`dirtree_entry_expanded()`) down to "is this the current view
  root" — there's no tree-widget UI, navigation happens by moving the
  camera between one fully-drawn directory level at a time instead.
- `source/rpc.c` — dev-only remote control service, see above.

## Gotchas worth knowing before touching render.c

- **citro3d's `*Tilt` projection matrices (`Mtx_PerspTilt`,
  `Mtx_PerspStereoTilt`) pre-rotate clip space** to match the 3DS
  screen's physically-rotated framebuffer. Their NDC output does *not*
  correspond to normal horizontal/vertical screen axes. citro2d expects
  normal (non-rotated) pixel coordinates and handles the physical
  rotation itself internally. Any CPU-side world-to-screen projection
  (e.g. for placing 2D labels) needs a **separate, non-Tilt** projection
  matrix (`Mtx_Persp`/`Mtx_PerspStereo`) — see `compute_view_projection`
  vs `compute_view_projection_notilt` in `render.c`.
- **Mixing raw citro3d draws with citro2d draws in the same frame**
  requires re-asserting your own citro3d pipeline state (shader
  program, attribute info, texenv, depth test) on every 3D draw call,
  not just once at init — citro2d's text pass rebinds its own GPU state
  and doesn't restore yours. See `draw_scene()`'s comment.
- **Always call `C2D_Flush()`** after a batch of citro2d draws, before
  the next citro3d draw or `C3D_FrameEnd()`. Missing this caused a real
  hang-after-sustained-use bug (citro2d's internal object batch just
  kept accumulating unflushed draws frame after frame).
- The 3DS's "RGB8" GSP framebuffer format is actually byte-order
  **B,G,R** in memory (`GSP_BGR8_OES`) — see `render_capture_rgb()`.
- GPU vertex buffers must live in **linear-accessible memory**
  (`linearAlloc`/`linearFree`), not the regular heap.

## Other real bugs already found and fixed (context if similar symptoms return)

- `g_node_destroy()` had a use-after-free: it freed each child without
  saving `->next` first, so it looped on a dangling pointer instead of
  advancing through siblings. Only triggered on a *second* scan (destroy
  + rebuild), never the first — Phase 1 testing never exercised tree
  teardown, so it shipped unnoticed.
- `node_absname()`'s double-slash collapse only checked the very start
  of the string (correct for a bare `/` root, upstream's only case) —
  missed it for a device-prefixed root like `sdmc:/`, where the doubled
  slash lands after `sdmc:`, not at position 0. Silently broke
  `FSUSER_OpenDirectory` (but not `OpenFile`), so every directory
  vanished from scans while files kept working — very confusing until
  diagnosed with a one-off stat-logging build.
- `main.c`'s physical SELECT (rescan) handler called `scanfs()` +
  `mapv_build_scene()` directly instead of going through
  `mapv_scan_and_build()`, forgetting the `view_root`/`selected_node`
  reset that has to happen between them (`scanfs()` frees the entire
  old tree). RPC's `SCAN` command used the correct wrapper, so RPC
  testing never caught this — a reminder that RPC-path parity with
  physical-input paths matters, not just RPC coverage existing.

## When adding new navigation/camera features

Camera state lives in `mapv.c`'s static `cam` plus the animation-phase
state machine (`CamAnimPhase`, `mapv_camera_tick()`). `cam.base_distance`
is the stable per-view-level reference (set once by `mapv_camera_init()`,
untouched by live zoom) — use it, not `cam.distance`, whenever you need
a scale-invariant reference (e.g. flooring a distance relative to "how
big is the current scene", not an absolute world-unit constant). An
absolute floor bit a real bug once (near-empty directories got a
microscopic treemap footprint, and a fixed-constant minimum zoom
distance put the camera almost inside them) — see `set_camera_focus_goal()`.

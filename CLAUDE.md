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
   the resulting `fsv3ds.3dsx` back. Password auth needs
   `sshpass -p ubuntu`; if `ssh`/`rsync`/`scp` hang indefinitely right
   after "Trying private key: ~/.ssh/id_rsa" (visible with `-v`) instead
   of ever offering password auth, this environment's ssh-agent has keys
   loaded that the remote box doesn't recognize and openssh stalls
   negotiating them before it'll fall back to password -- add
   `-o PreferredAuthentications=password -o PubkeyAuthentication=no` to
   force straight to password auth and skip the stall. (Also add
   `-o StrictHostKeyChecking=no` since it's a throwaway build VM.)
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
  upstream `geometry.c`) plus the camera-animation state machine that
  upstream has no equivalent for (that logic lived in GTK event
  handlers there).
- `source/treev.c` — the TreeV (cylindrical) layout engine, mirroring
  `mapv.c`'s structure and one-level-at-a-time design. See its header
  comment for why TreeV's ancestor-relative radius/angle math needed
  `dirtree_stub.c`'s rule widened (below) where MapV never did.
- `source/nav.c` — navigation identity (view root, selection) shared
  between MapV and TreeV, extracted out of mapv.c so drilling/
  selecting in one mode doesn't leave the other mode's state stale
  across a mode switch.
- `source/viz.c` — mode dispatcher (`VizMode`: `VIZ_MAPV`/`VIZ_TREEV`).
  main.c and rpc.c call through here (`viz_cycle_selection()` etc.)
  instead of calling `mapv_*`/`treev_*` directly, so a button press or
  RPC command doesn't need to know which mode is active. render.c
  still branches on `viz_get_mode()` itself (see `EyeInfo` in
  render.c) since C has no polymorphism for the differently-typed
  vertex/camera/label accessors.
- `source/render.c` — citro3d rendering from scratch. Upstream's
  `ogl.c` is OpenGL; there is no OpenGL on 3DS, only the low-level
  PICA200 GPU API via citro3d.
- `source/dirtree_stub.c` — stubs upstream's GTK directory-tree widget
  query (`dirtree_entry_expanded()`) down to "is this the current view
  root, or one of its ancestors" — there's no tree-widget UI,
  navigation happens by moving the camera between one fully-drawn
  directory level at a time instead. The ancestor-chain part (not just
  view_root itself) exists for TreeV (see treev.c's header comment);
  verified as a behavioral no-op for MapV, which only ever asks this
  about view_root's *children*.
- `source/color.c` — real per-extension file coloring via `fnmatch()`.
  Colors are looked up through a `ColorScheme`-indexed table
  (`color_set_scheme()`/`color_get_scheme()`) rather than one fixed
  palette, so settings.c's scheme picker can swap the whole palette
  without touching the pattern-matching logic.
- `source/settings.c` — user-facing settings (color scheme, label mode)
  persisted to `sdmc:/3ds/fsv3ds/settings.cfg`. The seam between ui.c
  (presentation) and the modules that actually apply a setting: its
  setters re-run `color_assign_recursive()` + `viz_rebuild()` themselves
  so ui.c doesn't need to know that mechanic.
- `source/ui.c` — the touch-driven bottom screen (status bar, button
  rail, content screens, footer breadcrumb). Owns its own C2D text
  buffer and draws into a `C3D_RenderTarget` that render.c creates and
  hands it once per frame (`ui_draw()`), same pattern as the top
  screen's stereo pair. `ui_handle_touch()` is the single entry point
  main.c (and rpc.c's `TOUCH` command) funnel taps through. The Folder
  screen is a real filesystem browser (`fsv3ds_scandir()` + `stat()`
  directly on `sdmc:/`, same low-level calls scanfs.c uses) rather than
  reusing nav.c's tree -- deliberately independent of scan state, so it
  works before any scan and can reach folders the current scan root
  never covered; "Use this" saves the chosen path as settings.c's
  `default_root` and hands off to `ui_scan_with_feedback()`, the shared
  helper (also used by main.c's SELECT handler and rpc.c's `SCAN`) that
  shows a one-frame "Scanning..." overlay before the blocking scanfs()
  walk -- added after the folder browser made it trivial to point a
  scan at something big enough to freeze the app with no feedback.
- `source/rpc.c` — dev-only remote control service, see above. Has a
  `MODE [MAPV|TREEV]` command for switching/querying the active
  visualization mode remotely — useful since there's no way to press
  the physical X button over RPC — and a `TOUCH x y` command so ui.c's
  touch UI stays RPC-testable the same way button input already was;
  `SHOT` takes an optional `BOTTOM` arg to capture the bottom screen
  instead of the top.

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
- **Never call `render_frame()` more than once per main-loop iteration**
  outside of `main()`'s own single call site. A "Scanning..." overlay
  feature briefly called `render_frame()` an extra time from inside an
  action handler (to present the overlay before a blocking `scanfs()`
  call), stacking 2-3 citro3d frame cycles within one call chain
  instead of the normal one-per-iteration. On a large enough scene this
  overflowed libctru's GPU command buffer (`GPUCMD_AddInternal` calling
  `svcBreak` panic — confirmed via a Luma3DS crash dump, see
  `luma3ds_exception_dump_parser` on GitHub for the real parser; the
  ARM11 dump's LR pointed straight at `GPUCMD_AddInternal` in
  `libctru/source/gpu/gpu.c`). Crashed the whole console (needed a
  physical restart), not just the app. Fixed by making the overlay
  deferred/2-phase (`ui_request_scan()` + `ui_process_pending_scan()`
  in `ui.c`, the latter called right after `main()`'s own
  `render_frame()`) instead of ever calling `render_frame()` from
  anywhere else. `rpc.c`'s `SCAN` command deliberately does NOT use
  this path at all — it calls `viz_scan_and_build()` directly, since an
  RPC client isn't watching the physical screen mid-round-trip anyway,
  so the overlay buys nothing there and it's one less place stacking
  extra frame cycles.

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
- TreeV's camera framing was sizing "how far back does the camera need
  to be to fit this tall spike" off the *horizontal* FOV. The 3DS top
  screen is landscape (400x240), so the actual vertical FOV is
  narrower than the horizontal one — using the wrong one underframes
  height specifically, which didn't show up in MapV (fixed, small
  box heights) but clipped TreeV's size-driven spikes (and their
  labels) off the top of frame. See `treev.c`'s `half_tan_vfov()`.
  General lesson: a "fill the frame" distance calc needs the
  axis-correct FOV (horizontal for width/depth, vertical for height),
  not the same FOV reused for both.
- `ui.c`'s folder browser (the Folder screen) originally computed its
  per-page entry capacity as `BROWSE_VISIBLE_ROWS - (has_up ? 1 : 0)`,
  with no headroom reserved for the "More" pagination indicator itself.
  At the SD card root (`has_up` false, real cards routinely have 10+
  top-level folders) a full page of real entries left "More" with
  nowhere to draw but the footer's row -- caught by RPC screenshot
  before it reached the user. Fixed by reserving one row
  unconditionally (`... - 1`); draw and touch-hit-test must compute
  `visible` identically or a tap that looks like it's on "More" can
  silently land on a folder entry instead.
- Not a bug in this app's code, but worth knowing: the 3DS's sdmc FAT
  driver returns `st_mtime == 0` for files/directories that plainly
  aren't from 1970 (first surfaced by ui.c's Info screen, the first
  place any of `NodeDesc`'s atime/mtime/ctime fields were ever
  displayed). `ui.c`'s `format_time()` shows "unknown" rather than the
  literal epoch date for `t == 0` -- if atime/ctime ever get surfaced
  somewhere too, they'll need the same guard.

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

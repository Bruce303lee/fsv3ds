# fsv3ds

A homebrew 3DS port of [**fsv**](https://github.com/mcuelenaere/fsv), the
"3D File System Visualizer" — the file-browser-as-a-cyberspace-city thing
from *Jurassic Park* (SGI's `fsn`). Point it at a folder on your SD card
and it lays out directories and files as a 3D treemap you fly around with
the circle pad, in stereoscopic 3D, on real 3DS hardware.

![platform](https://img.shields.io/badge/platform-Nintendo%203DS-blue)
![license](https://img.shields.io/badge/license-LGPL--2.1-lightgrey)

## Features

- **MapV treemap rendering** — directories and files as 3D boxes, sized
  by (subtree) byte size, colored by type
- **TreeV cylindrical rendering** — directories as curved wedge
  platforms fanned around a central axis, files as boxes standing on
  them, height driven by size. Toggle live with X.
- **Navigation** — drill into a directory, go back up, cycle selection
  within a row or jump to the row above/below, all with a cinematic
  zoom-out/pan/zoom-in camera move
- **Real per-extension file coloring** — ROMs green, homebrew installables
  gold, archives red, images magenta, audio/video cyan, text/config grey,
  directories tan (`source/color.c`)
- **Stereoscopic 3D** — full autostereoscopic rendering using the
  console's own 3D slider, not a fake 2D depth trick
- **Live labels** — the selected node's name, drawn in 3D space with a
  legibility shadow, scaled by actual camera distance; can be switched to
  label every visible node instead of just the selection (Settings)
- **Touch-driven bottom screen** (`source/ui.c`) — an iOS-ish status bar
  (battery/wifi), a left button rail (Folder / Settings / Info / Log), and
  a footer breadcrumb. Folder opens a real folder browser starting at the
  SD card root (independent of whatever's currently scanned -- reads the
  filesystem directly, so it works before any scan too), with paging and
  a ".." row; "Use this" rescans starting at whatever folder you land on.
  Settings offers a color scheme preset picker (Default / High-Contrast /
  Monochrome) and a label-mode toggle (Selected only / All / Off), both
  persisted to `sdmc:/3ds/fsv3ds/settings.cfg`; Info shows details for the
  selected node; Log shows the same scrollback the RPC `LOG` command
  streams.
- **A dev-only remote control service** — see [`RPC.md`](RPC.md). Not
  something the app needs to run; a debugging aid so scans, screenshots,
  button presses, and touch taps can be driven over the network during
  development.

## Controls

| Input | Action |
|---|---|
| Circle Pad | Orbit the camera |
| L / R (held) | Zoom out / in |
| D-pad Left/Right | Cycle selection within the current view |
| D-pad Up/Down | Jump to the nearest block in the row above/below (MapV only) |
| A | Drill into the selected directory |
| B | Go back up to the parent |
| X | Toggle MapV / TreeV |
| Y | Screenshot → `sdmc:/fsv3ds_screenshot.ppm` |
| SELECT | Rescan `sdmc:/3ds` from the top |
| START | Exit |
| Touch (bottom screen) | Folder/Settings/Info/Log rail, settings toggles |

## Building

Requires [devkitPro](https://devkitpro.org/) with the `3ds-dev` package
group (devkitARM, libctru, citro3d, citro2d):

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH

make
```

Produces `fsv3ds.3dsx`. Copy it to `/3ds/` on your SD card and launch it
from the Homebrew Launcher.

By default it scans `sdmc:/3ds` (see `SCAN_ROOT` in `source/main.c` —
change it and rebuild to point at a different folder).

## Status

A ground-up C/citro3d rewrite of upstream fsv's rendering and platform
layer, reusing its layout algorithms and general design where they carry
over cleanly. Both of upstream's main views are implemented — **MapV**
(treemap) and **TreeV** (cylindrical), switchable live with X, sharing
the same navigation state (`source/nav.c`) and dispatched through
`source/viz.c`. The bottom screen is a touch-driven UI (`source/ui.c`,
`source/settings.c`) rather than upstream's GTK dialogs/dirtree widget.
Not yet ported: TreeV's folder-tab decoration and inter-platform
branch/loop connectors (upstream-only cosmetic details), and full
custom per-category color picking (color schemes are presets, not
arbitrary RGB).

## Why does this exist / how was it built

This is an from-scratch platform port, not a recompile: 3DS homebrew has
no OpenGL (rendering goes through citro3d, the low-level PICA200 GPU
API), no GLib (a minimal compatible subset — `GNode`, `GList`,
`GMemChunk`, `GStringChunk` — lives in `source/compat/`), and no desktop
windowing toolkit (there's no GTK equivalent; the whole UI is direct
citro2d/citro3d drawing plus button input). Upstream's layout math
(`geometry.c`'s MapV algorithm) and general node/tree model ported
across with only light changes; everything touching GTK, OpenGL, or the
desktop filesystem's Unix-specific bits (owner/group, `pwd.h`) did not
and was rewritten or dropped.

## License

fsv3ds is a derivative work of fsv, which is licensed under the
**GNU Lesser General Public License v2.1** — see [`COPYING`](COPYING).
`source/compat/scandir_compat.c` is adapted from glibc (also LGPL).

fsv itself: Copyright © 1999 Daniel Richard G. \<skunk@mit.edu\>, with
this fork/mirror maintained at
[mcuelenaere/fsv](https://github.com/mcuelenaere/fsv).

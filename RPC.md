# fsv3ds RPC service

A development-only remote control port built into fsv3ds (`source/rpc.c`,
`source/rpc.h`). It lets you trigger scans, pull screenshots, read log
output, and simulate button presses over the network instead of
physically operating the console or shuttling files over FTP.

**Not a shipped feature**, but deliberately left in the release build --
still a plaintext TCP port, but every request now requires a username and
password (`settings.c`'s `rpc_user`/`rpc_pass`, stored in
`settings.cfg`), so it's not wide open to anything else on the same LAN.
Change the defaults by editing `settings.cfg`'s `rpc_user=`/`rpc_pass=`
lines over FTP and relaunching. Credentials travel in plaintext (no
TLS), so this is still only appropriate on a trusted home LAN, not the
open internet.

## Status: ready to connect

The service starts automatically when fsv3ds launches (`rpc_init()` is
called right after `render_init()` in `main.c`). As long as fsv3ds is the
app currently running on the console, the port is listening.

- **Host**: the 3DS's IP on your LAN (currently `192.168.13.142`)
- **Port**: `5151`
- **Transport**: raw TCP, one command per connection

## Why this exists

Before this, every debug cycle meant swapping between two homebrew apps
on the console (the FTP server, to move files; fsv3ds itself, to see
anything) and manually pressing buttons on the handheld. That's fine for
deploying a new build (still requires FTP — RPC can't replace the running
binary), but painfully slow for the "change camera angle, rebuild, look
at it, repeat" loop that Phase 2 needed. With RPC, everything except
*deploying new code* — scanning, screenshotting, reading what the app
logged — happens without touching the handheld at all.

## Protocol

Connect, send one line, read the response, connection closes. Simple
request/response, no persistent session state -- which is exactly why
credentials travel with every single request rather than a one-time
login: there's no session for a login to attach to.

Every line has the form:

```
<user> <pass> <COMMAND> [args...]
```

e.g. `fsv3ds fsv3ds-2026 PING`. A bad or missing user/pass gets back
`ERR unauthorized` before the real command is even looked at; a
malformed line (missing the user/pass fields entirely) gets back
`ERR usage: <user> <pass> <COMMAND> [args...]`. Everything below omits
the `<user> <pass>` prefix for brevity -- add it to every example.

Text-reply commands respond with a single line and close:

| Command | Reply | What it does |
|---|---|---|
| `PING` | `PONG` | Liveness check |
| `SCAN` | `OK vertices=<n>` | Rescans the current default folder (`settings.c`'s `default_root`, same path physical SELECT uses -- set via the Folder screen's "Use this"), then renders one frame so a subsequent `SHOT` reflects it immediately |
| `MODE [MAPV\|TREEV]` | `OK mode=<MAPV\|TREEV>` | With an arg, switches the active visualization mode (there's no way to press the physical X button over RPC); with no arg, just reports the current mode |
| `KEY <NAME>` | `OK` or `ERR unknown key` | Injects a synthetic button press, merged into `hidKeysDown()` for exactly one frame. Names: `A B X Y L R START SELECT UP DOWN LEFT RIGHT` |
| `TOUCH <x> <y>` | `OK` or `ERR usage: TOUCH x y` | Injects a synthetic tap at bottom-screen pixel coordinates (0-319, 0-239), consumed by `ui_handle_touch()` on the next frame -- the only way to drive the touch UI (Folder/Settings/Info/Log rail, settings toggles) without physically tapping the console. One tap per command, no drag/gesture support. |

Binary-reply commands send a one-line header, then exactly that many raw
bytes, then close:

| Command | Header | Payload |
|---|---|---|
| `SHOT` | `PPM <size>\n` | A complete binary PPM (P6) file — 400x240 top-screen capture, un-rotated and color-corrected, ready to save straight to disk |
| `SHOT BOTTOM` | `PPM <size>\n` | Same, but a 320x240 capture of the bottom screen (the touch UI) instead -- always mono, no stereo there |
| `LOG` | `LOG <size>\n` | The contents of the in-memory log ring buffer (4KB, fills once and stops — restart the app to clear it). The same content is shown live on the bottom screen's Log panel. |

Unrecognized commands (after auth succeeds) get back `ERR unknown command`.

### What ends up in the log

Not every `printf()` in the app — just the summary lines, routed through
`rpc_logf()` (which both prints to the console *and* appends to the ring
buffer) instead of plain `printf()`:
- `rpc: listening on ...` (on startup)
- `scan complete: N nodes` + per-type breakdown
- `cam: root ...` / `cam: d1=... d2=... dist=...` / `cam: near=... far=...`
  (camera framing diagnostics, added while debugging the "thin line"
  rendering bug)
- `mapv: N nodes, M vertices`
- `screenshot -> sdmc:/fsv3ds_screenshot.ppm` (when Y is pressed physically)

High-frequency chatter (the per-directory `scanning: sdmc:/...` lines
during a scan) intentionally stays plain `printf()` — console-only, not
logged — so the ring buffer doesn't fill with noise on a single scan.

## Client

`rpc_client.py` (repo root's parent, `/home/kali/Projects/3ds/rpc_client.py`)
is a minimal reference client. It prepends the credentials (`RPC_USER`/
`RPC_PASS` constants at the top of the script, matching `settings.c`'s
defaults) to every command automatically, so callers just pass the
command itself:

```bash
python3 rpc_client.py PING
python3 rpc_client.py SCAN
python3 rpc_client.py SHOT              # writes rpc_shot.ppm in the cwd
python3 rpc_client.py SHOT BOTTOM       # writes rpc_shot_bottom.ppm instead
python3 rpc_client.py LOG
python3 rpc_client.py KEY A             # simulate pressing A next frame
python3 rpc_client.py TOUCH 36 94       # simulate tapping bottom-screen (36,94)
```

Or talk to it directly with netcat for one-off text commands (remember
the credentials prefix -- this won't work without it):

```bash
printf 'fsv3ds fsv3ds-2026 PING\n' | nc -q1 192.168.13.142 5151
```

(netcat is awkward for `SHOT`/`LOG` since you'd have to parse the binary
header yourself — use the Python client for those.)

## Limitations / implementation notes

- **One command at a time.** Each connection handles exactly one command
  then closes. No pipelining, no persistent session.
- **Runs on the main thread**, not a background thread. `rpc_poll()` is
  called once per frame from the main loop, right after `render_frame()`.
  This is deliberate: citro3d GPU calls and the scan/screenshot logic all
  need to happen on the main thread anyway, so keeping RPC there avoids
  any cross-thread locking. The cost is that a slow/stuck client can
  stall rendering — bounded to 3 seconds via `select()` in `recv_line()`
  (devkitARM's SOC service doesn't support `SO_RCVTIMEO`, so the timeout
  is hand-rolled).
- **Deploying new code still needs FTP.** RPC can drive the *running*
  binary but obviously can't replace itself — after any rebuild, the
  cycle is: switch the console back to the FTP app, push the new
  `.3dsx`, relaunch fsv3ds, and RPC is back up automatically.

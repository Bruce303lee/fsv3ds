/* launcher.h - fsv3ds Phase 9
 *
 * Launching another homebrew .3dsx file. See launcher.c for why this
 * needs raw IPC to Luma3DS/Rosalina's "hb:ldr" port rather than any
 * normal title-launch API.
 */
#ifndef FSV3DS_LAUNCHER_H
#define FSV3DS_LAUNCHER_H

#include "common.h"

/* TRUE if this process has access to Luma3DS/Rosalina's "hb:ldr"
 * homebrew-loader IPC port -- checked once, lazily, and cached. FALSE
 * under any other launch method (old CFW, direct title install,
 * etc.), in which case launcher_launch() will always fail too; check
 * this first to decide whether to offer a "Launch" UI action at all. */
gboolean launcher_available( void );

/* Tells hb:ldr to load `path` (an absolute sdmc:/ path to a .3dsx)
 * once this process exits, with a single argv entry (`path` itself,
 * matching what a normally-launched .3dsx sees as its own argv[0]).
 * Returns FALSE if hb:ldr is unavailable or rejects the request (bad
 * path, IPC error) -- does NOT exit the process either way. On a TRUE
 * return, the caller must let its own main loop end on its own (same
 * cleanup path as a normal exit) for hb:ldr to actually take over. */
gboolean launcher_launch( const char *path );

#endif /* FSV3DS_LAUNCHER_H */

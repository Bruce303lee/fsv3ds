/* rpc.h - fsv3ds development-only remote control service
 *
 * NOT part of the upstream fsv port -- a debugging aid so scans,
 * screenshots, and button presses can be driven over the network
 * instead of physically handling the console. Opens a plaintext,
 * unauthenticated TCP command port; only ever intended to run on a
 * private home LAN while actively developing, never as a shipped
 * feature.
 */
#ifndef FSV3DS_RPC_H
#define FSV3DS_RPC_H

#include <3ds.h>

#include "compat/gtypes.h"

void rpc_init( void );
void rpc_fini( void );

/* Call once per main-loop iteration. Accepts a connection if one is
 * waiting, reads a single command line, executes it synchronously
 * (scans/screenshots run right there on the main thread -- same
 * thread citro3d needs), replies, and closes. Never blocks longer
 * than a couple of seconds even against a stuck client. */
void rpc_poll( void );

/* Bitmask of keys the last-handled "KEY <NAME>" command asked to
 * simulate; OR this into hidKeysDown()'s result once, then it self-clears. */
u32 rpc_take_injected_keys( void );

/* One-shot injected touch from a "TOUCH x y" command, mirroring
 * rpc_take_injected_keys() -- so ui.c's touch-driven bottom screen
 * stays testable over RPC the way button input already is (no
 * circle-pad/3D-slider equivalent exists here, just a single tap
 * point; there's no gesture/drag support, one tap per call). Returns
 * TRUE and fills the x/y outputs if a touch was injected since the
 * last call, FALSE (leaving them untouched) otherwise. */
gboolean rpc_take_injected_touch( int *x, int *y );

/* printf() + append to an in-memory ring buffer retrievable via the
 * "LOG" command. Use for summary/status lines, not per-item chatter
 * (e.g. not the per-directory scan progress -- that stays plain printf). */
__attribute__((format(printf, 1, 2)))
void rpc_logf( const char *fmt, ... );

/* Read-only access to the same ring buffer the "LOG" command streams
 * over the network, for ui.c's on-console Log screen -- so the bottom
 * screen shows the identical scrollback an RPC client would see.
 * *out_len receives the buffer's current length; the returned pointer
 * is valid until the next rpc_logf() call. */
const char *rpc_log_buffer( size_t *out_len );

#endif /* FSV3DS_RPC_H */

/* launcher.c - see launcher.h
 *
 * Launching another .3dsx isn't a normal OS-level "start this title"
 * call -- a loose .3dsx on the SD card has no titleID, so none of
 * APT's title-launch/chainload APIs (aptSetChainloader() etc., which
 * all take a titleID) apply to it. On Luma3DS, the actual homebrew
 * loader is a persistent low-level stub ("hb:ldr") that fsv3ds itself
 * is running *inside of* -- .3dsx "apps" are just code blobs it loads
 * and jumps to, and when that code's main() returns, control comes
 * back to the stub, which checks (via this IPC port) whether a new
 * target was set and loads that instead of exiting for real or
 * falling back to hbmenu.
 *
 * This is exactly the mechanism devkitPro's own hbmenu uses for its
 * "Rosalina" loader backend (source/loaders/rosalina.c in the
 * devkitPro/3ds-hbmenu repo) -- ported here close to verbatim (same
 * two raw IPC commands, same argv buffer layout) rather than
 * reinvented, since hb:ldr's wire protocol isn't documented anywhere
 * else and hbmenu's source is the reference implementation every
 * other homebrew launcher on this platform is built against.
 */
#include <3ds.h>
#include <string.h>

#include "launcher.h"

static Handle hbldr_handle = 0;
static gboolean hbldr_checked = FALSE;
static gboolean hbldr_ok = FALSE;


static gboolean
ensure_hbldr( void )
{
	if (!hbldr_checked) {
		hbldr_checked = TRUE;
		hbldr_ok = R_SUCCEEDED( svcConnectToPort( &hbldr_handle, "hb:ldr" ) );
	}
	return hbldr_ok;
}


gboolean
launcher_available( void )
{
	return ensure_hbldr( );
}


/* IPC command 0x20002: sets the .3dsx path hb:ldr will load next.
 * `path` must NOT include the "sdmc:" device prefix -- hb:ldr wants a
 * plain archive-relative path (e.g. "/3ds/foo/foo.3dsx"). */
static Result
hbldr_set_target( const char *path )
{
	u32 path_len = (u32)strlen( path ) + 1;
	u32 *cmdbuf = getThreadCommandBuffer( );
	Result rc;

	cmdbuf[0] = IPC_MakeHeader( 2, 0, 2 );
	cmdbuf[1] = IPC_Desc_StaticBuffer( path_len, 0 );
	cmdbuf[2] = (u32)path;

	rc = svcSendSyncRequest( hbldr_handle );
	if (R_SUCCEEDED(rc))
		rc = (Result)cmdbuf[1];
	return rc;
}


/* IPC command 0x30002: sets argv for the next launch. Buffer layout
 * (matches libctru's __system_initArgv() on the read side): a
 * leading u32 argc, followed by that many NUL-terminated strings
 * packed back to back with no padding between them. Always a single
 * argv[0] entry here -- the path itself, same as what a normally
 * double-clicked .3dsx sees as its own argv[0]; fsv3ds has no reason
 * to pass anything else along. */
#define LAUNCH_ARGBUF_SIZE 1024

static Result
hbldr_set_argv( const char *path )
{
	u32 argbuf[LAUNCH_ARGBUF_SIZE / sizeof(u32)];
	size_t len = strlen( path ) + 1;
	u32 *cmdbuf;
	Result rc;

	if (len >= LAUNCH_ARGBUF_SIZE - sizeof(u32))
		return -1; /* pathologically long path -- refuse rather than overflow */

	argbuf[0] = 1; /* argc */
	memcpy( &argbuf[1], path, len );

	cmdbuf = getThreadCommandBuffer( );
	cmdbuf[0] = IPC_MakeHeader( 3, 0, 2 );
	cmdbuf[1] = IPC_Desc_StaticBuffer( sizeof(argbuf), 1 );
	cmdbuf[2] = (u32)argbuf;

	rc = svcSendSyncRequest( hbldr_handle );
	if (R_SUCCEEDED(rc))
		rc = (Result)cmdbuf[1];
	return rc;
}


gboolean
launcher_launch( const char *path )
{
	const char *target = path;

	if (!ensure_hbldr( ))
		return FALSE;

	if (!strncmp( target, "sdmc:/", 6 ))
		target += 5; /* hb:ldr wants the path without the device prefix */

	if (R_FAILED( hbldr_set_target( target ) ))
		return FALSE;
	/* argv[0] keeps the "sdmc:/" prefix -- matches what a normal
	 * launch's own argv[0] looks like, unlike hbldr_set_target()'s arg. */
	if (R_FAILED( hbldr_set_argv( path ) ))
		return FALSE;

	return TRUE;
}

/* end launcher.c */

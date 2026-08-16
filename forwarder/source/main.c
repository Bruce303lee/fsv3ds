/* fsv3ds forwarder - a minimal native CIA whose entire job is to
 * chainload sdmc:/3ds/fsv3ds/fsv3ds.3dsx via Luma3DS/Rosalina's
 * "hb:ldr" IPC port, then exit -- installed as its own title so it
 * gets a Home Menu icon, unlike a loose .3dsx which only the
 * Homebrew Launcher can run.
 *
 * Same two raw IPC calls as fsv3ds's own source/launcher.c (which is
 * itself ported from devkitPro/3ds-hbmenu's Rosalina loader backend)
 * -- duplicated here rather than shared, since this is a separate,
 * intentionally tiny build target with no dependency on the rest of
 * fsv3ds's source tree.
 *
 * The one thing this specific build is for verifying on hardware:
 * whether hb:ldr is reachable from a process launched as its own
 * installed CIA title (Home Menu -> this icon), not just from code
 * running inside a Homebrew-Launcher-hosted .3dsx the way fsv3ds
 * itself normally does. If hb:ldr turns out to be a persistent,
 * system-wide Luma3DS service (the expectation going in), this
 * should just work; if not, svcConnectToPort() below fails and this
 * exits having done nothing.
 */
#include <3ds.h>
#include <string.h>

#define TARGET_PATH "/3ds/fsv3ds/fsv3ds.3dsx"       /* no "sdmc:" prefix -- see hbldr_set_target() */
#define TARGET_ARGV "sdmc:/3ds/fsv3ds/fsv3ds.3dsx"   /* argv[0] keeps it, matching a normal launch */


static Result
hbldr_set_target( Handle h, const char *path )
{
	u32 path_len = (u32)strlen( path ) + 1;
	u32 *cmdbuf = getThreadCommandBuffer( );
	Result rc;

	cmdbuf[0] = IPC_MakeHeader( 2, 0, 2 );
	cmdbuf[1] = IPC_Desc_StaticBuffer( path_len, 0 );
	cmdbuf[2] = (u32)path;

	rc = svcSendSyncRequest( h );
	if (R_SUCCEEDED(rc))
		rc = (Result)cmdbuf[1];
	return rc;
}


static Result
hbldr_set_argv( Handle h, const char *path )
{
	u32 argbuf[256];
	size_t len = strlen( path ) + 1;
	u32 *cmdbuf;
	Result rc;

	if (len >= sizeof(argbuf) - sizeof(u32))
		return -1;

	argbuf[0] = 1; /* argc */
	memcpy( &argbuf[1], path, len );

	cmdbuf = getThreadCommandBuffer( );
	cmdbuf[0] = IPC_MakeHeader( 3, 0, 2 );
	cmdbuf[1] = IPC_Desc_StaticBuffer( sizeof(argbuf), 1 );
	cmdbuf[2] = (u32)argbuf;

	rc = svcSendSyncRequest( h );
	if (R_SUCCEEDED(rc))
		rc = (Result)cmdbuf[1];
	return rc;
}


int
main( void )
{
	Handle hbldr;

	if (R_SUCCEEDED( svcConnectToPort( &hbldr, "hb:ldr" ) )) {
		hbldr_set_target( hbldr, TARGET_PATH );
		hbldr_set_argv( hbldr, TARGET_ARGV );
		svcCloseHandle( hbldr );
	}

	return 0;
}

/* end main.c */

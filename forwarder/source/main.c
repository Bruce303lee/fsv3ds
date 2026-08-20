/* fsv3ds forwarder - a minimal native CIA whose entire job is to
 * chainload sdmc:/3ds/fsv3ds/fsv3ds.3dsx via Luma3DS's "hb:ldr" system,
 * then exit -- installed as its own title so it gets a Home Menu icon,
 * unlike a loose .3dsx which only the Homebrew Launcher can run.
 *
 * Ground-truthed against Luma3DS's own loader sysmodule source
 * (sysmodules/loader/source/hbldr.c/loader.c in github.com/LumaTeam/
 * Luma3DS) after two wrong guesses that cost real hardware round-trips
 * -- worth recording precisely instead of half-remembering next time:
 *
 * hb:ldr's SetTarget/SetArgv (same two raw IPC commands as fsv3ds's
 * own source/launcher.c, duplicated here since this is a separate tiny
 * build target) just write into a single global pending-target buffer
 * inside the loader sysmodule -- ANY process can call them, no
 * permission check, confirmed on hardware. But writing that buffer does
 * *nothing* by itself. The buffer is only ever consulted inside
 * hbldrLoadProcess(), which Loader calls in place of a normal NCCH
 * load *only* when the specific title ID being launched matches
 * Luma_SharedConfig->hbldr_3dsx_tid (hbldrIs3dsxTitle() in hbldr.c) --
 * i.e. the one reserved "Homebrew Launcher" title slot every hacks.
 * guide-based Luma3DS setup has (sd:/luma/config.ini's
 * hbldr_3dsx_titleid, default 0004000000D921E00 -- yes, that's a
 * distinct, separate, pre-existing Home Menu icon, not this one).
 * Nothing about a foreign process merely exiting causes Loader to
 * launch that title on its own.
 *
 * So after setting the target, this forwarder must *itself* ask Home
 * Menu to launch that reserved title in its place -- the standard APT
 * mechanism an application uses to hand off to a different title
 * (3dbrew's NS_and_APT_Services page: "APT:PrepareToDoApplicationJump
 * and APT:DoApplicationJump are used by applications, for launching
 * native/non-NATIVE_FIRM applications... notify Home Menu that title
 * launching needs done, Home Menu does the actual title launching").
 * That jump is what actually lands back in hbldrLoadProcess() with our
 * pending target now populated.
 *
 * Also load-bearing (not obvious from this file alone): assets/app.rsf
 * needs Checkpoint's full FileSystemAccess/IoAccessControl/
 * ServiceAccessControl/Dependency lists, not a hand-trimmed subset --
 * an incomplete ACI here doesn't fail cleanly, it hangs the whole
 * console during process bring-up (confirmed on hardware, needed a
 * hard reset, no crash dump since no exception ever fires).
 */
#include <3ds.h>
#include <string.h>

#define TARGET_PATH "/3ds/fsv3ds/fsv3ds.3dsx"       /* no "sdmc:" prefix -- see hbldr_set_target() */
#define TARGET_ARGV "sdmc:/3ds/fsv3ds/fsv3ds.3dsx"   /* argv[0] keeps it, matching a normal launch */

/* The one reserved "Homebrew Launcher" title Luma3DS's loader
 * substitutes hb:ldr's stub for -- sd:/luma/config.ini's
 * hbldr_3dsx_titleid, default value used by every hacks.guide-based
 * setup (see the file header comment). Not configurable per-app; if a
 * user has customized their own config.ini this would need to match
 * that instead. */
#define HB_LAUNCHER_TITLE_ID 0x000400000D921E00ULL


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
	Result target_rc = -1, argv_rc = -1;

	if (R_SUCCEEDED( svcConnectToPort( &hbldr, "hb:ldr" ) )) {
		target_rc = hbldr_set_target( hbldr, TARGET_PATH );
		argv_rc = hbldr_set_argv( hbldr, TARGET_ARGV );
		svcCloseHandle( hbldr );
	}

	if (R_SUCCEEDED( target_rc ) && R_SUCCEEDED( argv_rc )) {
		if (R_SUCCEEDED( APT_PrepareToDoApplicationJump( 0, HB_LAUNCHER_TITLE_ID, MEDIATYPE_SD ) ))
			APT_DoApplicationJump( NULL, 0, NULL );
	}

	return 0;
}

/* end main.c */

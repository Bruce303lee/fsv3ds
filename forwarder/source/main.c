/* fsv3ds forwarder - a minimal native CIA whose entire job is to
 * chainload sdmc:/3ds/fsv3ds.3dsx via Luma3DS's "hb:ldr" system, then
 * exit -- installed as its own title so it gets a Home Menu icon,
 * unlike a loose .3dsx which only the Homebrew Launcher can run.
 *
 * Target path is flat sdmc:/3ds/fsv3ds.3dsx, matching where every
 * other homebrew .3dsx sits (and where Universal-Updater's generic
 * .3dsx-extension install action actually places it) -- NOT
 * sdmc:/3ds/fsv3ds/fsv3ds.3dsx, which was this project's own manual-
 * FTP-deploy convention and doesn't match the wider ecosystem. Mixing
 * the two up once already caused a real crash: Loader (Luma3DS's own
 * system module, not this forwarder or fsv3ds) took an "undefined
 * instruction" exception while handling a title-jump into the
 * reserved Homebrew Launcher slot for a target path that didn't
 * exist. A missing-file failure was already confirmed to fail
 * *gracefully* (bounce to Home Menu, no dump) once earlier in this
 * project -- this crash only appeared after the aptMainLoop-wait fix
 * below changed the timing of this process's own exit, suggesting a
 * genuine race/edge case in Loader itself when a title-jump target
 * fails to load while the *calling* process is still winding down.
 * Not fully root-caused inside Luma3DS's own code, and not going to
 * be -- the fix here is simply to never hand Loader a path that isn't
 * actually there (see the stat check in main()), which sidesteps the
 * system-level edge case entirely regardless of its exact cause.
 * fsv3ds's own settings.cfg still lives in sdmc:/3ds/fsv3ds/ as its
 * private config directory (source/settings.c) -- unrelated, and
 * unaffected by this.
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
 *
 * One more thing that isn't obvious from reading this file top to
 * bottom: after a successful DoApplicationJump below, this process
 * must NOT fall through to a normal voluntary exit. DoApplicationJump
 * terminates the calling process itself, asynchronously, via Home
 * Menu/PM -- letting our own code immediately return from main() (and
 * run libctru's default aptExit()/srvExit() teardown) races that
 * self-exit against the external termination the jump just triggered.
 * That race doesn't fail every time -- it showed up as launches
 * getting progressively worse on repeat (1st OK, 2nd crashed to Home
 * Menu, 3rd froze the whole console), confirmed as forwarder-specific
 * (fsv3ds itself relaunched cleanly 3x in a row via the physical
 * Homebrew Launcher, which never goes through this jump at all).
 * Fixed by looping on aptMainLoop() instead, which returns false once
 * Home Menu/PM actually want this process closed -- let APT drive the
 * termination instead of contending with it.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define TARGET_PATH "/3ds/fsv3ds.3dsx"       /* no "sdmc:" prefix -- see hbldr_set_target() */
#define TARGET_ARGV "sdmc:/3ds/fsv3ds.3dsx"   /* argv[0] keeps it, matching a normal launch */

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
	FILE *probe;

	/* Never hand Loader a target that isn't there -- see file header
	 * for the crash this avoids. */
	probe = fopen( "sdmc:" TARGET_PATH, "rb" );
	if (!probe)
		return 0;
	fclose( probe );

	if (R_SUCCEEDED( svcConnectToPort( &hbldr, "hb:ldr" ) )) {
		target_rc = hbldr_set_target( hbldr, TARGET_PATH );
		argv_rc = hbldr_set_argv( hbldr, TARGET_ARGV );
		svcCloseHandle( hbldr );
	}

	if (R_SUCCEEDED( target_rc ) && R_SUCCEEDED( argv_rc )) {
		if (R_SUCCEEDED( APT_PrepareToDoApplicationJump( 0, HB_LAUNCHER_TITLE_ID, MEDIATYPE_SD ) )) {
			if (R_SUCCEEDED( APT_DoApplicationJump( NULL, 0, NULL ) )) {
				/* Let APT drive our termination -- see file header. */
				while (aptMainLoop( ))
					svcSleepThread( 50000000LL ); /* 50ms */
			}
		}
	}

	return 0;
}

/* end main.c */

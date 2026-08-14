/* scandir_compat.h - portable scandir()/alphasort(), for platforms
 * (like devkitARM/newlib) that don't provide the BSD scandir() extension.
 * Ported from fsv's own lib/scandir.c (glibc-derived). */
#ifndef FSV3DS_SCANDIR_COMPAT_H
#define FSV3DS_SCANDIR_COMPAT_H

#include <dirent.h>

int fsv3ds_scandir( const char *dir, struct dirent ***namelist,
	int (*select)( const struct dirent * ),
	int (*cmp)( const void *, const void * ) );
int fsv3ds_alphasort( const void *a, const void *b );

#endif /* FSV3DS_SCANDIR_COMPAT_H */

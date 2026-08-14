/* scandir_compat.c - see scandir_compat.h
 *
 * Adapted from fsv's lib/scandir.c, itself "a slightly edited
 * amalgamation of dirent/scandir.c and dirent/alphasort.c from
 * glibc-2.1.2" (LGPL, Free Software Foundation). Renamed to avoid any
 * ambiguity with a libc-provided scandir() on some future target.
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "scandir_compat.h"

#define _D_EXACT_NAMLEN(d) (strlen( (d)->d_name ))
#define _D_ALLOC_NAMLEN(d) (sizeof (d)->d_name > 1 ? sizeof (d)->d_name : \
                            _D_EXACT_NAMLEN(d) + 1)

int
fsv3ds_scandir( const char *dir, struct dirent ***namelist,
	int (*select)( const struct dirent * ),
	int (*cmp)( const void *, const void * ) )
{
	DIR *dp = opendir( dir );
	struct dirent **v = NULL;
	size_t vsize = 0, i;
	struct dirent *d;
	int save;

	if (dp == NULL)
		return -1;

	save = errno;
	errno = 0;

	i = 0;
	while ((d = readdir( dp )) != NULL) {
		if (select == NULL || (*select)( d )) {
			struct dirent *vnew;
			size_t dsize;

			errno = 0;

			if (i == vsize) {
				struct dirent **newv;

				vsize = (vsize == 0) ? 10 : vsize * 2;
				newv = (struct dirent **)realloc( v, vsize * sizeof(*v) );
				if (newv == NULL)
					break;
				v = newv;
			}

			dsize = &d->d_name[_D_ALLOC_NAMLEN(d)] - (char *)d;
			vnew = (struct dirent *)malloc( dsize );
			if (vnew == NULL)
				break;

			v[i++] = (struct dirent *)memcpy( vnew, d, dsize );
		}
	}

	if (errno != 0) {
		save = errno;
		closedir( dp );
		while (i > 0)
			free( v[--i] );
		free( v );
		errno = save;
		return -1;
	}

	closedir( dp );
	errno = save;

	if (cmp != NULL)
		qsort( v, i, sizeof(*v), cmp );
	*namelist = v;

	return (int)i;
}


int
fsv3ds_alphasort( const void *a, const void *b )
{
	return strcmp( (*(const struct dirent **)a)->d_name,
	                (*(const struct dirent **)b)->d_name );
}

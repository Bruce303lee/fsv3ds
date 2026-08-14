/* dirtree_stub.c - see dirtree_stub.h */
#include "common.h"
#include "dirtree_stub.h"
#include "mapv.h"

boolean
dirtree_entry_expanded( GNode *dnode )
{
	return (dnode == mapv_view_root( )) ? TRUE : FALSE;
}

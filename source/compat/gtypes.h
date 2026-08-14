/* gtypes.h - minimal GLib-style type shim for the 3DS port of fsv.
 * devkitARM/newlib has no GLib, so upstream fsv's pervasive use of
 * gboolean/GList/GNode/etc. is reproduced here with just the subset
 * the ported code actually calls. */
#ifndef FSV3DS_GTYPES_H
#define FSV3DS_GTYPES_H

#include <stdint.h>

typedef int            gint;
typedef unsigned int    guint;
typedef int              gboolean;
typedef char             gchar;
typedef float            gfloat;
typedef double           gdouble;
typedef void            *gpointer;
typedef const void      *gconstpointer;

typedef int8_t   gint8;
typedef uint8_t  guint8;
typedef int16_t  gint16;
typedef uint16_t guint16;
typedef int32_t  gint32;
typedef uint32_t guint32;
typedef int64_t  gint64;
typedef uint64_t guint64;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef gint (*GCompareFunc)( gconstpointer a, gconstpointer b );

#endif /* FSV3DS_GTYPES_H */

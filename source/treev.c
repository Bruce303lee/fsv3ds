/* treev.c - fsv3ds Phase 5
 *
 * Port of geometry.c's TreeV layout engine (treev_reshape_platform's
 * cubic-equation platform sizing and the row-packing in
 * treev_build_dir are close ports; the GL *drawing* functions are a
 * from-scratch vertex-emission rewrite, same reasoning as mapv.c).
 *
 * The one-level-at-a-time design (dirtree_stub.h: only nav_view_root()
 * counts as "expanded") interacts with TreeV differently than it did
 * with MapV, and is worth spelling out:
 *
 * MapV's layout (mapv_init_recursive) computes ABSOLUTE box corners
 * for every node by recursing over the WHOLE tree unconditionally
 * (expand state only gates *drawing*, not layout) -- a node's position
 * doesn't depend on any "is this expanded" check, just on its parent's
 * already-computed absolute box. TreeV's layout is different: a
 * platform's absolute radius (geometry_treev_platform_r0) is a running
 * sum of its ancestors' *depths*, and upstream only computes an
 * ancestor's depth (via treev_reshape_platform) when that ancestor is
 * itself "expanded" -- multiple simultaneously-expanded directories
 * nested inside each other is exactly what a real dirtree widget
 * supports, and what upstream's TreeV was designed around.
 *
 * Our dirtree_stub.c reports only nav_view_root() as expanded, which
 * would leave every one of view_root's ancestors un-reshaped (still
 * "a leaf" by upstream's own definition) and geometry_treev_platform_r0()
 * would walk off a chain of never-computed depths. Fixed by widening
 * dirtree_stub.c's rule to "view_root, or an ancestor of view_root" --
 * verified in that file's comment that this is a no-op for MapV (a
 * child is never its own ancestor, and MapV only ever asks about
 * view_root's *children*), so it's TreeV-motivated but harmless to
 * share.
 *
 * Given that, treev_init_recursive()'s full-tree walk (assigning every
 * node's leaf.height) turns out to be unnecessary for us: leaf.height
 * depends only on the node's own size, not on any ancestor's layout,
 * so it's computed lazily for just view_root's direct children in
 * treev_build_scene() instead of over the whole tree every time.
 */
#include "common.h"
#include "treev.h"
#include "dirtree_stub.h"
#include "nav.h"
#include "rpc.h"
#include "color.h"

#include <stdlib.h>
#include <3ds.h> /* linearAlloc/linearFree -- see mapv.c's comment */

/* From upstream geometry.c */
#define TREEV_MIN_ARC_WIDTH           90.0
#define TREEV_MAX_ARC_WIDTH           225.0
#define TREEV_MIN_CORE_RADIUS         8192.0
#define TREEV_CORE_GROW_FACTOR        1.25
#define TREEV_CURVE_GRANULARITY       5.0
#define TREEV_PLATFORM_HEIGHT         158.2
#define TREEV_PLATFORM_SPACING_WIDTH  512.0
#define TREEV_LEAF_HEIGHT_MULTIPLIER  1.0
#define TREEV_LEAF_NODE_EDGE          256.0
#define TREEV_PLATFORM_SPACING_DEPTH  2048.0

/* Upper bound on treev_gldraw_platform's curve segment count
 * (ceil(TREEV_MAX_ARC_WIDTH / TREEV_CURVE_GRANULARITY) + slack), used
 * to size fixed-length stack buffers for the platform's edge points. */
#define TREEV_MAX_SEGS 64

/* From upstream camera.h */
#define NEAR_TO_DISTANCE_RATIO 0.5
#define FAR_TO_NEAR_RATIO      128.0

/* cam.fov is horizontal (see render.c's horiz_to_vert_fov() -- the
 * same conversion, duplicated here since treev.c doesn't link
 * against citro3d/C3D_AspectRatioTop). Needed because a "fill the
 * frame" distance computed from the horizontal FOV badly
 * underestimates how far back the camera needs to be for a *tall*
 * object: the top screen is landscape (400x240), so the vertical FOV
 * is narrower than the horizontal one -- a spike that's mostly height
 * (leaf.height can run into the tens of thousands for a big directory,
 * vs. a fixed 256-unit footprint) needs the narrower vertical FOV to
 * frame correctly, not the wider horizontal one. Using the horizontal
 * FOV for both was the bug: it made tall spikes (and their labels)
 * stick out past the top of frame even after "fixing" d2 to go
 * through *a* FOV conversion -- confirmed still broken on hardware,
 * this is why. */
#define TOP_SCREEN_ASPECT (400.0 / 240.0)

static double
half_tan_vfov( double fov_h_deg )
{
	double half_h_rad = RAD( 0.5 * fov_h_deg );
	double half_v_rad = atan( tan( half_h_rad ) / TOP_SCREEN_ASPECT );

	return tan( half_v_rad );
}

#define SIDE_SHADE 0.65f
static const RGBcolor fallback_color = { 1.0f, 0.0f, 0.0f };

/* Auto-grown/shrunk by treev_arrange() to keep the view root's total
 * angular footprint within [TREEV_MIN_ARC_WIDTH, TREEV_MAX_ARC_WIDTH]
 * -- reset to TREEV_MIN_CORE_RADIUS at the top of every
 * treev_build_scene() rather than persisted across calls the way
 * upstream persists it for the app's whole lifetime: our one-level
 * design re-lays-out from scratch on every drill/up/cycle/rescan (no
 * incremental expand/collapse to preserve continuity for), so each
 * call is an independent layout problem, not a continuation of the
 * last one. */
static double treev_core_radius = TREEV_MIN_CORE_RADIUS;


/* ---- layout helpers (ported from geometry.c's geometry_treev_*) ---- */

static boolean
treev_is_leaf( GNode *node )
{
	if (NODE_IS_DIR(node) && dirtree_entry_expanded( node ))
		return FALSE;
	return TRUE;
}


static double
treev_platform_r0( GNode *dnode )
{
	GNode *up_node;
	double r0 = 0.0;

	if (NODE_IS_METANODE(dnode))
		return treev_core_radius;

	up_node = dnode->parent;
	while (up_node != NULL) {
		r0 += TREEV_PLATFORM_SPACING_DEPTH;
		r0 += TREEV_GEOM_PARAMS(up_node)->platform.depth;
		up_node = up_node->parent;
	}
	r0 += treev_core_radius;

	return r0;
}


static double
treev_platform_theta( GNode *dnode )
{
	GNode *up_node;
	double theta = 0.0;

	up_node = dnode;
	while (up_node != NULL) {
		theta += TREEV_GEOM_PARAMS(up_node)->platform.theta;
		up_node = up_node->parent;
	}

	return theta;
}


/* This assigns an arc width and (estimated) depth to a directory
 * platform -- ported near-verbatim from geometry.c, solving the same
 * cubic (Cardano's method) for depth given area/inner-radius/aspect-
 * ratio-1. Only change: child count via a plain sibling walk instead
 * of upstream's g_list_length((GList *)dnode->children) cast trick
 * (same GNode/GList-aliasing pattern already avoided elsewhere in this
 * port). */
static void
treev_reshape_platform( GNode *dnode, double r0 )
{
#define edge05 (0.5 * TREEV_LEAF_NODE_EDGE)
#define edge15 (1.5 * TREEV_LEAF_NODE_EDGE)
	static const double w = TREEV_PLATFORM_SPACING_WIDTH;
	static const double w_2 = SQR(TREEV_PLATFORM_SPACING_WIDTH);
	static const double w_3 = SQR(TREEV_PLATFORM_SPACING_WIDTH) * TREEV_PLATFORM_SPACING_WIDTH;
	static const double w_4 = SQR(TREEV_PLATFORM_SPACING_WIDTH) * SQR(TREEV_PLATFORM_SPACING_WIDTH);
	double area;
	double A, A_2, A_3, r, r_2, r_3, r_4, ka, kb, kc, kd, d, theta;
	double depth, arc_width, min_arc_width;
	double k;
	int n;
	GNode *c;

	n = 0;
	for (c = dnode->children; c != NULL; c = c->next)
		++n;

	k = edge15 * ceil( sqrt( (double)MAX(1, n) ) ) + edge05;
	area = SQR(k);

	A = area;
	A_2 = SQR(A);
	A_3 = A*A_2;
	r = r0;
	r_2 = SQR(r);
	r_3 = r*r_2;
	r_4 = SQR(r_2);
	ka = 72.0*(A*r - w*(A + r)) - 64.0*r_3 + 48.0*r_2*w - 36.0*w_2 + 24.0*r*w_2 - 8.0*w_3;
#define T1 72.0*A*w_2 - 132.0*A*r*w_2 - 240.0*A*w*r_3 + 120.0*A*w_2*r_2 - 24.0*A_2*w*r - 60.0*w_3*r
#define T2 12.0*(w_2*r_2 + A_2*w_2 - w_4*r + w_4*r_2 + A*w_3 + w_3)
#define T3 48.0*(w_2*r_4 - w_2*r_3 - w_3*r_3) + 96.0*(A_3 + w_3*r_2)
#define T4 192.0*A*r_4 + 156.0*A_2*r_2 + 3.0*w_4 + 144.0*A_2*w + 264.0*A*w*r_2
	kb = 12.0*sqrt( T1 + T2 + T3 + T4 );
#undef T1
#undef T2
#undef T3
#undef T4
	kc = cos( atan2( kb, ka ) / 3.0 );
	kd = cbrt( hypot( ka, kb ) );
	d = (- w - 2.0*r)/3.0 + ((8.0*r_2 - 4.0*w*r + 2.0*w_2)/3.0 + 4.0*A + 2.0*w)*kc/kd + kc*kd/6.0;
	theta = 180.0*(d + w)/(PI*(r + d));

	depth = d;
	arc_width = theta;

	/* Adjust depth upward to accomodate an integral number of rows */
	depth += (edge15 - fmod( depth - edge05, edge15 )) + edge05;

	/* Final arc width must be at least large enough to yield an
	 * inner edge length that is two leaf node edges long */
	min_arc_width = (180.0 * (2.0 * TREEV_LEAF_NODE_EDGE + TREEV_PLATFORM_SPACING_WIDTH) / PI) / r0;

	TREEV_GEOM_PARAMS(dnode)->platform.arc_width = MAX(min_arc_width, arc_width);
	TREEV_GEOM_PARAMS(dnode)->platform.depth = depth;

#undef edge05
#undef edge15
}


/* Ported from treev_arrange_recursive(), simplified: always does a
 * full reshape (no TREEV_NEED_REARRANGE flag -- every
 * treev_build_scene() call lays out from scratch, there's no
 * incremental expand/collapse to preserve). Recursion still touches
 * every directory sibling at each level along view_root's ancestor
 * chain (not the whole tree -- see this file's header comment), since
 * non-expanded siblings return immediately from treev_is_leaf() and
 * contribute zero arc width, cheap even for a directory with many
 * subdirectories. */
static void
treev_arrange_recursive( GNode *dnode, double r0 )
{
	GNode *node;
	double subtree_r0;
	double arc_width, subtree_arc_width = 0.0;
	double theta;

	if (NODE_IS_DIR(dnode)) {
		if (treev_is_leaf( dnode ))
			return;
		treev_reshape_platform( dnode, r0 );
	}

	subtree_r0 = r0 + TREEV_GEOM_PARAMS(dnode)->platform.depth + TREEV_PLATFORM_SPACING_DEPTH;
	node = dnode->children;
	while (node != NULL) {
		if (!NODE_IS_DIR(node))
			break; /* dirs always sort first -- see scanfs.c's compare_node() */
		treev_arrange_recursive( node, subtree_r0 );
		/* No morph/deployment animation in this port -- weight is a
		 * hard 0 or 1 from dirtree_entry_expanded(), same
		 * simplification mapv.c's mapv_init_recursive already makes. */
		arc_width = (dirtree_entry_expanded( node ) ? 1.0 : 0.0) *
			MAX(TREEV_GEOM_PARAMS(node)->platform.arc_width, TREEV_GEOM_PARAMS(node)->platform.subtree_arc_width);
		TREEV_GEOM_PARAMS(node)->platform.theta = arc_width; /* temporary value */
		subtree_arc_width += arc_width;
		node = node->next;
	}
	TREEV_GEOM_PARAMS(dnode)->platform.subtree_arc_width = subtree_arc_width;

	/* Spread expanded children, sweeping counterclockwise. In our
	 * one-directory-expanded-at-a-time design at most one child here
	 * ever has nonzero arc_width (the one on view_root's ancestor
	 * path), but the sweep is written generally, matching upstream. */
	theta = -0.5 * subtree_arc_width;
	node = dnode->children;
	while (node != NULL) {
		if (!NODE_IS_DIR(node))
			break;
		arc_width = TREEV_GEOM_PARAMS(node)->platform.theta;
		TREEV_GEOM_PARAMS(node)->platform.theta = theta + 0.5 * arc_width;
		theta += arc_width;
		node = node->next;
	}
}


/* Ported from treev_arrange(): grows/shrinks treev_core_radius until
 * the (single, in our design) expanded top-level directory's arc width
 * settles within bounds. Iteration-capped defensively -- upstream runs
 * this loop for the app's whole lifetime with no cap; we don't have a
 * debugger handy on hardware if a future change ever made it not
 * converge. */
static void
treev_arrange( void )
{
	int guard;

	for (guard = 0; guard < 32; guard++) {
		treev_arrange_recursive( globals.fstree, treev_core_radius );

		if (TREEV_GEOM_PARAMS(globals.fstree)->platform.subtree_arc_width > TREEV_MAX_ARC_WIDTH) {
			treev_core_radius *= TREEV_CORE_GROW_FACTOR;
		}
		else if ((TREEV_GEOM_PARAMS(globals.fstree)->platform.subtree_arc_width < TREEV_MIN_ARC_WIDTH) &&
			(TREEV_GEOM_PARAMS(globals.fstree)->platform.depth > TREEV_MIN_CORE_RADIUS)) {
			treev_core_radius = MAX(TREEV_MIN_CORE_RADIUS, treev_core_radius / TREEV_CORE_GROW_FACTOR);
		}
		else
			break;
	}
}


/* Ported from treev_build_dir()'s row-packing half (the GL-drawing
 * half is emit_leaves_recursive()/emit_platform() below). Assigns
 * leaf.theta/leaf.distance to each of dnode's children, packed into
 * rows going outward from r0, and finalizes dnode's platform.depth
 * from however many rows were actually used (treev_reshape_platform's
 * depth was only an estimate). Plain sibling walk instead of upstream's
 * g_list_length/g_list_last, same reasoning as treev_reshape_platform. */
static void
layout_children_rows( GNode *dnode, double r0 )
{
#define edge05 (0.5 * TREEV_LEAF_NODE_EDGE)
#define edge15 (1.5 * TREEV_LEAF_NODE_EDGE)
	GNode *node, *last;
	double pos_r, pos_theta, arc_len, inter_arc_width;
	int n, row_node_count, remaining_node_count;

	remaining_node_count = 0;
	last = NULL;
	for (node = dnode->children; node != NULL; node = node->next) {
		++remaining_node_count;
		last = node;
	}

	pos_r = r0 + TREEV_LEAF_NODE_EDGE;
	node = last;
	while (node != NULL) {
		arc_len = pos_r * RAD(TREEV_GEOM_PARAMS(dnode)->platform.arc_width) - TREEV_PLATFORM_SPACING_WIDTH;
		row_node_count = (int)floor( (arc_len - edge05) / edge15 );
		inter_arc_width = DEG(edge15 / pos_r);

		pos_theta = 0.5 * inter_arc_width * (double)(MIN(row_node_count, remaining_node_count) - 1);
		for (n = 0; (n < row_node_count) && (node != NULL); n++) {
			TREEV_GEOM_PARAMS(node)->leaf.theta = pos_theta;
			TREEV_GEOM_PARAMS(node)->leaf.distance = pos_r - r0;
			pos_theta -= inter_arc_width;
			node = node->prev;
		}

		remaining_node_count -= row_node_count;
		pos_r += edge15;
	}

	pos_r -= edge05;
	TREEV_GEOM_PARAMS(dnode)->platform.depth = pos_r - r0;
#undef edge05
#undef edge15
}


/* ---- vertex buffer emission ---- */

static TreeVVertex *vbuf = NULL;
static unsigned int vbuf_count = 0;
static unsigned int vbuf_capacity = 0;

static TreeVLabel *labels = NULL;
static unsigned int label_count = 0;

/* Cached layout results from the last full treev_build_scene() --
 * view_root's own absolute r0/theta and platform top Z don't change
 * just because the selection cycled to a different sibling, so
 * treev_cycle_selection() can re-emit (rebuild_vbuf()) without paying
 * for a full layout + treev_camera_init() reset, mirroring mapv.c's
 * rebuild_vbuf()/mapv_cycle_selection() split. */
static double cached_r0, cached_theta_platform, cached_platform_top_z;

#define VERTS_PER_LEAF 30 /* 4 side quads + 1 top quad, 6 verts/quad -- see mapv.c */

static void
push_vertex( double x, double y, double z, const float *rgb )
{
	TreeVVertex *v = &vbuf[vbuf_count++];

	v->x = (float)x;
	v->y = (float)y;
	v->z = (float)z;
	v->r = rgb[0];
	v->g = rgb[1];
	v->b = rgb[2];
	v->a = 1.0f;
}

static void
push_tri( const double *a, const double *b, const double *c, const float *rgb )
{
	push_vertex( a[0], a[1], a[2], rgb );
	push_vertex( b[0], b[1], b[2], rgb );
	push_vertex( c[0], c[1], c[2], rgb );
}

static void
push_quad( const double *a, const double *b, const double *c, const double *d, const float *rgb )
{
	push_tri( a, b, c, rgb );
	push_tri( a, c, d, rgb );
}

static void
brighten( float *c )
{
	c[0] += (1.0f - c[0]) * 0.6f;
	c[1] += (1.0f - c[1]) * 0.6f;
	c[2] += (1.0f - c[2]) * 0.6f;
}


/* World-space rewrite of treev_gldraw_leaf(): emits one leaf box.
 * r0/theta_platform are view_root's own ABSOLUTE radius/angle
 * (treev_platform_r0/_theta( view_root )) -- since there's no matrix
 * stack (see this file's header comment), the leaf's own world angle
 * has to be built by hand as theta_platform + leaf.theta instead of
 * relying on an ambient glRotated(platform.theta) already being on the
 * stack the way upstream's treev_gldraw_leaf can. Z needs no such
 * adjustment -- ancestor transforms in upstream only ever rotate
 * around Z, never translate/rotate it, so platform.height is already
 * an absolute world Z. */
static void
emit_leaf( GNode *node, double r0, double theta_platform, double platform_top_z, boolean is_selected )
{
	double edge = TREEV_LEAF_NODE_EDGE;
	double height = TREEV_GEOM_PARAMS(node)->leaf.height;
	double z0 = platform_top_z;
	double z1 = z0 + height;
	double theta_abs = theta_platform + TREEV_GEOM_PARAMS(node)->leaf.theta;
	double sin_t = sin( RAD(theta_abs) );
	double cos_t = cos( RAD(theta_abs) );
	double cx = r0 + TREEV_GEOM_PARAMS(node)->leaf.distance;
	double local[4][2];
	double w[4][2]; /* world-space XY, after rotation */
	const RGBcolor *node_color = NODE_DESC(node)->color;
	float top_col[3], side_col[3];
	double bl[3], br[3], fr[3], fl[3];
	double tbl[3], tbr[3], tfr[3], tfl[3];
	int i;

	if (node_color == NULL)
		node_color = &fallback_color;

	top_col[0] = node_color->r;
	top_col[1] = node_color->g;
	top_col[2] = node_color->b;
	side_col[0] = top_col[0] * SIDE_SHADE;
	side_col[1] = top_col[1] * SIDE_SHADE;
	side_col[2] = top_col[2] * SIDE_SHADE;

	if (is_selected) {
		brighten( top_col );
		brighten( side_col );
	}

	local[0][0] = cx - 0.5 * edge; local[0][1] = -0.5 * edge; /* front-left  */
	local[1][0] = cx + 0.5 * edge; local[1][1] = -0.5 * edge; /* front-right */
	local[2][0] = cx + 0.5 * edge; local[2][1] =  0.5 * edge; /* rear-right  */
	local[3][0] = cx - 0.5 * edge; local[3][1] =  0.5 * edge; /* rear-left   */

	for (i = 0; i < 4; i++) {
		w[i][0] = local[i][0] * cos_t - local[i][1] * sin_t;
		w[i][1] = local[i][0] * sin_t + local[i][1] * cos_t;
	}

	fl[0] = w[0][0]; fl[1] = w[0][1]; fl[2] = z0;
	fr[0] = w[1][0]; fr[1] = w[1][1]; fr[2] = z0;
	br[0] = w[2][0]; br[1] = w[2][1]; br[2] = z0;
	bl[0] = w[3][0]; bl[1] = w[3][1]; bl[2] = z0;

	tfl[0] = w[0][0]; tfl[1] = w[0][1]; tfl[2] = z1;
	tfr[0] = w[1][0]; tfr[1] = w[1][1]; tfr[2] = z1;
	tbr[0] = w[2][0]; tbr[1] = w[2][1]; tbr[2] = z1;
	tbl[0] = w[3][0]; tbl[1] = w[3][1]; tbl[2] = z1;

	if (vbuf_count + VERTS_PER_LEAF > vbuf_capacity)
		return; /* shouldn't happen -- capacity is precomputed */

	push_quad( bl, tbl, tbr, br, side_col ); /* rear  */
	push_quad( br, tbr, tfr, fr, side_col ); /* right */
	push_quad( fr, tfr, tfl, fl, side_col ); /* front */
	push_quad( fl, tfl, tbl, bl, side_col ); /* left  */
	push_quad( tfl, tfr, tbr, tbl, top_col ); /* top  */
}


/* World-space rewrite of treev_gldraw_platform(): the curved wedge
 * view_root itself stands on (inner wall, outer wall, leading/trailing
 * straight end caps, curved top). Ported closely -- same edge-point
 * buffer + leading/trailing tangent-offset approach -- just emitting
 * triangulated quads into vbuf instead of glBegin(GL_QUADS). Unlike
 * MapV (which never draws view_root's own box, only its children's --
 * see mapv.c), TreeV needs this: a curved wedge floor is the only
 * thing that makes the leaf boxes standing on it read as "on a
 * platform" rather than floating. */
static void
emit_platform( GNode *dnode, double r0, double theta_platform, double top_z )
{
	double arc_width = TREEV_GEOM_PARAMS(dnode)->platform.arc_width;
	double depth = TREEV_GEOM_PARAMS(dnode)->platform.depth;
	double r1 = r0 + depth;
	int seg_count = (int)ceil( arc_width / TREEV_CURVE_GRANULARITY );
	double seg_arc_width;
	double inner[TREEV_MAX_SEGS + 1][2], outer[TREEV_MAX_SEGS + 1][2];
	double theta, sin_t, cos_t;
	double half_w = 0.5 * TREEV_PLATFORM_SPACING_WIDTH;
	const RGBcolor *node_color = NODE_DESC(dnode)->color;
	float top_col[3], side_col[3];
	int s;

	if (seg_count > TREEV_MAX_SEGS)
		seg_count = TREEV_MAX_SEGS; /* arc_width is bounded by TREEV_MAX_ARC_WIDTH, shouldn't trigger */
	seg_arc_width = arc_width / (double)seg_count;

	if (node_color == NULL)
		node_color = &fallback_color;
	top_col[0] = node_color->r; top_col[1] = node_color->g; top_col[2] = node_color->b;
	side_col[0] = top_col[0] * SIDE_SHADE;
	side_col[1] = top_col[1] * SIDE_SHADE;
	side_col[2] = top_col[2] * SIDE_SHADE;

	theta = theta_platform - 0.5 * arc_width;
	for (s = 0; s <= seg_count; s++) {
		double px0, py0, px1, py1;

		sin_t = sin( RAD(theta) );
		cos_t = cos( RAD(theta) );
		px0 = r0 * cos_t; py0 = r0 * sin_t;
		px1 = r1 * cos_t; py1 = r1 * sin_t;

		if (s == 0) {
			double dx = -sin_t * half_w, dy = cos_t * half_w;
			px0 += dx; py0 += dy;
			px1 += dx; py1 += dy;
		}
		else if (s == seg_count) {
			double dx = sin_t * half_w, dy = -cos_t * half_w;
			px0 += dx; py0 += dy;
			px1 += dx; py1 += dy;
		}

		inner[s][0] = px0; inner[s][1] = py0;
		outer[s][0] = px1; outer[s][1] = py1;

		theta += seg_arc_width;
	}

	if (vbuf_count + (unsigned int)((3 * seg_count + 2) * 6) > vbuf_capacity)
		return; /* shouldn't happen -- capacity is precomputed */

	for (s = 0; s < seg_count; s++) {
		double a[3], b[3], c[3], d[3];

		/* inner wall */
		a[0] = inner[s][0];   a[1] = inner[s][1];   a[2] = 0.0;
		b[0] = inner[s][0];   b[1] = inner[s][1];   b[2] = top_z;
		c[0] = inner[s+1][0]; c[1] = inner[s+1][1]; c[2] = top_z;
		d[0] = inner[s+1][0]; d[1] = inner[s+1][1]; d[2] = 0.0;
		push_quad( a, b, c, d, side_col );

		/* outer wall (opposite winding -- outward-facing) */
		a[0] = outer[s][0];   a[1] = outer[s][1];   a[2] = 0.0;
		d[0] = outer[s][0];   d[1] = outer[s][1];   d[2] = top_z;
		c[0] = outer[s+1][0]; c[1] = outer[s+1][1]; c[2] = top_z;
		b[0] = outer[s+1][0]; b[1] = outer[s+1][1]; b[2] = 0.0;
		push_quad( a, b, c, d, side_col );

		/* top */
		a[0] = inner[s][0];   a[1] = inner[s][1];   a[2] = top_z;
		b[0] = outer[s][0];   b[1] = outer[s][1];   b[2] = top_z;
		c[0] = outer[s+1][0]; c[1] = outer[s+1][1]; c[2] = top_z;
		d[0] = inner[s+1][0]; d[1] = inner[s+1][1]; d[2] = top_z;
		push_quad( a, b, c, d, top_col );
	}

	{
		double a[3], b[3], c[3], d[3];

		/* leading edge cap */
		a[0] = inner[0][0]; a[1] = inner[0][1]; a[2] = 0.0;
		b[0] = outer[0][0]; b[1] = outer[0][1]; b[2] = 0.0;
		c[0] = outer[0][0]; c[1] = outer[0][1]; c[2] = top_z;
		d[0] = inner[0][0]; d[1] = inner[0][1]; d[2] = top_z;
		push_quad( a, b, c, d, side_col );

		/* trailing edge cap */
		a[0] = inner[seg_count][0]; a[1] = inner[seg_count][1]; a[2] = top_z;
		b[0] = outer[seg_count][0]; b[1] = outer[seg_count][1]; b[2] = top_z;
		c[0] = outer[seg_count][0]; c[1] = outer[seg_count][1]; c[2] = 0.0;
		d[0] = inner[seg_count][0]; d[1] = inner[seg_count][1]; d[2] = 0.0;
		push_quad( a, b, c, d, side_col );
	}
}


static unsigned int
platform_vertex_budget( double arc_width )
{
	int seg_count = (int)ceil( arc_width / TREEV_CURVE_GRANULARITY );

	if (seg_count > TREEV_MAX_SEGS)
		seg_count = TREEV_MAX_SEGS;

	return (unsigned int)((3 * seg_count + 2) * 6);
}


static void
push_label( GNode *node, double r0, double theta_platform, double platform_top_z, GNode *selected_node )
{
	double theta_abs = theta_platform + TREEV_GEOM_PARAMS(node)->leaf.theta;
	double r = r0 + TREEV_GEOM_PARAMS(node)->leaf.distance;
	TreeVLabel *l = &labels[label_count++];

	l->x = (float)(r * cos( RAD(theta_abs) ));
	l->y = (float)(r * sin( RAD(theta_abs) ));
	l->z = (float)(platform_top_z + TREEV_GEOM_PARAMS(node)->leaf.height);
	l->name = NODE_DESC(node)->name;
	l->is_selected = (node == selected_node);
}


/* ---- camera ---- */

static TreeVCameraState cam;

static float goal_target_r, goal_target_theta, goal_target_z;
static float anim_peak_distance;
static float anim_end_distance;
static boolean anim_cinematic;

typedef enum {
	CAM_ANIM_IDLE,
	CAM_ANIM_ZOOM_OUT,
	CAM_ANIM_PAN,
	CAM_ANIM_ZOOM_IN
} CamAnimPhase;

static CamAnimPhase anim_phase = CAM_ANIM_IDLE;


/* See mapv.c's set_camera_focus_goal() -- same zoom-out/pan/zoom-in
 * choreography, adapted to TreeV's polar target. Leaf *footprint* is a
 * fixed constant (TREEV_LEAF_NODE_EDGE) unlike MapV's size-driven
 * boxes, but leaf *height* is very much not (sqrt(subtree size in
 * bytes) -- a big directory can be tens of thousands of units tall
 * against a 256-unit footprint), so the fill distance still needs a
 * MAPV_NODE_WIDTH-style per-node span -- just against height instead
 * of footprint, since a tall node's height is what determines how far
 * back the camera needs to sit to keep its top (and label) on screen. */
static void
set_camera_focus_goal( GNode *node, double r0, double theta_platform, double platform_top_z, boolean cinematic )
{
	double theta_abs = theta_platform + TREEV_GEOM_PARAMS(node)->leaf.theta;
	double r = r0 + TREEV_GEOM_PARAMS(node)->leaf.distance;

	goal_target_r = (float)r;
	goal_target_theta = (float)theta_abs;
	goal_target_z = (float)(platform_top_z + 0.5 * TREEV_GEOM_PARAMS(node)->leaf.height);

	anim_cinematic = cinematic;

	if (!cinematic) {
		anim_phase = CAM_ANIM_PAN;
		return;
	}

	{
		/* Two separate fits (footprint against horizontal FOV, height
		 * against vertical FOV -- see half_tan_vfov()'s comment), not
		 * one MAX'd span run through a single FOV. */
		double fill_h = TREEV_LEAF_NODE_EDGE * (0.5 / tan( RAD(0.5 * cam.fov) ));
		double fill_v = TREEV_GEOM_PARAMS(node)->leaf.height * (0.5 / half_tan_vfov( cam.fov ));
		double fill_distance = MAX(fill_h, fill_v);
		float min_end;

		anim_end_distance = (float)(1.25 * fill_distance);

		min_end = cam.base_distance * 0.06f; /* see mapv.c's set_camera_focus_goal() for why relative, not absolute */
		if (anim_end_distance < min_end)
			anim_end_distance = min_end;
	}
	anim_peak_distance = MAX( cam.distance, anim_end_distance ) * 1.5f;

	anim_phase = CAM_ANIM_ZOOM_OUT;
}


void
treev_camera_tick( void )
{
	const float EASE = 0.15f;

	switch (anim_phase) {
		case CAM_ANIM_ZOOM_OUT:
			cam.distance += (anim_peak_distance - cam.distance) * EASE;
			if (fabsf( anim_peak_distance - cam.distance ) < anim_peak_distance * 0.03f)
				anim_phase = CAM_ANIM_PAN;
			break;

		case CAM_ANIM_PAN: {
			float dr, dtheta, dz, remaining;

			cam.target_r += (goal_target_r - cam.target_r) * EASE;
			cam.target_theta += (goal_target_theta - cam.target_theta) * EASE;
			cam.target_z += (goal_target_z - cam.target_z) * EASE;

			dr = goal_target_r - cam.target_r;
			dtheta = goal_target_theta - cam.target_theta;
			dz = goal_target_z - cam.target_z;
			/* dtheta contributes on the order of (r * radians) to keep
			 * this a sane distance-like quantity alongside dr/dz -- a
			 * few degrees off at a large radius is still "basically
			 * there" the same way a few world-units off in dr is. */
			remaining = sqrtf( dr * dr + dz * dz + (float)SQR(RAD(dtheta) * cam.target_r) );
			if (remaining < cam.distance * 0.02f)
				anim_phase = anim_cinematic ? CAM_ANIM_ZOOM_IN : CAM_ANIM_IDLE;
			break;
		}

		case CAM_ANIM_ZOOM_IN:
			cam.distance += (anim_end_distance - cam.distance) * EASE;
			if (fabsf( anim_end_distance - cam.distance ) < anim_end_distance * 0.03f)
				anim_phase = CAM_ANIM_IDLE;
			break;

		case CAM_ANIM_IDLE:
		default:
			return;
	}

	cam.near_clip = (float)(NEAR_TO_DISTANCE_RATIO * cam.distance);
	cam.far_clip = (float)(FAR_TO_NEAR_RATIO * cam.near_clip);
}


/* See mapv_camera_init() -- same per-view-level framing approach
 * (deviating from upstream's camera_init(FSV_TREEV), which frames the
 * *entire* tree via geometry_treev_get_extents(root_dnode); we only
 * ever have one platform on screen, so frame just that). phi=35 (not
 * upstream's phi=90 top-down) for the same reason MapV uses 35 instead
 * of upstream's phi=0 -- a top-down view flattens away the one axis
 * (height) that makes the scene readable, confirmed the same class of
 * bug on hardware for MapV already.
 *
 * d2 used to be a raw size (platform_h + max_leaf_h) compared directly
 * against d1 (a proper FOV-derived distance) -- fine for MapV, whose
 * heights are small fixed constants that never dominate d1, but TreeV
 * heights scale with sqrt(subtree size) and can run into the tens of
 * thousands, dwarfing the platform footprint. Left unconverted, a big
 * directory's spike (and its label) stuck out past the top of frame
 * even in the overview shot -- confirmed on hardware. Running it
 * through the *horizontal* FOV conversion (matching d1) wasn't enough
 * either -- still confirmed cutting spikes off on hardware, because a
 * tall object needs the narrower *vertical* FOV, not the horizontal
 * one (see half_tan_vfov()'s comment). Now uses that, and target_z
 * centers on the vertical midpoint of the *whole* scene (ground to
 * tallest spike) instead of just the platform's own (comparatively
 * tiny) height. */
static void
treev_camera_init( GNode *view_root, double r0, double theta_platform, double max_leaf_h )
{
	double arc_width = TREEV_GEOM_PARAMS(view_root)->platform.arc_width;
	double depth = TREEV_GEOM_PARAMS(view_root)->platform.depth;
	double r1 = r0 + depth;
	double platform_h = TREEV_GEOM_PARAMS(view_root)->platform.height;
	double total_h = platform_h + max_leaf_h;
	double platform_span, d1, d2, d;

	cam.fov = 60.0f;

	platform_span = 2.0 * r1 * sin( RAD(0.5 * MIN(arc_width, 180.0)) );
	d1 = platform_span * (0.5 / tan( RAD(0.5 * cam.fov) ));
	d2 = total_h * (0.5 / half_tan_vfov( cam.fov ));
	d = MAX(d1, d2);

	cam.orbit_theta = 0.0f;
	cam.phi = 35.0f;
	cam.distance = (float)(1.6 * d);
	cam.base_distance = cam.distance;

	cam.target_r = (float)(r0 + 0.5 * depth);
	cam.target_theta = (float)theta_platform;
	cam.target_z = (float)(0.5 * total_h);

	goal_target_r = cam.target_r;
	goal_target_theta = cam.target_theta;
	goal_target_z = cam.target_z;
	anim_phase = CAM_ANIM_IDLE;

	cam.near_clip = (float)(NEAR_TO_DISTANCE_RATIO * cam.distance);
	cam.far_clip = (float)(FAR_TO_NEAR_RATIO * cam.near_clip);

	rpc_logf( "treev cam: r0=%.0f depth=%.0f arc=%.0f dist=%.0f\n", r0, depth, arc_width, (double)cam.distance );
}


void
treev_camera_orbit( float dtheta, float dphi, float distance_factor )
{
	cam.orbit_theta += dtheta;
	cam.phi = CLAMP(cam.phi + dphi, 5.0f, 85.0f);
	cam.distance *= distance_factor;
	if (cam.distance < cam.base_distance * 0.06f)
		cam.distance = cam.base_distance * 0.06f;

	cam.near_clip = (float)(NEAR_TO_DISTANCE_RATIO * cam.distance);
	cam.far_clip = (float)(FAR_TO_NEAR_RATIO * cam.near_clip);
}


/* ---- scene build ---- */

/* Re-emits the vertex buffer + labels for view_root's children using
 * the cached layout from the last full treev_build_scene() -- the
 * part that needs to happen both on a full rebuild AND whenever just
 * the selection changes (see mapv.c's rebuild_vbuf() comment; same
 * reasoning: highlight colors are baked into vbuf at emit time). */
static void
rebuild_vbuf( void )
{
	GNode *view_root = nav_view_root( );
	GNode *selected_node = nav_selected_node( );
	GNode *node;
	unsigned int nleaves = 0, budget;

	for (node = view_root->children; node != NULL; node = node->next)
		++nleaves;

	if (vbuf != NULL)
		linearFree( vbuf );
	budget = platform_vertex_budget( TREEV_GEOM_PARAMS(view_root)->platform.arc_width ) + nleaves * VERTS_PER_LEAF;
	vbuf_capacity = budget;
	vbuf_count = 0;
	vbuf = (TreeVVertex *)linearAlloc( vbuf_capacity * sizeof(TreeVVertex) );
	if (vbuf == NULL)
		quit( "Out of linear memory" );

	if (labels != NULL)
		free( labels );
	labels = (TreeVLabel *)xmalloc( MAX(1u, nleaves) * sizeof(TreeVLabel) );
	label_count = 0;

	emit_platform( view_root, cached_r0, cached_theta_platform, cached_platform_top_z );
	for (node = view_root->children; node != NULL; node = node->next) {
		emit_leaf( node, cached_r0, cached_theta_platform, cached_platform_top_z, node == selected_node );
		push_label( node, cached_r0, cached_theta_platform, cached_platform_top_z, selected_node );
	}

	rpc_logf( "treev: %u leaves, %u vertices\n", nleaves, vbuf_count );
}


void
treev_build_scene( void )
{
	GNode *view_root, *node, *selected_node;
	double r0, theta_platform, platform_top_z, max_leaf_h = 0.0;

	view_root = nav_view_root( );
	if (view_root == NULL || !NODE_IS_DIR(view_root)) {
		nav_reset( );
		view_root = nav_view_root( );
	}
	selected_node = nav_selected_node( );

	treev_core_radius = TREEV_MIN_CORE_RADIUS;

	TREEV_GEOM_PARAMS(globals.fstree)->platform.theta = 90.0;
	TREEV_GEOM_PARAMS(globals.fstree)->platform.depth = 0.0;
	TREEV_GEOM_PARAMS(globals.fstree)->platform.arc_width = TREEV_MAX_ARC_WIDTH;
	TREEV_GEOM_PARAMS(globals.fstree)->platform.height = 0.0;

	TREEV_GEOM_PARAMS(root_dnode)->leaf.theta = 0.0;
	TREEV_GEOM_PARAMS(root_dnode)->leaf.distance = 0.5 * TREEV_PLATFORM_SPACING_DEPTH;
	TREEV_GEOM_PARAMS(root_dnode)->platform.theta = 0.0;

	treev_arrange( ); /* reshapes view_root + its whole ancestor chain */

	/* view_root's own platform height + its children's leaf heights --
	 * see this file's header comment for why full-tree recursion
	 * (upstream's treev_init_recursive) isn't needed here. */
	TREEV_GEOM_PARAMS(view_root)->platform.height = TREEV_PLATFORM_HEIGHT;
	for (node = view_root->children; node != NULL; node = node->next) {
		int64 size = MAX(64, NODE_DESC(node)->size);
		double h;

		if (NODE_IS_DIR(node))
			size += DIR_NODE_DESC(node)->subtree.size;
		h = sqrt( (double)size ) * TREEV_LEAF_HEIGHT_MULTIPLIER;
		TREEV_GEOM_PARAMS(node)->leaf.height = h;
		max_leaf_h = MAX(max_leaf_h, h);
	}

	r0 = treev_platform_r0( view_root );
	theta_platform = treev_platform_theta( view_root );

	if (view_root->children != NULL)
		layout_children_rows( view_root, r0 ); /* finalizes platform.depth */

	platform_top_z = TREEV_GEOM_PARAMS(view_root)->platform.height;

	cached_r0 = r0;
	cached_theta_platform = theta_platform;
	cached_platform_top_z = platform_top_z;

	rebuild_vbuf( );

	treev_camera_init( view_root, r0, theta_platform, max_leaf_h );
	if (selected_node != NULL)
		set_camera_focus_goal( selected_node, r0, theta_platform, platform_top_z, FALSE );
}


/* ---- navigation wrappers (see treev.h) ---- */

void
treev_cycle_selection( int dir )
{
	GNode *view_root = nav_view_root( );

	if (nav_selected_node( ) == NULL || view_root == NULL)
		return;
	if (view_root->children == NULL || view_root->children->next == NULL)
		return;

	nav_cycle_selection( dir );

	rebuild_vbuf( ); /* re-bake highlight colors -- see mapv.c's rebuild_vbuf() comment */
	set_camera_focus_goal( nav_selected_node( ), cached_r0, cached_theta_platform, cached_platform_top_z, TRUE );
}


gboolean
treev_drill_selected( void )
{
	if (!nav_drill_selected( ))
		return FALSE;

	treev_build_scene( );
	return TRUE;
}


gboolean
treev_go_up( void )
{
	if (!nav_go_up( ))
		return FALSE;

	treev_build_scene( );
	return TRUE;
}


const TreeVVertex *
treev_vertex_data( void )
{
	return vbuf;
}


unsigned int
treev_vertex_count( void )
{
	return vbuf_count;
}


const TreeVCameraState *
treev_camera( void )
{
	return &cam;
}


const TreeVLabel *
treev_label_data( void )
{
	return labels;
}


unsigned int
treev_label_count( void )
{
	return label_count;
}

/* end treev.c */

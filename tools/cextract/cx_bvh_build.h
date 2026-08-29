/* cx_bvh_build.h -- THE BVH BUILDER, and there is exactly one of it.
 *
 * ======================================================= WHY IT IS SHARED ==
 * Two artefacts carry a BVH now: bvh.bin ('B3BV', the STATIC WORLD, one per
 * track, tools/cextract/cx_bvh.c) and carbvh.bin ('B3CV', the per-car MODEL
 * BVHs, one file for the whole fleet, tools/cextract/cx_car_bvh.c).  The
 * SHADER walks both with the same stackless escape loop, so the two files
 * have to agree about the tree's shape down to the last bit of the leaf
 * packing -- and the honest way to make two files agree about a format is to
 * build them with one builder rather than to keep two in step by review.
 *
 * So the binned-SAH build, the depth-first flatten, the escape indices and
 * the node writer live here, and both stages call them.  Everything ABOVE the
 * builder -- what geometry goes in, what space it is in, what the header says
 * -- stays in the stage that owns it, because that is the part the two really
 * do differ about.
 *
 * *** NOTHING IN THIS FILE IS A CLAIM ABOUT BURNOUT 3. ***  The Xbox had no
 * ray tracing and no acceleration structure of any kind; this is a modern
 * construction, marked INSPIRED, and must never be cited as game behaviour.
 * See the EVIDENCE MARKS block in src/burnout3_aftereffects.h.
 *
 * ============================================================= THE CONTRACT
 * A binned-SAH BVH2, flattened DEPTH-FIRST with ESCAPE INDICES so the shader
 * can walk it with NO STACK.  That is not a preference: the target dialect is
 * ESSL 1.00, which permits an array index only where it is a
 * constant-index-expression -- a traversal stack pointer never is.  A node
 * that misses jumps to its escape, a node that hits descends to index + 1,
 * and a leaf tests its triangles and then jumps to its escape.
 *
 * THE LEAF PACKING IS A CONTRACT WITH THE SHADER.  ESSL 1.00 has no bitwise
 * operators and no floatBitsToInt, so a node's two spare float slots have to
 * carry `escape` and BOTH halves of the leaf: the runtime encodes
 * `first_tri * 8 + (tri_count - 1)` into one float, exact while
 * first_tri < 2^21.  CXV_LEAF_MAX caps the count at 8 (three bits) and every
 * stage REFUSES to write a file whose first index would need a 25th bit.
 *
 * The build is SINGLE-THREADED and every tie is broken by index, so the same
 * inputs give the same bytes.  Both stages' validators gate that with a
 * double-run diff.
 */
#ifndef CX_BVH_BUILD_H
#define CX_BVH_BUILD_H

#include <stddef.h>
#include <stdint.h>

#include "cx_common_c.h"

/* ------------------------------------------------------------- the shape */

#define CXV_LEAF_MAX     8       /* the packing's ceiling (3 bits)          */
/* THE LEAF SIZE, and it turned out to be a WEB decision rather than a
 * desktop one.  Going from 4 to 8 nearly halves the node count (117k -> 65k
 * on US_C3_V1, 547k -> 302k on AS_M1_V1) and the artefact with it, and it is
 * worth NOTHING measurable on the desktop -- eight triangle tests instead of
 * four is ALU a modern GPU does not notice.  On the web a node visit is two
 * texture2D fetches and the fetches are the whole bill, so halving them is
 * most of why the option holds 60 there.  It cannot exceed CXV_LEAF_MAX:
 * three bits is all the float packing has. */
#define CXV_LEAF_TARGET  8       /* stop splitting at or below this         */
#define CXV_BINS        16       /* SAH bins per axis                       */
#define CXV_TRI_MAX     2097151u /* first_tri * 8 + 7 must stay exact in f32 */
#define CXV_NODE_REC    0x30
#define CXV_TRI_REC     0x30

typedef struct {
    float v[9];        /* v0 xyz, v1 xyz, v2 xyz                            */
    float opacity;
    uint32_t source;   /* the owning stage's own tag                        */
    uint32_t ref;
    float c[3];        /* centroid, the build's sort key                    */
    float lo[3], hi[3];
} CxvTri;

typedef struct {
    float lo[3], hi[3];
    uint32_t escape, first, count, depth;
} CxvNode;

typedef struct {
    CxvTri  *tri;
    size_t   ntri, captri;
    CxvNode *node;
    size_t   nnode, capnode;
    uint32_t *ord;      /* the permutation the build leaves triangles in    */
    int       err;
} CxvBuild;

/* ------------------------------------------------------------- the pieces */

void  cxv_box_reset(float lo[3], float hi[3]);
void  cxv_box_add(float lo[3], float hi[3], const float p[3]);
void  cxv_box_union(float lo[3], float hi[3],
                    const float olo[3], const float ohi[3]);
float cxv_box_area(const float lo[3], const float hi[3]);

/* Append one triangle.  A DEGENERATE triangle is dropped and reported as 0:
 * it can never be hit and would only widen a box.  -1 on allocation failure
 * (and `B->err` is set). */
int cxv_tri_push(CxvBuild *B, const float a[3], const float b[3],
                 const float c[3], float opacity,
                 uint32_t source, uint32_t ref);

/* Build the tree over `B->ord` (which the caller has filled with 0..ntri-1)
 * and flatten it into `B->node`.  0 on success.  `*maxdepth` is RAISED to the
 * deepest node reached, never lowered -- so a caller building many trees into
 * one file gets the file's own worst depth for free. */
int cxv_build(CxvBuild *B, uint32_t *maxdepth);

/* One 0x30 node record, little-endian, in the layout both artefacts share. */
void cxv_put_node(cxc_buf *o, const CxvNode *n);

/* Free everything cxv_tri_push / cxv_build allocated.  The struct is left
 * zeroed, so it can be reused for the next tree. */
void cxv_build_free(CxvBuild *B);

#endif /* CX_BVH_BUILD_H */

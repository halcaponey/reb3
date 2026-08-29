/* cx_bvh_build.c -- the one BVH builder.  See cx_bvh_build.h for why it is
 * shared, and for the shader contract the flattening exists to satisfy.
 *
 * This file is a pure MOVE out of cx_bvh.c: every line of the build below ran
 * unchanged as that stage's private half, and the artefact it produces for the
 * static world is byte-identical across the move.  Nothing here knows what
 * geometry it is over.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cx_bvh_build.h"

/* --------------------------------------------------------------- helpers */

void cxv_box_reset(float lo[3], float hi[3])
{
    int k;
    for (k = 0; k < 3; k++) { lo[k] = 3.4e38f; hi[k] = -3.4e38f; }
}

void cxv_box_add(float lo[3], float hi[3], const float p[3])
{
    int k;
    for (k = 0; k < 3; k++) {
        if (p[k] < lo[k]) lo[k] = p[k];
        if (p[k] > hi[k]) hi[k] = p[k];
    }
}

void cxv_box_union(float lo[3], float hi[3],
                   const float olo[3], const float ohi[3])
{
    int k;
    for (k = 0; k < 3; k++) {
        if (olo[k] < lo[k]) lo[k] = olo[k];
        if (ohi[k] > hi[k]) hi[k] = ohi[k];
    }
}

float cxv_box_area(const float lo[3], const float hi[3])
{
    float d0 = hi[0] - lo[0], d1 = hi[1] - lo[1], d2 = hi[2] - lo[2];
    if (d0 < 0.0f || d1 < 0.0f || d2 < 0.0f) return 0.0f;
    return 2.0f * (d0 * d1 + d1 * d2 + d2 * d0);
}

int cxv_tri_push(CxvBuild *B, const float a[3], const float b[3],
                 const float c[3], float opacity,
                 uint32_t source, uint32_t ref)
{
    CxvTri *t;
    int k;

    /* A degenerate triangle can never be hit and would only widen a box. */
    if ((a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) ||
        (a[0] == c[0] && a[1] == c[1] && a[2] == c[2]) ||
        (b[0] == c[0] && b[1] == c[1] && b[2] == c[2])) return 0;

    if (B->ntri == B->captri) {
        size_t cap = B->captri ? B->captri * 2 : (1u << 16);
        CxvTri *n = (CxvTri *)realloc(B->tri, cap * sizeof *n);
        if (!n) { B->err = 1; return -1; }
        B->tri = n; B->captri = cap;
    }
    t = &B->tri[B->ntri++];
    memcpy(t->v + 0, a, 3 * sizeof(float));
    memcpy(t->v + 3, b, 3 * sizeof(float));
    memcpy(t->v + 6, c, 3 * sizeof(float));
    t->opacity = opacity;
    t->source  = source;
    t->ref     = ref;
    cxv_box_reset(t->lo, t->hi);
    cxv_box_add(t->lo, t->hi, a);
    cxv_box_add(t->lo, t->hi, b);
    cxv_box_add(t->lo, t->hi, c);
    for (k = 0; k < 3; k++) t->c[k] = (t->lo[k] + t->hi[k]) * 0.5f;
    return 0;
}

/* ------------------------------------------------------------- the build
 *
 * TWO PHASES, and both are ITERATIVE.  A recursive builder is the natural
 * shape and it is the wrong one here: the worst shipped track is nearly a
 * million triangles, a SAH split is allowed to be arbitrarily lopsided, and
 * a run of lopsided splits turns a tree into a list -- so the recursion
 * depth is not bounded by log2(n) and the frame carries a kilobyte of bin
 * arrays.  These stages are also LINKED INTO THE GAME and run in-process
 * (src/burnout3_isodata.c), where the stack is not this file's to spend.
 * Both work stacks below are on the heap; the depth bound in phase 1 is a
 * second guard rail behind them.
 *
 *   phase 1  split the triangle permutation into a POOL of nodes carrying
 *            explicit left/right indices, in whatever order the work stack
 *            happens to produce.
 *   phase 2  flatten the pool DEPTH-FIRST into the emitted array, so a
 *            node's left child is always the next index and the shader can
 *            descend with `i + 1`, and fill in the ESCAPE index -- the first
 *            index past the whole subtree -- as each subtree closes.
 */

typedef struct {
    float    lo[3], hi[3];
    uint32_t first, count;     /* leaf when count != 0                     */
    int32_t  left, right;      /* -1 on a leaf                             */
    uint32_t depth;
} CxvPool;

typedef struct { uint32_t node, lo, hi, depth; } CxvWork;

/* Binned SAH over the widest axis of the CENTROID box, the standard
 * construction.  Every tie -- an empty bin, an equal cost, a zero-extent
 * centroid box -- falls back to a median split by the CURRENT order, which is
 * a function of the input order alone, so the tree is deterministic without a
 * sort key of its own. */
int cxv_build(CxvBuild *B, uint32_t *maxdepth)
{
    CxvPool  *pool = NULL;
    size_t    npool = 0, cappool = 0;
    CxvWork  *stack = NULL;
    size_t    nstack = 0, capstack = 0;
    uint32_t *tmp = NULL;
    int       rc = -1;

    tmp = (uint32_t *)malloc(B->ntri * sizeof *tmp);
    if (!tmp) goto done;

#define POOL_NEW(dst) do {                                                   \
        if (npool == cappool) {                                              \
            size_t c_ = cappool ? cappool * 2 : (1u << 15);                  \
            CxvPool *n_ = (CxvPool *)realloc(pool, c_ * sizeof *n_);         \
            if (!n_) goto done;                                              \
            pool = n_; cappool = c_;                                         \
        }                                                                    \
        memset(&pool[npool], 0, sizeof pool[0]);                             \
        pool[npool].left = pool[npool].right = -1;                           \
        (dst) = (uint32_t)npool++;                                           \
    } while (0)

#define PUSH(nd, l, h, d) do {                                               \
        if (nstack == capstack) {                                            \
            size_t c_ = capstack ? capstack * 2 : 256;                       \
            CxvWork *n_ = (CxvWork *)realloc(stack, c_ * sizeof *n_);        \
            if (!n_) goto done;                                              \
            stack = n_; capstack = c_;                                       \
        }                                                                    \
        stack[nstack].node = (nd);  stack[nstack].lo = (l);                  \
        stack[nstack].hi = (h);     stack[nstack].depth = (d);               \
        nstack++;                                                            \
    } while (0)

    {
        uint32_t root;
        POOL_NEW(root);
        PUSH(root, 0u, (uint32_t)B->ntri, 0u);
    }

    while (nstack) {
        CxvWork  w = stack[--nstack];
        uint32_t lo = w.lo, hi = w.hi, n = hi - lo, i;
        float    clo[3], chi[3], blo[3], bhi[3];
        int      axis, best_bin = -1, k;
        float    extent, scale = 0.0f;
        float    bin_lo[CXV_BINS][3], bin_hi[CXV_BINS][3];
        uint32_t bin_n[CXV_BINS];
        float    right_lo[CXV_BINS][3], right_hi[CXV_BINS][3];
        uint32_t right_n[CXV_BINS];
        uint32_t mid, l_idx, r_idx;

        if (w.depth > *maxdepth) *maxdepth = w.depth;

        cxv_box_reset(blo, bhi);
        cxv_box_reset(clo, chi);
        for (i = lo; i < hi; i++) {
            const CxvTri *t = &B->tri[B->ord[i]];
            cxv_box_union(blo, bhi, t->lo, t->hi);
            cxv_box_add(clo, chi, t->c);
        }
        memcpy(pool[w.node].lo, blo, sizeof blo);
        memcpy(pool[w.node].hi, bhi, sizeof bhi);
        pool[w.node].depth = w.depth;

        if (n <= CXV_LEAF_TARGET) {
            pool[w.node].first = lo;
            pool[w.node].count = n;
            continue;
        }

        axis = 0;
        extent = chi[0] - clo[0];
        if (chi[1] - clo[1] > extent) { axis = 1; extent = chi[1] - clo[1]; }
        if (chi[2] - clo[2] > extent) { axis = 2; extent = chi[2] - clo[2]; }

        mid = lo + n / 2;
        /* PAST A DEPTH BOUND, MEDIAN ONLY.  A SAH split is allowed to be
         * lopsided and a run of lopsided splits is what turns a tree into a
         * list; a median split halves, so the bound below caps the depth at
         * about `bound + log2(n)` whatever the geometry does.  It is a
         * guard rail, not a policy: no shipped track reaches it. */
        if (extent > 1e-6f && w.depth < 48u) {
            scale = (float)CXV_BINS / extent;
            for (k = 0; k < CXV_BINS; k++) {
                cxv_box_reset(bin_lo[k], bin_hi[k]);
                bin_n[k] = 0;
            }
            for (i = lo; i < hi; i++) {
                const CxvTri *t = &B->tri[B->ord[i]];
                int bi = (int)((t->c[axis] - clo[axis]) * scale);
                if (bi < 0) bi = 0;
                if (bi >= CXV_BINS) bi = CXV_BINS - 1;
                bin_n[bi]++;
                cxv_box_union(bin_lo[bi], bin_hi[bi], t->lo, t->hi);
            }
            cxv_box_reset(right_lo[CXV_BINS - 1], right_hi[CXV_BINS - 1]);
            right_n[CXV_BINS - 1] = 0;
            for (k = CXV_BINS - 1; k >= 1; k--) {
                memcpy(right_lo[k - 1], right_lo[k], sizeof right_lo[k]);
                memcpy(right_hi[k - 1], right_hi[k], sizeof right_hi[k]);
                right_n[k - 1] = right_n[k] + bin_n[k];
                cxv_box_union(right_lo[k - 1], right_hi[k - 1],
                              bin_lo[k], bin_hi[k]);
            }
            {
                float    llo[3], lhi[3], best = 3.4e38f;
                uint32_t ln = 0;
                cxv_box_reset(llo, lhi);
                for (k = 0; k < CXV_BINS - 1; k++) {
                    float c;
                    ln += bin_n[k];
                    cxv_box_union(llo, lhi, bin_lo[k], bin_hi[k]);
                    if (!ln || !right_n[k]) continue;
                    c = (float)ln * cxv_box_area(llo, lhi) +
                        (float)right_n[k] *
                        cxv_box_area(right_lo[k], right_hi[k]);
                    if (c < best) { best = c; best_bin = k; }
                }
            }
            if (best_bin >= 0) {
                uint32_t nl = 0, nr = 0;
                for (i = lo; i < hi; i++) {
                    const CxvTri *t = &B->tri[B->ord[i]];
                    int bi = (int)((t->c[axis] - clo[axis]) * scale);
                    if (bi < 0) bi = 0;
                    if (bi >= CXV_BINS) bi = CXV_BINS - 1;
                    if (bi <= best_bin) B->ord[lo + nl++] = B->ord[i];
                    else                tmp[nr++] = B->ord[i];
                }
                memcpy(B->ord + lo + nl, tmp, (size_t)nr * sizeof *tmp);
                mid = lo + nl;
            }
        }
        if (mid <= lo || mid >= hi) mid = lo + n / 2;

        POOL_NEW(l_idx);
        POOL_NEW(r_idx);
        pool[w.node].left  = (int32_t)l_idx;
        pool[w.node].right = (int32_t)r_idx;
        /* LIFO: push right first so left is popped first.  The pool order
         * that produces is irrelevant -- phase 2 lays the tree out. */
        PUSH(r_idx, mid, hi, w.depth + 1u);
        PUSH(l_idx, lo, mid, w.depth + 1u);
    }

#undef PUSH
#undef POOL_NEW

    /* ---- phase 2: flatten depth-first, filling in the escapes ---------- */
    B->node = (CxvNode *)malloc(npool * sizeof *B->node);
    if (!B->node) goto done;
    B->capnode = npool;
    B->nnode = 0;

    {
        /* one entry per pending step: `src >= 0` is "emit this pool node",
         * `src < 0` is "the subtree rooted at emitted node `out` is done". */
        struct { int32_t src; uint32_t out; } *st = NULL;
        size_t ns = 0, cs = 0;
        int ok = 0;

#define ST_PUSH(s, o) do {                                                   \
        if (ns == cs) {                                                      \
            size_t c_ = cs ? cs * 2 : 256;                                   \
            void *n_ = realloc(st, c_ * sizeof *st);                         \
            if (!n_) { free(st); goto done; }                                \
            st = n_; cs = c_;                                                \
        }                                                                    \
        st[ns].src = (s); st[ns].out = (o); ns++;                            \
    } while (0)

        ST_PUSH(0, 0u);
        while (ns) {
            int32_t  src = st[ns - 1].src;
            uint32_t out = st[ns - 1].out;
            ns--;
            if (src < 0) {                       /* subtree closed */
                B->node[out].escape = (uint32_t)B->nnode;
                continue;
            }
            {
                CxvPool *p = &pool[src];
                uint32_t me = (uint32_t)B->nnode++;
                CxvNode *N = &B->node[me];
                memcpy(N->lo, p->lo, sizeof N->lo);
                memcpy(N->hi, p->hi, sizeof N->hi);
                N->first = p->first;
                N->count = p->count;
                N->depth = p->depth;
                N->escape = me + 1u;             /* a leaf's, and correct */
                if (!p->count) {
                    ST_PUSH(-1, me);
                    ST_PUSH(p->right, 0u);
                    ST_PUSH(p->left, 0u);
                }
            }
        }
        ok = 1;
#undef ST_PUSH
        free(st);
        if (!ok) goto done;
    }

    rc = 0;

done:
    free(pool);
    free(stack);
    free(tmp);
    return rc;
}

/* ------------------------------------------------------------------ write */

void cxv_put_node(cxc_buf *o, const CxvNode *n)
{
    int k;
    for (k = 0; k < 3; k++) cxc_put_f32(o, n->lo[k]);
    for (k = 0; k < 3; k++) cxc_put_f32(o, n->hi[k]);
    cxc_put_u32(o, n->escape);
    cxc_put_u32(o, n->first);
    cxc_put_u32(o, n->count);
    cxc_put_u32(o, n->depth);
    cxc_put_u32(o, 0u);
    cxc_put_u32(o, 0u);
}

void cxv_build_free(CxvBuild *B)
{
    free(B->tri);
    free(B->node);
    free(B->ord);
    memset(B, 0, sizeof *B);
}

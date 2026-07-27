/* ===========================================================================
 * layout_tree.c — manual (BSP) tiling, and tabbed / stacked containers.
 *
 * THE ONE STRUCTURAL DECISION, stated up front because everything else follows
 * from it: the tree does NOT own the windows.
 *
 * Desktop.windows[] stays the membership and ordering store. It is what
 * collect_clients walks, what the seven dynamic layouts consume, what
 * Desktop.focused indexes into, and what attach policy, promote, zoom and the
 * mouse swap all manipulate. The tree is an INDEX synchronised to it: leaves
 * reference windows by HWND, and any window in the array without a leaf is
 * attached on the next pass while any leaf whose window has gone is pruned.
 *
 * The alternative — making the tree authoritative — would mean rewriting all of
 * the above and leaving mshell with one layout that works differently from the
 * other seven. This way BSP is an eighth layout, a desktop switches to and from
 * it freely, and a desktop that never uses it pays for nothing but a NULL
 * pointer.
 *
 * The cost of the decision is that the tree can be *stale*, so every entry
 * point begins by reconciling it. That is deliberate: reconciling is O(n) over
 * a desktop's windows, which is the same order the tiler already runs at, and
 * it means no other file has to know the tree exists.
 *
 * CONTAINERS. A split node whose mode is TABBED or STACKED shows one child at a
 * time instead of dividing its area. That reuses the `layout_hidden` flag
 * monocle needs anyway, so the machinery for "in the layout but not on screen"
 * is shared rather than duplicated.
 * =========================================================================== */
#include "mshell.h"
#include "layout_math.h"

/* Nodes come from a fixed pool per desktop rather than malloc: the WM's hot
 * paths do no allocation anywhere else, and a tree deep enough to exhaust this
 * is a tree nobody can read. */
#define TREE_MAX_NODES (MAX_WINDOWS_PER_DESKTOP * 2)

typedef struct TreeNode {
    /* A leaf has hwnd != NULL and no children; a split has two children. */
    HWND             hwnd;
    struct TreeNode *a, *b;      /* first / second child           */
    struct TreeNode *parent;
    SplitMode        mode;       /* SPLIT_H / SPLIT_V / TABBED / STACKED */
    float            ratio;      /* a's share of the axis, 0.1 .. 0.9    */
    int              active;     /* container: which child is showing    */
    bool             used;
} TreeNode;

typedef struct {
    TreeNode  pool[TREE_MAX_NODES];
    TreeNode *root;
    int       in_use;
} Tree;

/* One tree per desktop slot. Indexed by desktop ID modulo the table, resolved
 * through a small map so a desktop's tree follows its ID rather than its slot
 * (slots are re-sorted on every create). */
static Tree s_trees[MAX_DESKTOPS];
static int  s_tree_owner[MAX_DESKTOPS];   /* desktop id owning each tree, 0 = free */

static Tree *tree_for(int desktop_id, bool create) {
    for (int i = 0; i < MAX_DESKTOPS; i++)
        if (s_tree_owner[i] == desktop_id) return &s_trees[i];
    if (!create) return NULL;

    for (int i = 0; i < MAX_DESKTOPS; i++) {
        if (s_tree_owner[i] == 0 || !desktop_by_id(s_tree_owner[i])) {
            /* Free, or owned by a desktop that no longer exists. Reclaiming the
             * latter lazily is what keeps this table the same size as the
             * desktop table without a destroy hook. */
            memset(&s_trees[i], 0, sizeof(s_trees[i]));
            s_tree_owner[i] = desktop_id;
            return &s_trees[i];
        }
    }
    return NULL;
}

static TreeNode *node_alloc(Tree *t) {
    for (int i = 0; i < TREE_MAX_NODES; i++) {
        if (t->pool[i].used) continue;
        TreeNode *n = &t->pool[i];
        memset(n, 0, sizeof(*n));
        n->used  = true;
        n->ratio = 0.5f;
        t->in_use++;
        return n;
    }
    return NULL;
}

static void node_free(Tree *t, TreeNode *n) {
    if (!n || !n->used) return;
    n->used = false;
    t->in_use--;
}

static TreeNode *tree_find(TreeNode *n, HWND hwnd) {
    if (!n) return NULL;
    if (n->hwnd == hwnd) return n;
    TreeNode *r = tree_find(n->a, hwnd);
    return r ? r : tree_find(n->b, hwnd);
}

/* The leftmost leaf, used as the insertion point when nothing is focused. */
static TreeNode *tree_first_leaf(TreeNode *n) {
    if (!n) return NULL;
    if (n->hwnd) return n;
    TreeNode *r = tree_first_leaf(n->a);
    return r ? r : tree_first_leaf(n->b);
}

/* ---------------------------------------------------------------------------
 * Insert `hwnd` by splitting `at`, which becomes a split node with the old
 * window in one child and the new one in the other.
 *
 * Splitting the FOCUSED leaf is what makes manual tiling manual: where a window
 * lands is a consequence of where you were, not of an insertion policy.
 * --------------------------------------------------------------------------- */
static void tree_insert(Tree *t, TreeNode *at, HWND hwnd, SplitMode mode) {
    if (!t->root) {
        TreeNode *n = node_alloc(t);
        if (!n) return;
        n->hwnd = hwnd;
        t->root = n;
        return;
    }
    if (!at) at = tree_first_leaf(t->root);
    if (!at) return;

    TreeNode *moved = node_alloc(t);
    TreeNode *fresh = node_alloc(t);
    if (!moved || !fresh) { node_free(t, moved); node_free(t, fresh); return; }

    moved->hwnd   = at->hwnd;
    moved->parent = at;
    fresh->hwnd   = hwnd;
    fresh->parent = at;

    /* `at` stops being a leaf and becomes the split. Reusing the node rather
     * than allocating a new one keeps every parent pointer above it valid. */
    at->hwnd   = NULL;
    at->a      = moved;
    at->b      = fresh;
    at->mode   = mode;
    at->ratio  = 0.5f;
    at->active = 1;                /* a new tab is the one you want to see */
}

/* Remove a leaf; its sibling takes the parent's place. */
static void tree_remove(Tree *t, HWND hwnd) {
    TreeNode *n = tree_find(t->root, hwnd);
    if (!n) return;

    TreeNode *p = n->parent;
    if (!p) {                       /* the only window */
        node_free(t, n);
        t->root = NULL;
        return;
    }

    TreeNode *sib = (p->a == n) ? p->b : p->a;

    /* Collapse the parent into the sibling. The parent node is reused as the
     * sibling's content so nothing above it has to be re-pointed. */
    p->hwnd   = sib->hwnd;
    p->mode   = sib->mode;
    p->ratio  = sib->ratio;
    p->active = sib->active;
    p->a      = sib->a;
    p->b      = sib->b;
    if (p->a) p->a->parent = p;
    if (p->b) p->b->parent = p;

    node_free(t, sib);
    node_free(t, n);
}

/* ---------------------------------------------------------------------------
 * Reconcile the tree with the desktop's window list.
 *
 * This is the price of the tree not owning the windows, and it is paid here so
 * that nothing outside this file has to call an attach or detach hook. Windows
 * that appeared are inserted at the focused leaf; leaves whose window has left
 * the desktop (closed, moved, floated) are removed.
 * --------------------------------------------------------------------------- */
static void tree_sync(Tree *t, Desktop *dt) {
    /* Prune first: removing frees nodes that inserting may need. */
    for (int guard = 0; guard < TREE_MAX_NODES; guard++) {
        HWND stale = NULL;

        for (int i = 0; i < TREE_MAX_NODES && !stale; i++) {
            TreeNode *n = &t->pool[i];
            if (!n->used || !n->hwnd) continue;

            bool present = false;
            for (int j = 0; j < dt->count; j++) {
                if (dt->windows[j] != n->hwnd) continue;
                ManagedWindow *mw = window_find(dt->windows[j]);
                present = mw && !mw->is_floating && !mw->app_hidden &&
                          !IsIconic(dt->windows[j]);
                break;
            }
            if (!present) stale = n->hwnd;
        }

        if (!stale) break;
        tree_remove(t, stale);
    }

    /* Then insert anything new, at the focused leaf. */
    HWND focus = desktop_get_focused();
    for (int i = 0; i < dt->count; i++) {
        HWND h = dt->windows[i];
        ManagedWindow *mw = window_find(h);
        if (!mw || mw->is_floating || mw->app_hidden || IsIconic(h)) continue;
        if (tree_find(t->root, h)) continue;

        TreeNode *at = (focus && focus != h) ? tree_find(t->root, focus) : NULL;
        tree_insert(t, at, h, g.next_split);
    }
}

/* ---------------------------------------------------------------------------
 * Walk the tree, emitting a rect per visible leaf.
 * --------------------------------------------------------------------------- */
/* Mark every leaf under `n` hidden — the other side of a container. */
static void tree_hide_all(TreeNode *n) {
    if (!n) return;
    if (n->hwnd) {
        ManagedWindow *mw = window_find(n->hwnd);
        if (mw) mw->layout_hidden = true;
        return;
    }
    tree_hide_all(n->a);
    tree_hide_all(n->b);
}

static void tree_place(TreeNode *n, RECT area, TreeEmitFn emit, void *ctx) {
    if (!n) return;

    if (n->hwnd) { emit(n->hwnd, area, ctx); return; }

    if (n->mode == SPLIT_TABBED || n->mode == SPLIT_STACKED) {
        /* A container gives its whole area to one child and hides the rest.
         * Hiding is what layout_hidden exists for — the same mechanism monocle
         * uses, rather than a second way of saying "not on screen". */
        TreeNode *show = n->active ? n->b : n->a;
        TreeNode *hide = n->active ? n->a : n->b;
        tree_place(show, area, emit, ctx);
        tree_hide_all(hide);
        return;
    }

    float r = clamp_f(n->ratio, 0.1f, 0.9f);
    RECT  x = area, y = area;

    if (n->mode == SPLIT_V) {
        int w = (int)((float)(area.right - area.left) * r);
        x.right = area.left + w;
        y.left  = area.left + w;
    } else {
        int h = (int)((float)(area.bottom - area.top) * r);
        x.bottom = area.top + h;
        y.top    = area.top + h;
    }
    tree_place(n->a, x, emit, ctx);
    tree_place(n->b, y, emit, ctx);
}

/* ---------------------------------------------------------------------------
 * The public entry point: lay out one monitor's slice using the tree.
 * --------------------------------------------------------------------------- */
void layout_tree_run(Desktop *dt, RECT area, TreeEmitFn emit, void *ctx) {
    Tree *t = tree_for(dt->id, true);
    if (!t) return;

    tree_sync(t, dt);
    if (!t->root) return;

    /* Everything starts visible; the container walk marks what it hides. */
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (mw) mw->layout_hidden = false;
    }

    tree_place(t->root, area, emit, ctx);
}

/* ---------------------------------------------------------------------------
 * Actions
 * --------------------------------------------------------------------------- */

/* The split direction the NEXT window will use. Set rather than applied
 * immediately, because "split vertically" is a statement about where the next
 * window goes — there is nothing to split until one arrives. */
void layout_tree_set_split(SplitMode mode) {
    g.next_split = mode;
    log_msg(LOG_INFO, L"next split: %ls",
            mode == SPLIT_V ? L"vertical" : L"horizontal");
}

/* Flip the split that contains the focused window. */
void layout_tree_rotate(void) {
    Desktop *dt = desktop_current();
    Tree    *t  = tree_for(dt->id, false);
    HWND     f  = desktop_get_focused();
    if (!t || !f) return;

    TreeNode *n = tree_find(t->root, f);
    if (!n || !n->parent) return;

    TreeNode *p = n->parent;
    p->mode = (p->mode == SPLIT_V) ? SPLIT_H : SPLIT_V;
    tile_current();
}

/* Turn the focused window's split into a tabbed/stacked container, or back. */
void layout_tree_set_container(SplitMode mode) {
    Desktop *dt = desktop_current();
    Tree    *t  = tree_for(dt->id, false);
    HWND     f  = desktop_get_focused();
    if (!t || !f) return;

    TreeNode *n = tree_find(t->root, f);
    if (!n || !n->parent) return;

    TreeNode *p = n->parent;
    /* Asking for the mode it already has means "go back to a plain split",
     * which is what makes one binding a toggle rather than a one-way door. */
    p->mode   = (p->mode == mode) ? SPLIT_V : mode;
    p->active = (p->b == n) ? 1 : 0;
    tile_current();
}

/* Show the next child of the focused window's container. */
void layout_tree_cycle_container(int delta) {
    Desktop *dt = desktop_current();
    Tree    *t  = tree_for(dt->id, false);
    HWND     f  = desktop_get_focused();
    if (!t || !f) return;

    TreeNode *n = tree_find(t->root, f);
    while (n && n->parent &&
           n->parent->mode != SPLIT_TABBED && n->parent->mode != SPLIT_STACKED)
        n = n->parent;
    if (!n || !n->parent) return;

    TreeNode *p = n->parent;
    p->active = (p->active + delta) & 1;

    /* Focus follows the tab you switched to, or you would be looking at one
     * window and typing into another. */
    TreeNode *now = p->active ? p->b : p->a;
    TreeNode *leaf = tree_first_leaf(now);
    if (leaf && leaf->hwnd) {
        desktop_focus_update(leaf->hwnd);
        tile_current();
        window_focus(leaf->hwnd);
    } else {
        tile_current();
    }
}

/* Resize the split containing the focused window. */
void layout_tree_resize(float delta) {
    Desktop *dt = desktop_current();
    Tree    *t  = tree_for(dt->id, false);
    HWND     f  = desktop_get_focused();
    if (!t || !f) return;

    TreeNode *n = tree_find(t->root, f);
    if (!n || !n->parent) return;

    /* Growing "the focused window" means growing whichever side it is on, so
     * the same key does the intuitive thing from either child. */
    TreeNode *p = n->parent;
    p->ratio = clamp_f(p->ratio + (p->a == n ? delta : -delta), 0.1f, 0.9f);
    tile_current();
}

/* Drop a desktop's tree — called when the desktop is destroyed so the pool slot
 * is reusable straight away rather than at the next allocation. */
void layout_tree_forget(int desktop_id) {
    for (int i = 0; i < MAX_DESKTOPS; i++)
        if (s_tree_owner[i] == desktop_id) {
            memset(&s_trees[i], 0, sizeof(s_trees[i]));
            s_tree_owner[i] = 0;
            return;
        }
}


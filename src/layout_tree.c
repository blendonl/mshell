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
 * There is one tree per desktop AND MONITOR, because the tiler lays a desktop
 * out one display at a time and a tree spanning them all would place every
 * window on every display. See the table below.
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

/* ---------------------------------------------------------------------------
 * One tree per DESKTOP AND MONITOR, not per desktop.
 *
 * A desktop spans every display, and the tiler already treats it that way: it
 * groups the desktop's windows by monitor and lays each group out into that
 * monitor's work area, once per monitor. A single tree per desktop cannot
 * survive that. It holds every window on the desktop, so each monitor's pass
 * placed ALL of them into that monitor's area — every window positioned twice
 * per tiling pass, ending up wherever the last monitor's pass put it, with the
 * other display's windows piled on top of it.
 *
 * Keyed by the pair, each pass sees only the windows that live on the display
 * being laid out, and the splits you build on one screen are that screen's.
 *
 * The table is still MAX_DESKTOPS entries rather than desktops × monitors: a
 * tree is 512 nodes, so a full cross product would be megabytes of mostly
 * untouched pool for a layout most desktops never select. Entries are claimed
 * on demand and reclaimed from desktops that are gone and monitors that have
 * been unplugged, which covers any realistic number of BSP desktops. If it does
 * fill up, layout_tree_run says so rather than dropping the windows — see the
 * fallback there.
 * --------------------------------------------------------------------------- */
typedef struct {
    int desktop_id;      /* 0 = free */
    int monitor;
} TreeOwner;

static Tree      s_trees[MAX_DESKTOPS];
static TreeOwner s_tree_owner[MAX_DESKTOPS];

/* The monitor a window counts as being on, spelled exactly the way
 * collect_clients spells it — including the defensive clamp, so a window with a
 * stale index lands in the same tree the tiler will lay out. */
static int tree_monitor_of(const ManagedWindow *mw) {
    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    return mon;
}

/* Is this window one the tree on `mon` is responsible for? The same filter
 * collect_clients applies, plus the monitor test — the two must agree, or a
 * window is tiled by one and ignored by the other. */
static bool tree_owns_window(HWND hwnd, int mon) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || mw->is_floating || mw->tracked_only || mw->app_hidden) return false;
    if (IsIconic(hwnd)) return false;
    return tree_monitor_of(mw) == mon;
}

static Tree *tree_for(int desktop_id, int monitor, bool create) {
    for (int i = 0; i < MAX_DESKTOPS; i++)
        if (s_tree_owner[i].desktop_id == desktop_id &&
            s_tree_owner[i].monitor    == monitor) return &s_trees[i];
    if (!create) return NULL;

    for (int i = 0; i < MAX_DESKTOPS; i++) {
        TreeOwner *o = &s_tree_owner[i];
        /* Free, owned by a desktop that no longer exists, or owned by a monitor
         * that has been unplugged. Reclaiming the last two lazily is what keeps
         * this table small without a destroy hook or a hotplug hook. */
        bool stale = o->desktop_id == 0 ||
                     !desktop_by_id(o->desktop_id) ||
                     (g.monitor_count > 0 && o->monitor >= g.monitor_count);
        if (!stale) continue;

        memset(&s_trees[i], 0, sizeof(s_trees[i]));
        o->desktop_id = desktop_id;
        o->monitor    = monitor;
        return &s_trees[i];
    }

    static bool warned;
    if (!warned) {
        warned = true;
        log_msg(LOG_WARN, L"bsp: no tree left for desktop %d monitor %d — "
                          L"%d desktop/monitor pairs are already using the "
                          L"manual layout. That display falls back to "
                          L"master-stack.",
                desktop_id, monitor, MAX_DESKTOPS);
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
static void tree_sync(Tree *t, Desktop *dt, int mon) {
    /* Prune first: removing frees nodes that inserting may need. A window that
     * has MOVED to another display is stale here too — it is pruned from this
     * tree and inserted into that monitor's on its pass. */
    for (int guard = 0; guard < TREE_MAX_NODES; guard++) {
        HWND stale = NULL;

        for (int i = 0; i < TREE_MAX_NODES && !stale; i++) {
            TreeNode *n = &t->pool[i];
            if (!n->used || !n->hwnd) continue;

            bool present = false;
            for (int j = 0; j < dt->count; j++) {
                if (dt->windows[j] != n->hwnd) continue;
                present = tree_owns_window(dt->windows[j], mon);
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
        if (!tree_owns_window(h, mon)) continue;
        if (tree_find(t->root, h)) continue;

        /* tree_find returns NULL for a focus that is on another display, which
         * is what we want: a window arriving on THIS screen splits the leftmost
         * leaf of THIS screen, not nothing at all. */
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
bool layout_tree_run(Desktop *dt, int monitor, RECT area, TreeEmitFn emit,
                     void *ctx) {
    Tree *t = tree_for(dt->id, monitor, true);
    if (!t) return false;              /* table full — the caller falls back */

    tree_sync(t, dt, monitor);
    if (!t->root) return false;        /* nothing of ours here; let the caller
                                        * place whatever it thinks is */

    /* Everything on THIS display starts visible; the container walk marks what
     * it hides. Restricted to this monitor's windows because the other
     * monitor's pass has already decided about its own, and clearing the flag
     * for the whole desktop would un-hide the far side of a container over
     * there — the same pass that just hid it. */
    for (int i = 0; i < dt->count; i++) {
        if (!tree_owns_window(dt->windows[i], monitor)) continue;
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (mw) mw->layout_hidden = false;
    }

    tree_place(t->root, area, emit, ctx);
    return true;
}

/* ---------------------------------------------------------------------------
 * Actions
 * --------------------------------------------------------------------------- */

/* Every action below acts on the split holding the FOCUSED window, so the tree
 * it needs is the one for that window's display. Resolved here rather than in
 * five places, and never created: an action before the first tiling pass has
 * nothing to act on either way. */
static Tree *tree_of_focus(HWND *focus_out) {
    HWND f = desktop_get_focused();
    if (!f) return NULL;

    ManagedWindow *mw = window_find(f);
    if (!mw) return NULL;

    *focus_out = f;
    return tree_for(desktop_current()->id, tree_monitor_of(mw), false);
}

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
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
    if (!t || !f) return;

    TreeNode *n = tree_find(t->root, f);
    if (!n || !n->parent) return;

    TreeNode *p = n->parent;
    p->mode = (p->mode == SPLIT_V) ? SPLIT_H : SPLIT_V;
    tile_current();
}

/* Turn the focused window's split into a tabbed/stacked container, or back. */
void layout_tree_set_container(SplitMode mode) {
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
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
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
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
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
    if (!t || !f) return;

    TreeNode *n = tree_find(t->root, f);
    if (!n || !n->parent) return;

    /* Growing "the focused window" means growing whichever side it is on, so
     * the same key does the intuitive thing from either child. */
    TreeNode *p = n->parent;
    p->ratio = clamp_f(p->ratio + (p->a == n ? delta : -delta), 0.1f, 0.9f);
    tile_current();
}

/* Drop a desktop's trees — called when the desktop is destroyed so the pool
 * slots are reusable straight away rather than at the next allocation. Plural:
 * a desktop that spanned three displays owns three of them, and stopping at the
 * first would strand the rest until something else needed the space. */
void layout_tree_forget(int desktop_id) {
    for (int i = 0; i < MAX_DESKTOPS; i++)
        if (s_tree_owner[i].desktop_id == desktop_id) {
            memset(&s_trees[i], 0, sizeof(s_trees[i]));
            s_tree_owner[i].desktop_id = 0;
            s_tree_owner[i].monitor    = 0;
        }
}


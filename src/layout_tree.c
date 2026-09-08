#include "mshell.h"
#include "tree_algebra.h"

_Static_assert(TREE_MAX_NODES >= MAX_WINDOWS_PER_DESKTOP * 2,
               "the node pool must hold an internal node per window");
_Static_assert((int)TREE_SPLIT_V == (int)SPLIT_V, "SplitMode must map onto TreeSplit");
_Static_assert((int)TREE_SPLIT_H == (int)SPLIT_H, "SplitMode must map onto TreeSplit");
_Static_assert((int)TREE_SPLIT_TABBED == (int)SPLIT_TABBED, "SplitMode must map onto TreeSplit");
_Static_assert((int)TREE_SPLIT_STACKED == (int)SPLIT_STACKED, "SplitMode must map onto TreeSplit");

typedef struct {
    int desktop_id;
    int monitor;
} TreeOwner;

static Tree      s_trees[MAX_DESKTOPS];
static TreeOwner s_tree_owner[MAX_DESKTOPS];

static int tree_monitor_of(const ManagedWindow *mw) {
    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    return mon;
}

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
        bool stale = o->desktop_id == 0 ||
                     !desktop_by_id(o->desktop_id) ||
                     (g.monitor_count > 0 && o->monitor >= g.monitor_count);
        if (!stale) continue;

        tree_reset(&s_trees[i]);
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

static void tree_sync(Tree *t, Desktop *dt, int mon) {
    HWND stale[TREE_MAX_NODES];
    int  stale_n = 0;

    for (int i = 0; i < TREE_MAX_NODES; i++) {
        TreeNode *n = &t->pool[i];
        if (!n->used || !n->window) continue;

        bool present = false;
        for (int j = 0; j < dt->count; j++) {
            if (dt->windows[j] != (HWND)n->window) continue;
            present = tree_owns_window(dt->windows[j], mon);
            break;
        }
        if (!present) stale[stale_n++] = (HWND)n->window;
    }

    for (int i = 0; i < stale_n; i++) tree_remove(t, stale[i]);

    HWND focus = desktop_focused_of(dt);
    for (int i = 0; i < dt->count; i++) {
        HWND h = dt->windows[i];
        if (!tree_owns_window(h, mon)) continue;
        if (tree_find(t, h)) continue;

        TreeNode *at = (focus && focus != h) ? tree_find(t, focus) : NULL;
        tree_insert(t, at, h, (TreeSplit)g.next_split);
    }
}

typedef struct {
    TreeEmitFn emit;
    void      *ctx;
} EmitCtx;

static void tree_emit_place(void *window, TreeRect area, void *ctx) {
    EmitCtx *e = (EmitCtx *)ctx;
    RECT     r = { area.left, area.top, area.right, area.bottom };
    e->emit((HWND)window, r, e->ctx);
}

static void tree_emit_hide(void *window, void *ctx) {
    (void)ctx;
    ManagedWindow *mw = window_find((HWND)window);
    if (mw) mw->layout_hidden = true;
}

bool layout_tree_run(Desktop *dt, int monitor, RECT area, TreeEmitFn emit,
                     void *ctx) {
    Tree *t = tree_for(dt->id, monitor, true);
    if (!t) return false;

    tree_sync(t, dt, monitor);
    if (!t->root) return false;

    for (int i = 0; i < dt->count; i++) {
        if (!tree_owns_window(dt->windows[i], monitor)) continue;
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (mw) mw->layout_hidden = false;
    }

    EmitCtx  e = { emit, ctx };
    TreeRect r = { area.left, area.top, area.right, area.bottom };
    tree_place(t, r, tree_emit_place, tree_emit_hide, &e);
    return true;
}

static Tree *tree_of_focus(HWND *focus_out) {
    HWND f = desktop_get_focused();
    if (!f) return NULL;

    ManagedWindow *mw = window_find(f);
    if (!mw) return NULL;

    *focus_out = f;
    return tree_for(desktop_current()->id, tree_monitor_of(mw), false);
}

void layout_tree_set_split(SplitMode mode) {
    g.next_split = mode;
    log_msg(LOG_INFO, L"next split: %ls",
            mode == SPLIT_V ? L"vertical" : L"horizontal");
}

void layout_tree_rotate(void) {
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
    if (!t || !f) return;

    if (tree_rotate(t, f)) tile_current();
}

void layout_tree_set_container(SplitMode mode) {
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
    if (!t || !f) return;

    if (tree_set_container(t, f, (TreeSplit)mode)) tile_current();
}

void layout_tree_cycle_container(int delta) {
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
    if (!t || !f) return;

    void *next = NULL;
    if (!tree_cycle_container(t, f, delta, &next)) return;

    if (next) {
        desktop_focus_update((HWND)next);
        tile_current();
        window_focus((HWND)next);
    } else {
        tile_current();
    }
}

void layout_tree_resize(float delta) {
    HWND  f = NULL;
    Tree *t = tree_of_focus(&f);
    if (!t || !f) return;

    if (tree_resize(t, f, delta)) tile_current();
}

void layout_tree_forget(int desktop_id) {
    for (int i = 0; i < MAX_DESKTOPS; i++)
        if (s_tree_owner[i].desktop_id == desktop_id) {
            tree_reset(&s_trees[i]);
            s_tree_owner[i].desktop_id = 0;
            s_tree_owner[i].monitor    = 0;
        }
}

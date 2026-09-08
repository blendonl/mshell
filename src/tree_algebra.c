#include "tree_algebra.h"

#include <string.h>

#define TREE_RATIO_MIN 0.1f
#define TREE_RATIO_MAX 0.9f

float tree_clamp_ratio(float v) {
    if (v < TREE_RATIO_MIN) return TREE_RATIO_MIN;
    if (v > TREE_RATIO_MAX) return TREE_RATIO_MAX;
    return v;
}

void tree_reset(Tree *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
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

static TreeNode *find_from(TreeNode *n, const void *window) {
    if (!n) return NULL;
    if (n->window == window) return n;
    TreeNode *r = find_from(n->a, window);
    return r ? r : find_from(n->b, window);
}

TreeNode *tree_find(const Tree *t, const void *window) {
    if (!t || !window) return NULL;
    return find_from(t->root, window);
}

TreeNode *tree_first_leaf(TreeNode *n) {
    if (!n) return NULL;
    if (n->window) return n;
    TreeNode *r = tree_first_leaf(n->a);
    return r ? r : tree_first_leaf(n->b);
}

static int leaves_under(const TreeNode *n) {
    if (!n) return 0;
    if (n->window) return 1;
    return leaves_under(n->a) + leaves_under(n->b);
}

int tree_leaf_count(const Tree *t) {
    return t ? leaves_under(t->root) : 0;
}

int tree_depth_of(const TreeNode *n) {
    int d = 0;
    for (const TreeNode *p = n; p && p->parent; p = p->parent) d++;
    return d;
}

bool tree_insert(Tree *t, TreeNode *at, void *window, TreeSplit mode) {
    if (!t || !window) return false;

    if (!t->root) {
        TreeNode *n = node_alloc(t);
        if (!n) return false;
        n->window = window;
        t->root   = n;
        return true;
    }

    if (!at) at = tree_first_leaf(t->root);
    if (!at || !at->window) return false;

    TreeNode *moved = node_alloc(t);
    TreeNode *fresh = node_alloc(t);
    if (!moved || !fresh) {
        node_free(t, moved);
        node_free(t, fresh);
        return false;
    }

    moved->window = at->window;
    moved->parent = at;
    fresh->window = window;
    fresh->parent = at;

    at->window = NULL;
    at->a      = moved;
    at->b      = fresh;
    at->mode   = mode;
    at->ratio  = 0.5f;
    at->active = 1;
    return true;
}

bool tree_remove(Tree *t, const void *window) {
    TreeNode *n = tree_find(t, window);
    if (!n) return false;

    TreeNode *p = n->parent;
    if (!p) {
        node_free(t, n);
        t->root = NULL;
        return true;
    }

    TreeNode *sib = (p->a == n) ? p->b : p->a;

    p->window = sib->window;
    p->mode   = sib->mode;
    p->ratio  = sib->ratio;
    p->active = sib->active;
    p->a      = sib->a;
    p->b      = sib->b;
    if (p->a) p->a->parent = p;
    if (p->b) p->b->parent = p;

    node_free(t, sib);
    node_free(t, n);
    return true;
}

static void hide_all(const TreeNode *n, TreeHideFn hide, void *ctx) {
    if (!n) return;
    if (n->window) {
        if (hide) hide(n->window, ctx);
        return;
    }
    hide_all(n->a, hide, ctx);
    hide_all(n->b, hide, ctx);
}

static void place_from(const TreeNode *n, TreeRect area,
                       TreePlaceFn place, TreeHideFn hide, void *ctx) {
    if (!n) return;

    if (n->window) {
        if (place) place(n->window, area, ctx);
        return;
    }

    if (n->mode == TREE_SPLIT_TABBED || n->mode == TREE_SPLIT_STACKED) {
        const TreeNode *show = n->active ? n->b : n->a;
        const TreeNode *skip = n->active ? n->a : n->b;
        place_from(show, area, place, hide, ctx);
        hide_all(skip, hide, ctx);
        return;
    }

    float    r = tree_clamp_ratio(n->ratio);
    TreeRect x = area, y = area;

    if (n->mode == TREE_SPLIT_V) {
        int w = (int)((float)(area.right - area.left) * r);
        x.right = area.left + w;
        y.left  = area.left + w;
    } else {
        int h = (int)((float)(area.bottom - area.top) * r);
        x.bottom = area.top + h;
        y.top    = area.top + h;
    }

    place_from(n->a, x, place, hide, ctx);
    place_from(n->b, y, place, hide, ctx);
}

void tree_place(const Tree *t, TreeRect area,
                TreePlaceFn place, TreeHideFn hide, void *ctx) {
    if (!t) return;
    place_from(t->root, area, place, hide, ctx);
}

bool tree_rotate(Tree *t, const void *window) {
    TreeNode *n = tree_find(t, window);
    if (!n || !n->parent) return false;

    TreeNode *p = n->parent;
    p->mode = (p->mode == TREE_SPLIT_V) ? TREE_SPLIT_H : TREE_SPLIT_V;
    return true;
}

bool tree_resize(Tree *t, const void *window, float delta) {
    TreeNode *n = tree_find(t, window);
    if (!n || !n->parent) return false;

    TreeNode *p = n->parent;
    p->ratio = tree_clamp_ratio(p->ratio + (p->a == n ? delta : -delta));
    return true;
}

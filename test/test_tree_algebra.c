#include "../src/tree_algebra.h"
#include "tests.h"

#include <stdlib.h>

static char handles[TREE_MAX_NODES + 8];

static void *win(int i) { return &handles[i]; }

typedef struct {
    void    *window;
    TreeRect area;
} Placed;

typedef struct {
    Placed placed[TREE_MAX_NODES];
    int    placed_n;
    void  *hidden[TREE_MAX_NODES];
    int    hidden_n;
} Capture;

static void on_place(void *window, TreeRect area, void *ctx) {
    Capture *c = (Capture *)ctx;
    if (c->placed_n >= TREE_MAX_NODES) return;
    c->placed[c->placed_n].window = window;
    c->placed[c->placed_n].area   = area;
    c->placed_n++;
}

static void on_hide(void *window, void *ctx) {
    Capture *c = (Capture *)ctx;
    if (c->hidden_n >= TREE_MAX_NODES) return;
    c->hidden[c->hidden_n++] = window;
}

static Capture run(const Tree *t, TreeRect area) {
    Capture c = {0};
    tree_place(t, area, on_place, on_hide, &c);
    return c;
}

static const Placed *placed_for(const Capture *c, void *window) {
    for (int i = 0; i < c->placed_n; i++)
        if (c->placed[i].window == window) return &c->placed[i];
    return NULL;
}

static bool was_hidden(const Capture *c, void *window) {
    for (int i = 0; i < c->hidden_n; i++)
        if (c->hidden[i] == window) return true;
    return false;
}

static TreeRect rect(int l, int t, int r, int b) {
    TreeRect out = {l, t, r, b};
    return out;
}

static bool rect_eq(TreeRect a, TreeRect b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

static bool parents_consistent(const TreeNode *n, const TreeNode *parent) {
    if (!n) return true;
    if (n->parent != parent) return false;
    if (n->window && (n->a || n->b)) return false;
    if (!n->window && (!n->a || !n->b)) return false;
    return parents_consistent(n->a, n) && parents_consistent(n->b, n);
}

static bool tree_intact(const Tree *t, int expect_leaves) {
    if (!parents_consistent(t->root, NULL)) return false;
    if (tree_leaf_count(t) != expect_leaves) return false;

    int used = 0;
    for (int i = 0; i < TREE_MAX_NODES; i++)
        if (t->pool[i].used) used++;
    if (used != t->in_use) return false;

    int expect_nodes = expect_leaves == 0 ? 0 : expect_leaves * 2 - 1;
    return used == expect_nodes;
}

static void test_empty(void) {
    Tree t;
    tree_reset(&t);

    CHECK(t.root == NULL, "a reset tree has no root");
    CHECK(t.in_use == 0, "a reset tree has no nodes in use");
    CHECK(tree_leaf_count(&t) == 0, "a reset tree has no leaves");
    CHECK(tree_find(&t, win(0)) == NULL, "find on an empty tree misses");
    CHECK(!tree_remove(&t, win(0)), "remove on an empty tree reports nothing");
    CHECK(tree_first_leaf(t.root) == NULL, "no first leaf in an empty tree");

    Capture c = run(&t, rect(0, 0, 100, 100));
    CHECK(c.placed_n == 0, "placing an empty tree emits nothing");
    CHECK(c.hidden_n == 0, "placing an empty tree hides nothing");
}

static void test_null_window_is_rejected(void) {
    Tree t;
    tree_reset(&t);

    CHECK(!tree_insert(&t, NULL, NULL, TREE_SPLIT_V),
          "inserting a NULL window is refused");
    CHECK(t.root == NULL, "a refused insert leaves the tree empty");

    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);

    CHECK(tree_find(&t, NULL) == NULL,
          "find(NULL) does not match the internal node holding no window");
    CHECK(!tree_remove(&t, NULL),
          "remove(NULL) does not tear out an internal node");
    CHECK(tree_intact(&t, 2), "the tree is unchanged after the NULL probes");
}

static void test_single_window_fills_the_area(void) {
    Tree t;
    tree_reset(&t);

    CHECK(tree_insert(&t, NULL, win(0), TREE_SPLIT_V), "first insert succeeds");
    CHECK(t.root != NULL && t.root->window == win(0), "the root is the window");
    CHECK(t.root->parent == NULL, "the root has no parent");
    CHECK(tree_intact(&t, 1), "one window is one node");

    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(c.placed_n == 1, "one window places once");
    CHECK(rect_eq(c.placed[0].area, rect(0, 0, 800, 600)),
          "a lone window gets the whole area");
}

static void test_vertical_split_halves_the_width(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);

    CHECK(tree_intact(&t, 2), "two windows are three nodes");
    CHECK(t.root->window == NULL, "the root became an internal node");
    CHECK(t.root->a->window == win(0), "the incumbent moved into the a slot");
    CHECK(t.root->b->window == win(1), "the newcomer took the b slot");
    CHECK(t.root->active == 1, "the newcomer is the active child");

    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(c.placed_n == 2, "both windows place");
    CHECK(rect_eq(placed_for(&c, win(0))->area, rect(0, 0, 400, 600)),
          "a vertical split gives the first window the left half");
    CHECK(rect_eq(placed_for(&c, win(1))->area, rect(400, 0, 800, 600)),
          "a vertical split gives the second window the right half");
}

static void test_horizontal_split_halves_the_height(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_H);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_H);

    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(rect_eq(placed_for(&c, win(0))->area, rect(0, 0, 800, 300)),
          "a horizontal split gives the first window the top half");
    CHECK(rect_eq(placed_for(&c, win(1))->area, rect(0, 300, 800, 600)),
          "a horizontal split gives the second window the bottom half");
}

static void test_split_covers_the_area_exactly(void) {
    for (int w = 1; w <= 200; w++) {
        for (int r = 1; r <= 9; r++) {
            Tree t;
            tree_reset(&t);
            tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
            tree_insert(&t, NULL, win(1), TREE_SPLIT_V);
            t.root->ratio = (float)r / 10.f;

            Capture c = run(&t, rect(0, 0, w, 50));
            const Placed *a = placed_for(&c, win(0));
            const Placed *b = placed_for(&c, win(1));

            CHECK(a && b, "both halves place at width %d ratio %d", w, r);
            if (!a || !b) continue;
            CHECK(a->area.right == b->area.left,
                  "the halves meet at width %d ratio %d", w, r);
            CHECK(a->area.left == 0 && b->area.right == w,
                  "the halves span the area at width %d ratio %d", w, r);
            CHECK(a->area.right >= 0 && a->area.right <= w,
                  "the seam stays inside the area at width %d ratio %d", w, r);
        }
    }
}

static void test_insert_at_a_chosen_leaf(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);

    TreeNode *at = tree_find(&t, win(1));
    CHECK(at != NULL, "the target leaf is found");
    CHECK(tree_insert(&t, at, win(2), TREE_SPLIT_H),
          "inserting at a chosen leaf succeeds");
    CHECK(tree_intact(&t, 3), "three windows are five nodes");

    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(c.placed_n == 3, "all three windows place");
    CHECK(rect_eq(placed_for(&c, win(0))->area, rect(0, 0, 400, 600)),
          "the untouched window keeps its half");
    CHECK(rect_eq(placed_for(&c, win(1))->area, rect(400, 0, 800, 300)),
          "the split leaf keeps the top of its own half");
    CHECK(rect_eq(placed_for(&c, win(2))->area, rect(400, 300, 800, 600)),
          "the newcomer takes the bottom of that half");
}

static void test_insert_defaults_to_the_first_leaf(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(2), TREE_SPLIT_V);

    CHECK(tree_first_leaf(t.root)->window == win(0),
          "the first leaf is the deepest a-side window");
    CHECK(tree_depth_of(tree_find(&t, win(0))) == 2,
          "the repeatedly split window sinks two levels down");
    CHECK(tree_depth_of(tree_find(&t, win(1))) == 1,
          "the second window stays one level down");
    CHECK(tree_intact(&t, 3), "the tree is well formed");
}

static void test_remove_splices_the_sibling_up(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);
    tree_insert(&t, tree_find(&t, win(1)), win(2), TREE_SPLIT_H);

    CHECK(tree_remove(&t, win(1)), "removing an inner leaf reports success");
    CHECK(tree_intact(&t, 2), "the freed nodes went back to the pool");
    CHECK(tree_find(&t, win(1)) == NULL, "the removed window is gone");

    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(rect_eq(placed_for(&c, win(2))->area, rect(400, 0, 800, 600)),
          "the sibling inherits the whole sub-area");
    CHECK(rect_eq(placed_for(&c, win(0))->area, rect(0, 0, 400, 600)),
          "the far side is untouched");
}

static void test_remove_the_last_window_empties_the_tree(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);

    CHECK(tree_remove(&t, win(0)), "removing the root leaf succeeds");
    CHECK(t.root == NULL, "the tree is empty again");
    CHECK(t.in_use == 0, "the node went back to the pool");
    CHECK(tree_intact(&t, 0), "an emptied tree is well formed");

    CHECK(tree_insert(&t, NULL, win(3), TREE_SPLIT_V),
          "the emptied tree accepts a new window");
    CHECK(tree_intact(&t, 1), "the reused tree is well formed");
}

static void test_remove_an_absent_window_changes_nothing(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);

    int before = t.in_use;
    CHECK(!tree_remove(&t, win(9)), "removing an absent window reports nothing");
    CHECK(t.in_use == before, "no nodes were freed");
    CHECK(tree_intact(&t, 2), "the tree is unchanged");
}

static void test_removing_every_window_drains_the_pool(void) {
    const int n = 40;

    for (int order = 0; order < 3; order++) {
        Tree t;
        tree_reset(&t);
        for (int i = 0; i < n; i++)
            tree_insert(&t, NULL, win(i), (i & 1) ? TREE_SPLIT_H : TREE_SPLIT_V);
        CHECK(tree_intact(&t, n), "order %d: %d windows went in", order, n);

        for (int k = 0; k < n; k++) {
            int i = order == 0 ? k
                  : order == 1 ? n - 1 - k
                               : (k * 7 + 3) % n;
            if (order == 2) {
                for (int probe = 0; probe < n; probe++) {
                    i = (k * 7 + 3 + probe) % n;
                    if (tree_find(&t, win(i))) break;
                }
            }
            CHECK(tree_remove(&t, win(i)), "order %d: removed window %d",
                  order, i);
            CHECK(tree_intact(&t, n - k - 1),
                  "order %d: the tree is well formed after %d removals",
                  order, k + 1);
        }

        CHECK(t.root == NULL, "order %d: the tree ends empty", order);
        CHECK(t.in_use == 0, "order %d: the pool is drained", order);
    }
}

static void test_pool_exhaustion_is_refused_cleanly(void) {
    Tree t;
    tree_reset(&t);

    int inserted = 0;
    for (int i = 0; i < TREE_MAX_NODES + 4; i++) {
        if (!tree_insert(&t, NULL, win(i % (TREE_MAX_NODES + 8)),
                         TREE_SPLIT_V))
            break;
        inserted++;
    }

    CHECK(inserted > 0, "some windows fit");
    CHECK(inserted <= TREE_MAX_NODES, "no more windows than nodes fit");
    CHECK(t.in_use <= TREE_MAX_NODES, "the pool never overflows");
    CHECK(tree_intact(&t, inserted),
          "a refused insert leaves the tree well formed");

    int free_nodes = 0;
    for (int i = 0; i < TREE_MAX_NODES; i++)
        if (!t.pool[i].used) free_nodes++;
    CHECK(free_nodes + t.in_use == TREE_MAX_NODES,
          "no node was leaked by the refused insert");
}

static void test_tabbed_shows_one_and_hides_the_rest(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);
    t.root->mode = TREE_SPLIT_TABBED;

    t.root->active = 1;
    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(c.placed_n == 1, "a tabbed container places exactly one window");
    CHECK(c.placed[0].window == win(1), "the active child is the one shown");
    CHECK(rect_eq(c.placed[0].area, rect(0, 0, 800, 600)),
          "the shown tab gets the whole area");
    CHECK(was_hidden(&c, win(0)), "the inactive child is hidden");

    t.root->active = 0;
    c = run(&t, rect(0, 0, 800, 600));
    CHECK(c.placed[0].window == win(0), "flipping active swaps which tab shows");
    CHECK(was_hidden(&c, win(1)), "the other child is hidden instead");
}

static void test_stacked_behaves_like_tabbed(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);
    t.root->mode   = TREE_SPLIT_STACKED;
    t.root->active = 1;

    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(c.placed_n == 1, "a stacked container places exactly one window");
    CHECK(c.placed[0].window == win(1), "the active child is shown");
    CHECK(was_hidden(&c, win(0)), "the inactive child is hidden");
}

static void test_tabbed_hides_a_whole_subtree(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);
    tree_insert(&t, tree_find(&t, win(0)), win(2), TREE_SPLIT_H);

    t.root->mode   = TREE_SPLIT_TABBED;
    t.root->active = 1;

    Capture c = run(&t, rect(0, 0, 800, 600));
    CHECK(c.placed_n == 1, "only the active branch places");
    CHECK(c.placed[0].window == win(1), "the active branch is the b side");
    CHECK(was_hidden(&c, win(0)) && was_hidden(&c, win(2)),
          "every leaf under the inactive branch is hidden");
}

static void test_place_never_visits_a_window_twice(void) {
    Tree t;
    tree_reset(&t);
    for (int i = 0; i < 30; i++)
        tree_insert(&t, NULL, win(i), (i % 3 == 0) ? TREE_SPLIT_H
                                                   : TREE_SPLIT_V);

    Capture c = run(&t, rect(0, 0, 1920, 1080));
    CHECK(c.placed_n == 30, "every window places exactly once");

    for (int i = 0; i < c.placed_n; i++)
        for (int j = i + 1; j < c.placed_n; j++)
            CHECK(c.placed[i].window != c.placed[j].window,
                  "window %d is not placed twice", i);

    for (int i = 0; i < c.placed_n; i++) {
        TreeRect a = c.placed[i].area;
        CHECK(a.left >= 0 && a.top >= 0 && a.right <= 1920 && a.bottom <= 1080,
              "placement %d stays inside the work area", i);
        CHECK(a.right >= a.left && a.bottom >= a.top,
              "placement %d is not inverted", i);
    }
}

static void test_placements_do_not_overlap(void) {
    Tree t;
    tree_reset(&t);
    for (int i = 0; i < 12; i++)
        tree_insert(&t, NULL, win(i), (i % 2) ? TREE_SPLIT_H : TREE_SPLIT_V);

    Capture c = run(&t, rect(0, 0, 1024, 768));
    for (int i = 0; i < c.placed_n; i++) {
        for (int j = i + 1; j < c.placed_n; j++) {
            TreeRect a = c.placed[i].area, b = c.placed[j].area;
            bool disjoint = a.right <= b.left || b.right <= a.left ||
                            a.bottom <= b.top || b.bottom <= a.top;
            CHECK(disjoint, "placements %d and %d do not overlap", i, j);
        }
    }
}

static void test_rotate_flips_the_parent_split(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);

    CHECK(tree_rotate(&t, win(1)), "rotating a split window succeeds");
    CHECK(t.root->mode == TREE_SPLIT_H, "vertical rotates to horizontal");
    CHECK(tree_rotate(&t, win(1)), "rotating again succeeds");
    CHECK(t.root->mode == TREE_SPLIT_V, "horizontal rotates back to vertical");

    Tree solo;
    tree_reset(&solo);
    tree_insert(&solo, NULL, win(0), TREE_SPLIT_V);
    CHECK(!tree_rotate(&solo, win(0)), "a lone window has nothing to rotate");
    CHECK(!tree_rotate(&t, win(9)), "an absent window cannot be rotated");
}

static void test_resize_moves_the_seam_and_clamps(void) {
    Tree t;
    tree_reset(&t);
    tree_insert(&t, NULL, win(0), TREE_SPLIT_V);
    tree_insert(&t, NULL, win(1), TREE_SPLIT_V);

    CHECK(tree_resize(&t, win(0), 0.1f), "resizing from the a side succeeds");
    CHECK(t.root->ratio > 0.59f && t.root->ratio < 0.61f,
          "growing the a side raises the ratio, got %f",
          (double)t.root->ratio);

    CHECK(tree_resize(&t, win(1), 0.1f), "resizing from the b side succeeds");
    CHECK(t.root->ratio > 0.49f && t.root->ratio < 0.51f,
          "growing the b side lowers the ratio, got %f",
          (double)t.root->ratio);

    for (int i = 0; i < 50; i++) tree_resize(&t, win(0), 0.1f);
    CHECK(t.root->ratio <= 0.9f, "the ratio clamps at the top");
    for (int i = 0; i < 50; i++) tree_resize(&t, win(0), -0.1f);
    CHECK(t.root->ratio >= 0.1f, "the ratio clamps at the bottom");

    CHECK(!tree_resize(&t, win(9), 0.1f), "an absent window cannot be resized");
}

static void test_clamp_ratio_bounds(void) {
    CHECK(tree_clamp_ratio(0.5f) > 0.49f && tree_clamp_ratio(0.5f) < 0.51f,
          "a mid ratio passes through");
    CHECK(tree_clamp_ratio(-1.f) >= 0.1f, "a negative ratio clamps up");
    CHECK(tree_clamp_ratio(9.f) <= 0.9f, "a huge ratio clamps down");
}

static void test_churn_keeps_the_tree_consistent(void) {
    Tree t;
    tree_reset(&t);

    unsigned seed = 12345u;
    int      live[64] = {0};
    int      live_n = 0;

    for (int step = 0; step < 20000; step++) {
        seed = seed * 1103515245u + 12345u;
        unsigned roll = (seed >> 16) & 0x7fff;

        if (live_n == 0 || (roll % 100) < 60) {
            if (live_n >= 64) continue;
            int id = -1;
            for (int cand = 0; cand < 64 && id < 0; cand++) {
                bool taken = false;
                for (int i = 0; i < live_n; i++)
                    if (live[i] == cand) { taken = true; break; }
                if (!taken) id = cand;
            }
            if (id < 0) continue;

            TreeNode *at = NULL;
            if (live_n > 0) at = tree_find(&t, win(live[roll % (unsigned)live_n]));

            if (tree_insert(&t, at, win(id), (TreeSplit)(roll % 4)))
                live[live_n++] = id;
        } else {
            int k  = (int)(roll % (unsigned)live_n);
            int id = live[k];
            CHECK(tree_remove(&t, win(id)), "step %d removes window %d",
                  step, id);
            live[k] = live[--live_n];
        }

        CHECK(tree_intact(&t, live_n),
              "step %d leaves the tree well formed with %d live windows",
              step, live_n);

        Capture c = run(&t, rect(0, 0, 1600, 900));
        CHECK(c.placed_n + c.hidden_n == live_n,
              "step %d accounts for every live window", step);
    }
}

int main(void) {
    test_empty();
    test_null_window_is_rejected();
    test_single_window_fills_the_area();
    test_vertical_split_halves_the_width();
    test_horizontal_split_halves_the_height();
    test_split_covers_the_area_exactly();
    test_insert_at_a_chosen_leaf();
    test_insert_defaults_to_the_first_leaf();
    test_remove_splices_the_sibling_up();
    test_remove_the_last_window_empties_the_tree();
    test_remove_an_absent_window_changes_nothing();
    test_removing_every_window_drains_the_pool();
    test_pool_exhaustion_is_refused_cleanly();
    test_tabbed_shows_one_and_hides_the_rest();
    test_stacked_behaves_like_tabbed();
    test_tabbed_hides_a_whole_subtree();
    test_place_never_visits_a_window_twice();
    test_placements_do_not_overlap();
    test_rotate_flips_the_parent_split();
    test_resize_moves_the_seam_and_clamps();
    test_clamp_ratio_bounds();
    test_churn_keeps_the_tree_consistent();
    return tests_report("tree_algebra");
}

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define TREE_MAX_NODES 512

typedef enum {
    TREE_SPLIT_V = 0,
    TREE_SPLIT_H,
    TREE_SPLIT_TABBED,
    TREE_SPLIT_STACKED,
} TreeSplit;

typedef struct {
    int left, top, right, bottom;
} TreeRect;

typedef struct TreeNode {
    void            *window;
    struct TreeNode *a, *b;
    struct TreeNode *parent;
    TreeSplit        mode;
    float            ratio;
    int              active;
    bool             used;
} TreeNode;

typedef struct {
    TreeNode  pool[TREE_MAX_NODES];
    TreeNode *root;
    int       in_use;
} Tree;

typedef void (*TreePlaceFn)(void *window, TreeRect area, void *ctx);
typedef void (*TreeHideFn)(void *window, void *ctx);

void tree_reset(Tree *t);

TreeNode *tree_find(const Tree *t, const void *window);
TreeNode *tree_first_leaf(TreeNode *n);
int       tree_leaf_count(const Tree *t);
int       tree_depth_of(const TreeNode *n);

bool tree_insert(Tree *t, TreeNode *at, void *window, TreeSplit mode);
bool tree_remove(Tree *t, const void *window);

void tree_place(const Tree *t, TreeRect area,
                TreePlaceFn place, TreeHideFn hide, void *ctx);

bool  tree_rotate(Tree *t, const void *window);
bool  tree_resize(Tree *t, const void *window, float delta);
float tree_clamp_ratio(float v);

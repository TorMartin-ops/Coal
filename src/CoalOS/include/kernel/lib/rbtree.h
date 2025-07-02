/*
 * Adapted from code Copyright © 2017 Jason Ekstrand
 * MIT License
 */
 #ifndef RBTREE_H
 #define RBTREE_H
 
 #include <kernel/core/types.h> // Provides bool, uintptr_t, NULL, size_t etc.
 
 // Forward declaration
 struct vma_struct;
 
 /** A red-black tree node
  *
  * Embed this structure into the structure you want to store in the tree.
  */
 struct rb_node {
     /** Parent and color of this node
      * LSB=1 for black, LSB=0 for red. Upper bits = parent pointer.
      */
     uintptr_t parent_color; // Renamed from 'parent' for clarity
 
     struct rb_node *left;
     struct rb_node *right;
 };
 
 /** Return the parent node of the given node or NULL if it is the root */
 static inline struct rb_node *
 rb_node_parent(struct rb_node *n) {
     // Mask off the color bit to get the parent pointer
     return (struct rb_node *)(n->parent_color & ~1UL);
 }
 
 /** A red-black tree structure */
 struct rb_tree {
     struct rb_node *root;
 };
 
 // --- Core RB Tree API (Implementation in rbtree.c) ---
 
 /** Initialize a red-black tree */
 void rb_tree_init(struct rb_tree *T);
 
 /** Returns true if the red-black tree is empty */
 static inline bool
 rb_tree_is_empty(const struct rb_tree *T) {
     return T->root == NULL;
 }
 
 /** Insert a node into a tree at a particular location
  *
  * \param T           The red-black tree
  * \param parent      Parent node where new node should be inserted (can be NULL if tree empty)
  * \param node_ptr    Pointer to the rb_node field within the structure to insert
  * \param insert_left If true, insert as left child, else as right child
  */
 void rb_tree_insert_at(struct rb_tree *T, struct rb_node *parent,
                        struct rb_node *node_ptr, bool insert_left);
 
 /** Remove a node from a tree
  *
  * \param T         The red-black tree
  * \param node_ptr  Pointer to the rb_node field within the structure to remove
  */
 void rb_tree_remove(struct rb_tree *T, struct rb_node *node_ptr);
 
 /** Get the first (left-most) node in the tree or NULL */
 struct rb_node *rb_tree_first(struct rb_tree *T);
 
 /** Get the last (right-most) node in the tree or NULL */
 struct rb_node *rb_tree_last(struct rb_tree *T);
 
 /** Get the next node (in-order successor) in the tree or NULL */
 struct rb_node *rb_node_next(struct rb_node *node_ptr);
 
 /** Get the previous node (in-order predecessor) in the tree or NULL */
 struct rb_node *rb_node_prev(struct rb_node *node_ptr);
 
 // --- VMA Specific Helpers (Implementation in rbtree.c) ---
 
 /**
  * @brief Finds the VMA node in the tree that contains the given address.
  * Searches based on the interval [vm_start, vm_end).
  *
  * @param root The root of the VMA RB Tree.
  * @param addr The virtual address to search for.
  * @return Pointer to the vma_struct containing addr, or NULL if not found.
  */
 struct vma_struct* rbtree_find_vma(struct rb_node *root, uintptr_t addr);
 
 /**
  * @brief Finds a VMA node whose interval overlaps with [start, end).
  *
  * @param root The root of the VMA RB Tree.
  * @param start The start address of the interval to check.
  * @param end The end address of the interval to check.
  * @return Pointer to an overlapping vma_struct, or NULL if no overlap.
  */
 struct vma_struct* rbtree_find_overlap(struct rb_node *root, uintptr_t start, uintptr_t end);
 
 // --- Traversal ---
 
 typedef void (*rbtree_visit_func)(struct vma_struct *vma_node, void *data);
 
 /**
  * @brief Performs a post-order traversal of the VMA RB Tree.
  *
  * @param node The current node (start with root).
  * @param visit Function to call for each visited VMA node.
  * @param data Arbitrary data pointer passed to the visit function.
  */
 void rbtree_postorder_traverse(struct rb_node *node, rbtree_visit_func visit, void *data);
 
 
 // --- Utility Macro ---
 
 /** Retrieve the data structure containing a node */
 #ifndef offsetof
 #define offsetof(type, member) ((size_t)&(((type *)0)->member))
 #endif
 
 #define rb_entry(ptr, type, member) \
     ((type *)(((char *)(ptr)) - offsetof(type, member)))
 
 
 #endif // RBTREE_H
/*
 * Implementation of a Red-Black Tree for UiAOS kernel.
 * Adapted from code Copyright © 2017 Jason Ekstrand (MIT License)
 * Ported for kernel environment.
 */

 #include <kernel/lib/rbtree.h>
 #include <kernel/memory/mm.h> // Need vma_struct definition
 #include <kernel/core/types.h>
 #include <kernel/drivers/display/terminal.h> // For kernel-specific error reporting/panic
 #include <kernel/lib/string.h> // For memset (should be kernel's string.h)
 
 // Define kernel panic or assert mechanism if available
 #define KERNEL_ASSERT(condition, message) \
     do { \
         if (!(condition)) { \
             terminal_printf("Assertion Failed: %s (%s:%d)\n", message, __FILE__, __LINE__); \
             asm volatile ("cli; hlt"); /* Halt on assertion failure */ \
         } \
     } while (0)
 
 // --- Static Inline Helpers from Header (Definitions if needed) ---
 // (These are often directly in the header for inlining)
 
 // --- Color Manipulation ---
 static inline bool
 rb_node_is_black(struct rb_node *n) {
     // NULL nodes (leaves) are considered black
     return (n == NULL) || (n->parent_color & 1);
 }
 
 static inline bool
 rb_node_is_red(struct rb_node *n) {
     // Node must exist and color bit must be 0
     return (n != NULL) && !(n->parent_color & 1);
 }
 
 static inline void
 rb_node_set_black(struct rb_node *n) {
     if (n) n->parent_color |= 1UL;
 }
 
 static inline void
 rb_node_set_red(struct rb_node *n) {
     if (n) n->parent_color &= ~1UL;
 }
 
 static inline void
 rb_node_copy_color(struct rb_node *dst, struct rb_node *src) {
     if (dst && src) dst->parent_color = (dst->parent_color & ~1UL) | (src->parent_color & 1);
 }
 
 static inline void
 rb_node_set_parent(struct rb_node *n, struct rb_node *p) {
     if (n) n->parent_color = (n->parent_color & 1) | (uintptr_t)p;
 }
 
 // --- Tree Navigation ---
 static struct rb_node *
 rb_node_minimum(struct rb_node *node) {
     KERNEL_ASSERT(node != NULL, "rb_node_minimum on NULL");
     while (node->left)
         node = node->left;
     return node;
 }
 
 static struct rb_node *
 rb_node_maximum(struct rb_node *node) {
     KERNEL_ASSERT(node != NULL, "rb_node_maximum on NULL");
     while (node->right)
         node = node->right;
     return node;
 }
 
 // --- Core Tree Operations ---
 
 void rb_tree_init(struct rb_tree *T) {
     T->root = NULL;
 }
 
 // Transplant subtree rooted at v into the place of u
 static void
 rb_tree_splice(struct rb_tree *T, struct rb_node *u, struct rb_node *v) {
     KERNEL_ASSERT(u != NULL, "rb_tree_splice u is NULL");
     struct rb_node *p = rb_node_parent(u);
     if (p == NULL) {
         KERNEL_ASSERT(T->root == u, "rb_tree_splice root mismatch");
         T->root = v;
     } else if (u == p->left) {
         p->left = v;
     } else {
         KERNEL_ASSERT(u == p->right, "rb_tree_splice parent linkage error");
         p->right = v;
     }
     if (v)
         rb_node_set_parent(v, p);
 }
 
 // Left rotate around x
 static void
 rb_tree_rotate_left(struct rb_tree *T, struct rb_node *x) {
     KERNEL_ASSERT(x && x->right, "rb_tree_rotate_left invalid node");
     struct rb_node *y = x->right; // Set y
     x->right = y->left;           // Turn y's left subtree into x's right subtree
     if (y->left)
         rb_node_set_parent(y->left, x);
     rb_node_set_parent(y, rb_node_parent(x)); // Link x's parent to y
     rb_tree_splice(T, x, y); // Replace x with y (updates parent's child or root)
     y->left = x;                 // Put x on y's left
     rb_node_set_parent(x, y);
 }
 
 // Right rotate around y
 static void
 rb_tree_rotate_right(struct rb_tree *T, struct rb_node *y) {
     KERNEL_ASSERT(y && y->left, "rb_tree_rotate_right invalid node");
     struct rb_node *x = y->left; // Set x
     y->left = x->right;          // Turn x's right subtree into y's left subtree
     if (x->right)
         rb_node_set_parent(x->right, y);
     rb_node_set_parent(x, rb_node_parent(y)); // Link y's parent to x
     rb_tree_splice(T, y, x); // Replace y with x (updates parent's child or root)
     x->right = y;                // Put y on x's right
     rb_node_set_parent(y, x);
 }
 
 // Insert node at specific position and fix up RB properties
 void rb_tree_insert_at(struct rb_tree *T, struct rb_node *parent,
                        struct rb_node *node_ptr, bool insert_left)
 {
     // Initialize the new node (red, null children, correct parent)
     node_ptr->parent_color = (uintptr_t)parent; // Red (LSB=0) + parent pointer
     node_ptr->left = NULL;
     node_ptr->right = NULL;
 
     if (parent == NULL) {
         KERNEL_ASSERT(T->root == NULL, "Inserting into non-empty tree with NULL parent");
         T->root = node_ptr;
         rb_node_set_black(node_ptr); // Root must be black
         return;
     }
 
     // Link node into the tree
     if (insert_left) {
         KERNEL_ASSERT(parent->left == NULL, "Insert left target not NULL");
         parent->left = node_ptr;
     } else {
         KERNEL_ASSERT(parent->right == NULL, "Insert right target not NULL");
         parent->right = node_ptr;
     }
 
     // --- Insertion Fixup ---
     struct rb_node *z = node_ptr; // z is the newly inserted node (red)
     while (rb_node_is_red(rb_node_parent(z))) { // While parent is red (violates property)
         struct rb_node *z_p = rb_node_parent(z);
         struct rb_node *z_p_p = rb_node_parent(z_p); // Grandparent must exist and be black
 
         KERNEL_ASSERT(z_p_p != NULL, "Red parent has NULL grandparent");
         KERNEL_ASSERT(rb_node_is_black(z_p_p), "Red parent has red grandparent");
 
         if (z_p == z_p_p->left) { // Parent is left child
             struct rb_node *y = z_p_p->right; // Uncle y
             if (rb_node_is_red(y)) { // Case 1: Uncle is red
                 rb_node_set_black(z_p);
                 rb_node_set_black(y);
                 rb_node_set_red(z_p_p);
                 z = z_p_p; // Move z up to grandparent and continue loop
             } else { // Uncle y is black
                 if (z == z_p->right) { // Case 2: z is right child (triangle)
                     z = z_p;
                     rb_tree_rotate_left(T, z);
                     z_p = rb_node_parent(z); // z_p has changed after rotation
                     z_p_p = rb_node_parent(z_p);
                 }
                 // Case 3: z is left child (line)
                 KERNEL_ASSERT(z_p_p != NULL, "Grandparent became NULL case 3");
                 rb_node_set_black(z_p);
                 rb_node_set_red(z_p_p);
                 rb_tree_rotate_right(T, z_p_p);
                 // Loop terminates here because z_p is now black
             }
         } else { // Parent is right child (symmetric to above)
             struct rb_node *y = z_p_p->left; // Uncle y
             if (rb_node_is_red(y)) { // Case 1: Uncle is red
                 rb_node_set_black(z_p);
                 rb_node_set_black(y);
                 rb_node_set_red(z_p_p);
                 z = z_p_p;
             } else { // Uncle y is black
                 if (z == z_p->left) { // Case 2: z is left child (triangle)
                     z = z_p;
                     rb_tree_rotate_right(T, z);
                     z_p = rb_node_parent(z);
                     z_p_p = rb_node_parent(z_p);
                 }
                 // Case 3: z is right child (line)
                 KERNEL_ASSERT(z_p_p != NULL, "Grandparent became NULL case 3 symmetric");
                 rb_node_set_black(z_p);
                 rb_node_set_red(z_p_p);
                 rb_tree_rotate_left(T, z_p_p);
                 // Loop terminates
             }
         }
     } // End while loop
 
     rb_node_set_black(T->root); // Ensure root remains black
 }
 
 // Remove a node from the tree and fix up RB properties
 void rb_tree_remove(struct rb_tree *T, struct rb_node *z) {
     struct rb_node *x; // Node that replaces y (might be NULL)
     struct rb_node *x_p; // Parent of x (might be NULL if x is root, or y if y replaces z directly)
     struct rb_node *y = z; // y is the node either removed or moved
     bool y_was_black = rb_node_is_black(y);
 
     // Find node 'y' to splice out (either z or its successor) and node 'x' to replace it
     if (z->left == NULL) {
         x = z->right;
         x_p = rb_node_parent(z); // Parent of the node being actually removed
         rb_tree_splice(T, z, x); // Replace z with its right child x
     } else if (z->right == NULL) {
         x = z->left;
         x_p = rb_node_parent(z); // Parent of the node being actually removed
         rb_tree_splice(T, z, x); // Replace z with its left child x
     } else {
         // z has two children, find successor 'y' (minimum in right subtree)
         y = rb_node_minimum(z->right);
         y_was_black = rb_node_is_black(y);
         x = y->right; // Successor 'y' has no left child, x is its right child (or NULL)
 
         if (rb_node_parent(y) == z) {
             // y is z's immediate right child
             x_p = y; // x's effective parent (for fixup) is y itself
         } else {
             // y is deeper in the right subtree
             x_p = rb_node_parent(y); // x's original parent
             rb_tree_splice(T, y, x); // Replace y with its right child x
             y->right = z->right;     // Move z's right subtree under y
             rb_node_set_parent(y->right, y);
         }
         // Replace z with y
         rb_tree_splice(T, z, y);
         y->left = z->left; // Move z's left subtree under y
         rb_node_set_parent(y->left, y);
         rb_node_copy_color(y, z); // y takes z's original color
         // Now 'y' is in z's original position, 'x' replaced y, 'x_p' is parent of x's original location
     }
 
     // If the removed/moved node 'y' was black, the black-height might be violated
     if (!y_was_black) {
         return; // If y was red, removing it doesn't affect black-height
     }
 
     // --- Deletion Fixup ---
     // x is the node that potentially violates RB properties (double black or red-black)
     // x_p is its parent
     while (x != T->root && rb_node_is_black(x)) {
         KERNEL_ASSERT(x_p != NULL, "x is not root but parent is NULL");
         if (x == x_p->left) { // x is left child
             struct rb_node *w = x_p->right; // w is sibling of x
             KERNEL_ASSERT(w != NULL, "Black node x has NULL sibling w"); // Sibling must exist if x is black and not root
 
             if (rb_node_is_red(w)) { // Case 1: Sibling w is red
                 rb_node_set_black(w);
                 rb_node_set_red(x_p);
                 rb_tree_rotate_left(T, x_p);
                 w = x_p->right; // New sibling w (must be black)
             }
             // Now w is black
             KERNEL_ASSERT(rb_node_is_black(w), "Sibling should be black now");
             if (rb_node_is_black(w->left) && rb_node_is_black(w->right)) { // Case 2: Sibling w has two black children
                 rb_node_set_red(w); // Pull blackness up
                 x = x_p;           // Move violation up to parent
                 x_p = rb_node_parent(x); // Update x_p
             } else {
                 if (rb_node_is_black(w->right)) { // Case 3: Sibling w has red left, black right child
                     rb_node_set_black(w->left);
                     rb_node_set_red(w);
                     rb_tree_rotate_right(T, w);
                     w = x_p->right; // New sibling w
                 }
                 // Case 4: Sibling w has black left, red right child
                 KERNEL_ASSERT(rb_node_is_red(w->right), "Case 4 condition fail");
                 rb_node_copy_color(w, x_p); // w takes parent's color
                 rb_node_set_black(x_p);      // Parent becomes black
                 rb_node_set_black(w->right); // w's red child becomes black
                 rb_tree_rotate_left(T, x_p);
                 x = T->root; // Violation fixed, terminate loop
             }
         } else { // x is right child (symmetric)
             struct rb_node *w = x_p->left; // w is sibling of x
             KERNEL_ASSERT(w != NULL, "Black node x has NULL sibling w");
 
             if (rb_node_is_red(w)) { // Case 1: Sibling w is red
                 rb_node_set_black(w);
                 rb_node_set_red(x_p);
                 rb_tree_rotate_right(T, x_p);
                 w = x_p->left; // New sibling w (must be black)
             }
             // Now w is black
              KERNEL_ASSERT(rb_node_is_black(w), "Sibling should be black now symmetric");
             if (rb_node_is_black(w->right) && rb_node_is_black(w->left)) { // Case 2: Sibling w has two black children
                 rb_node_set_red(w);
                 x = x_p;
                 x_p = rb_node_parent(x);
             } else {
                 if (rb_node_is_black(w->left)) { // Case 3: Sibling w has black left, red right child
                     rb_node_set_black(w->right);
                     rb_node_set_red(w);
                     rb_tree_rotate_left(T, w);
                     w = x_p->left; // New sibling w
                 }
                 // Case 4: Sibling w has red left, black right child
                 KERNEL_ASSERT(rb_node_is_red(w->left), "Case 4 condition fail symmetric");
                 rb_node_copy_color(w, x_p);
                 rb_node_set_black(x_p);
                 rb_node_set_black(w->left);
                 rb_tree_rotate_right(T, x_p);
                 x = T->root; // Violation fixed
             }
         }
     } // End while loop
 
     if (x)
         rb_node_set_black(x); // Ensure root or final node is black
 }
 
 
 // --- Tree Traversal ---
 
 struct rb_node * rb_tree_first(struct rb_tree *T) {
     return T->root ? rb_node_minimum(T->root) : NULL;
 }
 
 struct rb_node * rb_tree_last(struct rb_tree *T) {
     return T->root ? rb_node_maximum(T->root) : NULL;
 }
 
 struct rb_node * rb_node_next(struct rb_node *node_ptr) {
      if (!node_ptr) return NULL;
     if (node_ptr->right) {
         // Successor is the minimum node in the right subtree
         return rb_node_minimum(node_ptr->right);
     } else {
         // Successor is the lowest ancestor whose left child is also an ancestor
         struct rb_node *p = rb_node_parent(node_ptr);
         while (p && node_ptr == p->right) {
             node_ptr = p;
             p = rb_node_parent(node_ptr);
         }
         return p;
     }
 }
 
 struct rb_node * rb_node_prev(struct rb_node *node_ptr) {
      if (!node_ptr) return NULL;
     if (node_ptr->left) {
         // Predecessor is the maximum node in the left subtree
         return rb_node_maximum(node_ptr->left);
     } else {
         // Predecessor is the lowest ancestor whose right child is also an ancestor
         struct rb_node *p = rb_node_parent(node_ptr);
         while (p && node_ptr == p->left) {
             node_ptr = p;
             p = rb_node_parent(node_ptr);
         }
         return p;
     }
 }
 
 
 // --- VMA Specific Functions (using rb_entry macro) ---
 
 /**
  * Finds the VMA containing addr using sloppy search and verification.
  */
 struct vma_struct* rbtree_find_vma(struct rb_node *root, uintptr_t addr) {
     struct rb_node *node = root;
     struct vma_struct *vma = NULL;
 
     while (node) {
         vma_struct_t *current_vma = rb_entry(node, vma_struct_t, rb_node); // Get VMA struct from node
 
         if (addr < current_vma->vm_start) {
             // Address is before this VMA, go left
             node = node->left;
         } else if (addr >= current_vma->vm_end) {
             // Address is after this VMA, go right
             node = node->right;
         } else {
             // Address is within this VMA's range
             vma = current_vma;
             break;
         }
     }
     return vma; // Return found VMA or NULL
 }
 
 
 /**
  * Finds a VMA overlapping the interval [start, end).
  */
 struct vma_struct* rbtree_find_overlap(struct rb_node *root, uintptr_t start, uintptr_t end) {
     struct rb_node *node = root;
     struct vma_struct *candidate = NULL; // Most recent node visited
 
     // Find the node <= start address (potential overlap start)
     while (node) {
         candidate = rb_entry(node, vma_struct_t, rb_node);
         if (start < candidate->vm_start) {
              // Interval starts before this node, potential overlap could be here or left
              node = node->left;
         } else if (start >= candidate->vm_end) {
              // Interval starts after this node, must go right
              node = node->right;
         } else {
             // start is within candidate: definite overlap
             return candidate;
         }
     }
 
     // If loop finished, 'candidate' is the node with the largest start address <= 'start'
     // (or NULL if tree is empty or start is before the first node).
     // Check if this candidate actually overlaps [start, end).
     if (candidate && candidate->vm_end > start) {
         return candidate;
     }
 
     // If candidate didn't overlap, check its successor if it exists.
     // The successor is the node with the smallest start address > candidate->vm_start.
     // If this successor's start address is less than 'end', it overlaps.
     if (candidate) {
          struct rb_node* next_node_ptr = rb_node_next(&candidate->rb_node);
          if (next_node_ptr) {
               struct vma_struct* next_vma = rb_entry(next_node_ptr, vma_struct_t, rb_node);
               if (next_vma->vm_start < end) {
                    return next_vma; // Successor overlaps
               }
          }
     } else {
          // If no candidate found initially (start < first node), check the very first node
          struct rb_node* first_node_ptr = root ? rb_node_minimum(root) : NULL;
          if (first_node_ptr) {
               struct vma_struct* first_vma = rb_entry(first_node_ptr, vma_struct_t, rb_node);
               if (first_vma->vm_start < end) {
                    return first_vma; // First node overlaps
               }
          }
     }
 
 
     return NULL; // No overlap found
 }
 
 
 // --- Traversal Implementation ---
 
 /**
  * Recursive helper for post-order traversal.
  */
 static void rbtree_postorder_recursive(struct rb_node *node, rbtree_visit_func visit, void *data) {
     if (node == NULL) {
         return;
     }
     rbtree_postorder_recursive(node->left, visit, data);
     rbtree_postorder_recursive(node->right, visit, data);
     visit(rb_entry(node, vma_struct_t, rb_node), data); // Visit VMA after children
 }
 
 /**
  * Public function to start post-order traversal.
  */
 void rbtree_postorder_traverse(struct rb_node *root, rbtree_visit_func visit, void *data) {
     rbtree_postorder_recursive(root, visit, data);
 }
 
 
 // --- Initialization for VMA Nodes ---
 // (This could arguably be in mm.c, but keep it with RB logic for now)
 
 /**
  * Initializes the RB node part within a VMA structure.
  */
 void rbtree_node_init(struct vma_struct *vma_node) {
      // Parent/Color, Left, Right are typically zeroed/nulled by rbtree_insert_at
      // or should be explicitly set there. This function might not be strictly needed
      // if rbtree_insert_at handles initialization.
      // If needed:
      // vma_node->rb_node.parent_color = 0; // NULL parent, Red color initially
      // vma_node->rb_node.left = NULL;
      // vma_node->rb_node.right = NULL;
 }
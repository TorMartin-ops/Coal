#include "types.h"
#include "mount_table.h"
#include "kmalloc.h"
#include "terminal.h"

static mount_t *global_mount_list = NULL;

int mount_table_add(mount_t *mnt) {
    if (!mnt) {
        terminal_write("[MountTable] Error: NULL mount entry.\n");
        return -1;
    }
    mnt->next = global_mount_list;
    global_mount_list = mnt;
    return 0;
}

int mount_table_remove(const char *mount_point) {
    if (!mount_point) {
        terminal_write("[MountTable] Error: NULL mount point.\n");
        return -1;
    }
    mount_t **prev = &global_mount_list;
    mount_t *curr = global_mount_list;
    while (curr) {
        if (strcmp(curr->mount_point, mount_point) == 0) {
            *prev = curr->next;
            kfree(curr, sizeof(mount_t));
            return 0;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    return -1;
}

mount_t *mount_table_find(const char *mount_point) {
    if (!mount_point)
        return NULL;
    mount_t *iter = global_mount_list;
    while (iter) {
        if (strcmp(iter->mount_point, mount_point) == 0)
            return iter;
        iter = iter->next;
    }
    return NULL;
}

void mount_table_list(void) {
    terminal_write("[MountTable] Mount entries:\n");
    mount_t *iter = global_mount_list;
    if (!iter) {
        terminal_write("  (none)\n");
        return;
    }
    while (iter) {
        terminal_write("  Mount point: ");
        terminal_write(iter->mount_point);
        terminal_write(" | FS: ");
        terminal_write(iter->fs_name);
        terminal_write("\n");
        iter = iter->next;
    }
}

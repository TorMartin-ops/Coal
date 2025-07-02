#include <kernel/fs/vfs/fs_util.h>
#include <kernel/lib/string.h>  // Your custom string functions (e.g., strlen, strcpy, strcmp, strrchr, strtok)
#include <kernel/core/types.h> 
/*----------------------------------------------------------------------------
 * fs_util_is_absolute
 *----------------------------------------------------------------------------
 * Check whether the input path is absolute. In Unix-like systems, an absolute
 * path starts with a forward slash ('/').
 *----------------------------------------------------------------------------
 */
bool fs_util_is_absolute(const char *path) {
    return (path && path[0] == '/');
}

/*----------------------------------------------------------------------------
 * fs_util_normalize_path
 *----------------------------------------------------------------------------
 * This function normalizes a file path. It removes redundant slashes and
 * resolves the special components "." (current directory) and ".." (parent).
 * The algorithm uses a simple stack-like approach.
 *----------------------------------------------------------------------------
 */
int fs_util_normalize_path(const char *path, char *normalized, size_t max_len) {
    if (!path || !normalized || max_len == 0) {
        return -1;
    }

    // Temporary buffer for tokenization; assumes FS_MAX_PATH_LENGTH is sufficient.
    char temp[4096];
    size_t path_len = strlen(path);
    if (path_len >= sizeof(temp)) {
        return -1;
    }
    strcpy(temp, path);

    // Array of pointers for components (maximum possible components)
    char *components[4096 / 2];
    size_t comp_count = 0;

    // Tokenize using '/' as delimiter.
    // Note: If you have your own strtok implementation in your libc, use that.
    char *token = strtok(temp, "/");
    while (token != NULL) {
        if (strcmp(token, ".") == 0) {
            // Skip current directory markers.
        } else if (strcmp(token, "..") == 0) {
            if (comp_count > 0) {
                comp_count--; // Pop last component
            }
        } else {
            components[comp_count++] = token;
        }
        token = strtok(NULL, "/");
    }

    // Reconstruct the normalized path.
    size_t pos = 0;
    if (fs_util_is_absolute(path)) {
        if (pos < max_len - 1) {
            normalized[pos++] = '/';
        } else {
            return -1;
        }
    }

    for (size_t i = 0; i < comp_count; i++) {
        size_t token_len = strlen(components[i]);
        if (pos + token_len >= max_len - 1) {
            return -1;
        }
        strcpy(&normalized[pos], components[i]);
        pos += token_len;
        if (i < comp_count - 1) {
            if (pos < max_len - 1) {
                normalized[pos++] = '/';
            } else {
                return -1;
            }
        }
    }

    // Ensure the result is null-terminated.
    normalized[pos] = '\0';

    // Special case: empty normalized path means root.
    if (pos == 0) {
        if (max_len < 2) return -1;
        normalized[0] = '/';
        normalized[1] = '\0';
    }

    return 0;
}

/*----------------------------------------------------------------------------
 * fs_util_get_extension
 *----------------------------------------------------------------------------
 * Returns a pointer to the file extension within a filename. If no extension
 * exists, returns NULL.
 *----------------------------------------------------------------------------
 */
const char *fs_util_get_extension(const char *filename) {
    if (!filename) {
        return NULL;
    }
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return NULL;
    }
    return dot + 1;
}

/*----------------------------------------------------------------------------
 * fs_util_split_path
 *----------------------------------------------------------------------------
 * Splits a full path into directory and base components.
 *----------------------------------------------------------------------------
 */
int fs_util_split_path(const char *path, char *dirname, size_t dmax, char *basename, size_t bmax) {
    if (!path || !dirname || !basename) {
        return -1;
    }

    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        // No slash found: directory is ".", basename is path.
        if (dmax < 2 || bmax < strlen(path) + 1) {
            return -1;
        }
        strcpy(dirname, ".");
        strcpy(basename, path);
    } else {
        size_t dir_len = last_slash - path;
        if (dir_len == 0) {
            // Path like "/filename": directory is root.
            if (dmax < 2) return -1;
            strcpy(dirname, "/");
        } else {
            if (dir_len >= dmax) return -1;
            memcpy(dirname, path, dir_len);
            dirname[dir_len] = '\0';
        }
        const char *base_part = last_slash + 1;
        if (strlen(base_part) >= bmax) return -1;
        strcpy(basename, base_part);
    }
    return 0;
}

/*----------------------------------------------------------------------------
 * fs_util_join_paths
 *----------------------------------------------------------------------------
 * Joins a directory and a file name into a single path. Avoids duplicate slashes.
 *----------------------------------------------------------------------------
 */
int fs_util_join_paths(const char *dir, const char *file, char *result, size_t max_len) {
    if (!dir || !file || !result) {
        return -1;
    }
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(file);
    
    // Check if we need an extra '/'.
    bool need_slash = (dir_len > 0 && dir[dir_len - 1] != '/');
    size_t total_len = dir_len + (need_slash ? 1 : 0) + file_len + 1;
    if (total_len > max_len) {
        return -1;
    }
    
    strcpy(result, dir);
    if (need_slash) {
        result[dir_len] = '/';
        result[dir_len + 1] = '\0';
    }
    strcat(result, file);
    return 0;
}

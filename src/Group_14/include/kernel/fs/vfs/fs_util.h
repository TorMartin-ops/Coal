#pragma once
#ifndef FS_UTIL_H
#define FS_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <kernel/core/types.h> 

/**
 * fs_util_is_absolute
 *
 * Returns true if the given path is absolute (starts with '/').
 *
 * @param path  Input file path.
 * @return      true if absolute, false otherwise.
 */
bool fs_util_is_absolute(const char *path);

/**
 * fs_util_normalize_path
 *
 * Normalizes the input file path by removing redundant slashes and resolving
 * "." and ".." elements. The normalized path is written to 'normalized'.
 *
 * @param path         The input file path.
 * @param normalized   Buffer to store the normalized path.
 * @param max_len      Maximum length of the output buffer.
 * @return 0 on success, -1 on error (e.g. output exceeds max_len).
 */
int fs_util_normalize_path(const char *path, char *normalized, size_t max_len);

/**
 * fs_util_get_extension
 *
 * Returns a pointer to the extension within filename (if present),
 * or NULL if there is no extension.
 *
 * @param filename     The file name (or full path).
 * @return             Pointer to the extension (without the dot), or NULL.
 */
const char *fs_util_get_extension(const char *filename);

/**
 * fs_util_split_path
 *
 * Splits a full file path into its directory and basename components.
 *
 * For example, given "/usr/local/bin/ls", dirname is "/usr/local/bin" and
 * basename is "ls".
 *
 * @param path       The full file path.
 * @param dirname    Output buffer for directory portion.
 * @param dmax       Maximum length for dirname.
 * @param basename   Output buffer for filename portion.
 * @param bmax       Maximum length for basename.
 * @return 0 on success, -1 on error.
 */
int fs_util_split_path(const char *path, char *dirname, size_t dmax, char *basename, size_t bmax);

/**
 * fs_util_join_paths
 *
 * Joins a directory path and a file name into a single path. Ensures that
 * a single '/' separator exists between the parts.
 *
 * @param dir      The directory path.
 * @param file     The file name.
 * @param result   Output buffer for the combined path.
 * @param max_len  Maximum length of the result buffer.
 * @return 0 on success, -1 on error.
 */
int fs_util_join_paths(const char *dir, const char *file, char *result, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* FS_UTIL_H */

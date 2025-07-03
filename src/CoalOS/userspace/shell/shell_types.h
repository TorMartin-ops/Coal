/**
 * @file shell_types.h
 * @brief Common type definitions and constants for Coal OS Shell
 * @version 2.0
 * @author Modular refactoring
 */

#ifndef SHELL_TYPES_H
#define SHELL_TYPES_H

//============================================================================
// Type Definitions
//============================================================================

#ifndef _SHELL_TYPES_DEFINED
#define _SHELL_TYPES_DEFINED

typedef signed   char      int8_t;
typedef unsigned char      uint8_t;
typedef signed   short     int16_t;
typedef unsigned short     uint16_t;
typedef signed   int       int32_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t           uintptr_t;
typedef uint32_t           size_t;
typedef int32_t            ssize_t;
typedef int32_t            pid_t;

typedef char bool;
#define true  1
#define false 0

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif

#endif // _SHELL_TYPES_DEFINED

//============================================================================
// Configuration Constants
//============================================================================

#define MAX_COMMAND_LENGTH 1024
#define MAX_ARGS 64
#define MAX_JOBS 32
#define MAX_HISTORY 100
#define MAX_ENV_VARS 64
#define MAX_PATH_LENGTH 256
#define MAX_ALIASES 32
#define MAX_COMPLETIONS 64

//============================================================================
// File Descriptors
//============================================================================

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

//============================================================================
// File Open Flags
//============================================================================

#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_CREAT      0x0040
#define O_TRUNC      0x0200
#define O_APPEND     0x0400

//============================================================================
// Signal Numbers
//============================================================================

#define SIGINT   2
#define SIGQUIT  3
#define SIGTERM  15
#define SIGKILL  9
#define SIGCHLD  17

//============================================================================
// Data Structures
//============================================================================

// Job states
typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} job_state_t;

// Job structure for job control
typedef struct job {
    int job_id;
    pid_t pgid;
    pid_t *pids;
    int num_processes;
    char *command;
    job_state_t state;
    bool background;
} job_t;

// Command structure
typedef struct command {
    char *args[MAX_ARGS];
    int argc;
    char *input_file;
    char *output_file;
    bool append_output;
    bool background;
} command_t;

// Pipeline structure
typedef struct pipeline {
    command_t commands[MAX_ARGS];
    int num_commands;
    bool background;
} pipeline_t;

// Environment variable
typedef struct env_var {
    char *name;
    char *value;
} env_var_t;

// Command alias
typedef struct alias {
    char *name;
    char *command;
} alias_t;

// Tab completion entry
typedef struct completion {
    char *text;
    int is_command;
} completion_t;

// Shell state
typedef struct {
    job_t jobs[MAX_JOBS];
    int num_jobs;
    char *history[MAX_HISTORY];
    int history_count;
    int history_index;
    int current_history_pos;
    env_var_t env_vars[MAX_ENV_VARS];
    int num_env_vars;
    alias_t aliases[MAX_ALIASES];
    int num_aliases;
    char cwd[MAX_PATH_LENGTH];
    bool running;
    int last_exit_status;
    bool tab_completion_enabled;
    bool history_navigation_enabled;
    bool auto_cd_enabled;
    char last_directory[MAX_PATH_LENGTH];
} shell_state_t;

// Global shell state (extern declaration)
extern shell_state_t g_shell;

#endif // SHELL_TYPES_H
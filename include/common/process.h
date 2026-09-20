// SPDX-License-Identifier: GPL-3.0-only
#ifndef COMMON_PROCESS_H
#define COMMON_PROCESS_H

#include <stdint.h>

typedef enum {
    COMMON_PROCESS_OK = 0,
    COMMON_PROCESS_INVALID_ARGUMENT,
    COMMON_PROCESS_LIMIT_EXCEEDED,
    COMMON_PROCESS_LAUNCH_FAILED,
    COMMON_PROCESS_WAIT_FAILED,
    COMMON_PROCESS_TIMED_OUT
} CommonProcessStatus;

/* Run an explicit executable with literal UTF-8 arguments; no shell expansion.
 * arguments is NULL-terminated, with at most 256 entries and 65536 total bytes.
 * An executable without a path separator uses the platform's normal executable
 * search. Callers pinning a tool must supply its captured absolute path. Environment,
 * working directory and standard streams are inherited. Windows preserves the
 * C runtime quoting convention; consumers must decode their command line as
 * Unicode. Inputs remain borrowed until return.
 *
 * timeout_ms=0 waits without a deadline; UINT32_MAX is invalid. A positive timeout kills and reaps the
 * child before returning TIMED_OUT. A private POSIX process group / Windows job
 * also contains ordinary descendants. A child deliberately escaping its POSIX
 * group is outside this cleanup contract; this is not a sandbox. On success,
 * exit_code receives the process exit code (128+signal for POSIX signals).
 * A POSIX exec failure is exit code 127. Other statuses leave exit_code=-1.
 * Timed calls close their process group/job on normal completion as well, so
 * background descendants cannot outlive a completed bounded operation. */
CommonProcessStatus common_process_run(const char *const *arguments, uint32_t timeout_ms,
                                      int *exit_code);
const char *common_process_status_name(CommonProcessStatus status);

#endif

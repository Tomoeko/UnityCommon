// SPDX-License-Identifier: GPL-3.0-only
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/process.h"
#include "common/utf8.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "common/string_builder.h"
#include "common/windows_utf8.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void append_argument(StringBuilder *command, const char *argument) {
    if (command->len)
        sb_append_char(command, ' ');
    sb_append_char(command, '"');
    for (const char *at = argument;;) {
        size_t slashes = 0;
        while (*at == '\\') {
            ++slashes;
            ++at;
        }
        const size_t count = (*at == '"' || !*at) ? slashes * 2 : slashes;
        for (size_t i = 0; i < count; ++i)
            sb_append_char(command, '\\');
        if (!*at)
            break;
        if (*at == '"')
            sb_append_char(command, '\\');
        sb_append_char(command, *at++);
    }
    sb_append_char(command, '"');
}

static CommonProcessStatus run_process(const char *const *arguments, uint32_t timeout_ms,
                                       int *exit_code) {
    StringBuilder command;
    sb_init(&command);
    for (size_t i = 0; arguments[i]; ++i)
        append_argument(&command, arguments[i]);
    wchar_t *application = common_windows_utf8_to_wide(arguments[0]);
    wchar_t *wide = sb_ok(&command) ? common_windows_utf8_to_wide(command.buf) : NULL;
    sb_free(&command);
    CommonProcessStatus result = COMMON_PROCESS_LAUNCH_FAILED;
    HANDLE job = NULL;
    PROCESS_INFORMATION process = {0};
    if (!application || !wide)
        goto cleanup;
    if (wcslen(wide) >= 32767) {
        result = COMMON_PROCESS_LIMIT_EXCEEDED;
        goto cleanup;
    }
    if (timeout_ms) {
        job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                             &limits, sizeof(limits)))
            goto cleanup;
    }
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(application, wide, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                         &startup, &process))
        goto cleanup;
    if ((job && !AssignProcessToJobObject(job, process.hProcess)) ||
        ResumeThread(process.hThread) == (DWORD)-1) {
        (void)TerminateProcess(process.hProcess, 127);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        goto cleanup;
    }
    const DWORD waited = WaitForSingleObject(process.hProcess, timeout_ms ? timeout_ms : INFINITE);
    if (waited != WAIT_OBJECT_0) {
        if (job)
            (void)TerminateJobObject(job, 127);
        else
            (void)TerminateProcess(process.hProcess, 127);
        (void)WaitForSingleObject(process.hProcess, INFINITE);
        result = waited == WAIT_TIMEOUT ? COMMON_PROCESS_TIMED_OUT : COMMON_PROCESS_WAIT_FAILED;
        goto cleanup;
    }
    DWORD code;
    if (!GetExitCodeProcess(process.hProcess, &code) || code > INT_MAX) {
        result = COMMON_PROCESS_WAIT_FAILED;
        goto cleanup;
    }
    *exit_code = (int)code;
    result = COMMON_PROCESS_OK;
cleanup:
    if (process.hThread)
        (void)CloseHandle(process.hThread);
    if (process.hProcess)
        (void)CloseHandle(process.hProcess);
    if (job)
        (void)CloseHandle(job);
    free(wide);
    free(application);
    return result;
}
#else
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool monotonic_ms(uint64_t *value) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return false;
    *value = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    return true;
}

static void stop_child(pid_t child, bool grouped) {
    (void)kill(grouped ? -child : child, SIGKILL);
    /* Also handle a setup failure before the child entered its group. */
    (void)kill(child, SIGKILL);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static CommonProcessStatus run_process(const char *const *arguments, uint32_t timeout_ms,
                                       int *exit_code) {
    uint64_t started = 0;
    if (timeout_ms && !monotonic_ms(&started))
        return COMMON_PROCESS_WAIT_FAILED;
    const pid_t child = fork();
    if (child < 0)
        return COMMON_PROCESS_LAUNCH_FAILED;
    if (!child) {
        if (timeout_ms && setpgid(0, 0) != 0)
            _exit(127);
        if (strchr(arguments[0], '/'))
            execv(arguments[0], (char *const *)arguments);
        else
            execvp(arguments[0], (char *const *)arguments);
        _exit(127);
    }
    /* Both sides establish the same group, avoiding a timeout/setup race. */
    if (timeout_ms && setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        stop_child(child, true);
        return COMMON_PROCESS_LAUNCH_FAILED;
    }
    int status;
    for (;;) {
        pid_t waited;
        if (timeout_ms) {
            siginfo_t information = {0};
            const int result = waitid(P_PID, (id_t)child, &information,
                                      WEXITED | WNOHANG | WNOWAIT);
            waited = result < 0 ? -1 : information.si_pid;
            if (waited == child) {
                /* Keep the direct child unreaped while using its group ID,
                 * so PID reuse cannot redirect descendant cleanup. */
                (void)kill(-child, SIGKILL);
                do {
                    waited = waitpid(child, &status, 0);
                } while (waited < 0 && errno == EINTR);
            }
        } else {
            waited = waitpid(child, &status, 0);
        }
        if (waited == child)
            break;
        if (waited < 0 && errno != EINTR) {
            if (errno != ECHILD)
                stop_child(child, timeout_ms != 0);
            return COMMON_PROCESS_WAIT_FAILED;
        }
        if (!timeout_ms)
            continue;
        uint64_t now;
        if (!monotonic_ms(&now)) {
            stop_child(child, true);
            return COMMON_PROCESS_WAIT_FAILED;
        }
        if (now - started >= timeout_ms) {
            stop_child(child, true);
            return COMMON_PROCESS_TIMED_OUT;
        }
        const struct timespec pause = {0, 10000000};
        (void)nanosleep(&pause, NULL);
    }
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = 128 + WTERMSIG(status);
    else
        return COMMON_PROCESS_WAIT_FAILED;
    return COMMON_PROCESS_OK;
}
#endif

CommonProcessStatus common_process_run(const char *const *arguments, uint32_t timeout_ms,
                                      int *exit_code) {
    if (exit_code)
        *exit_code = -1;
    if (!exit_code || timeout_ms == UINT32_MAX ||
        !arguments || !arguments[0] || !arguments[0][0])
        return COMMON_PROCESS_INVALID_ARGUMENT;
    size_t bytes = 0;
    for (size_t i = 0; arguments[i]; ++i) {
        if (i == 256)
            return COMMON_PROCESS_LIMIT_EXCEEDED;
        size_t length = 0;
        while (arguments[i][length]) {
            if (length == 65536 - bytes)
                return COMMON_PROCESS_LIMIT_EXCEEDED;
            ++length;
        }
        if (length == 65536 - bytes)
            return COMMON_PROCESS_LIMIT_EXCEEDED;
        bytes += length + 1;
        if (common_utf8_validate((const uint8_t *)arguments[i], length).status != COMMON_UTF8_OK)
            return COMMON_PROCESS_INVALID_ARGUMENT;
    }
    return run_process(arguments, timeout_ms, exit_code);
}

const char *common_process_status_name(CommonProcessStatus status) {
    switch (status) {
    case COMMON_PROCESS_OK: return "ok";
    case COMMON_PROCESS_INVALID_ARGUMENT: return "invalid-argument";
    case COMMON_PROCESS_LIMIT_EXCEEDED: return "limit-exceeded";
    case COMMON_PROCESS_LAUNCH_FAILED: return "launch-failed";
    case COMMON_PROCESS_WAIT_FAILED: return "wait-failed";
    case COMMON_PROCESS_TIMED_OUT: return "timed-out";
    default: return "unknown";
    }
}

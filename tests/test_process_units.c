// SPDX-License-Identifier: GPL-3.0-only
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "common/process.h"
#include "common/file_io.h"
#include "common/windows_utf8.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #value); return 1; \
} } while (0)

static const char *const literal_arguments[] = {
    "", "two words", "\"quoted\"", "one\\\"two", "trailing\\", "\\\\\"", "a\nb\tc",
    "$(literal) `literal` ; & | > < * ? %PATH%", "caf\xc3\xa9_\xe9\x9b\xaa"};

static void pause_ms(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    const struct timespec duration = {(time_t)(milliseconds / 1000),
                                      (long)(milliseconds % 1000) * 1000000};
    (void)nanosleep(&duration, NULL);
#endif
}

static int run_tests(int argc, char **argv) {
    if (argc >= 2) {
        if (strcmp(argv[1], "literals") == 0) {
            CHECK(argc == 2 + (int)(sizeof(literal_arguments) / sizeof(*literal_arguments)));
            for (int i = 2; i < argc; ++i)
                CHECK(strcmp(argv[i], literal_arguments[i - 2]) == 0);
            return 23;
        }
        if (strcmp(argv[1], "wait") == 0) {
            pause_ms(10000);
            return 29;
        }
        if (strcmp(argv[1], "nested") == 0) {
            CHECK(argc == 4);
            const char *nested[] = {argv[0], "grandchild", argv[2], argv[3], NULL};
            int code;
            CHECK(common_process_run(nested, 0, &code) == COMMON_PROCESS_OK);
            return code;
        }
        if (strcmp(argv[1], "grandchild") == 0) {
            CHECK(argc == 4);
            CHECK(common_file_write_new_atomic(argv[2], "started", 7) == COMMON_FILE_OK);
            pause_ms(2000);
            CHECK(common_file_write_new_atomic(argv[3], "survived", 8) == COMMON_FILE_OK);
            return 0;
        }
#ifndef _WIN32
        if (strcmp(argv[1], "signal") == 0) {
            (void)raise(SIGTERM);
            return 1;
        }
#endif
        return 1;
    }
    int code = 9;
    const char *missing[] = {"./nonexistent-process-test-executable", NULL};
    CommonProcessStatus status = common_process_run(missing, 1000, &code);
#ifdef _WIN32
    CHECK(status == COMMON_PROCESS_LAUNCH_FAILED && code == -1);
#else
    CHECK(status == COMMON_PROCESS_OK && code == 127);
#endif
    const char *arguments[16] = {argv[0], "literals"};
    for (size_t i = 0; i < sizeof(literal_arguments) / sizeof(*literal_arguments); ++i)
        arguments[i + 2] = literal_arguments[i];
    CHECK(common_process_run(arguments, 3000, &code) == COMMON_PROCESS_OK && code == 23);
    CHECK(common_process_run(arguments, 0, &code) == COMMON_PROCESS_OK && code == 23);
    const char *wait[] = {argv[0], "wait", NULL};
    CHECK(common_process_run(wait, 50, &code) == COMMON_PROCESS_TIMED_OUT && code == -1);
    char marker[80], completed[80];
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    CHECK(snprintf(marker, sizeof(marker), "process-descendant-%lu.tmp", pid) > 0);
    CHECK(snprintf(completed, sizeof(completed), "process-descendant-%lu.done", pid) > 0);
    const char *nested[] = {argv[0], "nested", marker, completed, NULL};
    CHECK(common_process_run(nested, 1000, &code) == COMMON_PROCESS_TIMED_OUT && code == -1);
    pause_ms(1300);
    CommonFileBytes content = {0};
    CHECK(common_file_read_regular(marker, 16, &content) == COMMON_FILE_OK);
    CHECK(content.size == 7 && memcmp(content.data, "started", 7) == 0);
    common_file_bytes_dispose(&content);
    CHECK(common_file_read_regular(completed, 16, &content) == COMMON_FILE_NOT_FOUND);
    CHECK(remove(marker) == 0);
#ifndef _WIN32
    const char *signal[] = {argv[0], "signal", NULL};
    CHECK(common_process_run(signal, 3000, &code) == COMMON_PROCESS_OK && code == 128 + SIGTERM);
#endif
    const char *invalid[] = {"no-path-search", NULL};
    CHECK(common_process_run(invalid, 0, &code) == COMMON_PROCESS_INVALID_ARGUMENT && code == -1);
    invalid[0] = "";
    CHECK(common_process_run(invalid, 0, &code) == COMMON_PROCESS_INVALID_ARGUMENT);
    CHECK(common_process_run(NULL, 0, &code) == COMMON_PROCESS_INVALID_ARGUMENT);
    CHECK(common_process_run(arguments, 0, NULL) == COMMON_PROCESS_INVALID_ARGUMENT);
    CHECK(common_process_run(arguments, UINT32_MAX, &code) == COMMON_PROCESS_INVALID_ARGUMENT);
    const char *bad_utf8[] = {argv[0], "\xff", NULL};
    CHECK(common_process_run(bad_utf8, 0, &code) == COMMON_PROCESS_INVALID_ARGUMENT);
    char large[65537];
    memset(large, 'x', sizeof(large));
    large[sizeof(large) - 1] = 0;
    const char *large_arguments[] = {argv[0], large, NULL};
    CHECK(common_process_run(large_arguments, 0, &code) == COMMON_PROCESS_LIMIT_EXCEEDED);
    const char *many[258] = {argv[0]};
    for (size_t i = 1; i < 257; ++i)
        many[i] = "";
    CHECK(common_process_run(many, 0, &code) == COMMON_PROCESS_LIMIT_EXCEEDED);
    for (int i = COMMON_PROCESS_OK; i <= COMMON_PROCESS_TIMED_OUT; ++i)
        CHECK(strcmp(common_process_status_name((CommonProcessStatus)i), "unknown") != 0);
    CHECK(strcmp(common_process_status_name((CommonProcessStatus)-1), "unknown") == 0);
    return 0;
}

COMMON_DEFINE_UTF8_MAIN(run_tests)

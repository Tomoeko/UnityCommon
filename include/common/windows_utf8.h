// SPDX-License-Identifier: GPL-3.0-only

#ifndef COMMON_WINDOWS_UTF8_H
#define COMMON_WINDOWS_UTF8_H

#include <stddef.h>

/*
 * Define one native process entry point around a UTF-8 implementation. On
 * Windows the OS-supplied UTF-16 command line is converted without using the
 * active ANSI code page. Other hosts call the implementation directly.
 */
#ifdef _WIN32

#include <stdbool.h>

typedef int (*CommonWindowsUtf8MainFunction)(int argc, char** argv);

int common_windows_run_utf8_main(
    int argc, wchar_t** wide_argv,
    CommonWindowsUtf8MainFunction implementation);

#define COMMON_DEFINE_UTF8_MAIN(implementation) \
    int wmain(int argc, wchar_t** wide_argv) { \
        return common_windows_run_utf8_main( \
            argc, wide_argv, (implementation)); \
    }

/*
 * Windows product paths are UTF-8 at every public C boundary.  These helpers
 * are the only conversion boundary before calling the UTF-16 Win32 APIs.
 * Returned strings use malloc/free ownership.  On failure GetLastError()
 * retains the conversion error or is set to ERROR_NOT_ENOUGH_MEMORY.
 */
wchar_t* common_windows_utf8_to_wide(const char* value);
char* common_windows_wide_to_utf8(const wchar_t* value);

/* Return the lexical root that must already exist and must never be passed to
 * _wmkdir().  Drive roots, UNC shares, and their extended \\?\ spellings are
 * supported.  Incomplete or unsupported device roots are rejected. */
bool common_windows_directory_root_length(
    const char* path, size_t length, size_t* out_length);

#else

#define COMMON_DEFINE_UTF8_MAIN(implementation) \
    int main(int argc, char** argv) { \
        return (implementation)(argc, argv); \
    }

#endif

#endif /* COMMON_WINDOWS_UTF8_H */

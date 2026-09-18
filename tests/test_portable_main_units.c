#include "common/windows_utf8.h"

#include <stdio.h>
#include <string.h>

static int portable_main_test(int argc, char** argv) {
    static const char expected[] = "caf\xc3\xa9_\xe9\x9b\xaa";
    if (argc != 2 || !argv || !argv[0] || !argv[1] ||
        strcmp(argv[1], expected) != 0) {
        fprintf(stderr, "UnityCommon UTF-8 command-line conversion mismatch\n");
        return 1;
    }
    return 0;
}

COMMON_DEFINE_UTF8_MAIN(portable_main_test)

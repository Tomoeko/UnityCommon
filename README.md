An experiment utilizing Codex, ChatGPT, and OpenAI heavily.

# UnityCommon

Portable C11 libraries for reading Unity data and building asset tools.

| CMake target | Provides |
| --- | --- |
| `UnityCommon::base` | Bounded file I/O, paths, UTF-8, streams, strings, SHA-256, and atomic output |
| `UnityCommon::serialized` | UnityFS, LZ4/LZMA, SerializedFile, TypeTrees, schemas, PPtr resolution, and build settings |
| `UnityCommon::test_support` | Fixture mutation helpers; available with testing enabled |

## Build and test

Requires CMake 3.10+ and a C11 compiler. The normal build needs no Unity installation.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Use `-DBUILD_TESTING=OFF` to build only the libraries. CTest's `--test-dir`
option requires CMake 3.20+; with older CMake, run `ctest` inside `build`.

To embed the library in another CMake project:

```cmake
add_subdirectory(external/UnityCommon)
target_link_libraries(your_tool PRIVATE UnityCommon::serialized)
```

Link the narrowest target needed; `serialized` already links `base`.
Public headers are in `include/`, implementations in `src/`, and test setup
in `cmake/Tests.cmake`. There is currently no standalone install/package target.

## Supported scope

SerializedFile support focuses on v22 and explicitly admitted Unity 2021.3
layouts. Individual APIs document supported versions, ownership, and limits.
Bounded physical views preserve input bytes; full parsing applies stricter
schema, uniqueness, and payload checks. Successful parsing does not establish
complete project recovery or compatibility with every Unity version.

Path discovery is a pathname-based convenience API. It does not establish a
stable filesystem snapshot or a complete player inventory.

## Fixtures

`tests/fixtures/` retains small controlled serialized files, writer inputs,
and machine-readable observations. Fixture hashes protect regression inputs;
they are not required hashes for a user's Unity executable. Some serialized
fixture namespaces retain historical names because those bytes are tested.

Optional reproduction requires Python 3.11+ and a Unity 2021.3.35f1 Editor:

```sh
python3 tests/fixtures/managed_reference_registry/run_fixture.py \
  --unity /path/to/Unity --output /tmp/managed-fixture
python3 tests/fixtures/serialized_metadata_tail/run_fixture.py \
  --unity /path/to/Unity --output /tmp/tail-fixture
python3 tests/fixtures/common_string_table/extract_verify.py \
  --source /path/to/binary2text
```

Generators create and remove isolated scratch projects. Use fresh output paths.
Private Editor builds are supported: identity is recorded, not compared to a
stock executable. Converter observations still validate the relevant arm64
layout and fixture bytes.

## License

GNU General Public License v3.0 only; see [LICENSE](LICENSE).
The bundled LZMA decoder in `src/io/lzma/` is Igor Pavlov's public-domain code;
its original notices are preserved. Unity binaries are not distributed.

cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(UnityCommon LANGUAGES C)
    include(CTest)
endif()

add_library(unity_common_language_options INTERFACE)
target_compile_features(unity_common_language_options INTERFACE c_std_11)
if(MSVC)
    target_compile_options(unity_common_language_options INTERFACE /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    # common.h exposes C11 atomic types and inline operations to consumers.
    target_compile_options(unity_common_language_options INTERFACE
        "$<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>")
endif()

add_library(unity_common_build_options INTERFACE)
target_link_libraries(unity_common_build_options INTERFACE
    unity_common_language_options)
if(MSVC)
    target_compile_options(unity_common_build_options INTERFACE /W4 /WX)
else()
    target_compile_options(unity_common_build_options INTERFACE
        -Wall -Wextra -Werror)
endif()

set(UNITY_COMMON_BASE_SOURCES
    src/common/ascii_glob.c
    src/common/executable_resource.c
    src/common/file_io.c
    src/common/output_publish.c
    src/common/path_discovery.c
    src/common/relative_path.c
    src/common/sha256.c
    src/common/stream.c
    src/common/string_builder.c
    src/common/utf8.c
)
if(WIN32)
    list(APPEND UNITY_COMMON_BASE_SOURCES src/common/windows_utf8.c)
endif()

add_library(unity_common_base STATIC ${UNITY_COMMON_BASE_SOURCES})
add_library(UnityCommon::base ALIAS unity_common_base)
target_include_directories(unity_common_base
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(unity_common_base
    PUBLIC unity_common_language_options
    PRIVATE unity_common_build_options)
set_target_properties(unity_common_base PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

add_library(unity_common_serialized STATIC
    src/io/lz4_decompress.c
    src/io/lzma/LzmaDec.c
    src/io/bundle_archive.c
    src/io/unity_input.c
    src/io/unity_pptr_resolver.c
    src/io/typetree.c
    src/io/typetree_schema_profile.c
    src/io/typetree_schema_registry.c
    src/io/serialized_file.c
    src/io/serialized_file_directory.c
    src/io/serialized_file_metadata_tail.c
    src/io/serialized_file_reference_index.c
    src/io/serialized_file_schema.c
    src/io/serialized_file_schema_context.c
    src/io/serialized_file_values.c
    src/io/serialized_file_managed_values.c
    src/io/serialized_file_managed_profile_internal.c
    src/io/serialized_metadata_reader_internal.c
    src/io/serialized_file_prefix.c
    src/io/unity_player_build_settings.c
)
add_library(UnityCommon::serialized ALIAS unity_common_serialized)
target_include_directories(unity_common_serialized
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(unity_common_serialized
    PUBLIC unity_common_base
    PRIVATE unity_common_build_options)
set_target_properties(unity_common_serialized PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)

if(BUILD_TESTING)
    include(cmake/Tests.cmake)
endif()

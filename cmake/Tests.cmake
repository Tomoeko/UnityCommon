if(BUILD_TESTING)
    add_library(unity_common_test_support STATIC
        tests/test_support/file_mutation.c)
    add_library(UnityCommon::test_support ALIAS unity_common_test_support)
    target_include_directories(unity_common_test_support
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(unity_common_test_support
        PUBLIC UnityCommon::base
        PRIVATE unity_common_build_options)
    set_target_properties(unity_common_test_support PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)

    function(unity_common_add_instrumented_object target header)
        add_library(${target} OBJECT ${ARGN})
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(${target} PRIVATE unity_common_build_options)
        if(MSVC)
            target_compile_options(${target} PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/${header}")
        else()
            target_compile_options(${target} PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/tests/${header}")
        endif()
        set_target_properties(${target} PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    endfunction()

    function(unity_common_add_unit_test target source library)
        add_executable(${target} "tests/${source}")
        target_link_libraries(${target} PRIVATE
            ${library} unity_common_build_options)
        set_target_properties(${target} PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    unity_common_add_unit_test(
        unity_common_ascii_glob_units
        test_ascii_glob_units.c UnityCommon::base)
    unity_common_add_unit_test(
        unity_common_file_io_units
        test_file_io_units.c UnityCommon::base)
    unity_common_add_unit_test(
        unity_common_process_units
        test_process_units.c UnityCommon::base)
    if(MINGW)
        set_property(TARGET unity_common_process_units APPEND_STRING
            PROPERTY LINK_FLAGS " -municode")
    endif()
    unity_common_add_unit_test(
        unity_common_path_discovery_units
        test_path_discovery_units.c UnityCommon::base)
    unity_common_add_unit_test(
        unity_common_relative_path_units
        test_relative_path_units.c UnityCommon::base)
    unity_common_add_unit_test(
        unity_common_utf8_units
        test_utf8_units.c UnityCommon::base)
    unity_common_add_unit_test(
        unity_common_executable_resource_units
        test_executable_resource_units.c UnityCommon::base)
    add_executable(unity_common_portable_main_units
        tests/test_portable_main_units.c)
    target_link_libraries(unity_common_portable_main_units PRIVATE
        UnityCommon::base unity_common_build_options)
    set_target_properties(unity_common_portable_main_units PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME unity_common_portable_main_utf8
        COMMAND unity_common_portable_main_units "café_雪")
    if(MINGW)
        set_property(TARGET unity_common_portable_main_units APPEND_STRING
            PROPERTY LINK_FLAGS " -municode")
    endif()

    set(_unity_common_resource_test_bin
        "${CMAKE_CURRENT_BINARY_DIR}/resource-layout/bin")
    set_target_properties(unity_common_executable_resource_units PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_unity_common_resource_test_bin}")
    foreach(_unity_common_config ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${_unity_common_config}"
            _unity_common_config_upper)
        set_target_properties(unity_common_executable_resource_units
            PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${_unity_common_config_upper}"
            "${_unity_common_resource_test_bin}")
    endforeach()
    set(_unity_common_resource_test_share
        "${CMAKE_CURRENT_BINARY_DIR}/resource-layout/share/unity-common/test")
    file(MAKE_DIRECTORY "${_unity_common_resource_test_share}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/executable_resource.txt"
        "${_unity_common_resource_test_share}/resource.txt" COPYONLY)

    unity_common_add_unit_test(
        unity_common_serialized_units
        test_common_units.c UnityCommon::serialized)
    unity_common_add_unit_test(
        unity_common_lz4_exact_units
        test_lz4_exact.c UnityCommon::serialized)
    unity_common_add_unit_test(
        unity_common_serialized_prefix_units
        test_serialized_file_prefix.c UnityCommon::serialized)
    unity_common_add_unit_test(
        unity_common_serialized_directory_units
        test_serialized_file_directory.c UnityCommon::serialized)
    unity_common_add_unit_test(
        unity_common_serialized_metadata_tail_units
        test_serialized_file_metadata_tail.c UnityCommon::serialized)
    target_sources(unity_common_serialized_metadata_tail_units PRIVATE
        tests/serialized_metadata_tail_fixture.c)
    unity_common_add_instrumented_object(unity_common_reference_index_test
        serialized_reference_index_test.h
        src/io/serialized_file_reference_index.c)
    unity_common_add_unit_test(unity_common_serialized_reference_index_units
        test_serialized_file_reference_index.c UnityCommon::serialized)
    target_sources(unity_common_serialized_reference_index_units PRIVATE
        $<TARGET_OBJECTS:unity_common_reference_index_test>)
    add_executable(unity_common_serialized_reference_index_writer
        tests/test_serialized_file_reference_index_writer.c)
    target_link_libraries(unity_common_serialized_reference_index_writer PRIVATE
        UnityCommon::serialized unity_common_build_options)
    set_target_properties(unity_common_serialized_reference_index_writer PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME unity_common_serialized_reference_index_writer
        COMMAND unity_common_serialized_reference_index_writer
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_reference_registry/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_reference_registry/without-tree.assets")
    unity_common_add_instrumented_object(unity_common_values_allocation_test
        serialized_values_allocation_test.h
        src/io/serialized_file_values.c)
    unity_common_add_unit_test(unity_common_serialized_values_units
        test_serialized_file_values.c UnityCommon::serialized)
    target_sources(unity_common_serialized_values_units PRIVATE
        $<TARGET_OBJECTS:unity_common_values_allocation_test>)
    add_executable(unity_common_serialized_values_writer
        tests/test_serialized_file_values_writer.c)
    target_link_libraries(unity_common_serialized_values_writer PRIVATE
        UnityCommon::serialized unity_common_build_options)
    set_target_properties(unity_common_serialized_values_writer PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME unity_common_serialized_values_writer
        COMMAND unity_common_serialized_values_writer
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/common_string_table/exact35.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/without-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/without-tree.assets")
    add_executable(unity_common_serialized_values_ordinary_writer
        tests/test_serialized_file_values_ordinary_writer.c)
    target_link_libraries(unity_common_serialized_values_ordinary_writer PRIVATE
        UnityCommon::serialized unity_common_build_options)
    set_target_properties(unity_common_serialized_values_ordinary_writer PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME unity_common_serialized_values_ordinary_writer
        COMMAND unity_common_serialized_values_ordinary_writer
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/common_string_table/exact35.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/without-tree.assets")
    unity_common_add_instrumented_object(unity_common_managed_values_allocation_test
        test_serialized_file_managed_values_allocation.h
        src/io/serialized_file_managed_values.c
        src/io/serialized_file_schema.c
        src/io/serialized_file_reference_index.c)
    add_executable(unity_common_serialized_managed_values_units
        tests/test_serialized_file_managed_values.c)
    add_executable(unity_common_serialized_managed_values_writer
        tests/test_serialized_file_managed_values_writer.c)
    target_sources(unity_common_serialized_managed_values_units PRIVATE
        $<TARGET_OBJECTS:unity_common_managed_values_allocation_test>)
    foreach(_managed_values_test IN ITEMS
            unity_common_serialized_managed_values_units
            unity_common_serialized_managed_values_writer)
        target_sources(${_managed_values_test} PRIVATE
            tests/test_serialized_file_managed_values_support.c)
        target_link_libraries(${_managed_values_test} PRIVATE
            UnityCommon::serialized unity_common_build_options)
        set_target_properties(${_managed_values_test} PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        add_test(NAME ${_managed_values_test}
            COMMAND ${_managed_values_test}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/common_string_table/exact35.bin"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_reference_registry/with-tree.assets")
    endforeach()
    add_library(unity_common_schema_allocation_test OBJECT
        src/io/serialized_file_schema.c)
    target_include_directories(unity_common_schema_allocation_test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(unity_common_schema_allocation_test
        PRIVATE unity_common_build_options)
    if(MSVC)
        target_compile_options(unity_common_schema_allocation_test PRIVATE
            "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/serialized_schema_allocation_test.h")
    else()
        target_compile_options(unity_common_schema_allocation_test PRIVATE
            -include "${CMAKE_CURRENT_SOURCE_DIR}/tests/serialized_schema_allocation_test.h")
    endif()
    set_target_properties(unity_common_schema_allocation_test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    unity_common_add_unit_test(unity_common_serialized_schema_units
        test_serialized_file_schema.c UnityCommon::serialized)
    target_sources(unity_common_serialized_schema_units PRIVATE
        $<TARGET_OBJECTS:unity_common_schema_allocation_test>)
    add_executable(unity_common_serialized_schema_writer
        tests/test_serialized_file_schema_writer.c)
    target_link_libraries(unity_common_serialized_schema_writer PRIVATE
        UnityCommon::serialized unity_common_build_options)
    set_target_properties(unity_common_serialized_schema_writer PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME unity_common_serialized_schema_writer
        COMMAND unity_common_serialized_schema_writer
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/common_string_table/exact35.bin"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/without-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/without-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_reference_registry/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_reference_registry/without-tree.assets")
    add_executable(unity_common_serialized_schema_context_units
        tests/test_serialized_file_schema_context.c)
    add_executable(unity_common_serialized_schema_context_writer
        tests/test_serialized_file_schema_context_writer.c)
    foreach(_unity_common_context_target
        unity_common_serialized_schema_context_units
        unity_common_serialized_schema_context_writer)
        target_sources(${_unity_common_context_target} PRIVATE
            tests/serialized_schema_context_fixture.c)
        target_link_libraries(${_unity_common_context_target} PRIVATE
            UnityCommon::serialized unity_common_build_options)
        set_target_properties(${_unity_common_context_target} PROPERTIES
            C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    endforeach()
    add_test(NAME unity_common_serialized_schema_context_units
        COMMAND unity_common_serialized_schema_context_units
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/with-tree.assets")
    add_test(NAME unity_common_serialized_schema_context_writer
        COMMAND unity_common_serialized_schema_context_writer
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed_reference_registry/with-tree.assets")
    unity_common_add_unit_test(
        unity_common_serialized_directory_consumer_units
        test_serialized_file_directory_consumer.c UnityCommon::serialized)
    add_executable(unity_common_serialized_directory_writer
        tests/test_serialized_file_directory_writer.c)
    target_link_libraries(unity_common_serialized_directory_writer PRIVATE
        UnityCommon::serialized unity_common_build_options)
    set_target_properties(unity_common_serialized_directory_writer PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME unity_common_serialized_directory_writer
        COMMAND unity_common_serialized_directory_writer
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_directory/without-tree.assets")
    add_executable(unity_common_serialized_metadata_tail_writer
        tests/test_serialized_file_metadata_tail_writer.c)
    target_link_libraries(unity_common_serialized_metadata_tail_writer PRIVATE
        UnityCommon::serialized unity_common_build_options)
    set_target_properties(unity_common_serialized_metadata_tail_writer PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME unity_common_serialized_metadata_tail_writer
        COMMAND unity_common_serialized_metadata_tail_writer
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/with-tree.assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serialized_metadata_tail/without-tree.assets")
    unity_common_add_unit_test(
        unity_common_input_units
        test_unity_input_units.c UnityCommon::serialized)
    target_link_libraries(unity_common_input_units PRIVATE
        unity_common_test_support)
    unity_common_add_unit_test(
        unity_common_pptr_resolver_units
        test_unity_pptr_resolver_units.c UnityCommon::serialized)
    unity_common_add_unit_test(
        unity_common_player_build_settings_units
        test_unity_player_build_settings_units.c UnityCommon::serialized)
    unity_common_add_unit_test(
        unity_common_typetree_schema_registry_units
        test_typetree_schema_registry.c UnityCommon::serialized)
    unity_common_add_unit_test(
        unity_common_typetree_value_digest_units
        test_typetree_value_digest.c UnityCommon::serialized)
    add_executable(unity_common_typetree_common_strings
        tests/test_typetree_common_strings.c)
    target_link_libraries(unity_common_typetree_common_strings PRIVATE
        UnityCommon::serialized unity_common_build_options)
    set_target_properties(unity_common_typetree_common_strings PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    add_test(NAME unity_common_typetree_common_strings
        COMMAND unity_common_typetree_common_strings
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/common_string_table/exact35.bin")

    if(APPLE)
        add_library(unity_common_portable_sha256_test_support STATIC
            src/common/sha256.c)
        target_compile_definitions(
            unity_common_portable_sha256_test_support PRIVATE
            UNITY_COMMON_FORCE_PORTABLE_SHA256)
        target_include_directories(
            unity_common_portable_sha256_test_support
            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(
            unity_common_portable_sha256_test_support PRIVATE
            unity_common_build_options)
        set_target_properties(
            unity_common_portable_sha256_test_support PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)

        unity_common_add_unit_test(
            unity_common_portable_sha256_units
            test_sha256_portable_units.c
            unity_common_portable_sha256_test_support)
    endif()
endif()

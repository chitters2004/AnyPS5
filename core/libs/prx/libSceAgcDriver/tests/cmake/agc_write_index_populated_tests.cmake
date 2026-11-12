if(BUILD_TESTING)
    add_test_executable(agc_write_index_populated_tests
            tests/execution/WriteIndexPopulated.cpp
            Execution/src/GuestMemory.cpp
            Execution/src/PerformanceTraceThread.cpp
    )
    target_include_directories(agc_write_index_populated_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_write_index_populated_tests PRIVATE shader_recompiler_agc_driver libc)
    set_target_properties(agc_write_index_populated_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_write_index_populated_tests)
    add_test(NAME agc_write_index_populated COMMAND agc_write_index_populated_tests --smoke)
    set_tests_properties(agc_write_index_populated PROPERTIES TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_write_index_populated PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

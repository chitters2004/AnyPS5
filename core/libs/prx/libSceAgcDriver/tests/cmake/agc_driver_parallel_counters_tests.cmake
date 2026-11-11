if(BUILD_TESTING)
    add_test_executable(agc_driver_parallel_counters_tests tests/ParallelCounters.cpp)
    target_include_directories(agc_driver_parallel_counters_tests PRIVATE ${LIBS_INCLUDE_DIR} ${ANYPS5_VULKAN_HEADERS_INCLUDE_DIR})
    target_link_libraries(agc_driver_parallel_counters_tests PRIVATE libSceAgcDriver)
    set_target_properties(agc_driver_parallel_counters_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_driver_parallel_counters_tests)
    add_test(NAME agc_driver_parallel_counters COMMAND agc_driver_parallel_counters_tests)
    set_tests_properties(agc_driver_parallel_counters PROPERTIES TIMEOUT 20
        ENVIRONMENT "APS5_PROFILE_COMMIT=1")
    if(WIN32)
        set_tests_properties(agc_driver_parallel_counters PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAgcDriver>")
    endif()
endif()

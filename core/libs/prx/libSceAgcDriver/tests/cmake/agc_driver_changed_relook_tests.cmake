if(BUILD_TESTING)
    add_test(NAME agc_driver_changed_relook_copied COMMAND agc_driver_changed_relook_tests)
    set_tests_properties(agc_driver_changed_relook_copied PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_HOST_IMPORT=1;APS5_VERIFY_PROOFS=1" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_changed_relook_copied PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
if(BUILD_TESTING)
    add_test(NAME agc_driver_changed_relook_walk COMMAND agc_driver_changed_relook_tests)
    set_tests_properties(agc_driver_changed_relook_walk PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_HOST_IMPORT=1;APS5_NO_CHANGED_RELOOK=1" TIMEOUT 30 SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_changed_relook_walk PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

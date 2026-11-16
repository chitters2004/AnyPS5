if(BUILD_TESTING)
    set_tests_properties(agc_driver_prepared_dispatch_eviction PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" TIMEOUT 60)
    add_test(NAME agc_driver_prepared_dispatch_resource_eviction COMMAND agc_driver_prepared_dispatch_eviction_tests --resource-eviction)
    set_tests_properties(agc_driver_prepared_dispatch_resource_eviction PROPERTIES ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_prepared_dispatch_resource_eviction PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

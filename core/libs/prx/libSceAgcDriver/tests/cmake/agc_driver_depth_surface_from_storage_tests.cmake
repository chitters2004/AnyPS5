if(BUILD_TESTING)
    add_test(NAME agc_driver_depth_surface_from_storage_stencil_note COMMAND agc_driver_depth_surface_from_storage_tests --stencil-storage-note)
    set_tests_properties(agc_driver_depth_surface_from_storage_stencil_note PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PIPELINE_JOURNAL=1;APS5_NO_HOST_IMPORT=1" SKIP_RETURN_CODE 77 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_depth_surface_from_storage_stencil_note PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

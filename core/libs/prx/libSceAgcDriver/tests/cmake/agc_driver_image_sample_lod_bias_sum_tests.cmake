if(BUILD_TESTING)
    add_test(NAME agc_driver_image_sample_lod_bias_sum_table COMMAND agc_driver_image_sample_lod_bias_sum_tests --table-counters)
    set_tests_properties(agc_driver_image_sample_lod_bias_sum_table PROPERTIES ENVIRONMENT "APS5_PROFILE_MODULE_VARIANTS=1;ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_image_sample_lod_bias_sum_table PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

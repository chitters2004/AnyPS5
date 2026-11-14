if(BUILD_TESTING)
    set_property(TEST agc_driver_scoped_lookup_proof APPEND PROPERTY ENVIRONMENT "APS5_PROFILE_LOOKUP=1")
    add_test(NAME agc_driver_scoped_lookup_proof_verify COMMAND agc_driver_scoped_lookup_proof_tests)
    set_tests_properties(agc_driver_scoped_lookup_proof_verify PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PIPELINE_JOURNAL=1;APS5_VERIFY_LOOKUP_C2=1" SKIP_RETURN_CODE 77 TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME agc_driver_scoped_lookup_proof_off COMMAND agc_driver_scoped_lookup_proof_tests)
    set_tests_properties(agc_driver_scoped_lookup_proof_off PROPERTIES
            ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_PIPELINE_JOURNAL=1;APS5_PROFILE_LOOKUP=1;APS5_NO_LOOKUP_C2=1" SKIP_RETURN_CODE 77 TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        set_tests_properties(agc_driver_scoped_lookup_proof_verify agc_driver_scoped_lookup_proof_off PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

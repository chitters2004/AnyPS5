if(BUILD_TESTING)
    add_test(NAME agc_driver_nan_proof_chains_off COMMAND agc_driver_nan_proof_chains_tests)
    set_tests_properties(agc_driver_nan_proof_chains_off PROPERTIES ENVIRONMENT "APS5_NO_NAN_PROOF=1;ANYPS5_NO_SHADER_CACHE=1" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_driver_nan_proof_chains_off PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

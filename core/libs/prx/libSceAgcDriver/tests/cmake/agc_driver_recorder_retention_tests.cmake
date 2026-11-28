if(BUILD_TESTING AND MINGW)
    target_compile_definitions(agc_driver_recorder_retention_tests PRIVATE APS5_AGC_ALLOCATION_COUNT=1)
endif()

if(BUILD_TESTING)
    set_tests_properties(agc_driver_recorder_retention PROPERTIES FAIL_REGULAR_EXPRESSION "SYNC-HAZARD-|VUID-" TIMEOUT 60)
endif()

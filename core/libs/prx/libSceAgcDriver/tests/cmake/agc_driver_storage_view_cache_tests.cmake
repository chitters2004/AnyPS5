if(BUILD_TESTING AND MINGW)
    target_compile_definitions(agc_driver_storage_view_cache_tests PRIVATE APS5_AGC_ALLOCATION_COUNT=1)
endif()

if(BUILD_TESTING)
    set_tests_properties(agc_driver_storage_view_cache PROPERTIES FAIL_REGULAR_EXPRESSION "VUID-" TIMEOUT 60)
endif()

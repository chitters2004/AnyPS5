if(BUILD_TESTING AND MINGW)
    target_compile_definitions(agc_driver_depth_surface_from_storage_tests PRIVATE APS5_AGC_ALLOCATION_COUNT=1)
endif()

if(BUILD_TESTING)
    set_property(TEST agc_driver_depth_surface_from_storage APPEND PROPERTY ENVIRONMENT "VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
    set_tests_properties(agc_driver_depth_surface_from_storage PROPERTIES FAIL_REGULAR_EXPRESSION "SYNC-HAZARD-|VUID-")
endif()

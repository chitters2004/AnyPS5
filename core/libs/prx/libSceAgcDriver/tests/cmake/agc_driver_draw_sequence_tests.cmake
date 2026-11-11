if(BUILD_TESTING)
    add_test_executable(agc_driver_draw_sequence_tests tests/DrawSequence.cpp)
    target_include_directories(agc_driver_draw_sequence_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(agc_driver_draw_sequence_tests PRIVATE libSceAgcDriver)
    set_target_properties(agc_driver_draw_sequence_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(agc_driver_draw_sequence_tests)
    add_test(NAME agc_driver_draw_sequence COMMAND agc_driver_draw_sequence_tests)
    set_tests_properties(agc_driver_draw_sequence PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
        ENVIRONMENT "APS5_DRAW_PIPELINE=1;APS5_PIPELINED_DRAWS=8")
    add_test(NAME agc_driver_draw_sequence_drained COMMAND agc_driver_draw_sequence_tests)
    set_tests_properties(agc_driver_draw_sequence_drained PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
        ENVIRONMENT "APS5_DRAW_PIPELINE=1;APS5_PIPELINED_DRAWS=8;APS5_NO_SEQUENCE_RING=1")
    if(WIN32)
        set_tests_properties(agc_driver_draw_sequence agc_driver_draw_sequence_drained PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceAgcDriver>")
    endif()
endif()

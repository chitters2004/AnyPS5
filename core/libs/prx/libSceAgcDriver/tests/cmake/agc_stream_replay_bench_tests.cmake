if(BUILD_TESTING)
    add_test_executable(agc_stream_replay_bench_tests tests/StreamReplayBench.cpp Graphics/src/CommandStream.cpp Graphics/src/SealedBatch.cpp Graphics/src/CommandThunks.cpp)
    target_include_directories(agc_stream_replay_bench_tests PRIVATE ${LIBS_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include)
    target_link_libraries(agc_stream_replay_bench_tests PRIVATE SDL2-static libc)
    configure_windows_unwind(agc_stream_replay_bench_tests)
    add_test(NAME agc_stream_replay_bench COMMAND agc_stream_replay_bench_tests)
    set_tests_properties(agc_stream_replay_bench PROPERTIES
            ENVIRONMENT "APS5_REPLAY_BENCH_BATCHES=2;APS5_REPLAY_BENCH_DRAWS=200" SKIP_RETURN_CODE 77 TIMEOUT 60)
    if(WIN32)
        set_tests_properties(agc_stream_replay_bench PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

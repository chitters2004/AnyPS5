if(BUILD_TESTING AND Python3_FOUND)
    add_test(NAME agc_push_bytes_readers COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/PushBytesReaders.py ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(BUILD_TESTING AND (NOT ANYPS5_AGC_EXECUTION_TESTS OR "ReplayGuard" IN_LIST ANYPS5_AGC_EXECUTION_TESTS))
    set(agcReplayGuardTests)
    foreach(agcReplayGuardSite IN ITEMS
            GuestMemory::Read GuestMemory::ReadCommitted GuestMemory::ReadCommittedUnsynced GuestMemory::Write
            GuestMemory::CollectWrites GuestMemory::CollectWritesUncached GuestMemory::FlushGpuWrites
            Recorder::Active HostImportFor ExistingHostImportFor
            StorageTexture::MarkDirty StorageTexture::MarkSliceDirty StorageTexture::MarkDrawn StorageTexture::MarkWholeWritten
            StorageTexture::MarkUnitsWritten StorageTexture::Flush StorageTexture::WriteBack StorageTexture::Detach)
        string(REPLACE "::" "_" agcReplayGuardName "${agcReplayGuardSite}")
        string(REGEX REPLACE "([a-z0-9])([A-Z])" "\\1_\\2" agcReplayGuardName "${agcReplayGuardName}")
        string(TOLOWER "agc_driver_replay_guard_refuses_${agcReplayGuardName}" agcReplayGuardTest)
        add_test(NAME ${agcReplayGuardTest} COMMAND agc_driver_replay_guard_tests --site ${agcReplayGuardSite})
        set_tests_properties(${agcReplayGuardTest} PROPERTIES PASS_REGULAR_EXPRESSION "AGC graphics: ${agcReplayGuardSite} was called while replaying an encoded command" ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1" TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        list(APPEND agcReplayGuardTests ${agcReplayGuardTest})
    endforeach()
    add_test(NAME agc_driver_replay_guard_off COMMAND agc_driver_replay_guard_tests --site Recorder::Active)
    set_tests_properties(agc_driver_replay_guard_off PROPERTIES PASS_REGULAR_EXPRESSION "the replay guard is off: Recorder::Active ran inside a replay scope" FAIL_REGULAR_EXPRESSION "was called while replaying" ENVIRONMENT "ANYPS5_NO_SHADER_CACHE=1;APS5_NO_REPLAY_GUARD=1" TIMEOUT 60 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    list(APPEND agcReplayGuardTests agc_driver_replay_guard_off)
    if(WIN32)
        set_tests_properties(${agcReplayGuardTests} PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()

#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_IMPORTWATCHRESTORE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_IMPORTWATCHRESTORE_HPP

#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"

struct ImportWatchRestore {
    const AgcDriver::Graphics::Context& context;
    AgcDriver::Graphics::ImportWatch decided;
    ~ImportWatchRestore() { AgcDriver::Graphics::SetImportWatch(context, decided); }
};

#endif

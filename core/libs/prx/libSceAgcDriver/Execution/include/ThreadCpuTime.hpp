#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADCPUTIME_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_THREADCPUTIME_HPP

#include <chrono>

namespace AgcDriver {

std::chrono::nanoseconds ThisThreadCpuTime() noexcept;

}

#endif

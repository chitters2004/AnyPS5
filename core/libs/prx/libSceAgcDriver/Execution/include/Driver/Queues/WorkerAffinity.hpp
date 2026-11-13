#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP

#include <cstdint>

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask();

void PinWorkerThread(const char* role);

void NameDriverThread(const char* name);

std::uint64_t ThreadCpuNanos();

void ReserveCommitterCore();

void ReserveFinisherCore();

}

#endif

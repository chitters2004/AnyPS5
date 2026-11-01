draws: 64; functions with such operations: 2137 in the MinGW objects, 1182 of them found and probed in the Linux build
estimated per draw: emutls 1219.5, rwlock 7.6, mutex 2717.0, fls/tls 44.9, atomic 6769.5

| function | calls per draw | emutls | rwlock | mutex | fls/tls | atomic |
|---|---|---|---|---|---|---|
| `AgcDriver::PerformanceTimer::Finish` | 51.22 | 819.5 | 0.0 | 1946.3 | 0.0 | 2663.4 |
| `std::_Sp_counted_base<` | 36.02 | 0.0 | 0.0 | 0.0 | 0.0 | 2881.2 |
| `AgcDriver::Mutex::unlock` | 14.06 | 0.0 | 0.0 | 281.2 | 0.0 | 140.6 |
| `AgcDriver::PerformanceTimer::PerformanceTimer` | 21.94 | 87.8 | 0.0 | 65.8 | 0.0 | 109.7 |
| `AgcDriver::DriverDetail::PacketTimer::~PacketTimer` | 18.81 | 0.0 | 0.0 | 150.5 | 0.0 | 0.0 |
| `AgcDriver::DriverDetail::Driver::flushBetweenPackets` | 18.81 | 37.6 | 0.0 | 0.0 | 0.0 | 244.6 |
| `AgcDriver::DriverDetail::Driver::CheckFailure` | 19.39 | 0.0 | 0.0 | 58.2 | 0.0 | 0.0 |
| `AgcDriver::DriverDetail::DeviceUseGate::unlock_shared` | 18.81 | 0.0 | 0.0 | 37.6 | 0.0 | 37.6 |
| `AgcDriver::Graphics::` | 38.44 | 38.4 | 0.0 | 0.0 | 0.0 | 0.0 |
| `std::unique_lock<AgcDriver::Mutex>::unlock` | 1.23 | 0.0 | 0.0 | 30.9 | 0.0 | 16.0 |
| `AgcDriver::DriverDetail::deferredLabels` | 25.09 | 0.0 | 0.0 | 0.0 | 25.1 | 0.0 |
| `AgcDriver::DriverDetail::DrawPipeline::Busy` | 20.81 | 20.8 | 0.0 | 0.0 | 0.0 | 0.0 |
| `ShaderRecompiler::` | 1.34 | 0.0 | 0.0 | 16.1 | 0.0 | 16.1 |
| `std::unique_lock<std::mutex>::unlock` | 1.52 | 0.0 | 0.0 | 18.2 | 0.0 | 0.0 |
| `ShaderRecompiler::` | 0.48 | 1.0 | 2.9 | 1.5 | 0.0 | 1.0 |
| `std::mutex::lock` | 1.64 | 0.0 | 0.0 | 16.4 | 0.0 | 0.0 |
| `AgcDriver::Graphics::DrawRejection[abi:cxx11]` | 15.75 | 15.8 | 0.0 | 0.0 | 0.0 | 0.0 |
| `ShaderRecompiler::` | 0.52 | 1.0 | 2.1 | 2.6 | 0.5 | 0.5 |
| `AgcDriver::Graphics::ColorTargetLayout::ColorTargetLayout` | 3.64 | 14.6 | 0.0 | 0.0 | 0.0 | 0.0 |
| `AgcDriver::GuestMemory::GpuMutexType::lock` | 3.17 | 12.7 | 0.0 | 0.0 | 0.0 | 6.3 |
| `ShaderRecompiler::BuildShaderStageInputInfo` | 2.72 | 0.0 | 0.0 | 0.0 | 8.2 | 24.5 |
| `AgcDriver::Graphics::ResidentBuffer::FlushChanged` | 1.14 | 0.0 | 0.0 | 11.4 | 0.0 | 10.3 |
| `AgcDriver::Graphics::` | 2.20 | 0.0 | 0.0 | 11.0 | 0.0 | 4.4 |
| `AgcDriver::DriverDetail::Driver::execute` | 0.41 | 8.5 | 0.0 | 1.6 | 0.0 | 7.7 |
| `AgcDriver::GuestMemory::` | 5.64 | 11.3 | 0.0 | 0.0 | 0.0 | 0.0 |

| thread_local variable | accesses per draw (sites times calls) |
|---|---|
| `AgcDriver::TraceBatchTag` | 822.0 |
| `AgcDriver::PerformanceContext::current` | 94.2 |
| `AgcDriver::Graphics::DeviceProcLookups()::count` | 53.3 |

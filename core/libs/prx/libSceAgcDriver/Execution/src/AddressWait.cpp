#include "prx/libSceAgcDriver/Execution/include/AddressWait.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <synchapi.h>

namespace AgcDriver::AddressWaitDetail {

void WaitOnWord(void* address, const void* old, std::size_t bytes, std::uint32_t milliseconds) noexcept {
    WaitOnAddress(address, const_cast<void*>(old), bytes, milliseconds == Forever ? INFINITE : static_cast<DWORD>(milliseconds));
}

void WakeOneOnWord(void* address) noexcept {
    WakeByAddressSingle(address);
}

void WakeAllOnWord(void* address) noexcept {
    WakeByAddressAll(address);
}

}
#endif

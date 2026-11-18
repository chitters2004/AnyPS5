#include "prx/libSceAgcDriver/Execution/include/AllocationCensus.hpp"

#include <cstddef>

extern "C" {

void* __real_malloc(std::size_t bytes);
void* __real_calloc(std::size_t count, std::size_t bytes);
void* __real_realloc(void* block, std::size_t bytes);

void* __wrap_malloc(std::size_t bytes) {
    void* block = __real_malloc(bytes);
    if (AgcDriver::AllocationCensus::Enabled()) AgcDriver::AllocationCensus::Note(AgcDriver::AllocationCensus::Source::Malloc, __builtin_return_address(0), bytes);
    return block;
}

void* __wrap_calloc(std::size_t count, std::size_t bytes) {
    void* block = __real_calloc(count, bytes);
    if (AgcDriver::AllocationCensus::Enabled()) AgcDriver::AllocationCensus::Note(AgcDriver::AllocationCensus::Source::Calloc, __builtin_return_address(0), count * bytes);
    return block;
}

void* __wrap_realloc(void* block, std::size_t bytes) {
    void* moved = __real_realloc(block, bytes);
    if (AgcDriver::AllocationCensus::Enabled()) AgcDriver::AllocationCensus::Note(AgcDriver::AllocationCensus::Source::Realloc, __builtin_return_address(0), bytes);
    return moved;
}

}

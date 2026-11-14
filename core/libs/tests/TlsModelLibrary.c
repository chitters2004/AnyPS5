#include <stddef.h>
#include <stdint.h>

#define TLS_MODEL_WORDS 32768

static _Thread_local uint64_t block[TLS_MODEL_WORDS];
_Thread_local uint64_t TlsModelLast;

size_t TlsModelBytes(void) {
    return sizeof(block);
}

void TlsModelStore(uint64_t seed) {
    for (size_t i = 0; i < TLS_MODEL_WORDS; ++i) block[i] = seed * 0x9E3779B97F4A7C15ull + i;
    TlsModelLast = seed;
}

int TlsModelCheck(uint64_t seed) {
    if (TlsModelLast != seed) return 0;
    for (size_t i = 0; i < TLS_MODEL_WORDS; ++i) {
        if (block[i] != seed * 0x9E3779B97F4A7C15ull + i) return 0;
    }
    return 1;
}

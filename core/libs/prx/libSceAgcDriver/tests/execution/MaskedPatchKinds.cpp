#include "Recompiler.hpp"
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using ShaderRecompiler::ClassifyMaskedPatch;
using ShaderRecompiler::MaskedPatchKinds;

void Expect(const MaskedPatchKinds& kinds, const MaskedPatchKinds& expected, const std::string& what) {
    if (!(kinds == expected)) throw std::runtime_error("masked patch kinds: " + what + " was classified wrongly");
}

}

int main() {
    try {
        Expect(ClassifyMaskedPatch(0, 4, true, false), {.pushOnly = true}, "a patch of push-data user words only");
        Expect(ClassifyMaskedPatch(0, 4, false, false), {.data = true}, "a patch of user words in shader data");
        Expect(ClassifyMaskedPatch(2, 0, false, false), {.buffers = true}, "a patch of guest buffer descriptors");
        Expect(ClassifyMaskedPatch(0, 0, false, true), {.srt = true}, "a patch of the flattened SRT");
        Expect(ClassifyMaskedPatch(1, 4, true, false), {.buffers = true}, "push-data user words with a buffer rewrite");
        Expect(ClassifyMaskedPatch(0, 4, true, true), {.srt = true}, "push-data user words with a flattened SRT rewrite");
        Expect(ClassifyMaskedPatch(1, 4, false, true), {.data = true, .buffers = true, .srt = true}, "a patch rewriting every kind");
        Expect(ClassifyMaskedPatch(0, 0, true, false), {.empty = true}, "a patch with nothing to rewrite");
        std::puts("masked patch kinds: push-only is counted only when push-data user words are the whole patch");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

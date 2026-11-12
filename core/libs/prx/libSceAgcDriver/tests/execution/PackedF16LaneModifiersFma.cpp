#include "PackedF16LaneModifiers.hpp"

int main() {
    constexpr std::array<Operation, 1> operations{Operation::Fma};
    return RunPackedF16LaneModifiers(operations);
}

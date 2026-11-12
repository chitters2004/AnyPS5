#include "PackedF16LaneModifiers.hpp"

int main() {
    constexpr std::array<Operation, 1> operations{Operation::Pack};
    return RunPackedF16LaneModifiers(operations);
}

#include "PackedF16LaneModifiers.hpp"

int main() {
    constexpr std::array<Operation, 3> operations{Operation::Mul, Operation::Min, Operation::Max};
    return RunPackedF16LaneModifiers(operations);
}

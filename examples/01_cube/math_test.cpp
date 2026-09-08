#include "math.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

using Vec4 = std::array<float, 4>;
Vec4 transform(const Mat4& matrix, Vec4 vector) {
    Vec4 result{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            result[row] += matrix.v[col * 4 + row] * vector[col];
    return result;
}
void near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 0.00001f) throw std::runtime_error(message);
}
int main() {
    try {
        const Mat4 projection = perspective(2.0f, 0.1f, 100.0f);
        auto nearPoint = transform(projection, {0,0,-0.1f,1});
        auto farPoint = transform(projection, {0,0,-100.0f,1});
        near(nearPoint[2] / nearPoint[3], 0.0f, "Near plane must map to depth 0");
        near(farPoint[2] / farPoint[3], 1.0f, "Far plane must map to depth 1");
        auto right = transform(projection, {1,0,-2,1});
        auto up = transform(projection, {0,1,-2,1});
        near(right[0] / right[3] * 2.0f, -up[1] / up[3], "Aspect correction / Vulkan Y flip is wrong");
        auto rotated = transform(rotationY(1.570796327f), {1,0,0,1});
        near(rotated[0], 0, "90 degree rotation X component");
        near(rotated[2], -1, "Right-handed Y rotation direction");
        auto combined = transform(translationZ(-6) * rotationY(1.570796327f), {1,0,0,1});
        near(combined[2], -7, "Model must rotate before view translates");
        auto center = transform(projection * translationZ(-6), {0,0,0,1});
        near(center[0], 0, "Projected center X"); near(center[1], 0, "Projected center Y");
        if (center[3] <= 0 || center[2] <= 0 || center[2] >= center[3])
            throw std::runtime_error("Cube center must be inside the depth clipping range");
        std::cout << "Projection depth, aspect, Y flip, rotation and transform order: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

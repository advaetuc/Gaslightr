#include "PixelContract.h"
#include <iostream>
#include <limits>
#include <vector>

static void Check(bool value) { if (!value) throw std::runtime_error("Contract assertion failed"); }
template<class F> static void Reject(F fn) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected);
}
int main() {
    try {
        using namespace Gaslightr;
        Check(DecodeHalf(0x3c00) == 1.0f);
        Check(DecodeHalf(0xc000) == -2.0f);
        Check(DecodeHalf(0x7bff) == 65504.0f);
        Check(DecodeHalf(0x0001) == std::ldexp(1.0f, -24));
        Check(std::signbit(DecodeHalf(0x8000)));
        Check(std::isinf(DecodeHalf(0x7c00)));
        Check(std::isnan(DecodeHalf(0x7e00)));
        std::vector<float> pixels{ -0.25f, 4.0f, 0.5f, 0.4f };
        ValidateTile(pixels, { 1, 1, 0, false });
        Reject([&] { ValidateTile(pixels, { 0, 1, 0, false }); });
        Reject([&] { ValidateTile(pixels, { 2049, 1, 0, false }); });
        Reject([&] { ValidateTile(pixels, { 2, 1, 0, false }); });
        Reject([&] { ValidateTile(pixels, { 1, 1, 3, false }); });
        Reject([&] { ValidateTile(pixels, { 1, 1, 0, true }); });
        pixels[0] = std::numeric_limits<float>::infinity();
        Reject([&] { ValidateTile(pixels, { 1, 1, 0, false }); });
        pixels[0] = 65505.0f;
        Reject([&] { ValidateTile(pixels, { 1, 1, 0, false }); });
        pixels[0] = 0.0f; pixels[3] = -0.1f;
        Reject([&] { ValidateTile(pixels, { 1, 1, 0, false }); });
        std::cout << "Pixel contract checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

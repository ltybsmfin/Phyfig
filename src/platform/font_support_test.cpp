// Pure CPU tests for the host-side font/placement helpers.
//
// Registered as the CTest `platform_font_support` test by
// src/platform/CMakeLists.txt.
//
// The ImGui sources are compiled into the test target because font_support.hpp
// declares the CPU coverage probe; the tests below never touch a GPU, window or
// Vulkan device and never assert bit-exact rasterization (hinting differs
// between runs).
#include "font_support.hpp"
#include "vulkan_host.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

bool rangesContain(const std::vector<ImWchar>& ranges, uint32_t codepoint) {
    for (std::size_t i = 0; i + 1 < ranges.size() && ranges[i] != 0; i += 2)
        if (ranges[i] <= codepoint && codepoint <= ranges[i + 1]) return true;
    return false;
}
bool listContains(const std::vector<uint32_t>& list, uint32_t codepoint) {
    return std::find(list.begin(), list.end(), codepoint) != list.end();
}
bool sortedUnique(const std::vector<uint32_t>& list) {
    for (std::size_t i = 1; i < list.size(); ++i)
        if (list[i - 1] >= list[i]) return false;
    return true;
}

void testDecodeUtf8() {
    const auto ascii = graphics::font_support::decodeUtf8("A0 ");
    CHECK(ascii == std::vector<uint32_t>({0x41, 0x30, 0x20}));

    const auto two = graphics::font_support::decodeUtf8("\xC3\xA9");       // é
    CHECK(two == std::vector<uint32_t>({0xE9}));
    const auto greek = graphics::font_support::decodeUtf8("\xCE\xB1");     // α
    CHECK(greek == std::vector<uint32_t>({0x3B1}));
    const auto subscript = graphics::font_support::decodeUtf8("\xE2\x82\x81"); // ₁
    CHECK(subscript == std::vector<uint32_t>({0x2081}));
    const auto arrow = graphics::font_support::decodeUtf8("\xE2\x86\x92"); // →
    CHECK(arrow == std::vector<uint32_t>({0x2192}));
    const auto emoji = graphics::font_support::decodeUtf8("\xF0\x9F\x98\x80"); // U+1F600
    CHECK(emoji == std::vector<uint32_t>({0x1F600}));

    CHECK(graphics::font_support::decodeUtf8("\xFF") == std::vector<uint32_t>({0xFFFD}));
    CHECK(graphics::font_support::decodeUtf8("\x80") == std::vector<uint32_t>({0xFFFD}));
    // A truncated lead plus its dangling continuation byte each become one
    // replacement character (Unicode maximal-subpart behaviour).
    CHECK(graphics::font_support::decodeUtf8("\xE2\x82") == std::vector<uint32_t>({0xFFFD, 0xFFFD}));
    CHECK(graphics::font_support::decodeUtf8("\xC0\x80") == std::vector<uint32_t>({0xFFFD}));
    CHECK(graphics::font_support::decodeUtf8("\xED\xA0\x80") == std::vector<uint32_t>({0xFFFD})); // surrogate
    // A bad byte only replaces itself; the rest of the string still decodes.
    // ("\xFF" "B" avoids the greedy hex escape swallowing the following 'B'.)
    CHECK(graphics::font_support::decodeUtf8("A\xFF" "B") == std::vector<uint32_t>({0x41, 0xFFFD, 0x42}));
}

void testRanges() {
    const auto merged = graphics::font_support::rangesFromCodepoints({0x44, 0x41, 0x42, 0x41});
    CHECK(merged.size() == 5);
    CHECK(merged[0] == 0x41 && merged[1] == 0x42);
    CHECK(merged[2] == 0x44 && merged[3] == 0x44);
    CHECK(merged[4] == 0);

    // Non-representable, NUL and surrogate code points are dropped.
    const auto dropped = graphics::font_support::rangesFromCodepoints({0x41, 0x1F600, 0x0, 0xD800});
    CHECK(dropped == std::vector<ImWchar>({0x41, 0x41, 0}));

    const auto empty = graphics::font_support::rangesFromCodepoints({});
    CHECK(empty == std::vector<ImWchar>({0}));

    // mergeRanges joins overlapping and adjacent ranges.
    const auto joined = graphics::font_support::mergeRanges({{0x40, 0x42}, {0x43, 0x45}, {0x50, 0x50}});
    CHECK(joined.size() == 5);
    CHECK(joined[0] == 0x40 && joined[1] == 0x45);
    CHECK(joined[2] == 0x50 && joined[3] == 0x50);
    CHECK(joined[4] == 0);
}

void testInteractiveRanges() {
    const auto ranges = graphics::font_support::buildInteractiveRanges();
    CHECK(rangesContain(ranges, 0x4E00));   // CJK
    CHECK(rangesContain(ranges, 0x9FAF));
    CHECK(rangesContain(ranges, 0x2081));   // ₁ subscript one
    CHECK(rangesContain(ranges, 0x2082));   // ₂
    CHECK(rangesContain(ranges, 0x0370));   // Greek
    CHECK(rangesContain(ranges, 0x03FF));
    CHECK(rangesContain(ranges, 0x2190));   // arrows
    CHECK(rangesContain(ranges, 0x21FF));
    CHECK(rangesContain(ranges, 0x2200));   // mathematical operators
    CHECK(rangesContain(ranges, 0x22FF));
    CHECK(rangesContain(ranges, 0x2100));   // letterlike
    CHECK(rangesContain(ranges, 0x214F));
    CHECK(rangesContain(ranges, 0x00B2));   // ²
    CHECK(rangesContain(ranges, 0x03BC));   // μ
    CHECK(!rangesContain(ranges, 0x30000)); // outside BMP is never emitted
    CHECK(ranges.back() == 0);

    const auto supplements = graphics::font_support::interactiveSupplementCodepoints();
    CHECK(sortedUnique(supplements));
    CHECK(listContains(supplements, 0x2081));
    CHECK(listContains(supplements, 0x2082));
    CHECK(listContains(supplements, 0x03B1));
    CHECK(listContains(supplements, 0x03BC));
    // Required Latin-1 characters join the probe set so a packing drop is caught.
    for (uint32_t codepoint : {0x6Du, 0x76u, 0xB0u, 0xB2u, 0xD7u, 0xF7u})
        CHECK(listContains(supplements, codepoint));

    const auto required = graphics::font_support::interactiveRequiredCodepoints();
    CHECK(sortedUnique(required));
    for (uint32_t codepoint : {0x6Du, 0x76u, 0x3B1u, 0x3BCu, 0x2081u, 0x2082u, 0x2192u})
        CHECK(listContains(required, codepoint));
}

void testTextRanges() {
    const auto codepoints = graphics::font_support::textCodepoints("m\xE2\x82\x81v\xE2\x82\x82"); // m₁v₂
    CHECK(sortedUnique(codepoints));
    CHECK(listContains(codepoints, 0x6D));
    CHECK(listContains(codepoints, 0x76));
    CHECK(listContains(codepoints, 0x2081));
    CHECK(listContains(codepoints, 0x2082));
    CHECK(listContains(codepoints, 0x20));   // ASCII space
    CHECK(listContains(codepoints, 0x7E));   // ASCII tilde
    CHECK(listContains(codepoints, 0x672A)); // 未 from the unknown-formula text
    CHECK(listContains(codepoints, 0x6E90)); // 源
    CHECK(!listContains(codepoints, 0x4E01)); // no CJK sweep

    const auto ranges = graphics::font_support::buildTextRanges("A\xE2\x82\x81");
    CHECK(rangesContain(ranges, 0x41));
    CHECK(rangesContain(ranges, 0x2081));
    CHECK(ranges.back() == 0);
}

void testRasterClamp() {
    std::string warning;
    CHECK(graphics::font_support::clampRasterPixelSize(54.2f, 16384.0f, &warning) == 55.0f);
    CHECK(warning.empty());
    CHECK(graphics::font_support::clampRasterPixelSize(18.0f, 16384.0f, &warning) == 18.0f);
    CHECK(warning.empty());

    warning.clear();
    CHECK(graphics::font_support::clampRasterPixelSize(1000.0f, 16384.0f, &warning) ==
          graphics::font_support::kMaxRasterPixelSize);
    CHECK(!warning.empty()); // never silently returns the base size

    warning.clear();
    CHECK(graphics::font_support::clampRasterPixelSize(1000.0f, 128.0f, &warning) == 128.0f);
    CHECK(!warning.empty());

    warning.clear();
    CHECK(graphics::font_support::clampRasterPixelSize(0.0f, 4096.0f, &warning) ==
          graphics::font_support::kBaseFontPixelSize);
    CHECK(!warning.empty());

    warning.clear();
    CHECK(graphics::font_support::clampRasterPixelSize(-5.0f, 4096.0f, &warning) ==
          graphics::font_support::kBaseFontPixelSize);
    CHECK(!warning.empty());

    warning.clear();
    CHECK(graphics::font_support::clampRasterPixelSize(
              std::numeric_limits<float>::quiet_NaN(), 4096.0f, &warning) ==
          graphics::font_support::kBaseFontPixelSize);
    CHECK(!warning.empty());

    warning.clear();
    CHECK(graphics::font_support::clampRasterPixelSize(1.0f, 4096.0f, &warning) ==
          graphics::font_support::kMinRasterPixelSize);
    CHECK(!warning.empty());
}

void testAtlasWidth() {
    CHECK(graphics::font_support::atlasWidthCeiling(4096) == 4096);
    CHECK(graphics::font_support::atlasWidthCeiling(3000) == 2048);
    CHECK(graphics::font_support::atlasWidthCeiling(1) == 1);
    CHECK(graphics::font_support::atlasWidthCeiling(0) == 1);
    CHECK(graphics::font_support::atlasWidthCeiling(16384) == 16384);

    CHECK(graphics::font_support::chooseAtlasWidth(16384, 0, false) == 2048);
    CHECK(graphics::font_support::chooseAtlasWidth(16384, 1, false) == 4096);
    CHECK(graphics::font_support::chooseAtlasWidth(16384, 2, false) == 8192);
    CHECK(graphics::font_support::chooseAtlasWidth(16384, 9, false) == 16384);
    CHECK(graphics::font_support::chooseAtlasWidth(2048, 0, true) == 2048);
    CHECK(graphics::font_support::chooseAtlasWidth(4096, 0, true) == 4096);
    CHECK(graphics::font_support::chooseAtlasWidth(1024, 0, true) == 1024);
    CHECK(graphics::font_support::chooseAtlasWidth(512, 3, false) == 512);
}

void testWindowPlacement() {
    using graphics::placeWindowInWorkArea;
    using graphics::WorkArea;

    // Fits: centred, size untouched.
    {
        const auto placed = placeWindowInWorkArea(1440, 960, 960, 640, WorkArea{0, 0, 1920, 1040});
        CHECK(placed.width == 1440 && placed.height == 960);
        CHECK(placed.x == 240 && placed.y == 40);
    }
    // Work area smaller than the desired size: the minimum shrinks so the window
    // still fits and stays shrinkable.
    {
        const auto placed = placeWindowInWorkArea(1440, 960, 1100, 780, WorkArea{0, 0, 1280, 720});
        CHECK(placed.width == 1280 && placed.height == 720);
        CHECK(placed.x == 0 && placed.y == 0);
    }
    // Secondary monitor left of the primary: negative work-area origin.
    {
        const auto placed = placeWindowInWorkArea(1440, 960, 960, 640, WorkArea{-1920, 0, 0, 1040});
        CHECK(placed.width == 1440 && placed.height == 960);
        CHECK(placed.x == -1680 && placed.y == 40);
    }
    // Minimum larger than the work area is reduced to the work area.
    {
        const auto placed = placeWindowInWorkArea(1440, 960, 960, 640, WorkArea{0, 0, 800, 600});
        CHECK(placed.width == 800 && placed.height == 600);
        CHECK(placed.x == 0 && placed.y == 0);
    }
    // Desired size below the minimum is raised to the minimum.
    {
        const auto placed = placeWindowInWorkArea(640, 480, 960, 640, WorkArea{0, 0, 1920, 1040});
        CHECK(placed.width == 960 && placed.height == 640);
        CHECK(placed.x == 480 && placed.y == 200);
    }
    // Degenerate work area still yields an inside, positive rect.
    {
        const auto placed = placeWindowInWorkArea(1440, 960, 960, 640, WorkArea{0, 0, 320, 240});
        CHECK(placed.width == 320 && placed.height == 240);
        CHECK(placed.x == 0 && placed.y == 0);
    }
    // A 1x1 work area must not divide by zero or go negative.
    {
        const auto placed = placeWindowInWorkArea(1440, 960, 960, 640, WorkArea{5, 7, 6, 8});
        CHECK(placed.width == 1 && placed.height == 1);
        CHECK(placed.x == 5 && placed.y == 7);
    }
}

} // namespace

int main() {
    testDecodeUtf8();
    testRanges();
    testInteractiveRanges();
    testTextRanges();
    testRasterClamp();
    testAtlasWidth();
    testWindowPlacement();
    if (failures == 0) {
        std::cerr << "font_support_test: all checks passed\n";
        return 0;
    }
    std::cerr << "font_support_test: " << failures << " check(s) failed\n";
    return 1;
}

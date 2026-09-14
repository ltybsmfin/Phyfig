#pragma once
// CPU-only font helpers shared by the Vulkan host and the editor's export path.
//
// Deliberately free of Vulkan, window and GPU state: everything here runs on the
// CPU and can be unit-tested without a device. The host uses it to
//   * assemble the interactive (full CJK + math supplements) glyph ranges,
//   * assemble the on-demand offscreen glyph ranges from the characters the
//     export actually draws (ASCII + unknown-formula text included),
//   * probe which characters the system fonts really contain, so a character the
//     source font lacks can be told apart from one that merely did not fit into
//     the atlas, and fallback fonts can be merged for the missing characters only,
//   * clamp the requested export raster size and the atlas width to the device
//     limit.
//
// The editor can call the same helpers to warn about characters no system font
// can draw before an export is started.
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace graphics {
namespace font_support {

// Interactive raster size. The interactive atlas keeps the full CJK set and is
// never enlarged: ImGui silently drops glyphs when packing overflows, and a
// full-CJK atlas at a large size is exactly the case that overflows.
inline constexpr float kBaseFontPixelSize = 18.0f;
// Documented bounds for the offscreen raster size. A request above the cap is
// clamped with a warning and never silently falls back to the base size.
inline constexpr float kMinRasterPixelSize = 8.0f;
inline constexpr float kMaxRasterPixelSize = 256.0f;

struct CodepointRange {
    uint32_t first;
    uint32_t last;
};

// Supplement ranges added on top of the CJK atlas. U+2081/U+2082 (subscript 1/2)
// are required for labels such as m₁ v₂ and are absent from Microsoft YaHei, so
// the host merges a fallback font for them (see planCoverage()).
inline const std::vector<CodepointRange>& supplementRanges() {
    static const std::vector<CodepointRange> ranges = {
        {0x0370, 0x03FF}, // Greek and Coptic: α μ θ Δ
        {0x2070, 0x209F}, // Superscripts and Subscripts: ₁ ₂
        {0x2100, 0x214F}, // Letterlike Symbols
        {0x2190, 0x21FF}, // Arrows: →
        {0x2200, 0x22FF}, // Mathematical Operators
    };
    return ranges;
}

// The editor draws this text in place of a formula resource it cannot resolve,
// so the offscreen atlas always has to contain it.
inline const char* unknownFormulaText() { return "未知公式资源"; }

// Decodes UTF-8, emitting U+FFFD for malformed, truncated, overlong or surrogate
// sequences instead of throwing. Bytes above U+10FFFF are rejected as well.
inline std::vector<uint32_t> decodeUtf8(std::string_view text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t size = text.size();
    std::size_t i = 0;
    while (i < size) {
        const unsigned char lead = bytes[i];
        uint32_t codepoint = 0;
        std::size_t extra = 0;
        if (lead < 0x80) {
            codepoint = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            codepoint = lead & 0x1Fu; extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            codepoint = lead & 0x0Fu; extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            codepoint = lead & 0x07u; extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= size) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool valid = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char next = bytes[i + k];
            if ((next & 0xC0) != 0x80) { valid = false; break; }
            codepoint = (codepoint << 6) | (next & 0x3Fu);
        }
        if (!valid) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        static const uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
        const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
        if (codepoint < minimum[extra] || codepoint > 0x10FFFF || surrogate) {
            out.push_back(0xFFFD);
            i += extra + 1;
            continue;
        }
        out.push_back(codepoint);
        i += extra + 1;
    }
    return out;
}

// Turns codepoints into a sorted, merged, zero-terminated ImWchar range array.
// Codepoints that ImWchar (16-bit by default) cannot represent, U+0000 and
// surrogate halves are dropped.
inline std::vector<ImWchar> rangesFromCodepoints(std::vector<uint32_t> codepoints) {
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    std::vector<ImWchar> ranges;
    std::size_t i = 0;
    while (i < codepoints.size()) {
        const uint32_t first = codepoints[i];
        if (first == 0 || first > 0xFFFF || (first >= 0xD800 && first <= 0xDFFF)) { ++i; continue; }
        uint32_t last = first;
        std::size_t j = i + 1;
        while (j < codepoints.size()) {
            const uint32_t next = codepoints[j];
            if (next > 0xFFFF || next != last + 1 || (next >= 0xD800 && next <= 0xDFFF)) break;
            last = next;
            ++j;
        }
        ranges.push_back(static_cast<ImWchar>(first));
        ranges.push_back(static_cast<ImWchar>(last));
        i = j;
    }
    ranges.push_back(0);
    return ranges;
}

// Merges pre-built ranges (sorted or not, possibly overlapping) into one
// zero-terminated array.
inline std::vector<ImWchar> mergeRanges(std::vector<CodepointRange> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const CodepointRange& a, const CodepointRange& b) {
        return a.first < b.first;
    });
    std::vector<ImWchar> out;
    bool open = false;
    uint32_t first = 0, last = 0;
    for (const auto& range : ranges) {
        // Plain comparisons rather than std::min/std::max: this header may be
        // included after windows.h without NOMINMAX.
        const uint32_t a = range.first < 0xFFFFu ? range.first : 0xFFFFu;
        const uint32_t b = range.last < 0xFFFFu ? range.last : 0xFFFFu;
        if (a > b || a == 0) continue;
        if (!open || a > last + 1) {
            if (open) { out.push_back(static_cast<ImWchar>(first)); out.push_back(static_cast<ImWchar>(last)); }
            first = a; last = b; open = true;
        } else if (b > last) {
            last = b;
        }
    }
    if (open) { out.push_back(static_cast<ImWchar>(first)); out.push_back(static_cast<ImWchar>(last)); }
    out.push_back(0);
    return out;
}

// Interactive atlas: the exact range set ImFontAtlas::GetGlyphRangesChineseFull()
// exposes, plus the math supplements and the handful of extras the editor draws.
// Written out explicitly so the set is unit-testable and independent of ImGui's
// internal static array.
inline std::vector<ImWchar> buildInteractiveRanges() {
    std::vector<CodepointRange> ranges = {
        {0x0020, 0x00FF}, // Basic Latin + Latin Supplement (² µ)
        {0x2000, 0x206F}, // General Punctuation
        {0x3000, 0x30FF}, // CJK Symbols/Punctuation, Hiragana, Katakana
        {0x31F0, 0x31FF}, // Katakana Phonetic Extensions
        {0xFF00, 0xFFEF}, // Half-width forms
        {0xFFFD, 0xFFFD}, // Replacement character
        {0x4E00, 0x9FAF}, // CJK Unified Ideographs
    };
    for (const auto& range : supplementRanges()) ranges.push_back(range);
    return mergeRanges(std::move(ranges));
}

// Characters the editor UI and the shipped formulas actually need. A supplement
// range contains unassigned code points (e.g. U+0378) that no font can ever
// provide; warning about those would be noise. This list is what the host warns
// about when no system font can draw it.
inline std::vector<uint32_t> interactiveRequiredCodepoints() {
    static const uint32_t required[] = {
        0x006D, 0x0076, 0x00B0, 0x00B2, 0x00D7, 0x00F7, // m v ° ² × ÷
        0x0394, 0x03B1, 0x03B8, 0x03BC,                 // Δ α θ μ
        0x2081, 0x2082,                                 // ₁ ₂
        0x2192,                                         // →
        0x2202, 0x2207, 0x2211, 0x221A, 0x221E, 0x222B, // ∂ ∇ ∑ √ ∞ ∫
        0x2248, 0x2260, 0x2264, 0x2265,                 // ≈ ≠ ≤ ≥
    };
    std::vector<uint32_t> codepoints(std::begin(required), std::end(required));
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

// The codepoints whose real coverage the host probes and verifies on every
// interactive build: the math supplements plus the explicitly required
// characters (including the Latin-1 × ÷ ° ² that the CJK base range covers but
// a packing overflow could still drop). The ~21k CJK ideographs are intentionally
// not probed again — that would rebuild the whole atlas once more at startup for
// no actionable fallback.
inline std::vector<uint32_t> interactiveSupplementCodepoints() {
    std::vector<uint32_t> codepoints;
    for (const auto& range : supplementRanges())
        for (uint32_t c = range.first; c <= range.last; ++c) codepoints.push_back(c);
    const auto required = interactiveRequiredCodepoints();
    codepoints.insert(codepoints.end(), required.begin(), required.end());
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

// On-demand offscreen set: ASCII, the decoded export text, the unknown-formula
// text and a small punctuation/symbol safety net. No CJK sweep, no full atlas.
inline std::vector<uint32_t> textCodepoints(std::string_view text) {
    std::vector<uint32_t> codepoints;
    for (uint32_t c = 0x20; c <= 0x7E; ++c) codepoints.push_back(c);
    const auto body = decodeUtf8(text);
    codepoints.insert(codepoints.end(), body.begin(), body.end());
    const auto unknown = decodeUtf8(unknownFormulaText());
    codepoints.insert(codepoints.end(), unknown.begin(), unknown.end());
    static const uint32_t extras[] = {
        0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B5, 0x00D7, 0x00F7, 0x2013, 0x2014,
        0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x2030, 0x2190, 0x2191, 0x2192,
        0x2193, 0x2212, 0x221E, 0x2248, 0x2264, 0x2265,
    };
    for (uint32_t c : extras) codepoints.push_back(c);
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

inline std::vector<ImWchar> buildTextRanges(std::string_view text) {
    return rangesFromCodepoints(textCodepoints(text));
}

// Rounds the requested export raster size up and clamps it into
// [kMinRasterPixelSize, min(kMaxRasterPixelSize, deviceMax)]. Every clamp is
// reported through `warning`; the caller must log it. Invalid input falls back
// to the base size with a warning instead of silently staying at 18 px.
inline float clampRasterPixelSize(float requested, float deviceMax, std::string* warning) {
    auto warn = [&](std::string message) { if (warning) *warning = std::move(message); };
    float target = requested;
    if (!std::isfinite(target) || target <= 0.0f) {
        warn("exportFontPixelSize is not a positive finite number; using " +
             std::to_string(static_cast<int>(kBaseFontPixelSize)) + " px");
        target = kBaseFontPixelSize;
    }
    target = std::ceil(target);
    const float device = (std::isfinite(deviceMax) && deviceMax > 0.0f) ? deviceMax : kMaxRasterPixelSize;
    const float cap = kMaxRasterPixelSize < device ? kMaxRasterPixelSize : device;
    if (target > cap) {
        warn("requested font raster " + std::to_string(static_cast<int>(requested)) +
             " px exceeds the cap " + std::to_string(static_cast<int>(cap)) + " px; using the cap");
        target = cap;
    }
    if (target < kMinRasterPixelSize && kMinRasterPixelSize <= cap) {
        warn("requested font raster is below the " + std::to_string(static_cast<int>(kMinRasterPixelSize)) +
             " px floor; using the floor");
        target = kMinRasterPixelSize;
    }
    return target;
}

struct FontSource {
    std::string label;
    std::filesystem::path path;
    int fontNo = 0;
    // True for Microsoft YaHei, the only source that can draw the Chinese UI.
    // The host refuses to start without it; the other sources are optional
    // fallbacks for characters YaHei lacks.
    bool primary = false;
    std::vector<unsigned char> bytes;
};

// Reads an environment variable without tripping MSVC's getenv deprecation
// warning (the host is compiled at /W4).
inline std::string environmentVariable(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value) {
        std::string result(value);
        std::free(value);
        return result;
    }
    return {};
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

// %WINDIR%\Fonts — the OS-provided system font directory. No machine-private
// path is ever hardcoded.
inline std::filesystem::path windowsFontsDirectory() {
    std::string root = environmentVariable("WINDIR");
    if (root.empty()) root = environmentVariable("SystemRoot");
    if (root.empty()) throw std::runtime_error("Cannot locate the Windows font directory (WINDIR is unset)");
    return std::filesystem::path(root) / "Fonts";
}

// Loads Microsoft YaHei (primary) and the Windows symbol fallbacks. The primary
// must exist for Chinese labels; the fallbacks are optional and only used for
// characters the primary font lacks.
inline std::vector<FontSource> loadSystemFontCandidates() {
    const auto directory = windowsFontsDirectory();
    const std::pair<const char*, const char*> candidates[] = {
        {"msyh.ttc", "primary"},
        {"msyh.ttf", "primary"},
        {"seguisym.ttf", "fallback"},
        {"segoeui.ttf", "fallback"},
    };
    std::vector<FontSource> sources;
    bool havePrimary = false;
    for (const auto& candidate : candidates) {
        const bool primary = std::strcmp(candidate.second, "primary") == 0;
        // msyh.ttc and msyh.ttf are alternatives; keep the first primary only.
        if (primary && havePrimary) continue;
        const auto path = directory / candidate.first;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error) continue;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) continue;
        const auto length = static_cast<std::size_t>(file.tellg());
        if (length < 100) continue;
        FontSource source;
        source.label = candidate.first;
        source.path = path;
        source.primary = primary;
        source.bytes.resize(length);
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(source.bytes.data()), static_cast<std::streamsize>(length))) continue;
        havePrimary = havePrimary || primary;
        sources.push_back(std::move(source));
    }
    return sources;
}

namespace detail {
// Builds a throwaway CPU-only atlas from the given sources and reports which of
// the requested codepoints actually rasterized. Never touches the GPU.
inline std::vector<uint32_t> probeCoverage(const std::vector<const FontSource*>& sources,
                                           const std::vector<uint32_t>& codepoints, float basePx) {
    std::vector<uint32_t> covered;
    if (codepoints.empty()) return covered;
    const std::vector<ImWchar> ranges = rangesFromCodepoints(codepoints);
    ImFontAtlas atlas;
    atlas.Flags |= ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
    atlas.TexDesiredWidth = 2048;
    bool first = true;
    for (const FontSource* source : sources) {
        if (!source || source->bytes.size() < 100) continue;
        ImFontConfig config;
        config.MergeMode = !first;
        config.FontNo = source->fontNo;
        config.FontDataOwnedByAtlas = false; // probe must not free the loaded bytes
        if (!atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(source->bytes.data()),
                static_cast<int>(source->bytes.size()), basePx, &config, ranges.data()))
            continue;
        first = false;
    }
    if (first || !atlas.Build() || atlas.Fonts.Size == 0) return covered;
    ImFont* font = atlas.Fonts[0];
    for (uint32_t codepoint : codepoints) {
        if (codepoint > 0xFFFF) continue;
        if (font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint))) covered.push_back(codepoint);
    }
    return covered;
}
} // namespace detail

inline std::vector<uint32_t> probeCoverage(const FontSource& source,
                                           const std::vector<uint32_t>& codepoints, float basePx) {
    return detail::probeCoverage({&source}, codepoints, basePx);
}

inline std::vector<uint32_t> probeCoverage(const std::vector<FontSource>& sources,
                                           const std::vector<uint32_t>& codepoints, float basePx) {
    std::vector<const FontSource*> pointers;
    pointers.reserve(sources.size());
    for (const auto& source : sources) pointers.push_back(&source);
    return detail::probeCoverage(pointers, codepoints, basePx);
}

struct CoveragePlan {
    std::vector<uint32_t> covered;  // codepoints at least one source contains
    std::vector<uint32_t> missing;  // codepoints no source contains
    // candidate index -> codepoints the primary font lacks but this source has
    std::vector<std::pair<std::size_t, std::vector<uint32_t>>> fallbacks;
};

// Probes the candidates in order. The first source is the primary font; every
// later source is only merged for the characters still missing at that point.
inline CoveragePlan planCoverage(const std::vector<FontSource>& sources,
                                 const std::vector<uint32_t>& needed, float basePx) {
    CoveragePlan plan;
    std::vector<uint32_t> remaining = needed;
    for (std::size_t i = 0; i < sources.size() && !remaining.empty(); ++i) {
        const auto covered = probeCoverage(sources[i], remaining, basePx);
        if (covered.empty()) continue;
        plan.covered.insert(plan.covered.end(), covered.begin(), covered.end());
        if (i > 0) plan.fallbacks.emplace_back(i, covered);
        std::vector<uint32_t> rest;
        rest.reserve(remaining.size());
        for (uint32_t codepoint : remaining)
            if (!std::binary_search(covered.begin(), covered.end(), codepoint)) rest.push_back(codepoint);
        remaining.swap(rest);
    }
    plan.missing = std::move(remaining);
    std::sort(plan.covered.begin(), plan.covered.end());
    plan.covered.erase(std::unique(plan.covered.begin(), plan.covered.end()), plan.covered.end());
    return plan;
}

// Editor-facing helper: returns the requested characters that no installed font
// can draw (sorted, unique), so the editor can warn before an export starts. It
// loads the system fonts, so callers should cache the result. The host uses the
// same probe internally; this is only a convenience wrapper.
inline std::vector<uint32_t> missingGlyphsForText(std::string_view text, float basePx = kBaseFontPixelSize) {
    const auto candidates = loadSystemFontCandidates();
    const auto needed = textCodepoints(text);
    if (candidates.empty()) return needed;
    return planCoverage(candidates, needed, basePx).missing;
}

// Largest power of two that does not exceed the device limit.
inline int atlasWidthCeiling(int maxDimension) {    if (maxDimension < 1) return 1;
    int width = 1;
    while (width <= maxDimension / 2) width *= 2;
    return width;
}

// Atlas width for attempt `attempt`: a power of two, at most the device ceiling,
// widened geometrically on every retry. Full-CJK builds start wider than the
// on-demand offscreen builds.
inline int chooseAtlasWidth(int maxDimension, int attempt, bool fullCjk) {
    const int ceiling = atlasWidthCeiling(maxDimension);
    int width = fullCjk ? 4096 : 2048;
    if (width > ceiling) width = ceiling;
    for (int i = 0; i < attempt && width < ceiling; ++i) width *= 2;
    return width < ceiling ? width : ceiling;
}

} // namespace font_support
} // namespace graphics

#pragma once
#include <functional>
#include <string>

namespace graphics {
// Physical-pixel geometry shared by the host and its CPU-only tests.
struct WorkArea {
    int left = 0, top = 0, right = 0, bottom = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
};
struct WindowRect {
    int x = 0, y = 0, width = 0, height = 0;
};
// Clamps the desired outer window size into the monitor work area, shrinking the
// requested minimum when the work area itself is smaller, then centres the window
// and keeps it fully inside. Pure function so the boundary cases are unit-tested;
// createWindow() feeds it the monitor nearest the pointer. All values are
// physical pixels: no DPI font scaling is applied to the window or the canvas.
// Uses local lambdas rather than std::min/std::max so the header stays valid in
// translation units that pulled in windows.h without NOMINMAX.
inline WindowRect placeWindowInWorkArea(int desiredWidth, int desiredHeight,
    int minWidth, int minHeight, const WorkArea& area) {
    const auto atLeast = [](int value, int floorValue) { return value < floorValue ? floorValue : value; };
    const auto atMost = [](int value, int ceilingValue) { return value > ceilingValue ? ceilingValue : value; };
    const auto clampTo = [&](int value, int low, int high) { return atLeast(atMost(value, high), low); };
    const int availableWidth = atLeast(area.width(), 1);
    const int availableHeight = atLeast(area.height(), 1);
    const int minimumWidth = atMost(atLeast(minWidth, 1), availableWidth);
    const int minimumHeight = atMost(atLeast(minHeight, 1), availableHeight);
    const int width = clampTo(desiredWidth, minimumWidth, availableWidth);
    const int height = clampTo(desiredHeight, minimumHeight, availableHeight);
    int x = area.left + (availableWidth - width) / 2;
    int y = area.top + (availableHeight - height) / 2;
    x = clampTo(x, area.left, area.right - width);
    y = clampTo(y, area.top, area.bottom - height);
    return {x, y, width, height};
}
struct View {
    std::function<void(float)> draw;
    std::function<void()> poll;
    std::function<void(unsigned)> smokeInput;
    std::function<void(unsigned)> smokeAfterFrame;
    std::function<void(unsigned,unsigned)> captureFrame;
    std::function<bool()> closeRequested;
    std::function<bool()> shouldClose;
    std::function<bool()> takeShowRequest;
    std::function<void(bool,bool,unsigned)> windowState;
    std::function<void()> presented;
};
struct Options {
    bool smoke=false, validation=false, capture=false;
    bool escapeCloses=false;
    std::wstring title=L"Vulkan graphics";
    // Offscreen PNG sequence export for the standalone render process.
    // exportFrames > 0 switches run() into offscreen mode: the window stays
    // hidden and is only used to pick a Vulkan device/surface, the render target
    // and readback are fixed at exportWidth x exportHeight with scale 1, no
    // swapchain is presented, and the host writes frame_000000.png ... plus
    // result.json into exportDirectory. The parent process owns ffmpeg and
    // cancellation, so the host only reports success through its exit code.
    unsigned exportWidth=0, exportHeight=0, exportFrames=0;
    std::string exportDirectory;
    // Offscreen glyph set. When non-empty the export atlas bakes only the UTF-8
    // characters in this string (plus ASCII and the editor's unknown-formula
    // text) instead of the full CJK atlas, which is what lets a large export
    // raster size stay inside the device texture limit. When empty the host keeps
    // the legacy full-CJK atlas and logs a warning.
    std::string exportGlyphText;
    // Target raster size of the exported text in physical pixels. It is rounded
    // up and clamped to a documented cap; the logical font size used by the draw
    // calls is never changed (the glyphs are rasterized through
    // ImFontConfig::RasterizerDensity), and the host never silently falls back
    // to 18 px without a warning.
    float exportFontPixelSize=18.0f;
};
int run(View view,Options options);
}

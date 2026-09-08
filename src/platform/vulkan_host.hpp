#pragma once
#include <functional>
#include <string>

namespace graphics {
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
};
int run(View view,Options options);
}

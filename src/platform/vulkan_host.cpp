#include <windows.h>
#include <vulkan/vulkan.h>
#include "vulkan_host.hpp"
#include "font_support.hpp"
#include "formula.hpp"
#include "offscreen_export.hpp"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic_uint validationErrors{0}, validationWarnings{0};
// Initial and minimum outer window sizes in physical pixels. The minimum is
// lowered further when the monitor work area itself is smaller, so the window
// always stays shrinkable on small desktops.
constexpr int kInitialWindowWidth = 1440, kInitialWindowHeight = 960;
constexpr int kMinWindowWidth = 960, kMinWindowHeight = 640;
// Bounded atlas retries: widen first, then shrink the offscreen raster, and
// finally fail with an explanatory error instead of silently dropping glyphs.
constexpr int kMaxAtlasAttempts = 8;
constexpr int kMaxRasterShrinks = 3;
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) + ")");
}
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++validationWarnings;
    std::cerr << "[Vulkan] " << data->pMessage << '\n';
    return VK_FALSE;
}
class VulkanHost {
public:
    explicit VulkanHost(graphics::View view, graphics::Options options)
        : offscreen_(options.exportFrames > 0), smoke_(options.smoke), requireValidation_(options.validation),
          capture_(options.capture), view_(std::move(view)), options_(std::move(options)) {}
    VulkanHost(const VulkanHost&) = delete;
    VulkanHost& operator=(const VulkanHost&) = delete;
    ~VulkanHost() { cleanup(); }

    void run() {
        createWindow();
        createInstance();
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = window_;
        check(vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_), "create Win32 surface");
        createDevice();
        createCommands();
        createGui();
        // Offscreen export is a batch job: no swapchain, no window messages, no
        // presentation. The interactive path below is unchanged.
        if (offscreen_) { runOffscreen(); return; }
        recreateSwapchain();
        auto previous = std::chrono::steady_clock::now();
        const auto start = previous;
        unsigned frame = 0;
        while (running_) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) running_ = false;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (!running_) break;
            if(view_.windowState) view_.windowState(IsWindowVisible(window_)!=FALSE,IsIconic(window_)!=FALSE,frame);
            if(view_.poll) view_.poll();
            if(view_.takeShowRequest && view_.takeShowRequest()) {
                ShowWindow(window_,IsIconic(window_)?SW_RESTORE:SW_SHOWNOACTIVATE);
                BringWindowToTop(window_);
            }
            if(view_.shouldClose && view_.shouldClose()) break;
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(), 0.1f);
            previous = now;
            if (smoke_ && now - start > std::chrono::seconds(30))
                throw std::runtime_error("Smoke test timed out");
            if (IsIconic(window_) || clientSize().width == 0 || clientSize().height == 0) {
                if (smoke_) {
                    ++minimizedSkips_;
                    ShowWindow(window_, SW_RESTORE);
                } else MsgWaitForMultipleObjects(0,nullptr,FALSE,20,QS_ALLINPUT); // Keep local automation responsive while minimized.
                continue;
            }
            if (resized_) recreateSwapchain();
            if (!guiVulkan_) continue;
            if(math_text::pending) rebuildFormulaAtlas();
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplWin32_NewFrame();
            if(smoke_ && view_.smokeInput) {
                // The scripted input owns smoke-test frames. Discard backend
                // cursor events so the real desktop pointer cannot displace a
                // synthetic press/release or delay it in ImGui's event queue.
                ImGui::GetIO().ClearEventsQueue();
                ImGui::GetIO().ConfigInputTrickleEventQueue = false;
                view_.smokeInput(frame);
            }
            ImGui::NewFrame();
            if(capture_ && view_.captureFrame) view_.captureFrame(frame,90);
            view_.draw(dt);
            ImGui::Render();
            if (!drawFrame()) continue;
            ++frame;
            if(view_.presented) view_.presented();
            if(view_.windowState) view_.windowState(IsWindowVisible(window_)!=FALSE,IsIconic(window_)!=FALSE,frame);
            if(capture_ && frame>=90) running_=false;
            if (smoke_) {
                if(view_.smokeAfterFrame) view_.smokeAfterFrame(frame);
                if (frame == 30) SetWindowPos(window_, nullptr, 0, 0, 1140, 820, SWP_NOMOVE | SWP_NOZORDER);
                if (frame == 50) SetWindowPos(window_, nullptr, 0, 0, 1480, 960, SWP_NOMOVE | SWP_NOZORDER);
                if (frame == 70) ShowWindow(window_, SW_MINIMIZE);
                if (frame == 150) running_ = false;
            }
        }
        check(vkDeviceWaitIdle(device_), "wait for shutdown");
        if (smoke_ && (frame < 120 || rebuilds_ < 3 || minimizedSkips_ == 0))
            throw std::runtime_error("Smoke test did not cover the required window lifecycle");
        std::cerr << "Rendered frames: " << frame << "; swapchain builds: " << rebuilds_
                  << "; minimized skips: " << minimizedSkips_ << '\n';
    }

private:
    HWND window_{};
    bool offscreen_ = false;
    bool running_ = true, resized_ = false, smoke_ = false, requireValidation_ = false, capture_ = false;
    graphics::View view_;
    graphics::Options options_;
    bool guiWin32_ = false, guiVulkan_ = false;
    unsigned rebuilds_ = 0, minimizedSkips_ = 0;
    VkInstance instance_{};
    VkDebugUtilsMessengerEXT messenger_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    uint32_t queueFamily_ = 0;
    VkQueue queue_{};
    VkSwapchainKHR swapchain_{};
    VkFormat colorFormat_{};
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    // Presentation waits are tied to acquired image indices, not the submit fence.
    std::vector<VkSemaphore> renderFinished_;
    VkCommandPool commandPool_{};
    VkCommandBuffer command_{};
    VkSemaphore imageAvailable_{};
    VkFence inFlight_{};
    VkFence presentationDone_{};
    bool presentationPending_ = false;
    // Image count handed to the ImGui Vulkan backend. The interactive path uses
    // the swapchain image count; offscreen mode has no swapchain and uses 2.
    uint32_t imageCount_ = 2;
    graphics::OffscreenExporter exporter_;
    // Window minimum, derived from the chosen monitor work area in createWindow().
    int minTrackWidth_ = kMinWindowWidth, minTrackHeight_ = kMinWindowHeight;
    // --- font atlas state -------------------------------------------------
    // Candidates in fallback order: Microsoft YaHei first, then the Windows
    // symbol fonts. atlasPlan_ records which candidate supplies which codepoint.
    std::vector<graphics::font_support::FontSource> fontCandidates_;
    graphics::font_support::CoveragePlan atlasPlan_;
    std::vector<ImWchar> atlasPrimaryRanges_;
    // Glyph range arrays are only referenced by ImFontConfig::GlyphRanges, so
    // they must outlive every rebuild (the atlas is rebuilt whenever a runtime
    // formula arrives). std::vector moves keep the heap buffer stable.
    std::vector<std::vector<ImWchar>> atlasRangeStorage_;
    std::vector<uint32_t> atlasVerifyCodepoints_;   // probed and verified on every build
    std::vector<uint32_t> atlasRequiredCodepoints_; // warned about when no font has them
    std::vector<uint32_t> atlasCoveredCodepoints_;  // sorted; present in a source
    std::vector<uint32_t> atlasSourceMissing_;      // absent from every source
    bool offscreenAtlas_ = false;
    float atlasBasePx_ = graphics::font_support::kBaseFontPixelSize;
    float atlasRasterPx_ = graphics::font_support::kBaseFontPixelSize;
    int maxImageDimension2D_ = 4096;
    // Offscreen mode picks its own colour format instead of the surface's
    // preferred one: UNORM first so no sRGB transfer function touches the stored
    // bytes, and the ImGui pipeline is built for exactly this format.
    void chooseOffscreenFormat() {
        const VkFormat candidates[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        for (auto candidate : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_, candidate, &properties);
            if ((properties.optimalTilingFeatures & needed) != needed) continue;
            colorFormat_ = candidate;
            return;
        }
        throw std::runtime_error("No 8-bit RGBA/BGRA colour format supports offscreen rendering");
    }
    void runOffscreen() {
        if (options_.smoke || options_.capture)
            throw std::runtime_error("Offscreen export cannot be combined with smoke or capture mode");
        if (!options_.exportWidth || !options_.exportHeight)
            throw std::runtime_error("Offscreen export needs a non-zero width and height");
        if (options_.exportDirectory.empty())
            throw std::runtime_error("Offscreen export needs an output directory");
        chooseOffscreenFormat();
        graphics::OffscreenExportConfig config;
        config.width = options_.exportWidth;
        config.height = options_.exportHeight;
        config.frames = options_.exportFrames;
        config.directory = std::filesystem::u8path(options_.exportDirectory);
        exporter_.create(physical_, device_, colorFormat_, config);
        imageCount_ = 2;
        createGuiRenderer();
        std::cerr << "Offscreen export: " << config.frames << " frames " << config.width << 'x' << config.height
                  << " -> " << options_.exportDirectory << '\n';
        for (unsigned frame = 0; frame < config.frames; ++frame) {
            // Same dynamic formula-atlas path as the interactive loop, so a
            // setCustom() issued after startup still lands before this frame.
            if (math_text::pending) rebuildFormulaAtlas();
            ImGui_ImplVulkan_NewFrame();
            // Fixed canvas, set after the backend NewFrame so the hidden window's
            // client size can never leak into the export geometry.
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = {static_cast<float>(config.width), static_cast<float>(config.height)};
            io.DisplayFramebufferScale = {1.0f, 1.0f};
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            // Required hook order: after NewFrame, before draw().
            if (view_.captureFrame) view_.captureFrame(frame, config.frames);
            view_.draw(0.0f);
            ImGui::Render();
            recordOffscreenFrame();
            submitOffscreenFrame();
            exporter_.writeFrame(queue_, frame);
        }
        if (validationErrors)
            throw std::runtime_error("Vulkan validation reported errors during the offscreen export");
        exporter_.writeResult();
        std::cerr << "Offscreen export complete: " << config.frames << " frames\n";
    }
    void recordOffscreenFrame() {
        check(vkResetCommandBuffer(command_, 0), "reset offscreen command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "begin offscreen commands");
        VkImageMemoryBarrier toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        // UNDEFINED is correct here: the attachment is cleared every frame, so the
        // previous contents are discarded and no read-after-write hazard exists.
        toColor.srcAccessMask = 0;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = exporter_.image();
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toColor);
        const VkExtent2D extent = exporter_.extent();
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = exporter_.imageView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.018f, 0.025f, 0.045f, 1.0f}};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(command_, &rendering);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_);
        vkCmdEndRendering(command_);
        exporter_.recordReadback(command_);
        check(vkEndCommandBuffer(command_), "end offscreen commands");
    }
    void submitOffscreenFrame() {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_;
        check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "submit offscreen frame");
        // writeFrame() waits for this submission before reading the mapped buffer.
    }
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<VulkanHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<VulkanHost*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return 1;
        if (message == WM_GETMINMAXINFO) {
            auto* bounds = reinterpret_cast<MINMAXINFO*>(lParam);
            bounds->ptMinTrackSize = {app ? app->minTrackWidth_ : kMinWindowWidth,
                                      app ? app->minTrackHeight_ : kMinWindowHeight};
            return 0;
        }
        if (app) {
            if (message == WM_SIZE) { app->resized_ = true; return 0; }
            if (message == WM_KEYDOWN && wParam == VK_ESCAPE && app->options_.escapeCloses) { app->running_=false; return 0; }
            
            if (message == WM_CLOSE) {
                if(!app->view_.closeRequested || app->view_.closeRequested()) app->running_ = false;
                return 0;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    void createWindow() {
        ImGui_ImplWin32_EnableDpiAwareness();
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VulkanKinematicsLesson";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) throw std::runtime_error("Cannot register window class");
        // Physical-pixel placement on the monitor nearest the pointer. The
        // desired size is clamped to that monitor's work area and centred there;
        // the minimum is lowered when the work area is smaller so the window
        // stays shrinkable. No DPI font scaling is applied anywhere in the host.
        POINT cursor{};
        if (!GetCursorPos(&cursor)) cursor = POINT{0, 0};
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!monitor || !GetMonitorInfoW(monitor, &info))
            throw std::runtime_error("Cannot read the monitor work area");
        const graphics::WorkArea work{info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
        minTrackWidth_ = std::min(kMinWindowWidth, work.width());
        minTrackHeight_ = std::min(kMinWindowHeight, work.height());
        const auto placement = graphics::placeWindowInWorkArea(kInitialWindowWidth, kInitialWindowHeight,
            minTrackWidth_, minTrackHeight_, work);
        window_ = CreateWindowExW(0, wc.lpszClassName,
            options_.title.c_str(),
            WS_OVERLAPPEDWINDOW, placement.x, placement.y, placement.width, placement.height,
            nullptr, nullptr, wc.hInstance, this);
        if (!window_) throw std::runtime_error("Cannot create window");
        // Offscreen export keeps the window hidden: it only exists so the Win32
        // surface can select a device/queue. Its client size is never used.
        if (!offscreen_) ShowWindow(window_, SW_SHOW);
        std::cerr << "Window: " << placement.width << 'x' << placement.height << " at "
                  << placement.x << ',' << placement.y << " (work area " << work.width() << 'x'
                  << work.height() << ", minimum " << minTrackWidth_ << 'x' << minTrackHeight_ << ")\n";
    }
    VkExtent2D clientSize() const {
        RECT rect{};
        GetClientRect(window_, &rect);
        return {static_cast<uint32_t>(rect.right), static_cast<uint32_t>(rect.bottom)};
    }
    void createInstance() {
        uint32_t count = 0;
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate layers");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "read layers");
        const char* validation = "VK_LAYER_KHRONOS_validation";
        const bool available = std::any_of(layers.begin(), layers.end(), [&](const auto& layer) {
            return std::strcmp(layer.layerName, validation) == 0;
        });
        bool enableValidation = requireValidation_;
#ifndef NDEBUG
        enableValidation = available;
#endif
        if (requireValidation_ && !available) throw std::runtime_error("Required validation layer is unavailable");
        if (!available) std::cerr << "Validation layer unavailable (install Vulkan SDK for diagnostics).\n";
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Physics graphics";
        app.apiVersion = VK_API_VERSION_1_3;
        std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
        if (enableValidation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = debugMessage;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (enableValidation) {
            info.enabledLayerCount = 1; info.ppEnabledLayerNames = &validation; info.pNext = &debug;
        }
        check(vkCreateInstance(&info, nullptr, &instance_), "create instance");
        if (enableValidation) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            if (!create) throw std::runtime_error("Debug messenger entry point unavailable");
            check(create(instance_, &debug, nullptr, &messenger_), "create debug messenger");
        }
        std::cerr << "Validation: " << (enableValidation ? "enabled" : "disabled") << '\n';
    }
    void createDevice() {
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "read GPUs");
        int bestScore = -1;
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) continue;
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
            VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            features.pNext = &maintenance;
            VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            supported.pNext = &features;
            vkGetPhysicalDeviceFeatures2(candidate, &supported);
            if (!features.dynamicRendering || !maintenance.swapchainMaintenance1) continue;
            uint32_t extensionCount = 0;
            check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr), "enumerate device extensions");
            std::vector<VkExtensionProperties> extensions(extensionCount);
            check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()), "read device extensions");
            if (std::none_of(extensions.begin(), extensions.end(), [](const auto& e) {
                return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            })) continue;
            if (std::none_of(extensions.begin(), extensions.end(), [](const auto& e) {
                return std::strcmp(e.extensionName, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) == 0;
            })) continue;
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t i = 0; i < familyCount; ++i) {
                VkBool32 present = VK_FALSE;
                check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present), "check presentation support");
                if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                uint32_t formats = 0, modes = 0;
                check(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface_, &formats, nullptr), "check surface formats");
                check(vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface_, &modes, nullptr), "check present modes");
                if (!formats || !modes) continue;
                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
                if (score > bestScore) { physical_ = candidate; queueFamily_ = i; bestScore = score; }
                break;
            }
        }
        if (!physical_) throw std::runtime_error("Need Vulkan 1.3, dynamic rendering, EXT_swapchain_maintenance1 and a combined graphics/present queue");
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical_, &properties);
        maxImageDimension2D_ = static_cast<int>(properties.limits.maxImageDimension2D);
        if (maxImageDimension2D_ <= 0) maxImageDimension2D_ = 4096;
        std::cerr << "GPU: " << properties.deviceName << " (maxImageDimension2D " << maxImageDimension2D_ << ")\n";
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily_; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        maintenance.swapchainMaintenance1 = VK_TRUE; features.pNext = &maintenance;
        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &features;
        info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = 2; info.ppEnabledExtensionNames = extensions;
        check(vkCreateDevice(physical_, &info, nullptr, &device_), "create logical device");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    }
    void createCommands() {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = queueFamily_; pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_), "create command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = commandPool_; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &allocation, &command_), "allocate command buffer");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &semaphore, nullptr, &imageAvailable_), "create acquire semaphore");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device_, &fence, nullptr, &inFlight_), "create submit fence");
        fence.flags = 0;
        check(vkCreateFence(device_, &fence, nullptr, &presentationDone_), "create presentation fence");
    }
    void createGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        configureFontAtlas();
        ImGui::StyleColorsDark();
        auto& style = ImGui::GetStyle();
        style.WindowPadding = {18,16}; style.FramePadding = {10,7}; style.ItemSpacing = {10,9};
        style.WindowRounding = 0; style.ChildRounding = 10; style.FrameRounding = 6; style.GrabRounding = 6;
        style.Colors[ImGuiCol_WindowBg] = {0.047f,0.063f,0.09f,1};
        style.Colors[ImGuiCol_ChildBg] = {0.071f,0.09f,0.125f,1};
        style.Colors[ImGuiCol_FrameBg] = {0.12f,0.16f,0.21f,1};
        style.Colors[ImGuiCol_Button] = {0.12f,0.19f,0.25f,1};
        style.Colors[ImGuiCol_ButtonHovered] = {0.16f,0.32f,0.39f,1};
        style.Colors[ImGuiCol_ButtonActive] = {0.12f,0.42f,0.47f,1};
        style.Colors[ImGuiCol_SliderGrab] = {0.25f,0.82f,0.77f,1};
        style.Colors[ImGuiCol_Text] = {0.9f,0.93f,0.96f,1};
        if (!ImGui_ImplWin32_Init(window_)) throw std::runtime_error("Cannot initialize Win32 GUI backend");
        guiWin32_ = true;
    }
    // ---------------------------------------------------------------------
    // Font atlas
    // ---------------------------------------------------------------------
    void configureFontAtlas() {
        ImGuiIO& io = ImGui::GetIO();
        fontCandidates_ = graphics::font_support::loadSystemFontCandidates();
        if (fontCandidates_.empty() || !fontCandidates_.front().primary)
            throw std::runtime_error("Microsoft YaHei system font is needed for Chinese labels");
        offscreenAtlas_ = offscreen_;
        if (offscreen_) {
            std::string warning;
            atlasRasterPx_ = graphics::font_support::clampRasterPixelSize(options_.exportFontPixelSize,
                static_cast<float>(maxImageDimension2D_), &warning);
            if (!warning.empty()) std::cerr << "[font] " << warning << '\n';
            if (options_.exportGlyphText.empty()) {
                // Compatibility path: without a needed-glyph list the host cannot
                // know which characters the export draws, so it keeps the full
                // CJK atlas and warns instead of silently dropping Chinese text.
                std::cerr << "[font] offscreen export did not provide exportGlyphText; keeping the full CJK atlas. "
                             "Pass the characters the export draws so the atlas can stay on demand.\n";
                atlasPrimaryRanges_ = graphics::font_support::buildInteractiveRanges();
                atlasVerifyCodepoints_ = graphics::font_support::interactiveSupplementCodepoints();
                atlasRequiredCodepoints_ = graphics::font_support::interactiveRequiredCodepoints();
            } else {
                atlasPrimaryRanges_ = graphics::font_support::buildTextRanges(options_.exportGlyphText);
                atlasVerifyCodepoints_ = graphics::font_support::textCodepoints(options_.exportGlyphText);
                atlasRequiredCodepoints_ = atlasVerifyCodepoints_;
            }
        } else {
            // The interactive atlas keeps the fixed 18 px full-CJK raster; the
            // export raster size never leaks into the editor UI.
            atlasRasterPx_ = graphics::font_support::kBaseFontPixelSize;
            atlasPrimaryRanges_ = graphics::font_support::buildInteractiveRanges();
            atlasVerifyCodepoints_ = graphics::font_support::interactiveSupplementCodepoints();
            atlasRequiredCodepoints_ = graphics::font_support::interactiveRequiredCodepoints();
        }
        atlasPlan_ = graphics::font_support::planCoverage(fontCandidates_, atlasVerifyCodepoints_, atlasBasePx_);
        atlasCoveredCodepoints_ = atlasPlan_.covered;
        atlasSourceMissing_ = atlasPlan_.missing;
        buildAtlas(true);
        if (offscreen_) {
            // Fixed logical canvas: the offscreen process never derives its
            // DisplaySize from the hidden window's client area.
            io.DisplaySize = {static_cast<float>(options_.exportWidth), static_cast<float>(options_.exportHeight)};
            io.DisplayFramebufferScale = {1.0f, 1.0f};
        }
    }
    // Adds every source for one build attempt. `rasterPx` is the physical glyph
    // raster; the logical size stays atlasBasePx_ so no draw call changes. The
    // range arrays are stored in atlasRangeStorage_ because ImFontConfig keeps a
    // pointer to them for the lifetime of the atlas.
    void addAtlasSources(ImFontAtlas& atlas, float rasterPx, int width) {
        atlas.Clear();
        atlas.TexDesiredWidth = width;
        atlasRangeStorage_.clear();
        const float density = atlasBasePx_ > 0.0f ? rasterPx / atlasBasePx_ : 1.0f;
        addFontSource(atlas, fontCandidates_[0], false, atlasPrimaryRanges_, density);
        for (const auto& fallback : atlasPlan_.fallbacks)
            addFontSource(atlas, fontCandidates_[fallback.first], true,
                graphics::font_support::rangesFromCodepoints(fallback.second), density);
    }
    void addFontSource(ImFontAtlas& atlas, const graphics::font_support::FontSource& source, bool merge,
        const std::vector<ImWchar>& ranges, float density) {
        atlasRangeStorage_.push_back(ranges);
        const std::vector<ImWchar>& stored = atlasRangeStorage_.back();
        const std::size_t length = source.bytes.size();
        void* copy = IM_ALLOC(length);
        std::memcpy(copy, source.bytes.data(), length);
        ImFontConfig config;
        config.MergeMode = merge;
        config.FontNo = source.fontNo;
        config.FontDataOwnedByAtlas = true;
        config.RasterizerDensity = density;
        if (!atlas.AddFontFromMemoryTTF(copy, static_cast<int>(length), atlasBasePx_, &config, stored.data())) {
            IM_FREE(copy);
            throw std::runtime_error("Cannot add font " + source.label);
        }
    }
    static std::string hexCodepoint(uint32_t codepoint) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<unsigned>(codepoint));
        return buffer;
    }
    static void restorePendingFormulas() {
        // math_text::initialize() moves pendingCustom into custom before building.
        // If that build throws, the next call would move an already-empty vector
        // and lose every runtime formula, so put the masks back into the pending
        // slot before retrying.
        math_text::pendingCustom = math_text::custom;
        math_text::pending = true;
    }
    bool verifyAtlas(ImFontAtlas& atlas, int maxDimension, std::string& problem) {
        if (atlas.Fonts.Size == 0) { problem = "the atlas contains no font"; return false; }
        for (int index : math_text::rectangles) {
            if (index < 0 || index >= atlas.CustomRects.Size) {
                problem = "a formula custom rect index is outside the atlas"; return false;
            }
            if (!atlas.GetCustomRectByIndex(index)->IsPacked()) {
                problem = "a formula mask did not fit into the atlas"; return false;
            }
        }
        if (atlas.TexWidth <= 0 || atlas.TexHeight <= 0) { problem = "the atlas texture is empty"; return false; }
        if (atlas.TexWidth > maxDimension || atlas.TexHeight > maxDimension) {
            problem = "atlas " + std::to_string(atlas.TexWidth) + "x" + std::to_string(atlas.TexHeight) +
                " exceeds the " + std::to_string(maxDimension) + " px device limit";
            return false;
        }
        ImFont* font = atlas.Fonts[0];
        auto inAtlas = [&](uint32_t codepoint) {
            return codepoint <= 0xFFFF && font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) != nullptr;
        };
        std::vector<uint32_t> packingFailed, sourceMissing;
        for (uint32_t codepoint : atlasVerifyCodepoints_) {
            if (inAtlas(codepoint)) continue;
            // Covered by a source but absent from the atlas means ImGui dropped
            // it during packing; uncovered means no font has it at all.
            if (std::binary_search(atlasCoveredCodepoints_.begin(), atlasCoveredCodepoints_.end(), codepoint))
                packingFailed.push_back(codepoint);
        }
        for (uint32_t codepoint : atlasRequiredCodepoints_) {
            if (!inAtlas(codepoint) &&
                !std::binary_search(atlasCoveredCodepoints_.begin(), atlasCoveredCodepoints_.end(), codepoint))
                sourceMissing.push_back(codepoint);
        }
        if (!packingFailed.empty()) {
            // ImGui silently drops glyphs when packing overflows; report them
            // instead of shipping a soft or incomplete atlas.
            problem = std::to_string(packingFailed.size()) + " required glyph(s) were dropped by atlas packing (first " +
                hexCodepoint(packingFailed.front()) + ')';
            return false;
        }
        if (!sourceMissing.empty()) {
            // No system font can draw these; the editor surfaces the same list in
            // its own warning. The atlas is still usable, so this is not fatal.
            std::cerr << "[font] warning: " << sourceMissing.size()
                      << " requested character(s) are absent from the system fonts:";
            for (std::size_t i = 0; i < sourceMissing.size() && i < 16; ++i)
                std::cerr << ' ' << hexCodepoint(sourceMissing[i]);
            if (sourceMissing.size() > 16) std::cerr << " ...";
            std::cerr << '\n';
        }
        atlasSourceMissing_ = sourceMissing;
        return true;
    }
    void logAtlas(float rasterPx, int requestedWidth, const ImFontAtlas& atlas) const {
        const float density = atlasBasePx_ > 0.0f ? rasterPx / atlasBasePx_ : 1.0f;
        std::cerr << "[font] " << (offscreenAtlas_ ? "offscreen" : "interactive") << " atlas "
                  << atlas.TexWidth << 'x' << atlas.TexHeight << " raster " << rasterPx << " px (base "
                  << atlasBasePx_ << " px, density " << density << "), glyphs "
                  << (atlas.Fonts.Size ? atlas.Fonts[0]->Glyphs.Size : 0) << ", requested width "
                  << requestedWidth << ", fallback sources " << atlasPlan_.fallbacks.size()
                  << ", source-missing " << atlasSourceMissing_.size() << '\n';
        if (offscreenAtlas_)
            std::cerr << "[font] requested exportFontPixelSize " << options_.exportFontPixelSize
                      << " -> actual raster " << rasterPx << " px\n";
    }
    // Builds the CPU atlas and verifies it against the device texture limit and
    // the required glyphs. Retries by widening the atlas first, then by lowering
    // the offscreen raster, and finally throws an explanatory error. Every step
    // is logged, so a soft result is never silently accepted.
    void buildAtlas(bool initial) {
        ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
        const int maxDimension = maxImageDimension2D_ > 0 ? maxImageDimension2D_ : 4096;
        const bool fullCjk = !offscreenAtlas_ || options_.exportGlyphText.empty();
        float raster = atlasRasterPx_;
        bool sourcesDirty = initial;
        int widenings = 0, shrinks = 0;
        std::string lastProblem;
        for (int attempt = 0; attempt < kMaxAtlasAttempts; ++attempt) {
            const int width = graphics::font_support::chooseAtlasWidth(maxDimension, widenings, fullCjk);
            if (sourcesDirty || atlas.Sources.Size == 0) {
                addAtlasSources(atlas, raster, width);
                sourcesDirty = false;
            } else {
                atlas.TexDesiredWidth = width;
            }
            std::string problem;
            bool ok = false;
            try {
                math_text::initialize(atlas);
                ok = true;
            } catch (const std::exception& error) {
                problem = error.what();
                restorePendingFormulas();
            }
            if (ok) ok = verifyAtlas(atlas, maxDimension, problem);
            if (ok) {
                atlasRasterPx_ = raster;
                logAtlas(raster, width, atlas);
                return;
            }
            lastProblem = problem;
            std::cerr << "[font] atlas attempt " << (attempt + 1) << '/' << kMaxAtlasAttempts << " failed at "
                      << width << " px width, raster " << raster << " px: " << problem << '\n';
            const int ceiling = graphics::font_support::atlasWidthCeiling(maxDimension);
            if (width < ceiling) {
                ++widenings;
                std::cerr << "[font] retrying with a wider atlas\n";
                continue;
            }
            if (offscreenAtlas_ && shrinks < kMaxRasterShrinks &&
                raster > graphics::font_support::kMinRasterPixelSize + 0.5f) {
                const float next = std::max(graphics::font_support::kMinRasterPixelSize, std::floor(raster * 0.75f));
                if (next < raster) {
                    ++shrinks;
                    raster = next;
                    sourcesDirty = true;
                    // Keep the widest atlas: a smaller raster still packs best at
                    // the device ceiling, so retrying from the narrow width again
                    // would only waste attempts.
                    widenings = 32;
                    std::cerr << "[font] retrying with a smaller raster " << raster << " px\n";
                    continue;
                }
            }
            break;
        }
        throw std::runtime_error("Cannot fit the font atlas inside the " + std::to_string(maxDimension) +
            " px device limit: " + lastProblem);
    }
    // Runtime formula rebuild: same frame-boundary idle lifecycle as before, but
    // the device atlas limit and glyph coverage are re-checked.
    void rebuildFormulaAtlas() {
        check(vkDeviceWaitIdle(device_), "wait before formula atlas upload");
        ImGui_ImplVulkan_DestroyFontsTexture();
        buildAtlas(false);
        if (!ImGui_ImplVulkan_CreateFontsTexture()) throw std::runtime_error("Cannot upload formula atlas");
    }
    void createGuiRenderer() {
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3; info.Instance = instance_; info.PhysicalDevice = physical_;
        info.Device = device_; info.QueueFamily = queueFamily_; info.Queue = queue_;
        info.DescriptorPoolSize = 32; info.MinImageCount = 2; info.ImageCount = imageCount_;
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT; info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat_;
        info.CheckVkResultFn = [](VkResult result) { check(result, "ImGui Vulkan operation"); };
        if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Cannot initialize Vulkan GUI backend");
        guiVulkan_ = true;
    }
    VkImageView createView(VkImage image, VkFormat format, VkImageAspectFlags aspect) const {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image; info.viewType = VK_IMAGE_VIEW_TYPE_2D; info.format = format;
        info.subresourceRange = {aspect, 0, 1, 0, 1};
        VkImageView view{};
        check(vkCreateImageView(device_, &info, nullptr, &view), "create image view");
        return view;
    }
    void recreateSwapchain() {
        // Deliberately conservative for a learning sample: one frame in flight and
        // idle on resize. A production renderer should retire resources asynchronously.
        check(vkDeviceWaitIdle(device_), "wait before swapchain rebuild");
        waitForPresentation();
        destroySwapchain();
        VkSurfaceCapabilitiesKHR capabilities;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities), "get surface capabilities");
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            throw std::runtime_error("Surface cannot be a color attachment");
        uint32_t count = 0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr), "count surface formats");
        if (!count) throw std::runtime_error("Surface has no usable formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()), "read surface formats");
        auto format = formats.front();
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
        for (auto candidate : formats)
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                format = candidate;
        colorFormat_ = format.format;
        extent_ = capabilities.currentExtent;
        if (extent_.width == std::numeric_limits<uint32_t>::max()) {
            extent_ = clientSize();
            extent_.width = std::clamp(extent_.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent_.height = std::clamp(extent_.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        if (!extent_.width || !extent_.height) { resized_ = true; return; }
        uint32_t imageCount = std::max(2u, capabilities.minImageCount + 1);
        if (capabilities.maxImageCount) imageCount = std::min(imageCount, capabilities.maxImageCount);
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_; info.minImageCount = imageCount;
        info.imageFormat = colorFormat_; info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent_; info.imageArrayLayers = 1; info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; info.preTransform = capabilities.currentTransform;
        for (auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (capabilities.supportedCompositeAlpha & alpha) { info.compositeAlpha = alpha; break; }
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR; info.clipped = VK_TRUE;
        check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "create swapchain");
        check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr), "count swapchain images");
        images_.resize(count);
        check(vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data()), "read swapchain images");
        views_.reserve(count); renderFinished_.resize(count, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32_t i = 0; i < count; ++i) {
            views_.push_back(createView(images_[i], colorFormat_, VK_IMAGE_ASPECT_COLOR_BIT));
            check(vkCreateSemaphore(device_, &semaphore, nullptr, &renderFinished_[i]), "create present semaphore");
        }
        imageCount_ = count;
        createGuiRenderer();
        resized_ = false; ++rebuilds_;
        std::cerr << "Swapchain: " << extent_.width << 'x' << extent_.height << " (" << count << " images)\n";
    }
    void record(uint32_t index) {
        check(vkResetCommandBuffer(command_, 0), "reset command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "begin commands");
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.image = images_[index]; barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = views_[index]; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.018f, 0.025f, 0.045f, 1.0f}};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent_}; rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color;
        vkCmdBeginRendering(command_, &rendering);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_);
        vkCmdEndRendering(command_);
        VkImageMemoryBarrier present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        present.srcQueueFamilyIndex = present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present.image = images_[index]; present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &present);
        check(vkEndCommandBuffer(command_), "end commands");
    }
    bool drawFrame() {
        if (!swapchain_) return false;
        check(vkWaitForFences(device_, 1, &inFlight_, VK_TRUE, UINT64_MAX), "wait for previous frame");
        waitForPresentation();
        uint32_t index = 0;
        VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable_, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { resized_ = true; return false; }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) check(acquired, "acquire image");
        record(index);
        // Reset only after a successful acquire so an out-of-date swapchain cannot deadlock the fence.
        check(vkResetFences(device_, 1, &inFlight_), "reset submit fence");
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &imageAvailable_; submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &renderFinished_[index];
        check(vkQueueSubmit(queue_, 1, &submit, inFlight_), "submit frame");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        VkSwapchainPresentFenceInfoEXT presentFence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        presentFence.swapchainCount = 1; presentFence.pFences = &presentationDone_;
        present.pNext = &presentFence;
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &renderFinished_[index];
        present.swapchainCount = 1; present.pSwapchains = &swapchain_; present.pImageIndices = &index;
        VkResult result = vkQueuePresentKHR(queue_, &present);
        presentationPending_ = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquired == VK_SUBOPTIMAL_KHR) resized_ = true;
        if (result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DATE_KHR && result != VK_SUBOPTIMAL_KHR) check(result, "present frame");
        return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
    }
    void waitForPresentation() {
        if (!presentationPending_) return;
        check(vkWaitForFences(device_, 1, &presentationDone_, VK_TRUE, UINT64_MAX), "wait for presentation resources");
        check(vkResetFences(device_, 1, &presentationDone_), "reset presentation fence");
        presentationPending_ = false;
    }
    void destroySwapchain() noexcept {
        if (!device_) return;
        if (guiVulkan_) { ImGui_ImplVulkan_Shutdown(); guiVulkan_ = false; }
        for (auto view : views_) vkDestroyImageView(device_, view, nullptr);
        views_.clear(); images_.clear();
        for (auto semaphore : renderFinished_) vkDestroySemaphore(device_, semaphore, nullptr);
        renderFinished_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    void cleanup() noexcept {
        if (device_) {
            vkDeviceWaitIdle(device_);
            // Release the offscreen target before the device goes away; its own
            // destructor also calls destroy(), so this is idempotent.
            exporter_.destroy();
            // A submit fence / DeviceWaitIdle alone does not cover presentation.
            if (presentationPending_) vkWaitForFences(device_, 1, &presentationDone_, VK_TRUE, UINT64_MAX);
            destroySwapchain();
            vkDestroyFence(device_, inFlight_, nullptr);
            vkDestroyFence(device_, presentationDone_, nullptr);
            vkDestroySemaphore(device_, imageAvailable_, nullptr);
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (guiWin32_) ImGui_ImplWin32_Shutdown();
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (messenger_) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance_, messenger_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
        if (window_) DestroyWindow(window_);
        UnregisterClassW(L"VulkanKinematicsLesson", GetModuleHandleW(nullptr));
    }
};
} // namespace


int graphics::run(View view,Options options) {
    validationErrors=0; validationWarnings=0;
    try {
        if(options.smoke && options.capture) throw std::runtime_error("Choose smoke or capture");
        { VulkanHost app(std::move(view),std::move(options)); app.run(); }
        std::cerr << "Validation errors: " << validationErrors << "; warnings: " << validationWarnings << '\n';
        return validationErrors ? 2 : 0;
    } catch(const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}

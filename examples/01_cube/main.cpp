#include <windows.h>
#include <vulkan/vulkan.h>
#include "math.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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
struct Vertex { float position[3]; float color[3]; };
std::vector<Vertex> cubeVertices() {
    // Separate vertices for each face allow sharp per-face colors.
    const std::array<std::array<float, 3>, 8> p{{
        {{-1,-1,-1}}, {{1,-1,-1}}, {{1,1,-1}}, {{-1,1,-1}},
        {{-1,-1,1}}, {{1,-1,1}}, {{1,1,1}}, {{-1,1,1}}
    }};
    const int faces[6][4] = {{4,5,6,7}, {1,0,3,2}, {0,4,7,3}, {5,1,2,6}, {7,6,2,3}, {0,1,5,4}};
    const float colors[6][3] = {{0.95f,0.2f,0.16f}, {0.1f,0.7f,0.95f}, {0.55f,0.25f,0.95f},
        {0.1f,0.8f,0.4f}, {1.0f,0.7f,0.12f}, {0.15f,0.8f,0.85f}};
    std::vector<Vertex> vertices;
    for (int face = 0; face < 6; ++face)
        for (int corner : {0,1,2,2,3,0}) {
            const auto& pos = p[faces[face][corner]];
            vertices.push_back({{pos[0],pos[1],pos[2]}, {colors[face][0],colors[face][1],colors[face][2]}});
        }
    return vertices;
}

class CubeApp {
public:
    explicit CubeApp(bool smoke, bool requireValidation) : smoke_(smoke), requireValidation_(requireValidation) {}
    CubeApp(const CubeApp&) = delete;
    CubeApp& operator=(const CubeApp&) = delete;
    ~CubeApp() { cleanup(); }

    void run() {
        createWindow();
        createInstance();
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = GetModuleHandleW(nullptr);
        surfaceInfo.hwnd = window_;
        check(vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_), "create Win32 surface");
        createDevice();
        createCommands();
        createVertices();
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
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(), 0.1f);
            previous = now;
            if (smoke_ && now - start > std::chrono::seconds(30))
                throw std::runtime_error("Smoke test timed out");
            if (IsIconic(window_) || clientSize().width == 0 || clientSize().height == 0) {
                if (smoke_) {
                    ++minimizedSkips_;
                    ShowWindow(window_, SW_RESTORE);
                } else WaitMessage(); // Do not busy-loop while minimized.
                continue;
            }
            if (resized_) recreateSwapchain();
            if (!paused_) yaw_ += dt * 0.65f;
            if (!drawFrame()) continue;
            ++frame;
            if (smoke_) {
                if (frame == 20) SetWindowPos(window_, nullptr, 0, 0, 720, 720, SWP_NOMOVE | SWP_NOZORDER);
                if (frame == 40) SetWindowPos(window_, nullptr, 0, 0, 540, 800, SWP_NOMOVE | SWP_NOZORDER);
                if (frame == 60) ShowWindow(window_, SW_MINIMIZE);
                if (frame == 80) {
                    handleKey(VK_SPACE);
                    if (!paused_) throw std::runtime_error("Pause test failed");
                    handleKey(VK_SPACE);
                    if (paused_) throw std::runtime_error("Resume test failed");
                    handleKey(VK_LEFT); handleKey('R');
                    if (yaw_ != 0.55f || pitch_ != -0.4f) throw std::runtime_error("Reset test failed");
                    std::cout << "Pause/resume/reset: passed\n";
                }
                if (frame == 120) handleKey(VK_ESCAPE);
            }
        }
        check(vkDeviceWaitIdle(device_), "wait for shutdown");
        if (smoke_ && (frame < 120 || rebuilds_ < 3 || minimizedSkips_ == 0))
            throw std::runtime_error("Smoke test did not cover the required window lifecycle");
        std::cout << "Rendered frames: " << frame << "; swapchain builds: " << rebuilds_
                  << "; minimized skips: " << minimizedSkips_ << '\n';
    }

private:
    HWND window_{};
    bool running_ = true, resized_ = false, paused_ = false, smoke_ = false, requireValidation_ = false;
    float yaw_ = 0.55f, pitch_ = -0.4f;
    unsigned rebuilds_ = 0, minimizedSkips_ = 0;
    VkInstance instance_{};
    VkDebugUtilsMessengerEXT messenger_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    uint32_t queueFamily_ = 0;
    VkQueue queue_{};
    VkSwapchainKHR swapchain_{};
    VkFormat colorFormat_{}, depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    // Presentation waits are tied to acquired image indices, not the submit fence.
    std::vector<VkSemaphore> renderFinished_;
    VkImage depthImage_{};
    VkDeviceMemory depthMemory_{};
    VkImageView depthView_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    VkCommandPool commandPool_{};
    VkCommandBuffer command_{};
    VkSemaphore imageAvailable_{};
    VkFence inFlight_{};
    VkFence presentationDone_{};
    bool presentationPending_ = false;
    VkBuffer vertexBuffer_{};
    VkDeviceMemory vertexMemory_{};

    void handleKey(WPARAM key) {
        switch (key) {
        case VK_ESCAPE: running_ = false; break;
        case VK_SPACE: paused_ = !paused_; break;
        case VK_LEFT: yaw_ -= 0.12f; break;
        case VK_RIGHT: yaw_ += 0.12f; break;
        case VK_UP: pitch_ -= 0.12f; break;
        case VK_DOWN: pitch_ += 0.12f; break;
        case 'R': yaw_ = 0.55f; pitch_ = -0.4f; break;
        }
    }
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<CubeApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<CubeApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app) {
            if (message == WM_SIZE) { app->resized_ = true; return 0; }
            // Ignore autorepeat for toggle keys; allow it for arrows.
            if (message == WM_KEYDOWN) {
                if (wParam != VK_SPACE || !(lParam & (1LL << 30))) app->handleKey(wParam);
                return 0;
            }
            if (message == WM_CLOSE) { app->running_ = false; return 0; }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    void createWindow() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"VulkanLearningCube";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) throw std::runtime_error("Cannot register window class");
        window_ = CreateWindowExW(0, wc.lpszClassName,
            L"Vulkan Cube | Space: pause | Arrows: rotate | R: reset | Esc: exit",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 760,
            nullptr, nullptr, wc.hInstance, this);
        if (!window_) throw std::runtime_error("Cannot create window");
        ShowWindow(window_, SW_SHOW);
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
        if (!available) std::cout << "Validation layer unavailable (install Vulkan SDK for diagnostics).\n";
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "Learning Vulkan: rotating cube";
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
        std::cout << "Validation: " << (enableValidation ? "enabled" : "disabled") << '\n';
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
            VkFormatProperties depthProperties;
            vkGetPhysicalDeviceFormatProperties(candidate, depthFormat_, &depthProperties);
            if (!(depthProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) continue;
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
        if (!physical_) throw std::runtime_error("Need Vulkan 1.3, dynamic rendering, EXT_swapchain_maintenance1, D32 depth and a combined graphics/present queue");
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical_, &properties);
        std::cout << "GPU: " << properties.deviceName << '\n';
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
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("No compatible memory type");
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
    void createVertices() {
        const auto vertices = cubeVertices();
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = vertices.size() * sizeof(Vertex); buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &buffer, nullptr, &vertexBuffer_), "create vertex buffer");
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device_, vertexBuffer_, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device_, &allocation, nullptr, &vertexMemory_), "allocate vertex memory");
        check(vkBindBufferMemory(device_, vertexBuffer_, vertexMemory_, 0), "bind vertex memory");
        void* mapped = nullptr;
        check(vkMapMemory(device_, vertexMemory_, 0, buffer.size, 0, &mapped), "map vertices");
        std::memcpy(mapped, vertices.data(), static_cast<size_t>(buffer.size));
        vkUnmapMemory(device_, vertexMemory_);
    }
    VkImageView createView(VkImage image, VkFormat format, VkImageAspectFlags aspect) const {
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = image; info.viewType = VK_IMAGE_VIEW_TYPE_2D; info.format = format;
        info.subresourceRange = {aspect, 0, 1, 0, 1};
        VkImageView view{};
        check(vkCreateImageView(device_, &info, nullptr, &view), "create image view");
        return view;
    }
    void createDepth() {
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D; image.format = depthFormat_;
        image.extent = {extent_.width, extent_.height, 1};
        image.mipLevels = 1; image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device_, &image, nullptr, &depthImage_), "create depth image");
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device_, depthImage_, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device_, &allocation, nullptr, &depthMemory_), "allocate depth memory");
        check(vkBindImageMemory(device_, depthImage_, depthMemory_, 0), "bind depth memory");
        depthView_ = createView(depthImage_, depthFormat_, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    VkShaderModule loadShader(const wchar_t* name) const {
        std::wstring executable(32768, L'\0');
        DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("Cannot locate executable directory");
        executable.resize(length);
        auto path = std::filesystem::path(executable).parent_path() / L"shaders" / name;
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open shader beside executable; build the shader target first");
        const auto bytes = static_cast<std::streamoff>(file.tellg());
        if (bytes <= 0 || bytes % 4) throw std::runtime_error("Invalid SPIR-V file size");
        std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(code.data()), bytes)) throw std::runtime_error("Cannot read shader");
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = static_cast<size_t>(bytes); info.pCode = code.data();
        VkShaderModule shader{};
        check(vkCreateShaderModule(device_, &info, nullptr, &shader), "create shader module");
        return shader;
    }
    void createPipeline() {
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "create pipeline layout");
        VkShaderModule vertex{}, fragment{};
        try {
            vertex = loadShader(L"cube.vert.spv"); fragment = loadShader(L"cube.frag.spv");
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment;
            VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attributes[2] = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)}
            };
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding;
            input.vertexAttributeDescriptionCount = 2; input.pVertexAttributeDescriptions = attributes;
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL; raster.lineWidth = 1.0f;
            raster.cullMode = VK_CULL_MODE_NONE; // Keep all faces: depth testing must resolve occlusion.
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = VK_TRUE; depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1; blend.pAttachments = &attachment;
            VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = states;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &colorFormat_; rendering.depthAttachmentFormat = depthFormat_;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.pNext = &rendering; info.stageCount = 2; info.pStages = stages;
            info.pVertexInputState = &input; info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport; info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample; info.pDepthStencilState = &depth;
            info.pColorBlendState = &blend; info.pDynamicState = &dynamic; info.layout = layout_;
            check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_), "create graphics pipeline");
        } catch (...) {
            vkDestroyShaderModule(device_, fragment, nullptr); vkDestroyShaderModule(device_, vertex, nullptr); throw;
        }
        vkDestroyShaderModule(device_, fragment, nullptr); vkDestroyShaderModule(device_, vertex, nullptr);
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
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_SRGB;
        for (auto candidate : formats)
            if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                format = candidate;
        colorFormat_ = format.format;
        extent_ = capabilities.currentExtent;
        if (extent_.width == std::numeric_limits<uint32_t>::max()) {
            extent_ = clientSize();
            extent_.width = std::clamp(extent_.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent_.height = std::clamp(extent_.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        if (!extent_.width || !extent_.height) { resized_ = true; return; }
        uint32_t imageCount = capabilities.minImageCount + 1;
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
        createDepth(); createPipeline();
        resized_ = false; ++rebuilds_;
        std::cout << "Swapchain: " << extent_.width << 'x' << extent_.height << " (" << count << " images)\n";
    }
    void record(uint32_t index) {
        check(vkResetCommandBuffer(command_, 0), "reset command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command_, &begin), "begin commands");
        VkImageMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // Discard last frame; clear every attachment.
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        barriers[0].image = images_[index]; barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[1].image = depthImage_; barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        const VkPipelineStageFlags attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        vkCmdPipelineBarrier(command_, attachmentStages, attachmentStages, 0, 0, nullptr, 0, nullptr, 2, barriers);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = views_[index]; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.018f, 0.025f, 0.045f, 1.0f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depthView_; depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent_}; rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color; rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(command_, &rendering);
        vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkViewport viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
        VkRect2D scissor{{0,0}, extent_};
        vkCmdSetViewport(command_, 0, 1, &viewport); vkCmdSetScissor(command_, 0, 1, &scissor);
        const float aspect = static_cast<float>(extent_.width) / static_cast<float>(extent_.height);
        // In portrait windows move the camera back so the cube stays fully visible.
        const Mat4 mvp = perspective(aspect) * translationZ(-6.0f / std::min(1.0f, aspect))
            * rotationY(yaw_) * rotationX(pitch_);
        vkCmdPushConstants(command_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), &mvp);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command_, 0, 1, &vertexBuffer_, &offset);
        vkCmdDraw(command_, 36, 1, 0, 0);
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
        vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device_, layout_, nullptr); layout_ = VK_NULL_HANDLE;
        vkDestroyImageView(device_, depthView_, nullptr); depthView_ = VK_NULL_HANDLE;
        vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE;
        vkFreeMemory(device_, depthMemory_, nullptr); depthMemory_ = VK_NULL_HANDLE;
        for (auto view : views_) vkDestroyImageView(device_, view, nullptr);
        views_.clear(); images_.clear();
        for (auto semaphore : renderFinished_) vkDestroySemaphore(device_, semaphore, nullptr);
        renderFinished_.clear();
        vkDestroySwapchainKHR(device_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE;
    }
    void cleanup() noexcept {
        if (device_) {
            vkDeviceWaitIdle(device_);
            // A submit fence / DeviceWaitIdle alone does not cover presentation.
            if (presentationPending_) vkWaitForFences(device_, 1, &presentationDone_, VK_TRUE, UINT64_MAX);
            destroySwapchain();
            vkDestroyBuffer(device_, vertexBuffer_, nullptr);
            vkFreeMemory(device_, vertexMemory_, nullptr);
            vkDestroyFence(device_, inFlight_, nullptr);
            vkDestroyFence(device_, presentationDone_, nullptr);
            vkDestroySemaphore(device_, imageAvailable_, nullptr);
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (messenger_) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy) destroy(instance_, messenger_, nullptr);
        }
        if (instance_) vkDestroyInstance(instance_, nullptr);
        if (window_) DestroyWindow(window_);
        UnregisterClassW(L"VulkanLearningCube", GetModuleHandleW(nullptr));
    }
};
} // namespace

int main(int argc, char** argv) {
    bool smoke = false, validation = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--smoke-test") { smoke = true; validation = true; }
        else if (argument == "--validation") validation = true;
        else if (argument == "--help") {
            std::cout << "Vulkan Cube [--validation] [--smoke-test]\nSpace: pause; arrows: rotate; R: reset; Esc: exit.\n";
            return 0;
        } else { std::cerr << "Unknown argument: " << argument << '\n'; return 1; }
    }
    try {
        { CubeApp app(smoke, validation); app.run(); }
        // Include resource-destruction diagnostics in the test result.
        std::cout << "Validation errors: " << validationErrors << "; warnings: " << validationWarnings << '\n';
        return validationErrors ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

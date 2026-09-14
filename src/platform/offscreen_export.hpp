#pragma once
// Fixed-size offscreen render target for the standalone PNG sequence export.
//
// This is deliberately separate from the interactive host loop: the export
// process owns one colour image, one readback buffer and one output directory,
// and never re-reads the live window size. All render areas and copy regions come
// from the same width/height pair, which is the fix for the reviewed "export
// region grew past the allocated image/buffer" defect.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace graphics {

struct OffscreenExportConfig {
    unsigned width = 0;
    unsigned height = 0;
    unsigned frames = 0;
    std::filesystem::path directory;
};

class OffscreenExporter {
public:
    OffscreenExporter() = default;
    OffscreenExporter(const OffscreenExporter&) = delete;
    OffscreenExporter& operator=(const OffscreenExporter&) = delete;
    ~OffscreenExporter() { destroy(); }

    // Validates the request, creates the output directory and allocates every
    // GPU resource. Throws on any failure so the caller keeps a single error
    // path; a partially created target is still released by destroy().
    void create(VkPhysicalDevice physical, VkDevice device, VkFormat format,
                const OffscreenExportConfig& config);
    // Releases all GPU resources. Idempotent and safe after a failed create();
    // also called by the destructor and by the host before vkDestroyDevice.
    void destroy() noexcept;

    VkImage image() const { return image_; }
    VkImageView imageView() const { return view_; }
    VkExtent2D extent() const { return {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)}; }
    unsigned frameCount() const { return frames_; }
    const std::filesystem::path& directory() const { return directory_; }

    // Records the colour-attachment -> transfer-src barrier, the image-to-buffer
    // copy and the transfer -> host barrier. Call after vkCmdEndRendering on the
    // frame's command buffer.
    void recordReadback(VkCommandBuffer commands);
    // Waits for the submitted frame, converts the mapped pixels into top-down
    // RGBA and writes frame_%06u.png. `frame` is the 0-based frame index.
    void writeFrame(VkQueue queue, unsigned frame);
    // Writes result.json next to the frame sequence.
    void writeResult() const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    bool bgra_ = false;
    unsigned width_ = 0, height_ = 0, frames_ = 0;
    std::filesystem::path directory_;
    std::vector<unsigned char> rgba_;
};

} // namespace graphics

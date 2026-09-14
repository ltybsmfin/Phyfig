#include "offscreen_export.hpp"

#include "png_writer.hpp"

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace graphics {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) + ")");
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    throw std::runtime_error("No suitable Vulkan memory type for the offscreen export");
}

std::string pad6(unsigned value) {
    std::string text = std::to_string(value);
    while (text.size() < 6) text.insert(text.begin(), '0');
    return text;
}
} // namespace

void OffscreenExporter::create(VkPhysicalDevice physical, VkDevice device, VkFormat format,
    const OffscreenExportConfig& config) {
    if (physical == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
        throw std::runtime_error("Offscreen export requires a Vulkan device");
    if (!config.width || !config.height)
        throw std::runtime_error("Offscreen export size must be non-zero");
    if (!config.frames)
        throw std::runtime_error("Offscreen export needs at least one frame");
    if (config.directory.empty())
        throw std::runtime_error("Offscreen export needs an output directory");
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(physical, &deviceProperties);
    if (config.width > deviceProperties.limits.maxImageDimension2D
        || config.height > deviceProperties.limits.maxImageDimension2D)
        throw std::runtime_error("Offscreen export size exceeds the device maxImageDimension2D");
    // PNG stores RGB(A) bytes; only these two 8-bit layouts can be mapped without
    // a conversion pass, and both sRGB and UNORM variants are accepted because
    // the stored bytes are copied verbatim (see writeFrame).
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgba = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!bgra && !rgba)
        throw std::runtime_error("Offscreen export needs an 8-bit RGBA or BGRA colour format");
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &formatProperties);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        throw std::runtime_error("Offscreen colour format cannot be a colour attachment");
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        throw std::runtime_error("Offscreen colour format cannot be a transfer source");
    std::error_code error;
    std::filesystem::create_directories(config.directory, error);
    if (error) throw std::runtime_error("Cannot create the export directory: " + config.directory.u8string());

    destroy();
    device_ = device;
    format_ = format;
    bgra_ = bgra;
    width_ = config.width;
    height_ = config.height;
    frames_ = config.frames;
    directory_ = config.directory;
    rgba_.resize(static_cast<std::size_t>(width_) * height_ * 4);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format_;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &imageInfo, nullptr, &image_), "create offscreen image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &allocation, nullptr, &imageMemory_), "allocate offscreen image memory");
    check(vkBindImageMemory(device_, image_, imageMemory_, 0), "bind offscreen image memory");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format_;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(device_, &viewInfo, nullptr, &view_), "create offscreen image view");

    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(width_) * height_ * 4;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer_), "create offscreen readback buffer");
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device_, &allocation, nullptr, &bufferMemory_), "allocate offscreen readback memory");
    check(vkBindBufferMemory(device_, buffer_, bufferMemory_, 0), "bind offscreen readback memory");
    check(vkMapMemory(device_, bufferMemory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "map offscreen readback memory");
}

void OffscreenExporter::destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (mapped_) { vkUnmapMemory(device_, bufferMemory_); mapped_ = nullptr; }
        if (buffer_ != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer_, nullptr);
        if (bufferMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, bufferMemory_, nullptr);
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, view_, nullptr);
        if (image_ != VK_NULL_HANDLE) vkDestroyImage(device_, image_, nullptr);
        if (imageMemory_ != VK_NULL_HANDLE) vkFreeMemory(device_, imageMemory_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    imageMemory_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    bufferMemory_ = VK_NULL_HANDLE;
    mapped_ = nullptr;
    format_ = VK_FORMAT_UNDEFINED;
    bgra_ = false;
    width_ = height_ = frames_ = 0;
    directory_.clear();
    rgba_.clear();
    rgba_.shrink_to_fit();
}

void OffscreenExporter::recordReadback(VkCommandBuffer commands) {
    if (image_ == VK_NULL_HANDLE || buffer_ == VK_NULL_HANDLE)
        throw std::runtime_error("Offscreen exporter is not initialized");
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image_;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;   // tightly packed rows of exactly `width_` pixels
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(commands, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &region);

    // findMemoryType() above requires HOST_COHERENT, so vkQueueWaitIdle alone
    // already makes the copy visible to the host. This explicit transfer -> host
    // barrier documents the dependency, but a barrier by itself does not make
    // writes visible to a non-coherent host read; that case is never selected and
    // would additionally need vkInvalidateMappedMemoryRanges.
    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = buffer_;
    toHost.offset = 0;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &toHost, 0, nullptr);
}

void OffscreenExporter::writeFrame(VkQueue queue, unsigned frame) {
    if (!mapped_ || !width_ || !height_) throw std::runtime_error("Offscreen exporter is not initialized");
    if (frame >= frames_) throw std::runtime_error("Offscreen frame index out of range");
    check(vkQueueWaitIdle(queue), "wait for the offscreen frame");
    const auto* source = static_cast<const unsigned char*>(mapped_);
    for (unsigned y = 0; y < height_; ++y) {
        const unsigned char* src = source + static_cast<std::size_t>(y) * width_ * 4;
        unsigned char* dst = rgba_.data() + static_cast<std::size_t>(y) * width_ * 4;
        for (unsigned x = 0; x < width_; ++x, src += 4, dst += 4) {
            dst[0] = bgra_ ? src[2] : src[0];
            dst[1] = src[1];
            dst[2] = bgra_ ? src[0] : src[2];
            // The sequence is a finished diagram on an opaque background, so the
            // alpha channel is forced opaque rather than composited later.
            // No sRGB transfer function is applied: the render target format is
            // the ImGui attachment format, so the stored bytes are already the
            // display-ready values and re-encoding would double-convert them.
            dst[3] = 255;
        }
    }
    image::png::write(directory_ / ("frame_" + pad6(frame) + ".png"), width_, height_, rgba_.data());
}

void OffscreenExporter::writeResult() const {
    const std::filesystem::path file = directory_ / "result.json";
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot write result.json: " + file.u8string());
    stream << "{\"ok\":true,\"frames\":" << frames_ << ",\"width\":" << width_
           << ",\"height\":" << height_ << "}\n";
    stream.close();
    if (!stream) throw std::runtime_error("Cannot write result.json: " + file.u8string());
}

} // namespace graphics

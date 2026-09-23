#include "offscreen_export.hpp"

#include "png_writer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
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

// Non-throwing variant used to pick the readback memory from a preference list.
bool findMemoryType(uint32_t& index, VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            index = i;
            return true;
        }
    return false;
}

std::string pad6(unsigned value) {
    std::string text = std::to_string(value);
    while (text.size() < 6) text.insert(text.begin(), '0');
    return text;
}

using Clock = std::chrono::steady_clock;
double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
// The progress file is refreshed on this cadence. It sits inside the 100..250 ms
// window the export worker's reader expects, and it is fast enough that a frame
// never has to wait for a publish.
constexpr auto kProgressInterval = std::chrono::milliseconds(100);
std::string fixed3(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    return buffer;
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
    {
        std::lock_guard<std::mutex> lock(timingMutex_);
        timing_ = OffscreenExportTiming{};
    }
    startedAt_ = Clock::now();
    lastPublishedAt_ = Clock::time_point{};

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
    // The host reads this buffer once per frame, so the memory type decides the
    // readback cost. Write-combined (HOST_VISIBLE|HOST_COHERENT without
    // HOST_CACHED) is tuned for the GPU writing and measured 18.8 ms per 1280x720
    // frame just to copy out, because every load is an uncached transaction.
    // Host-cached memory makes it an ordinary cached read; coherence is preferred
    // so no invalidate is needed, and a cached-but-incoherent type is still far
    // better than write-combined as long as the mapped range is invalidated after
    // each wait (see writeFrame).
    const VkMemoryPropertyFlags preferences[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    uint32_t readbackType = 0;
    VkMemoryPropertyFlags readbackFlags = 0;
    for (auto preference : preferences) {
        if (findMemoryType(readbackType, physical, requirements.memoryTypeBits, preference)) {
            readbackFlags = preference;
            break;
        }
    }
    if (readbackFlags == 0)
        throw std::runtime_error("No host-visible Vulkan memory type for the offscreen readback");
    coherent_ = (readbackFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    const bool cached = (readbackFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    readbackKind_ = cached ? (coherent_ ? "cached-coherent" : "cached") : (coherent_ ? "coherent" : "host-visible");
    allocation.memoryTypeIndex = readbackType;
    check(vkAllocateMemory(device_, &allocation, nullptr, &bufferMemory_), "allocate offscreen readback memory");
    check(vkBindBufferMemory(device_, buffer_, bufferMemory_, 0), "bind offscreen readback memory");
    check(vkMapMemory(device_, bufferMemory_, 0, VK_WHOLE_SIZE, 0, &mapped_), "map offscreen readback memory");
    startEncoders();
    startProgressPublisher();
}

void OffscreenExporter::destroy() noexcept {
    stopEncoders();
    stopProgressPublisher();
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
    coherent_ = true;
    readbackKind_.clear();
    width_ = height_ = frames_ = 0;
    directory_.clear();
    slots_.clear();
    freeSlots_.clear();
    readySlots_.clear();
    encodeFailure_.clear();
    writtenFrames_.store(0, std::memory_order_relaxed);
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

    // Make transfer writes available to host reads. Non-coherent allocations
    // additionally require the invalidation in writeFrame() after queue completion.
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
    // Timed per phase so the measured split says which part is worth optimising:
    // the queue stall is GPU-bound, the conversion is the host readback cost and
    // the PNG write is pure CPU deflate.
    const auto waitStart = Clock::now();
    check(vkQueueWaitIdle(queue), "wait for the offscreen frame");
    if (!coherent_) {
        // Cached but not coherent memory: the copy the GPU made is not guaranteed
        // visible to the CPU until the mapped range is invalidated. The preferred
        // memory type above is coherent, so this is the fallback path only.
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = bufferMemory_;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        check(vkInvalidateMappedMemoryRanges(device_, 1, &range), "invalidate offscreen readback memory");
    }
    const auto swizzleStart = Clock::now();
    // The readback buffer is host-visible, which on a discrete GPU usually means
    // write-combined memory: it is tuned for the GPU writing and the host reading
    // linearly. A per-byte conversion loop straight out of that mapping measured
    // 335 ms for one 1280x720 frame (363 ns per pixel, ~93% of the whole export),
    // because every scalar load was an uncached transaction. One bulk memcpy into a
    // cached staging slot is memory-bandwidth bound instead; the memory type is
    // chosen up front (see create) so the slot copy is an ordinary cached read.
    //
    // The slot waits for an encoder only when every slot is busy: that is the
    // backpressure which keeps at most `slots_` frames of pixels alive.
    std::size_t index = 0;
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        slotFree_.wait(lock, [this]() { return !freeSlots_.empty() || !encodersRunning_; });
        if (!encodersRunning_ || !encodeFailure_.empty()) {
            const std::string failure = encodeFailure_.empty() ? std::string("PNG encoder stopped") : encodeFailure_;
            lock.unlock();
            throw std::runtime_error("Offscreen frame " + std::to_string(frame) + " failed: " + failure);
        }
        index = freeSlots_.front();
        freeSlots_.pop_front();
    }
    EncodeSlot& slot = slots_[index];
    const std::size_t bytes = static_cast<std::size_t>(width_) * height_ * 4;
    std::memcpy(slot.pixels.data(), mapped_, bytes);
    slot.frame = frame;
    slot.filled = true;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        readySlots_.push_back(index);
    }
    slotReady_.notify_one();
    {
        std::lock_guard<std::mutex> lock(timingMutex_);
        timing_.waitSeconds += std::chrono::duration<double>(swizzleStart - waitStart).count();
        timing_.elapsedSeconds = secondsSince(startedAt_);
        // The render stage is the whole native loop, so its wall clock is what a
        // reader divides completed frames by to get frames/second.
        timing_.renderSeconds = timing_.elapsedSeconds;
    }
    publishProgress();
}

void OffscreenExporter::encodeLoop() {
    for (;;) {
        std::size_t index = 0;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            slotReady_.wait(lock, [this]() { return !readySlots_.empty() || !encodersRunning_; });
            if (readySlots_.empty()) return;  // drained and shutting down
            index = readySlots_.front();
            readySlots_.pop_front();
        }
        EncodeSlot& slot = slots_[index];
        const auto swizzleStart = Clock::now();
        // Colour conversion on cached memory: whole 32-bit words instead of four
        // byte accesses.
        std::uint32_t* pixels = slot.pixels.data();
        const std::size_t count = static_cast<std::size_t>(width_) * height_;
        if (bgra_) {
            // Memory order B,G,R,A -> R,G,B,255. Byte 0 of the little-endian word is
            // the first byte in memory, so B and R exchange places and A is forced.
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint32_t value = pixels[i];
                pixels[i] = (value & 0xFF00FF00u) | ((value & 0x00FF0000u) >> 16)
                    | ((value & 0x000000FFu) << 16) | 0xFF000000u;
            }
        } else {
            // R,G,B are already the stored order; only the alpha byte is forced
            // opaque. The sequence is a finished diagram on an opaque background, so
            // the alpha channel is not composited later.
            // No sRGB transfer function is applied: the render target format is the
            // ImGui attachment format, so the stored bytes are already the
            // display-ready values and re-encoding would double-convert them.
            for (std::size_t i = 0; i < count; ++i) pixels[i] |= 0xFF000000u;
        }
        const auto pngStart = Clock::now();
        try {
            image::png::write(directory_ / ("frame_" + pad6(slot.frame) + ".png"), width_, height_,
                reinterpret_cast<const unsigned char*>(pixels));
        } catch (const std::exception& error) {
            // An encoder must never let an exception escape its thread: record the
            // failure, release the slot and stop the pool. The render thread sees it
            // on its next wait and fails the export instead of hanging.
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (encodeFailure_.empty()) encodeFailure_ = error.what();
                slot.filled = false;
                freeSlots_.push_back(index);
                encodersRunning_ = false;
            }
            slotFree_.notify_all();
            return;
        }
        const auto finished = Clock::now();
        addEncodeSeconds(std::chrono::duration<double>(pngStart - swizzleStart).count(),
            std::chrono::duration<double>(finished - pngStart).count());
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            // Publish completion before releasing the slot: a waiter may observe
            // an empty queue immediately after this unlock and publish the final
            // progress/result without waiting for the notification below.
            writtenFrames_.fetch_add(1, std::memory_order_relaxed);
            slot.filled = false;
            freeSlots_.push_back(index);
        }
        // Counted only after png::write() returned: a frame that failed to reach the
        // disk throws above (killing the export) and never advances the counter.
        slotFree_.notify_all();
    }
}

void OffscreenExporter::addEncodeSeconds(double swizzleSeconds, double pngSeconds) {
    std::lock_guard<std::mutex> lock(timingMutex_);
    timing_.swizzleSeconds += swizzleSeconds;
    timing_.pngSeconds += pngSeconds;
}

void OffscreenExporter::startEncoders() {
    stopEncoders();
    // One encoder per core minus the one the render thread and the graphics driver
    // need, capped so a many-core machine does not hold dozens of frame buffers.
    // The deflate is the dominant per-frame cost and frames are independent files.
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 2;
    unsigned workers = std::min(kMaxEncoders, cores > 1 ? cores - 1 : 1u);
    // Bound the encoder working set, not just the number of staged images.
    // The PNG writer keeps raw bytes, a 32-bit hash-chain entry per byte,
    // compressed bytes and the assembled file. Ten RGBA frame sizes per
    // encoder conservatively cover these buffers and vector growth; one extra
    // frame is reserved for the producer. Driver/font allocations are separate.
    constexpr std::size_t memoryBudget = 512ull * 1024ull * 1024ull;
    const std::size_t frameBytes = static_cast<std::size_t>(width_) * height_ * 4;
    const std::size_t budgetWorkers = memoryBudget > frameBytes
        ? (memoryBudget - frameBytes) / (10 * frameBytes) : 0;
    workers = static_cast<unsigned>(std::min<std::size_t>(workers, std::max<std::size_t>(1, budgetWorkers)));
    const std::size_t slotCount = static_cast<std::size_t>(workers) + 1;
    slots_.clear();
    slots_.resize(slotCount);
    for (auto& slot : slots_)
        slot.pixels.resize(static_cast<std::size_t>(width_) * height_);
    freeSlots_.clear();
    readySlots_.clear();
    for (std::size_t i = 0; i < slotCount; ++i) freeSlots_.push_back(i);
    writtenFrames_.store(0, std::memory_order_relaxed);
    encodeFailure_.clear();
    // The PNG writer builds its CRC table lazily behind a plain `static bool`; warm
    // it on this thread before any encoder can race on that first call.
    const unsigned char warmup = 0;
    (void)image::png::crc32(&warmup, 1);
    encoderCount_ = workers;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        encodersRunning_ = true;
    }
    encoders_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) encoders_.emplace_back([this]() { encodeLoop(); });
}

void OffscreenExporter::waitForPendingWrites() {
    std::unique_lock<std::mutex> lock(queueMutex_);
    slotFree_.wait(lock, [this]() { return freeSlots_.size() == slots_.size() || !encodersRunning_; });
    if (!encodeFailure_.empty())
        throw std::runtime_error("Offscreen PNG encoding failed: " + encodeFailure_);
}

void OffscreenExporter::stopEncoders() noexcept {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!encodersRunning_ && encoders_.empty()) return;
    }
    // Drain first: pending frames must reach the disk even on the error path, where
    // the caller unwinds without calling waitForPendingWrites(). A failed encoder
    // clears encodersRunning_, so this wait cannot hang on a dead pool.
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        slotFree_.wait(lock, [this]() { return freeSlots_.size() == slots_.size() || !encodersRunning_; });
        encodersRunning_ = false;
    }
    slotReady_.notify_all();
    for (auto& encoder : encoders_)
        if (encoder.joinable()) encoder.join();
    encoders_.clear();
    encoderCount_ = 0;
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

void OffscreenExporter::noteComposeSeconds(double seconds) {
    if (!(seconds > 0.0)) return;
    std::lock_guard<std::mutex> lock(timingMutex_);
    timing_.composeSeconds += seconds;
}

OffscreenExportTiming OffscreenExporter::timing() const {
    std::lock_guard<std::mutex> lock(timingMutex_);
    OffscreenExportTiming snapshot = timing_;
    snapshot.completedFrames = writtenFrames_.load(std::memory_order_relaxed);
    return snapshot;
}

void OffscreenExporter::publishProgress(bool force) {
    if (directory_.empty() || frames_ == 0) return;
    const std::filesystem::path target = progressPath();
    const std::filesystem::path temporary = target.parent_path() / (target.filename().string() + ".tmp");
    // The snapshot is taken *inside* the publish lock: otherwise the background
    // publisher could capture an older counter, lose the lock to a frame that
    // published a newer one and then overwrite it, leaving a final progress file
    // that reports fewer frames than really reached the disk. Lock order is always
    // publish -> timing (the frame path releases timing before it publishes).
    std::lock_guard<std::mutex> lock(publishMutex_);
    const auto now = Clock::now();
    if (!force && lastPublishedAt_ != Clock::time_point{} && now - lastPublishedAt_ < kProgressInterval) return;
    lastPublishedAt_ = now;
    OffscreenExportTiming snapshot;
    {
        std::lock_guard<std::mutex> timingLock(timingMutex_);
        snapshot = timing_;
    }
    // Encoders finish out of order, so the published counter is the number of
    // frames that really reached the disk, never a frame index.
    snapshot.completedFrames = writtenFrames_.load(std::memory_order_relaxed);
    if (startedAt_ != Clock::time_point{}) {
        snapshot.elapsedSeconds = secondsSince(startedAt_);
        // renderSeconds is the render stage wall clock: the export worker turns
        // completedFrames/renderSeconds into frames per second, so it must never be
        // the CPU compose time (that would report a wildly optimistic rate).
        snapshot.renderSeconds = snapshot.elapsedSeconds;
    }
    const std::string body = std::string("{\"version\":1,\"stage\":\"render\",\"completedFrames\":")
        + std::to_string(snapshot.completedFrames) + ",\"totalFrames\":" + std::to_string(frames_)
        + ",\"elapsedSeconds\":" + fixed3(snapshot.elapsedSeconds)
        + ",\"renderSeconds\":" + fixed3(snapshot.renderSeconds)
        + ",\"readbackWriteSeconds\":" + fixed3(snapshot.readbackWriteSeconds()) + "}\n";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return;  // the next tick retries; a missing file is not fatal
    stream.write(body.data(), static_cast<std::streamsize>(body.size()));
    stream.close();
    if (!stream) return;
    std::error_code error;
    // A same-directory rename replaces the previous snapshot atomically, so a
    // reader never observes a half-written progress file.
    std::filesystem::rename(temporary, target, error);
    if (error) std::filesystem::remove(temporary, error);
}

void OffscreenExporter::startProgressPublisher() {
    stopProgressPublisher();
    {
        std::lock_guard<std::mutex> lock(publisherMutex_);
        publisherRunning_ = true;
    }
    // A dedicated publisher keeps the file fresh even while one frame is slow
    // (the PNG deflate can take far longer than the 250 ms contract), and it
    // keeps that cadence independent of the frame loop.
    publisher_ = std::thread([this]() {
        std::unique_lock<std::mutex> lock(publisherMutex_);
        while (publisherRunning_) {
            lock.unlock();
            publishProgress();
            lock.lock();
            publisherWake_.wait_for(lock, kProgressInterval, [this]() { return !publisherRunning_; });
        }
    });
}

void OffscreenExporter::stopProgressPublisher() noexcept {
    {
        std::lock_guard<std::mutex> lock(publisherMutex_);
        if (!publisherRunning_ && !publisher_.joinable()) return;
        publisherRunning_ = false;
    }
    publisherWake_.notify_all();
    if (publisher_.joinable()) publisher_.join();
}

} // namespace graphics

#pragma once
// Fixed-size offscreen render target for the standalone PNG sequence export.
//
// This is deliberately separate from the interactive host loop: the export
// process owns one colour image, one host-visible readback buffer and one output
// directory, and never re-reads the live window size. All render areas and copy
// regions come from the same width/height pair, which is the fix for the reviewed
// "export region grew past the allocated image/buffer" defect.
//
// The readback is copied into a small pool of staging slots and encoded by
// background threads: the capture (queue wait + cached copy) stays on the caller's
// thread so the GPU is never left waiting on a deflate, and the PNG files are
// still produced by the same writer from the same pixels.
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace graphics {

struct OffscreenExportConfig {
    unsigned width = 0;
    unsigned height = 0;
    unsigned frames = 0;
    std::filesystem::path directory;
};

// Measured split of the offscreen export. Every number is directly actionable:
// `composeSeconds` is the ImGui compose and command recording the host loop does
// per frame, `waitSeconds` is the queue stall that makes the mapped readback
// buffer valid, `swizzleSeconds` is the readback conversion and `pngSeconds` is
// the deflate plus file write. `renderSeconds` is the wall clock of the render
// stage (what the export worker turns into frames/second), and `completedFrames`
// only advances after a frame really reached the disk.
struct OffscreenExportTiming {
    double renderSeconds = 0.0;    // wall clock since the render stage started
    double composeSeconds = 0.0;   // CPU compose + command recording, accumulated
    double waitSeconds = 0.0;      // vkQueueWaitIdle, accumulated
    double swizzleSeconds = 0.0;   // readback conversion, accumulated
    double pngSeconds = 0.0;       // deflate + file write, accumulated
    double elapsedSeconds = 0.0;   // wall clock since create()
    unsigned completedFrames = 0;

    double readbackWriteSeconds() const { return waitSeconds + swizzleSeconds + pngSeconds; }
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
    // Waits for the submitted frame, copies the mapped pixels into a staging slot
    // and queues that slot for background encoding as frame_%06u.png. `frame` is
    // the 0-based frame index. Returns before the file exists; the frame is counted
    // in the progress file only once an encoder really wrote it, and
    // waitForPendingWrites() blocks until every queued frame is on disk.
    void writeFrame(VkQueue queue, unsigned frame);
    // Blocks until every queued frame has been encoded and written. Called by the
    // host after the last frame and again (harmlessly) by destroy().
    void waitForPendingWrites();
    // Writes result.json next to the frame sequence.
    void writeResult() const;

    // CPU seconds the host loop spent composing and recording one frame. The
    // exporter cannot see that work itself, so the caller reports it here. This is
    // profiling detail: the published progress file reports the *render stage wall
    // clock* as renderSeconds, which is what a reader can turn into frames/second.
    void noteComposeSeconds(double seconds);

    // Cross-worker progress contract: while the export runs, `directory()/progress.json`
    // carries {version:1, stage:"render", completedFrames, totalFrames,
    // elapsedSeconds, renderSeconds, readbackWriteSeconds}. A background publisher
    // refreshes it about every 100 ms so a slow frame can never stall the file,
    // and `force` writes the current snapshot immediately (used once after the
    // last frame, so a reader always sees completedFrames == totalFrames). The
    // stage stays "render" for the whole file: the export is not finished until
    // the caller writes result.json, and this file must not announce completion.
    void publishProgress(bool force = false);
    // Copy of the counters published so far; safe to call from any thread.
    OffscreenExportTiming timing() const;
    // Number of encoder threads actually started (1 when the pool is disabled).
    unsigned encoderCount() const { return encoderCount_; }
    // Which host memory the readback buffer landed in ("cached-coherent",
    // "coherent", "cached" or "host-visible"): this decides the readback cost and
    // is reported by the export benchmark.
    const std::string& readbackMemoryKind() const { return readbackKind_; }
    std::filesystem::path progressPath() const { return directory_ / "progress.json"; }

private:
    // Upper bound on background PNG encoder threads. Enough to saturate the deflate
    // on a normal desktop, low enough that the staging slots stay small.
    static constexpr unsigned kMaxEncoders = 6;

    void startProgressPublisher();
    void stopProgressPublisher() noexcept;
    void startEncoders();
    void stopEncoders() noexcept;
    // One staging slot: the pixels of a frame that is queued for, or busy in,
    // background encoding.
    struct EncodeSlot {
        std::vector<std::uint32_t> pixels;
        unsigned frame = 0;
        bool filled = false;
    };
    void encodeLoop();
    void addEncodeSeconds(double swizzleSeconds, double pngSeconds);

    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    bool bgra_ = false;
    // False when the readback memory type is cached but not coherent, which makes
    // the mapped range invalidate after every queue wait mandatory.
    bool coherent_ = true;
    unsigned width_ = 0, height_ = 0, frames_ = 0;
    std::filesystem::path directory_;
    std::string readbackKind_;

    // Telemetry state. `timingMutex_` guards the counters, `publishMutex_` keeps
    // two publishers from writing the same temporary file at once.
    mutable std::mutex timingMutex_;
    OffscreenExportTiming timing_;
    std::mutex publishMutex_;
    std::chrono::steady_clock::time_point startedAt_{};
    std::chrono::steady_clock::time_point lastPublishedAt_{};
    bool publisherRunning_ = false;
    std::thread publisher_;
    mutable std::mutex publisherMutex_;
    std::condition_variable publisherWake_;

    // Background PNG encoders. The deflate is the dominant cost of a frame and
    // every frame is an independent file, so encoding runs in parallel with the
    // render loop instead of serialising GPU and CPU. `writtenFrames_` is the only
    // source of the published completedFrames counter.
    std::vector<EncodeSlot> slots_;
    std::deque<std::size_t> freeSlots_;
    std::deque<std::size_t> readySlots_;
    std::vector<std::thread> encoders_;
    std::mutex queueMutex_;
    std::condition_variable slotFree_, slotReady_;
    bool encodersRunning_ = false;
    unsigned encoderCount_ = 0;
    std::string encodeFailure_;
    std::atomic<unsigned> writtenFrames_{0};
};

} // namespace graphics

#pragma once
#include "document.hpp"
#include <windows.h>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace physics_editor {
// Explicit, opt-in JSON-lines transport over the process standard handles.
// This is the editor side of the MCP bridge, NOT an MCP server: the wire format
// is exactly Pipe's {"ok":true,"result":...} / {"ok":false,"error":"..."} per
// UTF-8 line. stdout carries protocol only; diagnostics go to stderr.
//
// Ownership: all thread-shared state lives in a shared_ptr<State>; each worker
// captures its own reference, so a worker that outlives a bounded shutdown can
// never touch a destroyed transport (no use-after-free). Thread HANDLEs are
// closed by shutdown even when a worker is abandoned; closing a thread handle
// is unrelated to the stdin/stdout handles, which are never closed here.
//
// Ordering: the reader only ever pushes Incoming items. The UI thread pops them
// FIFO and is the sole producer of responses, so requests, protocol errors and
// the terminal marker keep their arrival order. Backpressure is explicit: poll
// never dispatches a request unless a response slot is free.
class StdioTransport {
public:
    static constexpr size_t kMaxRequestBytes = 1024 * 1024;
    static constexpr size_t kMaxResponseBytes = 1024 * 1024;
    static constexpr size_t kMaxQueuedRequests = 8;
    static constexpr size_t kMaxQueuedResponses = 8;
    static constexpr unsigned kReadBufferBytes = 16384;

    StdioTransport();
    ~StdioTransport();
    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    // UI thread only. Dispatches at most maxRequests queued items in order and
    // queues their responses for the writer thread. Never blocks on the client.
    void poll(const std::function<Json(const Json&)>& dispatch, size_t maxRequests = 4);

    bool closed() const;            // a terminal marker or a transport failure was processed
    bool failed() const;            // closed because the transport itself failed
    std::string closedReason() const;
    bool hasPending() const;        // items the UI thread has not processed yet

    void requestStop();             // non-blocking: no new work is accepted; the writer
                                    // drains queued responses and exits, the reader ends
                                    // at the next line, EOF or shutdown()
    void shutdown();                // idempotent: bounded graceful flush of queued output,
                                    // then cancel any blocking stdin/stdout I/O

private:
    struct Incoming {
        enum class Kind { Request, ProtocolError, Terminate };
        Kind kind = Kind::Request;
        Json request;
        std::string error;
        std::string reason;
        bool failed = false;
    };
    struct State {
        HANDLE input = INVALID_HANDLE_VALUE;
        HANDLE output = INVALID_HANDLE_VALUE;
        mutable std::mutex mutex;
        std::condition_variable incomingReady, outgoingReady, spaceReady;
        std::deque<Incoming> incoming;
        std::deque<std::string> outgoing;
        bool stop = false, closed = false, failed = false;
        std::string closedReason;
    };
    struct WorkerContext { std::shared_ptr<State> state; };

    static DWORD WINAPI readerEntry(LPVOID context);
    static DWORD WINAPI writerEntry(LPVOID context);
    static void readerLoop(const std::shared_ptr<State>& state);
    static void writerLoop(const std::shared_ptr<State>& state);
    static void markClosed(const std::shared_ptr<State>& state, const std::string& reason, bool failed = false);
    static bool enqueueIncoming(const std::shared_ptr<State>& state, Incoming&& item);
    static bool enqueueResponse(const std::shared_ptr<State>& state, const Json& response);
    static bool writeAll(const std::shared_ptr<State>& state, const std::string& line);
    void cancelWorker(HANDLE thread, unsigned long milliseconds);

    std::shared_ptr<State> state_;
    HANDLE readerThread_ = nullptr;
    HANDLE writerThread_ = nullptr;
    bool shutdown_ = false;
};
} // namespace physics_editor

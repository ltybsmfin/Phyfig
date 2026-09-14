#include "stdio_transport.hpp"
#include <algorithm>
#include <stdexcept>

namespace physics_editor {

StdioTransport::StdioTransport() {
    state_ = std::make_shared<State>();
    state_->input = GetStdHandle(STD_INPUT_HANDLE);
    state_->output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (state_->input == INVALID_HANDLE_VALUE || state_->input == nullptr)
        throw std::runtime_error("--stdio requires a valid stdin handle");
    if (state_->output == INVALID_HANDLE_VALUE || state_->output == nullptr)
        throw std::runtime_error("--stdio requires a valid stdout handle");

    auto* readerContext = new WorkerContext{state_};
    readerThread_ = CreateThread(nullptr, 0, &StdioTransport::readerEntry, readerContext, 0, nullptr);
    if (!readerThread_) {
        delete readerContext;
        throw std::runtime_error("Cannot start the stdio reader thread");
    }
    auto* writerContext = new WorkerContext{state_};
    writerThread_ = CreateThread(nullptr, 0, &StdioTransport::writerEntry, writerContext, 0, nullptr);
    if (!writerThread_) {
        delete writerContext;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stop = true;
        }
        state_->incomingReady.notify_all();
        state_->outgoingReady.notify_all();
        state_->spaceReady.notify_all();
        cancelWorker(readerThread_, 5000);
        CloseHandle(readerThread_);
        readerThread_ = nullptr;
        throw std::runtime_error("Cannot start the stdio writer thread");
    }
}

StdioTransport::~StdioTransport() { shutdown(); }

DWORD WINAPI StdioTransport::readerEntry(LPVOID context) {
    std::unique_ptr<WorkerContext> worker(static_cast<WorkerContext*>(context));
    const auto state = std::move(worker->state);
    readerLoop(state);
    return 0;
}

DWORD WINAPI StdioTransport::writerEntry(LPVOID context) {
    std::unique_ptr<WorkerContext> worker(static_cast<WorkerContext*>(context));
    const auto state = std::move(worker->state);
    writerLoop(state);
    return 0;
}

void StdioTransport::markClosed(const std::shared_ptr<State>& state, const std::string& reason, bool failed) {
    std::lock_guard<std::mutex> lock(state->mutex);
    // A failure that arrives after an earlier terminal marker (for example a
    // truncated write after a clean EOF) must still be reported: latch `failed`
    // even when `closed` is already set, and keep the first, more specific reason.
    if (failed) state->failed = true;
    if (!state->closed) {
        state->closed = true;
        state->closedReason = reason;
    }
    state->spaceReady.notify_all();
}

bool StdioTransport::enqueueIncoming(const std::shared_ptr<State>& state, Incoming&& item) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->spaceReady.wait(lock, [&] {
        return state->stop || state->closed || state->incoming.size() < kMaxQueuedRequests;
    });
    if (state->stop || state->closed) return false;
    state->incoming.push_back(std::move(item));
    state->incomingReady.notify_one();
    return true;
}

bool StdioTransport::enqueueResponse(const std::shared_ptr<State>& state, const Json& response) {
    std::string line;
    try {
        line = response.dump() + "\n";
    } catch (const std::exception&) {
        line = "{\"ok\":false,\"error\":\"Response could not be serialized\"}\n";
    }
    if (line.size() > kMaxResponseBytes)
        line = "{\"ok\":false,\"error\":\"Response exceeds 1 MB\"}\n";

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->outgoing.size() >= kMaxQueuedResponses) {
        // Same latch rule as markClosed(): a failure is never lost because an
        // earlier terminal marker already closed the transport.
        state->failed = true;
        if (!state->closed) {
            state->closed = true;
            state->closedReason = "response queue overflow (client is not reading)";
        }
        state->spaceReady.notify_all();
        return false;
    }
    state->outgoing.push_back(std::move(line));
    state->outgoingReady.notify_one();
    return true;
}

void StdioTransport::poll(const std::function<Json(const Json&)>& dispatch, size_t maxRequests) {
    std::vector<Incoming> batch;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        size_t reserved = 0; // response slots this batch will consume
        while (!state_->incoming.empty() && batch.size() < maxRequests && !state_->stop) {
            const auto& front = state_->incoming.front();
            // Never dispatch new work once the client can no longer receive a
            // response, and never pop more items than there are response slots.
            if (state_->failed && front.kind == Incoming::Kind::Request) break;
            const bool needsSlot = front.kind != Incoming::Kind::Terminate;
            if (needsSlot && state_->outgoing.size() + reserved >= kMaxQueuedResponses) break;
            if (needsSlot) ++reserved;
            batch.push_back(std::move(state_->incoming.front()));
            state_->incoming.pop_front();
        }
    }
    if (!batch.empty()) state_->spaceReady.notify_all();

    for (auto& item : batch) {
        if (item.kind == Incoming::Kind::Terminate) {
            markClosed(state_, item.reason, item.failed);
            continue;
        }
        if (item.kind == Incoming::Kind::ProtocolError) {
            enqueueResponse(state_, Json{{"ok", false}, {"error", item.error}});
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->failed || state_->stop) continue; // do not mutate without a deliverable response
        }
        Json response;
        try {
            response = Json{{"ok", true}, {"result", dispatch(item.request)}};
        } catch (const std::exception& e) {
            response = Json{{"ok", false}, {"error", e.what()}};
        } catch (...) {
            response = Json{{"ok", false}, {"error", "Unknown dispatch failure"}};
        }
        enqueueResponse(state_, response);
    }
}

void StdioTransport::readerLoop(const std::shared_ptr<State>& state) {
    std::string buffer;
    buffer.reserve(kReadBufferBytes);
    std::vector<char> chunk(kReadBufferBytes);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stop || state->closed) return;
        }
        DWORD read = 0;
        if (!ReadFile(state->input, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED) return; // shutdown cancellation
            Incoming terminal;
            terminal.kind = Incoming::Kind::Terminate;
            if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
                error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE) {
                terminal.reason = "stdin closed (Windows error " + std::to_string(error) + ")";
            } else {
                terminal.reason = "stdin read failed (Windows error " + std::to_string(error) + ")";
                terminal.failed = true;
            }
            enqueueIncoming(state, std::move(terminal));
            return;
        }
        if (read == 0) {
            Incoming terminal;
            terminal.kind = Incoming::Kind::Terminate;
            terminal.reason = "stdin closed";
            enqueueIncoming(state, std::move(terminal));
            return;
        }
        buffer.append(chunk.data(), read);

        for (;;) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                Incoming item;
                item.kind = Incoming::Kind::ProtocolError;
                item.error = "Empty request line";
                if (!enqueueIncoming(state, std::move(item))) return;
                continue;
            }
            if (line.size() > kMaxRequestBytes) {
                Incoming error;
                error.kind = Incoming::Kind::ProtocolError;
                error.error = "Request exceeds 1 MB";
                if (!enqueueIncoming(state, std::move(error))) return;
                Incoming terminal;
                terminal.kind = Incoming::Kind::Terminate;
                terminal.reason = "request line exceeds 1 MB";
                enqueueIncoming(state, std::move(terminal));
                return;
            }
            Incoming item;
            try {
                item.request = Json::parse(line);
                item.kind = Incoming::Kind::Request;
            } catch (const std::exception& e) {
                item.kind = Incoming::Kind::ProtocolError;
                item.error = std::string("Invalid JSON: ") + e.what();
            }
            if (!enqueueIncoming(state, std::move(item))) return;
        }
        if (buffer.size() > kMaxRequestBytes) {
            Incoming error;
            error.kind = Incoming::Kind::ProtocolError;
            error.error = "Request exceeds 1 MB";
            if (!enqueueIncoming(state, std::move(error))) return;
            Incoming terminal;
            terminal.kind = Incoming::Kind::Terminate;
            terminal.reason = "request line exceeds 1 MB";
            enqueueIncoming(state, std::move(terminal));
            return;
        }
    }
}

bool StdioTransport::writeAll(const std::shared_ptr<State>& state, const std::string& line) {
    size_t offset = 0;
    while (offset < line.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(line.size() - offset, 1u << 20));
        if (!WriteFile(state->output, line.data() + offset, chunk, &written, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_OPERATION_ABORTED) {
                // Cancelled by shutdown(). The line is truncated, so record a
                // failure instead of leaving the transport looking healthy, and
                // drop the remainder rather than retrying it forever.
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->outgoing.clear();
                }
                markClosed(state, "stdout write cancelled during shutdown", true);
                return false;
            }
            markClosed(state, "stdout write failed (Windows error " + std::to_string(error) + ")", true);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->outgoing.clear();
            }
            return false;
        }
        if (written == 0) {
            markClosed(state, "stdout write made no progress", true);
            return false;
        }
        offset += written;
    }
    return true;
}

void StdioTransport::writerLoop(const std::shared_ptr<State>& state) {
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->outgoingReady.wait(lock, [&] { return state->stop || !state->outgoing.empty(); });
            // `stop` only forbids new work: responses already queued (and the
            // line currently being written) are still flushed. The writer leaves
            // exactly when the queue has drained, so a clean EOF cannot strand a
            // response that the client is still waiting for.
            if (state->outgoing.empty()) return;
            line = std::move(state->outgoing.front());
            state->outgoing.pop_front();
        }
        state->spaceReady.notify_all();
        if (!writeAll(state, line)) return;
    }
}

void StdioTransport::cancelWorker(HANDLE thread, unsigned long milliseconds) {
    if (!thread) return;
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;) {
        // Repeat cancellation: the worker may not have entered its next blocking
        // I/O when the previous cancel arrived, so one shot can be missed.
        CancelSynchronousIo(thread);
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        const DWORD slice = static_cast<DWORD>(std::min<ULONGLONG>(50, deadline - now));
        if (WaitForSingleObject(thread, slice) == WAIT_OBJECT_0) break;
    }
}

void StdioTransport::requestStop() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop = true;
    }
    state_->incomingReady.notify_all();
    state_->outgoingReady.notify_all();
    state_->spaceReady.notify_all();
}

void StdioTransport::shutdown() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (shutdown_) return;
        shutdown_ = true;
        state_->stop = true;
    }
    state_->incomingReady.notify_all();
    state_->outgoingReady.notify_all();
    state_->spaceReady.notify_all();

    // Close thread handles even if a worker is abandoned: the worker keeps its
    // own shared_ptr<State> alive, so the transport may be destroyed safely.
    if (readerThread_) {
        cancelWorker(readerThread_, 5000);
        CloseHandle(readerThread_);
        readerThread_ = nullptr;
    }
    // The reader holds nothing the client needs, but a normal EOF can leave
    // already-computed responses (and the line currently in WriteFile) queued.
    // Let the writer drain them first; only a wedged client that keeps the pipe
    // full reaches the cancellation below, so shutdown stays bounded.
    if (writerThread_) {
        constexpr DWORD kWriterDrainMilliseconds = 2000;
        if (WaitForSingleObject(writerThread_, kWriterDrainMilliseconds) != WAIT_OBJECT_0)
            cancelWorker(writerThread_, 3000);
        CloseHandle(writerThread_);
        writerThread_ = nullptr;
    }
}

bool StdioTransport::closed() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->closed;
}

bool StdioTransport::failed() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->failed;
}

std::string StdioTransport::closedReason() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->closedReason;
}

bool StdioTransport::hasPending() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return !state_->incoming.empty();
}

} // namespace physics_editor

#pragma once
// BackgroundJob: a single-shot asynchronous external worker process for the editor.
//
// Contract
//  * start() launches args[0] with args[1..] directly, without a shell. args[0] is
//    passed as lpApplicationName; the caller resolves it to an absolute path (for
//    example with SearchPathW) and this class deliberately does not search PATH
//    itself. The child inherits the caller's current directory (the project root);
//    the run folder is never used as the child working directory. The caller must
//    therefore pass the run folder to the worker itself when the worker has to
//    write result.json.
//  * worker.log (stdout+stderr) and result.json live in the run folder. start()
//    refuses a folder that already contains either file, so a stale result can
//    never be mistaken for this run's output. start() never deletes anything.
//    It refuses a folder that already contains cleanup.json as well, so a stale
//    cleanup record can never make this job delete an unrelated directory.
//  * The worker is created suspended, assigned to a job object with
//    KILL_ON_JOB_CLOSE and only then resumed. poll()/cancel()/the destructor only
//    ever terminate that job, never an unrelated user process.
//  * poll() is non-blocking (WaitForSingleObject with a zero timeout) and must be
//    called from the UI thread until it returns true. Timeout enforcement is
//    poll-driven: a job whose process is still alive when poll() observes the
//    deadline is terminated as a whole tree and reported as failed.
//  * The worker result is {"ok":true,"result":...} or {"ok":false,"error":"..."}.
//    It is read only after the direct child exits, at most 8 MiB, and stored in
//    state.result; failures additionally publish state.error.
//  * Live progress: while the child runs it may publish <folder>/progress.json
//    (a complete JSON document atomically renamed into place by the worker).
//    poll() reads it at most every kProgressIntervalMs and copies the sample to
//    state["progress"]; the final sample is also read once more when the child
//    exits and when cancel() terminates it, so a short job's last sample and a
//    cancelled job's last sample are still visible. The read is bounded
//    (kMaxProgressBytes), tolerant (missing, empty, partial, oversized or corrupt
//    files simply mean "no new sample", never an error) and share-friendly
//    (FILE_SHARE_READ|WRITE|DELETE, so the worker can keep replacing the file).
//    Samples whose stage moved backwards are dropped and completedFrames /
//    overallFraction never decrease, so the editor can trust that the progress it
//    displays is monotonic. A worker that publishes nothing (for example the
//    formula compiler) behaves exactly as before: state simply has no "progress".
//  * The worker's own terminal sample is best effort: it publishes it by an atomic
//    replace it does not retry once it is about to exit, so the parent may read it
//    late or never. The real terminal state is decided from the exit code and
//    result.json alone, and is then published *on top of* the last trustworthy
//    sample: completed (only when the result says ok:true) pins the counters and
//    the fraction to 100%; failed and cancelled keep the last measured counters and
//    fraction (never inventing a new percentage) and drop the now meaningless rate and
//    estimate (fps, estimatedRemainingSeconds, estimateScope). A job that published
//    no progress keeps having no "progress" key, so the old behaviour is untouched.
//  * Leftover temporary directories: a worker that prepares its output in a
//    temporary directory (the export worker renders into
//    <output parent>/physics-export-<token>) cannot run its own cleanup when this
//    class kills its process tree: the worker is terminated, no "finally" runs,
//    and the temporary directory would stay behind. Such a worker first creates
//    that directory exclusively, then writes its ownership marker into it, and
//    only then publishes <folder>/cleanup.json - a record that names what it
//    created - and poll(), cancel() and the destructor remove exactly the one
//    directory that record describes, and only when every binding below holds:
//    the process tree was observed to have no active process left (a bounded
//    QueryInformationJobObject wait that reports ActiveProcesses == 0; a timeout
//    or a failed query keeps the directory and publishes status "kept" with that
//    reason); the record carries the exact marker and version, its token is 32
//    lowercase hex characters and equals the recorded directory name's
//    "physics-export-" suffix, that directory is a direct child of the declared
//    output parent, the declared output parent and target equal the ones this
//    process cached from <folder>/request.json *before* it started the child
//    (that file is worker-writable, so it is never re-read during cleanup),
//    jobRoot equals this job's absolute run folder, and the target is neither the
//    directory nor inside it. The directory must additionally carry the worker's
//    ownership marker <directory>/ownership.json: a regular file (never a
//    reparse point) whose token, jobRoot and target agree with the record, so a
//    directory this job did not create is never deleted, even when a forged or
//    half-written record names it. The recursive delete never follows a reparse
//    point (a symlink or junction inside the directory is removed as a link,
//    never traversed) and never leaves the recorded directory. Anything that
//    cannot be proven this way is kept, never scanned for and never guessed at,
//    and the reason is published as state["cleanup"] = {status,directory,reason}
//    with status "removed", "nothing" (the directory was already gone) or
//    "kept".
//    The deletion only happens after this object has released its own process and
//    job handles (the tree verdict is saved first, then the handles are closed,
//    then the directory is deleted on every exit path, including the destructor),
//    because a handle this process still holds must never keep the directory
//    alive. The contents are walked exactly once, while the ownership marker still
//    proves the directory is this job's; only the final RemoveDirectoryW of that
//    already emptied directory is retried, a short and bounded number of times and
//    only when Windows reports ERROR_SHARING_VIOLATION, because a process the job
//    just terminated can need a few more milliseconds to release it. The retry
//    never re-evaluates a binding, never re-walks the tree and never widens what
//    may be deleted.
//    state["cleanup"] only appears when a cleanup record was found, so a worker
//    that writes none leaves state exactly as before.
//  * start() replaces state with {"status":"running"}. Afterwards the caller may
//    attach task metadata (id, scope, object, output, ...); poll() and cancel()
//    only update status/result/error/progress, so that metadata survives to the
//    terminal state.
//  * Not copyable, not movable and not thread-safe: one owner, one thread.
//
// Fixed public interface:
//   Json state = {{"status","idle"}};  std::filesystem::path directory;
//   bool running() const;  void start(args, folder, timeoutSeconds);
//   bool poll();  void cancel();  ~BackgroundJob();
//
// timeoutSeconds == 0 means "no timeout".

// Include the standard library and document.hpp before windows.h so that its
// macros cannot affect json.hpp or the standard headers.
#include "document.hpp"

#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace physics_editor {

class BackgroundJob {
public:
    Json state = {{"status","idle"}};
    std::filesystem::path directory;

    BackgroundJob() = default;
    BackgroundJob(const BackgroundJob&) = delete;
    BackgroundJob& operator=(const BackgroundJob&) = delete;
    BackgroundJob(BackgroundJob&&) = delete;
    BackgroundJob& operator=(BackgroundJob&&) = delete;
    ~BackgroundJob() noexcept;

    // True while this object owns a child that has not been reaped by poll(),
    // cancel() or the destructor yet.
    bool running() const { return process_.get() != nullptr; }

    void start(const std::vector<std::wstring>& args, const std::filesystem::path& folder, unsigned timeoutSeconds);
    bool poll();
    void cancel();

private:
    // Move-only HANDLE owner. reset() tolerates NULL and INVALID_HANDLE_VALUE.
    class Handle {
    public:
        Handle() = default;
        explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
        ~Handle() { reset(); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        Handle(Handle&& other) noexcept : handle_(other.release()) {}
        Handle& operator=(Handle&& other) noexcept {
            if (this != &other) reset(other.release());
            return *this;
        }
        HANDLE get() const noexcept { return handle_; }
        explicit operator bool() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
        void reset(HANDLE handle = nullptr) noexcept {
            if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            handle_ = handle;
        }
        HANDLE release() noexcept { const HANDLE handle = handle_; handle_ = nullptr; return handle; }
    private:
        HANDLE handle_ = nullptr;
    };

    // Move-only search handle owner for FindFirstFileW. A search handle is not a
    // kernel object handle: it must be released with FindClose, never with
    // CloseHandle, so it gets its own owner instead of reusing Handle.
    class FindHandle {
    public:
        FindHandle() = default;
        explicit FindHandle(HANDLE handle) noexcept : handle_(handle) {}
        ~FindHandle() { reset(); }
        FindHandle(const FindHandle&) = delete;
        FindHandle& operator=(const FindHandle&) = delete;
        HANDLE get() const noexcept { return handle_; }
        explicit operator bool() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
        void reset(HANDLE handle = nullptr) noexcept {
            if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) FindClose(handle_);
            handle_ = handle;
        }
    private:
        HANDLE handle_ = nullptr;
    };

    Handle process_, thread_, job_;
    std::filesystem::path folder_;
    // Cached in start(), before the child exists. Cleanup only ever compares a
    // record against these: request.json is worker-writable and must not be
    // re-read once the worker can run.
    std::filesystem::path expectedTarget_;
    std::string expectedTargetReason_;
    ULONGLONG startedAt_ = 0;
    unsigned timeoutSeconds_ = 0;

    // Throttle for state["progress"]: poll() runs once per UI frame, but the
    // progress file must not be re-read and re-parsed every frame.
    static constexpr ULONGLONG kProgressIntervalMs = 200;
    static constexpr std::uintmax_t kMaxProgressBytes = 64ull * 1024ull;
    ULONGLONG lastProgressRead_ = 0;

    // Terminates the whole job (or the single process as a fallback). Never
    // touches a process outside this job.
    void terminateTree() noexcept {
        if (job_) TerminateJobObject(job_.get(), 1);
        if (process_) TerminateProcess(process_.get(), 1);
    }

    // Closes all handles. Closing the last job handle also triggers
    // KILL_ON_JOB_CLOSE for any descendant that is still alive.
    void closeRun() noexcept {
        thread_.reset();
        process_.reset();
        job_.reset();
    }

    // Only status/result/error are touched from here on, so metadata the caller
    // attached to state after start() (id, scope, object, output, ...) survives
    // completion, failure and cancellation. start() itself clears the whole state
    // when it begins a new task.
    void ensureStateObject() {
        if (!state.is_object()) state = Json::object();
    }

    void markCompleted(const Json& result) {
        ensureStateObject();
        state["status"] = "completed";
        state["result"] = result;
        state.erase("error");
        // Only a result with ok:true reaches this point: the target really was
        // committed, so the terminal sample is allowed to claim 100%.
        syncTerminalProgress("done", std::string());
    }

    void markFailed(const std::string& message, const Json* result) {
        ensureStateObject();
        state["status"] = "failed";
        state["error"] = clip(message);
        if (result != nullptr) state["result"] = *result;
        else state.erase("result");
        // The terminal state comes from the exit code and result.json, never from
        // the worker's last progress write: that write may never have been read.
        syncTerminalProgress("failed", message);
    }

    // Reads folder_/result.json after the direct child has exited. Returns an
    // empty string on success, otherwise a human readable reason.
    std::string readResultFile(Json& parsed) const {
        constexpr std::uintmax_t kMaxResultBytes = 8ull * 1024ull * 1024ull;
        std::error_code error;
        const auto path = folder_ / L"result.json";
        const bool exists = std::filesystem::exists(path, error);
        if (error) return "cannot inspect result.json: " + error.message();
        if (!exists) return "worker produced no result.json";
        const auto size = std::filesystem::file_size(path, error);
        if (error) return "cannot stat result.json: " + error.message();
        if (size > kMaxResultBytes) return "result.json exceeds the 8 MiB limit";
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return "cannot open result.json";
        std::string text;
        text.resize(static_cast<std::size_t>(size));
        if (size != 0) {
            stream.read(text.data(), static_cast<std::streamsize>(size));
            if (!stream || static_cast<std::uintmax_t>(stream.gcount()) != size)
                return "cannot read result.json completely";
        }
        parsed = Json::parse(text, nullptr, false);
        if (parsed.is_discarded()) return "result.json is not valid JSON";
        return {};
    }

    // Decides completed/failed from the exit code and the worker result file.
    void finish(DWORD exitCode) {
        Json parsed;
        const std::string readError = readResultFile(parsed);
        const bool parsedOk = readError.empty();

        if (exitCode != 0) {
            std::string message = "worker exited with code " + std::to_string(static_cast<unsigned long>(exitCode));
            const std::string detail = parsedOk ? resultErrorText(parsed) : std::string();
            if (!detail.empty()) message += ": " + detail;
            markFailed(message, parsedOk ? &parsed : nullptr);
            return;
        }
        if (!parsedOk) {
            markFailed(readError, nullptr);
            return;
        }
        if (!parsed.is_object() || !parsed.contains("ok") || !parsed["ok"].is_boolean()) {
            markFailed("result.json must be an object with a boolean \"ok\" field", &parsed);
            return;
        }
        if (parsed["ok"].get<bool>()) {
            markCompleted(parsed);
            return;
        }
        const std::string detail = resultErrorText(parsed);
        markFailed(detail.empty() ? "worker reported failure" : detail, &parsed);
    }

    // The worker's own failure text, from either the plain {"error":"..."} form or
    // the export worker's structured {"error":{"code":...,"message":"..."}} form.
    // It is what the failure reason shown to the user is built from.
    static std::string resultErrorText(const Json& parsed) {
        if (!parsed.is_object() || !parsed.contains("error")) return {};
        const Json& error = parsed["error"];
        if (error.is_string()) return error.get<std::string>();
        if (error.is_object() && error.contains("message") && error["message"].is_string())
            return error["message"].get<std::string>();
        return {};
    }

    // ---- live progress ----------------------------------------------------
    // Stage ordering used to reject a sample that moves backwards. Unknown stages
    // have no rank, so they neither advance nor rewind the published stage.
    static int progressStageRank(const std::string& stage) {
        if (stage == "prepare") return 0;
        if (stage == "render") return 1;
        if (stage == "encode") return 2;
        if (stage == "commit") return 3;
        if (stage == "done") return 4;
        if (stage == "failed" || stage == "cancelled") return 5;
        return -1;
    }

    // Drops fields whose type cannot be trusted instead of publishing a lie. A
    // fraction outside [0,1] is dropped as well; explicit nulls are preserved,
    // because "no stable sample yet" is itself meaningful information.
    static bool validateProgress(Json& sample) {
        if (!sample.is_object()) return false;
        if (!sample.contains("stage") || !sample["stage"].is_string()) return false;
        if (sample["stage"].get_ref<const std::string&>().empty()) return false;
        if (sample.contains("version")) {
            const Json& version = sample["version"];
            if (!version.is_number_integer() || version.get<long long>() != 1) return false;
        }
        for (const char* key : { "completedFrames", "totalFrames" })
            if (sample.contains(key) && !sample[key].is_number_unsigned()) sample.erase(key);
        // "Unknown yet" is meaningful for the two estimated fields, so an explicit
        // null is kept there; every other numeric field is dropped when it is not a
        // number, because a reader must never mistake null for a measurement.
        for (const char* key : { "fps", "elapsedSeconds", "overallFraction",
                                 "renderSeconds", "readbackWriteSeconds" })
            if (sample.contains(key) && !sample[key].is_number()) sample.erase(key);
        for (const char* key : { "phaseFraction", "estimatedRemainingSeconds" })
            if (sample.contains(key) && !sample[key].is_number() && !sample[key].is_null()) sample.erase(key);
        for (const char* key : { "overallFraction", "phaseFraction" }) {
            if (sample.contains(key) && sample[key].is_number()) {
                const double value = sample[key].get<double>();
                if (!(value >= 0.0 && value <= 1.0)) sample.erase(key);
            }
        }
        if (sample.contains("totalFrames") && sample["totalFrames"].get<std::uint64_t>() == 0)
            sample.erase("totalFrames");
        if (sample.contains("totalFrames") && sample.contains("completedFrames") &&
            sample["completedFrames"].get<std::uint64_t>() > sample["totalFrames"].get<std::uint64_t>())
            sample["completedFrames"] = sample["totalFrames"];
        return true;
    }

    // Publishes one validated sample into state["progress"], keeping the visible
    // progress monotonic: a sample from an earlier stage is dropped, the frame
    // counter and overall fraction never move backwards, and a sample that simply
    // omits them keeps the last trustworthy value instead of making the bar jump
    // back to zero.
    void publishProgress(Json sample) {
        ensureStateObject();
        if (state.contains("progress") && state["progress"].is_object()) {
            const Json& previous = state["progress"];
            if (previous.contains("stage") && previous["stage"].is_string()) {
                const int before = progressStageRank(previous["stage"].get<std::string>());
                const std::string after = sample.value("stage", std::string());
                const int rank = progressStageRank(after);
                if (before >= 0 && rank >= 0 && rank < before) return;
            }
            for (const char* key : { "completedFrames", "totalFrames", "overallFraction", "elapsedSeconds" })
                if (previous.contains(key) && !sample.contains(key)) sample[key] = previous[key];
            if (previous.contains("completedFrames") && previous["completedFrames"].is_number_unsigned() &&
                sample.contains("completedFrames") &&
                sample["completedFrames"].get<std::uint64_t>() < previous["completedFrames"].get<std::uint64_t>())
                sample["completedFrames"] = previous["completedFrames"];
            if (previous.contains("overallFraction") && previous["overallFraction"].is_number() &&
                sample.contains("overallFraction") &&
                sample["overallFraction"].get<double>() < previous["overallFraction"].get<double>())
                sample["overallFraction"] = previous["overallFraction"];
        }
        state["progress"] = std::move(sample);
    }

    // Publishes the real terminal state on top of the last trustworthy sample.
    // Called from markCompleted/markFailed/cancel() once the exit code and
    // result.json (or the cancel itself) have decided that state, so the editor and
    // MCP never show a running stage for a job that already ended. A job that
    // published no progress keeps having no "progress" key.
    void syncTerminalProgress(const char* stage, const std::string& message) {
        ensureStateObject();
        if (!state.contains("progress") || !state["progress"].is_object()) return;
        Json& progress = state["progress"];
        const bool done = std::string(stage) == "done";
        progress["stage"] = stage;
        progress["phase"] = stage;
        // The worker's own terminal payload carries both fields explicitly; the
        // synthesized one must have the same shape, or a reader would see a
        // different document depending on whether the last write was caught.
        progress["estimateScope"] = nullptr;
        progress["phaseFraction"] = done ? Json(1.0) : Json(nullptr);
        if (done) {
            progress["overallFraction"] = 1.0;
            progress["estimatedRemainingSeconds"] = 0.0;
            if (progress.contains("totalFrames") && progress["totalFrames"].is_number_unsigned())
                progress["completedFrames"] = progress["totalFrames"];
            return;
        }
        // The job is not advancing any more, so the last live sample's throughput
        // and estimate are stale and must not survive into the terminal state.
        progress.erase("fps");
        progress["estimatedRemainingSeconds"] = nullptr;
        if (!message.empty()) progress["message"] = clip(message);
        // A failure after publication can follow a genuine 100% sample. Preserve
        // that measured value; the failed/cancelled stage describes the outcome.
    }

    // 0 = nothing published yet, 1 = one usable sample, -1 = present but unusable.
    int readProgressFile(Json& parsed) const {
        const auto path = folder_ / L"progress.json";
        HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            const DWORD failure = GetLastError();
            if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND ||
                failure == ERROR_INVALID_NAME || failure == ERROR_SHARING_VIOLATION)
                return 0; // nothing to publish (yet); the next tick retries
            return -1;
        }
        Handle file(raw);
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size)) return -1;
        if (size.QuadPart <= 0) return 0; // the worker has not finished its first write
        if (static_cast<std::uintmax_t>(size.QuadPart) > kMaxProgressBytes) return -1;
        std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) return -1;
        text.resize(static_cast<std::size_t>(read));
        parsed = Json::parse(text, nullptr, false);
        if (parsed.is_discarded()) return -1;
        return validateProgress(parsed) ? 1 : -1;
    }

    // Throttled, best-effort progress refresh. Never throws and never changes the
    // job outcome: a worker without a progress file is perfectly normal.
    void refreshProgress(bool force) {
        if (folder_.empty()) return;
        const ULONGLONG now = GetTickCount64();
        if (!force && now - lastProgressRead_ < kProgressIntervalMs) return;
        lastProgressRead_ = now;
        try {
            Json sample;
            if (readProgressFile(sample) != 1) return;
            publishProgress(std::move(sample));
        } catch (...) {
            // Progress is advisory; it must never disturb the job or the editor.
        }
    }

    // ---- leftover temporary directory cleanup ------------------------------
    // See the contract at the top of this file. The cleanup record is worker
    // output and therefore untrusted: nothing is ever deleted from a path it
    // names directly. At most one directory is removed, and only when the record
    // and the request this process wrote agree on every binding below.
    enum class FileRead { ok, missing, failed };

    struct CleanupBudget {
        std::size_t remaining = kMaxCleanupEntries;
        bool exhausted = false;
        // Windows reported that an entry is still open. That is the one failure a
        // just-terminated process can cause and the one the caller may retry.
        bool sharing = false;
    };

    static constexpr wchar_t kCleanupFileName[] = L"cleanup.json";
    static constexpr wchar_t kCleanupTemporaryName[] = L"cleanup.json.tmp";
    static constexpr wchar_t kCleanupDirectoryPrefix[] = L"physics-export-";
    static constexpr char kCleanupMarker[] = "physics-export-cleanup";
    // The marker the worker leaves inside the directory it created, so cleanup can
    // tell "a directory this job made" from "a directory some record merely
    // names". Both files are written by the same worker, but only this one lives
    // inside the directory that is about to be deleted.
    static constexpr wchar_t kOwnershipFileName[] = L"ownership.json";
    static constexpr char kOwnershipMarker[] = "physics-export-ownership";
    static constexpr std::uintmax_t kMaxOwnershipBytes = 4096;
    static constexpr std::size_t kCleanupTokenLength = 32;
    static constexpr std::uintmax_t kMaxCleanupBytes = 16ull * 1024ull;
    static constexpr std::uintmax_t kMaxRequestBytes = 1ull << 20;
    static constexpr std::size_t kMaxCleanupEntries = 200000;
    // A terminated tree needs a moment to actually release its file handles; the
    // normal-exit path only reaps, so it waits far less.
    static constexpr unsigned kKillExitWaitMs = 5000;
    static constexpr unsigned kReapExitWaitMs = 250;
    // Bounded retry of the removal itself, for ERROR_SHARING_VIOLATION only: a
    // process the job just terminated may still hold an entry open for a few more
    // milliseconds. At most 7 * 30 ms of extra latency on an already verified
    // directory; nothing about the record is re-checked and nothing else is tried.
    static constexpr unsigned kCleanupRemoveAttempts = 8;
    static constexpr unsigned kCleanupRemoveRetryMs = 30;

    static bool isCleanupToken(const std::string& token) {
        if (token.size() != kCleanupTokenLength) return false;
        for (const char character : token)
            if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) return false;
        return true;
    }

    // Case-insensitive, separator- and trailing-separator-insensitive path
    // identity. Windows path comparisons are case-insensitive, and a trailing
    // separator never changes which directory a path denotes.
    static std::wstring foldedPath(const std::filesystem::path& path) {
        std::wstring text = path.lexically_normal().wstring();
        for (wchar_t& character : text) character = static_cast<wchar_t>(towlower(character));
        while (text.size() > 1 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
        return text;
    }

    static bool samePath(const std::filesystem::path& left, const std::filesystem::path& right) {
        return foldedPath(left) == foldedPath(right);
    }

    // True when `child` is a strict descendant of `parent`, judged lexically so
    // that no filesystem access can be tricked into following anything.
    static bool isInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
        auto candidate = child.lexically_normal().parent_path();
        for (;;) {
            if (samePath(candidate, parent)) return true;
            const auto next = candidate.parent_path();
            if (candidate.empty() || next == candidate) return false;
            candidate = next;
        }
    }

    // A path this class is willing to compare and delete under: absolute, with
    // root and (when required) a file name, and with every component already
    // normal. Windows strips trailing spaces and dots from a component, so a
    // component that is not already normal could denote a different directory.
    static bool isNormalAbsolutePath(const std::filesystem::path& path, bool requireFileName) {
        if (!path.is_absolute() || !path.has_root_directory()) return false;
        if (requireFileName && !path.has_filename()) return false;
        for (const auto& component : path) {
            const std::wstring text = component.wstring();
            if (text.empty() || text == L"." || text == L"..") return false;
            if (text.back() == L' ' || text.back() == L'.') return false;
        }
        return true;
    }

    // Reads a bounded regular file. `reason` explains a failure; a missing file
    // is reported separately so "no record" is never confused with "bad record".
    static FileRead readBoundedFile(const std::filesystem::path& path, std::uintmax_t limit,
                                    std::string& text, std::string& reason) {
        const std::string name = path.filename().u8string();
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        // A missing file is "no record", not a failure: implementations differ on
        // whether symlink_status reports ENOENT through the error_code.
        if (error == std::errc::no_such_file_or_directory) return FileRead::missing;
        if (error) { reason = "cannot inspect " + name + ": " + error.message(); return FileRead::failed; }
        if (status.type() == std::filesystem::file_type::not_found) return FileRead::missing;
        if (!std::filesystem::is_regular_file(status)) { reason = name + " is not a regular file"; return FileRead::failed; }
        const auto size = std::filesystem::file_size(path, error);
        if (error) { reason = "cannot stat " + name + ": " + error.message(); return FileRead::failed; }
        if (size > limit) {
            reason = name + " exceeds the " + std::to_string(limit) + " byte limit";
            return FileRead::failed;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream) { reason = "cannot open " + name; return FileRead::failed; }
        text.assign(static_cast<std::size_t>(size), '\0');
        if (size != 0) {
            stream.read(text.data(), static_cast<std::streamsize>(size));
            if (!stream || static_cast<std::uintmax_t>(stream.gcount()) != size) {
                reason = "cannot read " + name + " completely";
                return FileRead::failed;
            }
        }
        return FileRead::ok;
    }

    // The export target recorded in <folder>/request.json by this process before
    // start(). The worker can rewrite that file while it runs, so the answer is
    // cached in start() and cleanup compares records against the cached value
    // only; it never re-reads the request.
    std::filesystem::path requestedExportTarget(const std::filesystem::path& folder, std::string& reason) const {
        reason.clear();
        std::string text;
        const FileRead read = readBoundedFile(folder / L"request.json", kMaxRequestBytes, text, reason);
        if (read == FileRead::missing) {
            reason = "the run folder has no request.json to check the cleanup record against";
            return {};
        }
        if (read == FileRead::failed) return {};
        const Json request = Json::parse(text, nullptr, false);
        if (request.is_discarded() || !request.is_object() ||
            !request.contains("output") || !request["output"].is_string()) {
            reason = "request.json carries no export target";
            return {};
        }
        const std::string value = request["output"].get<std::string>();
        if (value.empty() || value.size() > 4096) {
            reason = "request.json carries an unusable export target";
            return {};
        }
        auto target = std::filesystem::u8path(value);
        // "D:/out/" and "D:/out" denote the same target; the recorded target is
        // compared in that same normal form.
        if (!target.has_filename()) target = target.parent_path();
        if (!isNormalAbsolutePath(target, true)) {
            reason = "request.json carries an export target that is not an absolute normal path";
            return {};
        }
        return target;
    }

    // Every check the cleanup record must pass. `tempDirectory` is only assigned
    // when the result is true.
    bool validateCleanupRecord(const Json& record, const std::filesystem::path& requested,
                               std::filesystem::path& tempDirectory, std::string& reason) const {
        for (const char* key : { "version", "marker", "token", "jobRoot",
                                 "outputParent", "tempDirectory", "target" })
            if (!record.contains(key)) {
                reason = std::string("cleanup.json is missing \"") + key + "\"";
                return false;
            }
        if (!record["version"].is_number_integer() || record["version"].get<long long>() != 1) {
            reason = "cleanup.json has an unsupported version";
            return false;
        }
        if (!record["marker"].is_string() || record["marker"].get_ref<const std::string&>() != kCleanupMarker) {
            reason = "cleanup.json carries no known marker";
            return false;
        }
        if (!record["token"].is_string() || !isCleanupToken(record["token"].get_ref<const std::string&>())) {
            reason = "cleanup.json carries no 32 character lowercase hex token";
            return false;
        }
        for (const char* key : { "jobRoot", "outputParent", "tempDirectory", "target" })
            if (!record[key].is_string() || record[key].get_ref<const std::string&>().empty()) {
                reason = std::string("cleanup.json \"") + key + "\" must be a non-empty string";
                return false;
            }

        const std::string token = record["token"].get<std::string>();
        const auto jobRoot = std::filesystem::u8path(record["jobRoot"].get<std::string>());
        const auto outputParent = std::filesystem::u8path(record["outputParent"].get<std::string>());
        const auto temp = std::filesystem::u8path(record["tempDirectory"].get<std::string>());
        const auto target = std::filesystem::u8path(record["target"].get<std::string>());

        const std::wstring expectedName = std::wstring(kCleanupDirectoryPrefix) +
            std::wstring(token.begin(), token.end());
        if (!isNormalAbsolutePath(temp, true) || temp.filename().wstring() != expectedName) {
            reason = "the recorded directory is not \"physics-export-\" plus the recorded token";
            return false;
        }
        if (!isNormalAbsolutePath(outputParent, false) || !isNormalAbsolutePath(target, true)) {
            reason = "the recorded output parent or target is not an absolute normal path";
            return false;
        }
        if (!samePath(temp.parent_path(), outputParent)) {
            reason = "the recorded directory is not a direct child of the recorded output parent";
            return false;
        }
        if (!samePath(outputParent, requested.parent_path())) {
            reason = "the recorded output parent is not the parent of the requested export target";
            return false;
        }
        if (!samePath(target, requested)) {
            reason = "the recorded target is not the requested export target";
            return false;
        }
        if (samePath(target, temp) || isInside(target, temp)) {
            reason = "the requested export target is the recorded directory or inside it";
            return false;
        }
        std::error_code error;
        const auto absoluteFolder = std::filesystem::absolute(folder_, error);
        if (error || !samePath(jobRoot, absoluteFolder)) {
            reason = "cleanup.json does not belong to this run folder";
            return false;
        }
        tempDirectory = temp;
        return true;
    }

    // Attributes of the entry itself: FILE_FLAG_OPEN_REPARSE_POINT makes a
    // symlink or junction report its own attributes instead of its target's.
    static bool queryEntryAttributes(const std::filesystem::path& path, DWORD& attributes) {
        HANDLE raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return false;
        Handle entry(raw);
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (!GetFileInformationByHandleEx(entry.get(), FileAttributeTagInfo, &info, sizeof(info))) return false;
        attributes = info.FileAttributes;
        return true;
    }

    // Deletes the contents of `parentDirectory` without ever following a reparse
    // point and without ever leaving that directory: every path is built from a
    // name FindFirstFileW returned. Returns false when something could not be
    // removed.
    static bool removeSubtree(const std::filesystem::path& parentDirectory, CleanupBudget& budget) {
        WIN32_FIND_DATAW data{};
        const auto pattern = (parentDirectory / L"*").wstring();
        HANDLE raw = FindFirstFileW(pattern.c_str(), &data);
        if (raw == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND; // empty directory
        FindHandle search(raw);
        // One removal that remembers *why* it failed: a sharing violation is the
        // only failure the caller retries, so it must not be lost in a bool.
        const auto remove = [&budget](const wchar_t* path, bool directory) {
            const BOOL removed = directory ? RemoveDirectoryW(path) : DeleteFileW(path);
            if (removed) return true;
            if (GetLastError() == ERROR_SHARING_VIOLATION) budget.sharing = true;
            return false;
        };
        bool complete = true;
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos) {
                complete = false;
                continue;
            }
            if (budget.remaining == 0) {
                budget.exhausted = true;
                return false;
            }
            --budget.remaining;
            const auto child = parentDirectory / name;
            DWORD attributes = 0;
            if (!queryEntryAttributes(child, attributes)) { complete = false; continue; }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                // Remove the link itself; never its target.
                if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    if (!remove(child.c_str(), true)) complete = false;
                } else if (!remove(child.c_str(), false)) {
                    complete = false;
                }
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!removeSubtree(child, budget)) complete = false;
                if (!remove(child.c_str(), true)) complete = false;
                if (budget.exhausted) return false;
            } else {
                if ((attributes & FILE_ATTRIBUTE_READONLY) != 0)
                    SetFileAttributesW(child.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
                if (!remove(child.c_str(), false)) complete = false;
            }
        } while (FindNextFileW(search.get(), &data));
        return complete;
    }

    // True only when the directory carries the worker's ownership marker and it
    // agrees with the record. readBoundedFile rejects anything that is not a
    // regular file, so a symlink, junction or device name is never accepted as a
    // marker.
    static bool verifyOwnershipMarker(const std::filesystem::path& recordedDirectory, const Json& record,
                                      std::string& reason) {
        std::string text;
        const FileRead read = readBoundedFile(recordedDirectory / kOwnershipFileName, kMaxOwnershipBytes, text, reason);
        if (read == FileRead::missing) {
            reason = "the recorded directory carries no ownership marker";
            return false;
        }
        if (read == FileRead::failed) return false;
        // readBoundedFile already refused symlinks and junctions; a file that is
        // itself a reparse point of another kind (a placeholder) is refused too.
        DWORD attributes = 0;
        if (!queryEntryAttributes(recordedDirectory / kOwnershipFileName, attributes) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            reason = "the ownership marker is not a plain file";
            return false;
        }
        const Json marker = Json::parse(text, nullptr, false);
        if (marker.is_discarded() || !marker.is_object()) {
            reason = "the ownership marker is not a JSON object";
            return false;
        }
        if (!marker.contains("version") || !marker["version"].is_number_integer() ||
            marker["version"].get<long long>() != 1) {
            reason = "the ownership marker has an unsupported version";
            return false;
        }
        if (!marker.contains("marker") || !marker["marker"].is_string() ||
            marker["marker"].get_ref<const std::string&>() != kOwnershipMarker) {
            reason = "the ownership marker carries no known marker";
            return false;
        }
        for (const char* key : { "token", "jobRoot", "target" })
            if (!marker.contains(key) || !marker[key].is_string() ||
                marker[key].get_ref<const std::string&>().empty()) {
                reason = std::string("the ownership marker \"") + key + "\" must be a non-empty string";
                return false;
            }
        if (marker.value("token", std::string()) != record.value("token", std::string())) {
            reason = "the ownership marker does not carry the recorded token";
            return false;
        }
        if (!samePath(std::filesystem::u8path(marker.value("jobRoot", std::string())),
                      std::filesystem::u8path(record.value("jobRoot", std::string())))) {
            reason = "the ownership marker does not belong to this run folder";
            return false;
        }
        if (!samePath(std::filesystem::u8path(marker.value("target", std::string())),
                      std::filesystem::u8path(record.value("target", std::string())))) {
            reason = "the ownership marker does not name the recorded target";
            return false;
        }
        return true;
    }

    // Returns true only when the recorded directory and everything below it is
    // gone. A directory that is itself a reparse point is never removed: its name
    // may have been stolen, and removing it could delete through the link.
    static bool removeRecordedDirectory(const std::filesystem::path& recordedDirectory, const Json& record,
                                        std::string& reason) {
        DWORD attributes = 0;
        if (!queryEntryAttributes(recordedDirectory, attributes)) {
            reason = "the recorded directory cannot be inspected";
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            reason = "the recorded directory is not a directory";
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            reason = "the recorded directory is a reparse point (symlink or junction)";
            return false;
        }
        if (!verifyOwnershipMarker(recordedDirectory, record, reason)) return false;
        // The walk below runs exactly once, while the ownership marker still proves
        // the directory is ours: re-walking after that marker was deleted could
        // follow a reparse point swapped in behind our back. Only the final
        // RemoveDirectoryW - the step the evidence shows a just-terminated process
        // can still block on an already empty directory - is retried, and only for
        // ERROR_SHARING_VIOLATION.
        CleanupBudget budget;
        if (!removeSubtree(recordedDirectory, budget)) {
            if (budget.exhausted)
                reason = "the recorded directory holds more entries than the cleanup limit (" +
                    std::to_string(kMaxCleanupEntries) + ")";
            else if (budget.sharing)
                reason = "the recorded directory could not be removed completely (an entry is still open)";
            else
                reason = "the recorded directory could not be removed completely";
            return false;
        }
        for (unsigned attempt = 0;; ++attempt) {
            if (RemoveDirectoryW(recordedDirectory.c_str())) return true;
            const DWORD failure = GetLastError();
            // Another actor removed it between the walk and this call.
            if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) return true;
            if (failure != ERROR_SHARING_VIOLATION || attempt + 1 >= kCleanupRemoveAttempts) {
                reason = describe("RemoveDirectoryW", failure);
                return false;
            }
            // The directory is empty and proven ours; the process that still holds
            // it open needs a few more milliseconds.
            Sleep(kCleanupRemoveRetryMs);
        }
    }

    void publishCleanup(const char* status, const std::filesystem::path& recordedDirectory, const std::string& reason) {
        ensureStateObject();
        Json payload = Json::object();
        payload["status"] = status;
        if (!recordedDirectory.empty()) payload["directory"] = recordedDirectory.u8string();
        if (!reason.empty()) payload["reason"] = clip(reason);
        state["cleanup"] = std::move(payload);
    }

    // Drops the record once the directory it described is gone (or was never
    // there). Only files inside this job's own run folder.
    void discardCleanupRecord() noexcept {
        DeleteFileW((folder_ / kCleanupFileName).c_str());
        DeleteFileW((folder_ / kCleanupTemporaryName).c_str());
    }

    // Gives every process of this job a bounded chance to exit, so no file is
    // deleted while a child that was just terminated still holds a handle to it.
    // Kills nothing and never waits longer than timeoutMs. Returns true only when
    // the job object positively reported zero active processes: a timeout and a
    // failed query both return false, because "not confirmed gone" must never be
    // treated as "gone" by a caller that is about to delete files.
    bool waitForTreeExit(unsigned timeoutMs) noexcept {
        if (!job_) return false;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
            if (!QueryInformationJobObject(job_.get(), JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr))
                return false;
            if (info.ActiveProcesses == 0) return true;
            if (GetTickCount64() >= deadline) return false;
            Sleep(5);
        }
    }

    // Removes the leftover temporary directory this job recorded, if any. Best
    // effort by design: it never throws, never scans a directory and never
    // touches a path the record does not prove it may touch. `treeExited` is the
    // verdict of waitForTreeExit(): without a confirmed exit nothing is deleted
    // and the record is kept, so a live worker never loses files under its feet.
    void cleanupManagedTemp(bool treeExited) noexcept {
        if (folder_.empty()) return;
        try {
            const auto recordPath = folder_ / kCleanupFileName;
            std::string text;
            std::string reason;
            const FileRead read = readBoundedFile(recordPath, kMaxCleanupBytes, text, reason);
            if (read == FileRead::missing) return; // no record: nothing is deleted
            if (read == FileRead::failed) {
                publishCleanup("kept", std::filesystem::path(), reason);
                return;
            }
            const Json record = Json::parse(text, nullptr, false);
            if (record.is_discarded() || !record.is_object()) {
                publishCleanup("kept", std::filesystem::path(), "cleanup.json is not a JSON object");
                return;
            }
            const auto requested = expectedTarget_;
            if (requested.empty()) {
                publishCleanup("kept", std::filesystem::path(),
                               expectedTargetReason_.empty()
                                   ? "the run folder had no usable request.json export target when the job started"
                                   : expectedTargetReason_);
                return;
            }
            std::filesystem::path recordedDirectory;
            if (!validateCleanupRecord(record, requested, recordedDirectory, reason)) {
                publishCleanup("kept", recordedDirectory, reason);
                return;
            }
            if (!treeExited) {
                // The directory may still be in use: keep it, keep the record and
                // say why, instead of deleting under a process that never left.
                publishCleanup("kept", recordedDirectory,
                               "the worker process tree did not confirm that every process exited; the recorded "
                               "directory is kept");
                return;
            }
            std::error_code error;
            const auto status = std::filesystem::symlink_status(recordedDirectory, error);
            if (error && error != std::errc::no_such_file_or_directory) {
                publishCleanup("kept", recordedDirectory, "the recorded directory cannot be inspected: " + error.message());
                return;
            }
            if (error || status.type() == std::filesystem::file_type::not_found) {
                // Nothing to delete; the directory is already gone, which is the
                // normal outcome when the worker cleaned up and then died before
                // it could drop the record.
                discardCleanupRecord();
                publishCleanup("nothing", recordedDirectory, "the recorded directory no longer exists");
                return;
            }
            if (!removeRecordedDirectory(recordedDirectory, record, reason)) {
                publishCleanup("kept", recordedDirectory, reason);
                return;
            }
            discardCleanupRecord();
            publishCleanup("removed", recordedDirectory, reason);
        } catch (const std::exception& exception) {
            // Keeping the directory and saying why is always better than
            // deleting something this code could not fully check.
            try {
                publishCleanup("kept", std::filesystem::path(),
                               std::string("the cleanup record could not be read: ") + exception.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                publishCleanup("kept", std::filesystem::path(), "the cleanup record could not be read");
            } catch (...) {
            }
        }
    }

    // CommandLineToArgvW-compatible quoting: empty arguments become "", and
    // backslashes are doubled before a quote or the closing quote.
    static std::wstring quoteArgument(const std::wstring& argument) {
        if (argument.empty()) return L"\"\"";
        if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
        std::wstring result;
        result.reserve(argument.size() + 2);
        result.push_back(L'"');
        std::size_t backslashes = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') { ++backslashes; continue; }
            if (character == L'"') {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'"');
        return result;
    }

    static std::string describe(const char* call, DWORD error) {
        return std::string(call) + " failed (Win32 error " + std::to_string(static_cast<unsigned long>(error)) + ")";
    }

    static std::string clip(std::string text) {
        constexpr std::size_t kMaxErrorBytes = 4096;
        if (text.size() > kMaxErrorBytes) {
            text.resize(kMaxErrorBytes);
            text += "...";
        }
        return text;
    }
};

inline BackgroundJob::~BackgroundJob() noexcept {
    terminateTree();
    // The tree verdict is saved before this object lets go of its own process and
    // job handles: a handle this process still holds must never keep the directory
    // about to be deleted alive, so cleanup runs last.
    const bool treeExited = waitForTreeExit(kKillExitWaitMs);
    closeRun();
    cleanupManagedTemp(treeExited);
}

inline void BackgroundJob::start(const std::vector<std::wstring>& args, const std::filesystem::path& folder, unsigned timeoutSeconds) {
    if (running()) throw std::runtime_error("BackgroundJob: a job is already running; poll() it to completion first");
    if (args.empty() || args.front().empty()) throw std::runtime_error("BackgroundJob: args[0] must be the executable path");
    if (folder.empty()) throw std::runtime_error("BackgroundJob: the run folder must not be empty");

    std::error_code error;
    const bool folderExists = std::filesystem::exists(folder, error);
    if (error) throw std::runtime_error("BackgroundJob: cannot inspect the run folder: " + error.message());
    if (folderExists) {
        if (!std::filesystem::is_directory(folder, error) || error)
            throw std::runtime_error("BackgroundJob: the run folder exists but is not a directory");
    } else if (!std::filesystem::create_directories(folder, error) || error) {
        throw std::runtime_error("BackgroundJob: cannot create the run folder: " + error.message());
    }

    const auto resultPath = folder / L"result.json";
    const auto logPath = folder / L"worker.log";
    const bool staleResult = std::filesystem::exists(resultPath, error);
    if (error) throw std::runtime_error("BackgroundJob: cannot inspect result.json: " + error.message());
    if (staleResult)
        throw std::runtime_error("BackgroundJob: the run folder already contains result.json; use a fresh folder so a stale result cannot be mistaken for this run");

    // A cleanup record left over from an earlier run must never be acted on by
    // this job: it could describe a directory this job does not own.
    const auto staleCleanup = std::filesystem::symlink_status(folder / kCleanupFileName, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("BackgroundJob: cannot inspect cleanup.json: " + error.message());
    if (!error && staleCleanup.type() != std::filesystem::file_type::not_found)
        throw std::runtime_error("BackgroundJob: the run folder already contains cleanup.json; use a fresh folder so a stale cleanup record cannot delete an unrelated directory");

    // Read the export target once, now, while request.json is still the file this
    // process wrote: once the worker runs it may rewrite that file, so cleanup
    // must compare a record against this cached value and never re-read it.
    std::string expectedTargetReason;
    const std::filesystem::path expectedTarget = requestedExportTarget(folder, expectedTargetReason);

    // Inheritable handles only for the two standard handles the child needs.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr));
    if (!nul) throw std::runtime_error(describe("CreateFileW(NUL)", GetLastError()));

    // CREATE_NEW (not CREATE_ALWAYS) so an existing log is never truncated or
    // deleted; a collision is reported instead.
    Handle log(CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_FILE_EXISTS || failure == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("BackgroundJob: the run folder already contains worker.log; use a fresh folder");
        throw std::runtime_error(describe("CreateFileW(worker.log)", failure));
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) throw std::runtime_error(describe("CreateJobObjectW", GetLastError()));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error(describe("SetInformationJobObject", GetLastError()));

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = nul.get();
    startup.StartupInfo.hStdOutput = log.get();
    startup.StartupInfo.hStdError = log.get();

    // Explicit handle whitelist: only NUL and worker.log cross the process
    // boundary, so unrelated inheritable handles in the editor are not leaked.
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes); // reports the required size
    if (attributeBytes == 0)
        throw std::runtime_error(describe("InitializeProcThreadAttributeList", GetLastError()));
    std::vector<unsigned char> attributeBuffer(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes))
        throw std::runtime_error(describe("InitializeProcThreadAttributeList", GetLastError()));
    struct AttributeListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
        ~AttributeListGuard() { if (list != nullptr) DeleteProcThreadAttributeList(list); }
    } attributeGuard;
    attributeGuard.list = attributeList;

    HANDLE inheritedHandles[2] = { nul.get(), log.get() };
    if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr))
        throw std::runtime_error(describe("UpdateProcThreadAttribute", GetLastError()));
    startup.lpAttributeList = attributeList;

    std::wstring commandLine;
    for (std::size_t index = 0; index < args.size(); ++index) {
        if (index != 0) commandLine.push_back(L' ');
        commandLine += quoteArgument(args[index]);
    }

    // CREATE_SUSPENDED: the child cannot run before it is inside the job.
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(args.front().c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
            reinterpret_cast<LPSTARTUPINFOW>(&startup), &processInfo))
        throw std::runtime_error(describe("CreateProcessW", GetLastError()));
    Handle processHandle(processInfo.hProcess);
    Handle threadHandle(processInfo.hThread);

    // Kills a still suspended/resumed child if any later step throws, so a failed
    // start can never leave an orphan process behind.
    struct ChildGuard {
        HANDLE process = nullptr;
        HANDLE job = nullptr;
        bool disarmed = false;
        ~ChildGuard() {
            if (disarmed) return;
            if (job != nullptr) TerminateJobObject(job, 1);
            if (process != nullptr) TerminateProcess(process, 1);
        }
    } childGuard;
    childGuard.process = processHandle.get();
    childGuard.job = job.get();

    if (!AssignProcessToJobObject(job.get(), processHandle.get()))
        throw std::runtime_error(describe("AssignProcessToJobObject", GetLastError()));
    if (ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1))
        throw std::runtime_error(describe("ResumeThread", GetLastError()));
    threadHandle.reset();

    folder_ = folder;
    directory = folder;
    expectedTarget_ = expectedTarget;
    expectedTargetReason_ = expectedTargetReason;
    startedAt_ = GetTickCount64();
    timeoutSeconds_ = timeoutSeconds;
    lastProgressRead_ = 0;
    state = Json{{"status","running"}};
    process_ = std::move(processHandle);
    job_ = std::move(job);
    childGuard.disarmed = true;
}

inline bool BackgroundJob::poll() {
    if (!running()) return false;

    refreshProgress(false);

    const DWORD wait = WaitForSingleObject(process_.get(), 0);
    if (wait == WAIT_TIMEOUT) {
        if (timeoutSeconds_ != 0) {
            const ULONGLONG limit = static_cast<ULONGLONG>(timeoutSeconds_) * 1000ull;
            if (GetTickCount64() - startedAt_ >= limit) {
                terminateTree();
                const bool treeExited = waitForTreeExit(kKillExitWaitMs);
                closeRun();
                cleanupManagedTemp(treeExited);
                markFailed("worker exceeded the " + std::to_string(timeoutSeconds_) + " s timeout", nullptr);
                return true;
            }
        }
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        terminateTree();
        const bool treeExited = waitForTreeExit(kKillExitWaitMs);
        closeRun();
        cleanupManagedTemp(treeExited);
        markFailed(describe("WaitForSingleObject", wait), nullptr);
        return true;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process_.get(), &exitCode)) {
        const DWORD failure = GetLastError();
        terminateTree();
        const bool treeExited = waitForTreeExit(kKillExitWaitMs);
        closeRun();
        cleanupManagedTemp(treeExited);
        markFailed(describe("GetExitCodeProcess", failure), nullptr);
        return true;
    }

    // Last chance to capture the worker's final progress (for example the
    // stage="done" sample a very short job publishes just before exiting).
    refreshProgress(true);

    try {
        finish(exitCode);
    } catch (const std::exception& exception) {
        markFailed(std::string("cannot read the worker result: ") + exception.what(), nullptr);
    } catch (...) {
        markFailed("cannot read the worker result", nullptr);
    }
    // A worker that exited on its own may still have left descendants in the job
    // (and, if it died abnormally, a temporary directory behind). The tree verdict
    // is saved first, then this job's own handles are closed (which also kills any
    // straggler through KILL_ON_JOB_CLOSE), and only then is the recorded leftover
    // directory deleted: a handle this process still holds must not keep it alive.
    const bool treeExited = waitForTreeExit(kReapExitWaitMs);
    closeRun();
    cleanupManagedTemp(treeExited);
    return true;
}

inline void BackgroundJob::cancel() {
    if (!running()) return;
    terminateTree();
    const bool treeExited = waitForTreeExit(kKillExitWaitMs);
    // Killing the process tree also skips the worker's own cleanup. Its record is
    // acted on only after this job released its own handles, so neither the
    // process handle nor the job handle can keep the directory alive.
    closeRun();
    cleanupManagedTemp(treeExited);
    // A cancelled job keeps the last progress it published, with the terminal
    // "cancelled" state published on top of it, so the editor never shows a
    // running stage next to a cancelled status.
    refreshProgress(true);
    ensureStateObject();
    state["status"] = "cancelled";
    syncTerminalProgress("cancelled", std::string());
}

} // namespace physics_editor

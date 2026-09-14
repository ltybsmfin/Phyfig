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
//  * start() replaces state with {"status":"running"}. Afterwards the caller may
//    attach task metadata (id, scope, object, output, ...); poll() and cancel()
//    only update status/result/error, so that metadata survives to the terminal
//    state.
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

    Handle process_, thread_, job_;
    std::filesystem::path folder_;
    ULONGLONG startedAt_ = 0;
    unsigned timeoutSeconds_ = 0;

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
    }

    void markFailed(const std::string& message, const Json* result) {
        ensureStateObject();
        state["status"] = "failed";
        state["error"] = clip(message);
        if (result != nullptr) state["result"] = *result;
        else state.erase("result");
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
            if (parsedOk && parsed.is_object() && parsed.contains("error") && parsed["error"].is_string())
                message += ": " + parsed["error"].get<std::string>();
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
        std::string message = "worker reported failure";
        if (parsed.contains("error") && parsed["error"].is_string()) message = parsed["error"].get<std::string>();
        markFailed(message, &parsed);
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
    closeRun();
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
    startedAt_ = GetTickCount64();
    timeoutSeconds_ = timeoutSeconds;
    state = Json{{"status","running"}};
    process_ = std::move(processHandle);
    job_ = std::move(job);
    childGuard.disarmed = true;
}

inline bool BackgroundJob::poll() {
    if (!running()) return false;

    const DWORD wait = WaitForSingleObject(process_.get(), 0);
    if (wait == WAIT_TIMEOUT) {
        if (timeoutSeconds_ != 0) {
            const ULONGLONG limit = static_cast<ULONGLONG>(timeoutSeconds_) * 1000ull;
            if (GetTickCount64() - startedAt_ >= limit) {
                terminateTree();
                closeRun();
                markFailed("worker exceeded the " + std::to_string(timeoutSeconds_) + " s timeout", nullptr);
                return true;
            }
        }
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        terminateTree();
        closeRun();
        markFailed(describe("WaitForSingleObject", wait), nullptr);
        return true;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process_.get(), &exitCode)) {
        const DWORD failure = GetLastError();
        terminateTree();
        closeRun();
        markFailed(describe("GetExitCodeProcess", failure), nullptr);
        return true;
    }

    try {
        finish(exitCode);
    } catch (const std::exception& exception) {
        markFailed(std::string("cannot read the worker result: ") + exception.what(), nullptr);
    } catch (...) {
        markFailed("cannot read the worker result", nullptr);
    }
    closeRun();
    return true;
}

inline void BackgroundJob::cancel() {
    if (!running()) return;
    terminateTree();
    closeRun();
    ensureStateObject();
    state["status"] = "cancelled";
}

} // namespace physics_editor

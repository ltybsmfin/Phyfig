// background_job_test.cpp - manual test for BackgroundJob.
//
// This file is intentionally NOT part of apps/editor/CMakeLists.txt. To run it,
// add a console executable target that links physics_document (which publishes
// the document.hpp/json.hpp include directories), for example:
//
//   add_executable(physics_background_job_test background_job_test.cpp)
//   target_link_libraries(physics_background_job_test PRIVATE physics_document)
//   target_compile_options(physics_background_job_test PRIVATE /W4 /utf-8)  # MSVC
//   add_test(NAME editor_background_job COMMAND physics_background_job_test)
//
// The test re-executes its own binary in a hidden "--worker" mode, so it needs
// no external helper executable. It writes only under a task-owned folder in
// %TEMP% and terminates every worker it starts. Cleanup canonicalizes the root
// and refuses to delete anything that is not a direct, marker-named child of
// %TEMP%, so a symlink or a mistyped path can never delete an unrelated folder.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "background_job.hpp"

using physics_editor::BackgroundJob;
using physics_editor::Json;

namespace {

void check(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

std::wstring modulePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) throw std::runtime_error("GetModuleFileNameW failed");
    return std::wstring(buffer.data(), length);
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("WideCharToMultiByte failed");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write " + path.u8string());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) throw std::runtime_error("cannot finish writing " + path.u8string());
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + path.u8string());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// Non-blocking pump: mirrors the editor's UI loop.
bool pump(BackgroundJob& job, unsigned timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (job.poll()) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(10);
    }
}

DWORD waitForPidFile(const std::filesystem::path& path, unsigned timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) {
            std::ifstream stream(path);
            DWORD pid = 0;
            if (stream >> pid) return pid;
        }
        Sleep(10);
    }
    throw std::runtime_error("worker did not report its pid file: " + path.u8string());
}

bool processAlive(DWORD pid) {
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_INVALID_PARAMETER) return false; // no such process
        throw std::runtime_error("OpenProcess failed with Win32 error " + std::to_string(static_cast<unsigned long>(failure)));
    }
    const DWORD wait = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return wait == WAIT_TIMEOUT;
}

bool waitUntilDead(DWORD pid, unsigned timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (!processAlive(pid)) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(20);
    }
}

// ---------------------------------------------------------------------------
// Child worker mode. argv[1] == "--worker", argv[2] == mode, argv[3] == folder.
// ---------------------------------------------------------------------------
int workerMain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    const std::wstring mode = argv[2];
    const std::filesystem::path folder = argv[3];
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const auto resultPath = folder / L"result.json";

    if (mode == L"echo") {
        std::uint64_t stdinBytes = 0;
        char buffer[4096];
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input != nullptr && input != INVALID_HANDLE_VALUE) {
            for (;;) {
                DWORD read = 0;
                if (!ReadFile(input, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) || read == 0) break;
                stdinBytes += read;
            }
        }
        std::cout << "stdout-marker\n";
        std::cerr << "stderr-marker\n";
        std::cout.flush();
        std::cerr.flush();

        Json payload = Json::array();
        for (int index = 4; index < argc; ++index) payload.push_back(toUtf8(argv[index]));
        writeText(resultPath, Json{{"ok",true},{"result",Json{{"args",payload},{"stdinBytes",stdinBytes}}}}.dump());
        return 0;
    }
    if (mode == L"fail") {
        writeText(resultPath, Json{{"ok",false},{"error","intentional failure"}}.dump());
        return 0;
    }
    if (mode == L"garbage") {
        writeText(resultPath, "this is not json");
        return 0;
    }
    if (mode == L"exit") {
        return argc >= 5 ? std::stoi(std::wstring(argv[4])) : 3;
    }
    if (mode == L"oversize") {
        writeText(resultPath, std::string(8u * 1024u * 1024u + 16u, 'x'));
        return 0;
    }
    if (mode == L"hang") {
        writeText(folder / L"pid", std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
        for (;;) Sleep(1000);
    }
    if (mode == L"tree") {
        writeText(folder / L"pid", std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
        const auto grandchildFolder = folder / L"grandchild";
        std::filesystem::create_directories(grandchildFolder, error);
        std::wstring commandLine = L"\"" + modulePath() + L"\" --worker hang \"" + grandchildFolder.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo))
            throw std::runtime_error("tree worker cannot spawn a grandchild");
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        for (;;) Sleep(1000);
    }
    return 2;
}

// Deletes only a directory this test created: it must canonicalize to a direct
// child of %TEMP% whose name carries the task-owned marker. Anything else is
// left untouched.
void removeTaskOwnedDirectory(const std::filesystem::path& folder) {
    const std::wstring marker = L"physics_background_job_taskowned_";
    std::error_code error;
    const auto tempPath = std::filesystem::temp_directory_path(error);
    if (error) throw std::runtime_error("cannot resolve %TEMP%: " + error.message());
    const auto temp = std::filesystem::weakly_canonical(tempPath, error);
    if (error) throw std::runtime_error("cannot canonicalize %TEMP%: " + error.message());
    const auto target = std::filesystem::weakly_canonical(folder, error);
    if (error) throw std::runtime_error("cannot resolve the test root: " + error.message());
    if (target.parent_path() != temp) return;
    const std::wstring name = target.filename().wstring();
    if (name.compare(0, marker.size(), marker) != 0) return;
    std::filesystem::remove_all(target, error);
}

int runTests() {
    const auto root = std::filesystem::temp_directory_path() / (L"physics_background_job_taskowned_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    removeTaskOwnedDirectory(root);
    std::filesystem::create_directories(root, error);
    check(!error, "cannot create the test root: " + error.message());

    unsigned counter = 0;
    const auto fresh = [&](const std::wstring& name) {
        return root / (name + L"_" + std::to_wstring(++counter));
    };
    const std::wstring self = modulePath();

    // --- idle invariants ---------------------------------------------------
    {
        BackgroundJob job;
        check(job.state["status"] == "idle", "a new job must start idle");
        check(!job.running(), "a new job must not be running");
        check(!job.poll(), "poll on an idle job must return false");
        job.cancel();
        check(job.state["status"] == "idle", "cancel on an idle job must not change state");
    }

    // --- argument validation ----------------------------------------------
    {
        BackgroundJob job;
        bool threw = false;
        try { job.start({}, fresh(L"no_args"), 5); } catch (const std::exception&) { threw = true; }
        check(threw, "start with no args must throw");
        check(!job.running(), "a rejected start must not run anything");

        threw = false;
        try { job.start({L""}, fresh(L"empty_exe"), 5); } catch (const std::exception&) { threw = true; }
        check(threw, "start with an empty executable must throw");

        threw = false;
        try { job.start({self}, std::filesystem::path(), 5); } catch (const std::exception&) { threw = true; }
        check(threw, "start with an empty run folder must throw");
    }

    // --- missing executable ------------------------------------------------
    {
        BackgroundJob job;
        bool threw = false;
        try { job.start({(root / L"missing" / L"missing.exe").wstring()}, fresh(L"missing_exe"), 5); }
        catch (const std::exception&) { threw = true; }
        check(threw, "start with a missing executable must throw");
        check(!job.running(), "a failed CreateProcess must not leave a running job");
    }

    // --- stale run folder --------------------------------------------------
    {
        const auto folder = fresh(L"stale_result");
        std::filesystem::create_directories(folder, error);
        writeText(folder / L"result.json", "{\"ok\":true}");
        BackgroundJob job;
        bool threw = false;
        try { job.start({self, L"--worker", L"hang", folder.wstring()}, folder, 5); }
        catch (const std::exception&) { threw = true; }
        check(threw, "start must refuse a pre-existing result.json");
        check(!job.running(), "a refused start must not launch a process");
    }
    {
        const auto folder = fresh(L"stale_log");
        std::filesystem::create_directories(folder, error);
        writeText(folder / L"worker.log", "stale");
        BackgroundJob job;
        bool threw = false;
        try { job.start({self, L"--worker", L"hang", folder.wstring()}, folder, 5); }
        catch (const std::exception&) { threw = true; }
        check(threw, "start must refuse a pre-existing worker.log");
        check(!job.running(), "a refused start must not launch a process");
    }

    // --- successful run: quoting, redirection, NUL stdin, result envelope --
    {
        const auto folder = fresh(L"echo");
        const std::vector<std::wstring> payload = {
            L"plain",
            L"has space",
            L"tab\there",
            L"quote\"inside",
            L"trailing\\",
            L"double\\\\",
            L"ends with quote\"",
            L"",
            L"混合中文 空格",
            L"\\\\server\\share\\path with space\\",
        };
        std::vector<std::wstring> args = {self, L"--worker", L"echo", folder.wstring()};
        args.insert(args.end(), payload.begin(), payload.end());

        BackgroundJob job;
        job.start(args, folder, 30);
        check(job.running(), "the job must run right after start");
        check(job.directory == folder, "start must record the run folder");
        check(job.state["status"] == "running", "start must publish status running");

        // The editor attaches task metadata after start(); it must survive.
        job.state["id"] = "task-echo";
        job.state["scope"] = "scene";
        job.state["object"] = "ball";
        job.state["output"] = folder.u8string();

        bool threw = false;
        try { job.start(args, fresh(L"echo_again"), 30); } catch (const std::exception&) { threw = true; }
        check(threw, "starting a second job while one runs must throw");
        check(job.state["id"] == "task-echo", "a rejected start must not clear caller metadata");

        check(pump(job, 20000), "the echo worker did not finish in time");
        check(job.state["status"] == "completed", "the echo worker must complete");
        check(job.state["id"] == "task-echo", "completion must preserve caller metadata");
        check(job.state["scope"] == "scene", "completion must preserve caller metadata");
        check(job.state["output"] == folder.u8string(), "completion must preserve the output path");
        check(!job.running(), "a completed job must not report running");
        check(!job.poll(), "poll after completion must return false");

        Json expected = Json::array();
        for (const auto& item : payload) expected.push_back(toUtf8(item));
        check(job.state["result"]["ok"] == true, "the result envelope must be stored in state.result");
        check(job.state["result"]["result"]["args"] == expected, "argument quoting round-trip failed");
        check(job.state["result"]["result"]["stdinBytes"] == 0, "worker stdin must be NUL");

        const std::string log = readText(folder / L"worker.log");
        check(log.find("stdout-marker") != std::string::npos, "stdout was not redirected to worker.log");
        check(log.find("stderr-marker") != std::string::npos, "stderr was not redirected to worker.log");
    }

    // --- worker reports ok:false ------------------------------------------
    {
        const auto folder = fresh(L"fail");
        BackgroundJob job;
        job.start({self, L"--worker", L"fail", folder.wstring()}, folder, 30);
        job.state["id"] = "task-fail";
        check(pump(job, 20000), "the failing worker did not finish in time");
        check(job.state["status"] == "failed", "ok:false must fail the job");
        check(job.state["error"] == "intentional failure", "the worker error text must be preserved");
        check(job.state["id"] == "task-fail", "failure must preserve caller metadata");
    }

    // --- non-zero exit code ------------------------------------------------
    {
        const auto folder = fresh(L"exit");
        BackgroundJob job;
        job.start({self, L"--worker", L"exit", folder.wstring(), L"7"}, folder, 30);
        check(pump(job, 20000), "the exiting worker did not finish in time");
        check(job.state["status"] == "failed", "a non-zero exit code must fail the job");
        check(job.state["error"].get<std::string>().find("7") != std::string::npos, "the exit code must be reported");
    }

    // --- invalid result.json ----------------------------------------------
    {
        const auto folder = fresh(L"garbage");
        BackgroundJob job;
        job.start({self, L"--worker", L"garbage", folder.wstring()}, folder, 30);
        check(pump(job, 20000), "the garbage worker did not finish in time");
        check(job.state["status"] == "failed", "invalid result.json must fail the job");
        check(job.state["error"].get<std::string>().find("JSON") != std::string::npos, "invalid JSON must be reported");
    }

    // --- result.json over the 8 MiB limit ---------------------------------
    {
        const auto folder = fresh(L"oversize");
        BackgroundJob job;
        job.start({self, L"--worker", L"oversize", folder.wstring()}, folder, 30);
        check(pump(job, 30000), "the oversize worker did not finish in time");
        check(job.state["status"] == "failed", "a result.json over 8 MiB must fail the job");
        check(job.state["error"].get<std::string>().find("8 MiB") != std::string::npos, "the size limit must be reported");
    }

    // --- cancel ------------------------------------------------------------
    {
        const auto folder = fresh(L"cancel");
        BackgroundJob job;
        job.start({self, L"--worker", L"hang", folder.wstring()}, folder, 0);
        job.state["id"] = "task-cancel";
        const DWORD pid = waitForPidFile(folder / L"pid", 10000);
        check(processAlive(pid), "the hanging worker must be alive before cancel");
        check(!job.poll(), "the hanging worker must not complete");
        job.cancel();
        check(job.state["status"] == "cancelled", "cancel must publish status cancelled");
        check(job.state["id"] == "task-cancel", "cancel must preserve caller metadata");
        check(!job.running(), "cancel must release the job");
        check(!job.poll(), "poll after cancel must return false");
        check(waitUntilDead(pid, 5000), "cancel must terminate the worker");
    }

    // --- poll-driven timeout ----------------------------------------------
    {
        const auto folder = fresh(L"timeout");
        BackgroundJob job;
        job.start({self, L"--worker", L"hang", folder.wstring()}, folder, 1);
        const DWORD pid = waitForPidFile(folder / L"pid", 10000);
        check(pump(job, 20000), "the timeout must be detected by poll");
        check(job.state["status"] == "failed", "a timeout must fail the job");
        check(job.state["error"].get<std::string>().find("timeout") != std::string::npos, "the timeout must be reported");
        check(waitUntilDead(pid, 5000), "a timeout must terminate the worker");
    }

    // --- whole process tree ------------------------------------------------
    {
        const auto folder = fresh(L"tree");
        BackgroundJob job;
        job.start({self, L"--worker", L"tree", folder.wstring()}, folder, 0);
        const DWORD child = waitForPidFile(folder / L"pid", 10000);
        const DWORD grandchild = waitForPidFile(folder / L"grandchild" / L"pid", 10000);
        check(processAlive(child) && processAlive(grandchild), "the tree worker and its grandchild must be alive");
        job.cancel();
        check(job.state["status"] == "cancelled", "cancel must publish status cancelled");
        check(waitUntilDead(child, 5000), "cancel must terminate the direct child");
        check(waitUntilDead(grandchild, 5000), "cancel must terminate the whole process tree");
    }

    // --- destructor --------------------------------------------------------
    {
        const auto folder = fresh(L"destructor");
        DWORD pid = 0;
        {
            BackgroundJob job;
            job.start({self, L"--worker", L"hang", folder.wstring()}, folder, 0);
            pid = waitForPidFile(folder / L"pid", 10000);
            check(processAlive(pid), "the destructor worker must be alive");
        }
        check(waitUntilDead(pid, 5000), "the destructor must terminate a running worker");
    }

    removeTaskOwnedDirectory(root);
    std::cout << "PASS: background job idle/validation/stale guards, metadata preservation, quoting, "
                 "redirection, NUL stdin, completed/failed/timeout/cancel, process tree kill and destructor cleanup\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring(argv[1]) == L"--worker") {
        try { return workerMain(argc, argv); }
        catch (const std::exception& exception) {
            std::cerr << "worker error: " << exception.what() << "\n";
            return 1;
        }
    }
    try { return runTests(); }
    catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << "\n";
        return 1;
    }
}

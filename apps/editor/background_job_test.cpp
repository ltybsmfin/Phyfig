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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "background_job.hpp"

// REPARSE_DATA_BUFFER and FSCTL_SET_REPARSE_POINT, for the junction fixture.
#include <winioctl.h>

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

// Publishes one complete JSON document the way the real workers do: write a
// temporary file, then atomically rename it over the target.
void publishProgress(const std::filesystem::path& folder, const physics_editor::Json& value) {
    const auto temporary = folder / L"progress.json.tmp";
    writeText(temporary, value.dump());
    if (!MoveFileExW(temporary.c_str(), (folder / L"progress.json").c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("worker cannot publish progress.json atomically");
}

// What state["progress"] currently shows, for history recording.
struct ProgressView {
    bool present = false;
    std::string stage;
    unsigned long long completed = 0;
    double overall = 0.0;
};

ProgressView progressOf(const BackgroundJob& job) {
    ProgressView view;
    if (!job.state.contains("progress") || !job.state["progress"].is_object()) return view;
    const auto& progress = job.state["progress"];
    view.present = true;
    view.stage = progress.value("stage", std::string());
    if (progress.contains("completedFrames")) view.completed = progress["completedFrames"].get<unsigned long long>();
    if (progress.contains("overallFraction")) view.overall = progress["overallFraction"].get<double>();
    return view;
}

// The cleanup result a job published, or an empty object when there is none, so a
// missing result is reported as a failed check instead of throwing.
Json cleanupOf(const Json& state) {
    if (!state.is_object() || !state.contains("cleanup")) return Json::object();
    const Json& cleanup = state.at("cleanup");
    return cleanup.is_object() ? cleanup : Json::object();
}

// What the cleanup worker reports once its directories and record exist.
struct CleanupLayout {
    std::string token;
    std::filesystem::path temp;
    std::filesystem::path keep;
};

CleanupLayout waitForLayout(const std::filesystem::path& folder, unsigned timeoutMs) {
    const auto path = folder / L"layout.json";
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) break;
        if (GetTickCount64() >= deadline)
            throw std::runtime_error("the cleanup worker did not report its layout");
        Sleep(10);
    }
    const Json value = Json::parse(readText(path), nullptr, false);
    if (value.is_discarded() || !value.is_object())
        throw std::runtime_error("the cleanup worker wrote an invalid layout.json");
    CleanupLayout layout;
    layout.token = value.value("token", std::string());
    layout.temp = std::filesystem::u8path(value.value("temp", std::string()));
    layout.keep = std::filesystem::u8path(value.value("keep", std::string()));
    return layout;
}

// ---------------------------------------------------------------------------
// Cleanup fixtures: the export worker renders into
// <output parent>/physics-export-<token>, records that directory in
// <run folder>/cleanup.json, and cannot clean up when the editor kills it.
// ---------------------------------------------------------------------------

// 32 lowercase hex characters, the shape the real worker's token has.
std::wstring randomToken() {
    static std::uint64_t state = 0x9e3779b97f4a7c15ull ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32) ^ GetTickCount64();
    const wchar_t* digits = L"0123456789abcdef";
    std::wstring token;
    token.reserve(32);
    for (int index = 0; index < 32; ++index) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        token.push_back(digits[(state >> 33) & 0xf]);
    }
    return token;
}

// The mount-point reparse payload. The Windows SDK ships REPARSE_DATA_BUFFER
// only in the WDK's ntifs.h, so the one layout this fixture needs (the buffer
// header followed by the mount-point union member's four offsets/lengths) is
// declared here instead of pulling in a driver header.
struct JunctionReparseHeader {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
};
static_assert(sizeof(JunctionReparseHeader) == 16, "unexpected mount-point reparse header layout");

// A directory junction is a mount-point reparse point. Unlike a symbolic link,
// creating one needs no privilege, so the "never follow a reparse point" guard
// can be exercised on a normal account.
bool createJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::create_directory(link, error);
    if (error) {
        std::cerr << "junction fixture: cannot create " << link.u8string() << ": " << error.message() << "\n";
        return false;
    }
    const std::wstring substitute = L"\\??\\" + std::filesystem::absolute(target).wstring();
    const std::wstring print = std::filesystem::absolute(target).wstring();
    // The four USHORTs plus both NUL-terminated names.
    const std::size_t namesBytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    const std::size_t reparseDataLength = 8 + namesBytes;
    // The input buffer must be exactly 8 + ReparseDataLength bytes: a larger one
    // is rejected with ERROR_INVALID_REPARSE_DATA, which is why the trailing
    // padding this payload is usually written with is absent here.
    std::vector<unsigned char> buffer(8 + reparseDataLength, 0);
    auto* header = reinterpret_cast<JunctionReparseHeader*>(buffer.data());
    header->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    header->ReparseDataLength = static_cast<USHORT>(reparseDataLength);
    header->Reserved = 0;
    header->SubstituteNameOffset = 0;
    header->SubstituteNameLength = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    header->PrintNameOffset = static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t));
    header->PrintNameLength = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    unsigned char* names = buffer.data() + sizeof(JunctionReparseHeader);
    std::memcpy(names, substitute.c_str(), header->SubstituteNameLength);
    std::memcpy(names + header->PrintNameOffset, print.c_str(), header->PrintNameLength);
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::cerr << "junction fixture: cannot open " << link.u8string()
                  << " (Win32 error " << GetLastError() << ")\n";
        std::filesystem::remove(link, error);
        return false;
    }
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    const DWORD failure = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        std::cerr << "junction fixture: FSCTL_SET_REPARSE_POINT failed (Win32 error " << failure << ")\n";
        std::filesystem::remove(link, error);
        return false;
    }
    return true;
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
    if (mode == L"progress") {
        // Keep each sample available until the parent has exercised repeated
        // polls. Wall-clock sleeps alone lose samples on a busy build machine.
        const unsigned steps = argc >= 5 ? static_cast<unsigned>(std::stoul(argv[4])) : 5;
        for (unsigned step = 1; step <= steps; ++step) {
            publishProgress(folder, Json{{"version",1},{"stage","render"},
                {"phase","render"},{"completedFrames",step},{"totalFrames",steps},
                {"elapsedSeconds",step * 0.5},{"renderSeconds",step * 0.5},
                {"readbackWriteSeconds",0.01},{"fps",2.0},
                {"estimatedRemainingSeconds",(steps - step) / 2.0},
                {"overallFraction",0.02 + 0.88 * static_cast<double>(step) / static_cast<double>(steps)}});
            Sleep(150);
            const auto acknowledged = folder / (L"ack-" + std::to_wstring(step));
            const ULONGLONG deadline = GetTickCount64() + 10000;
            while (!std::filesystem::exists(acknowledged)) {
                if (GetTickCount64() >= deadline)
                    throw std::runtime_error("parent did not acknowledge progress sample");
                Sleep(5);
            }
        }
        writeText(resultPath, Json{{"ok",true},{"result",Json{{"steps",steps}}}}.dump());
        return 0;
    }
    if (mode == L"progress_mix") {
        // A valid sample, then a corrupt file, then a sample that moves backwards
        // in stage, frames and fraction: only the valid samples may survive.
        publishProgress(folder, Json{{"version",1},{"stage","render"},{"completedFrames",1},
            {"totalFrames",3},{"overallFraction",0.3},{"fps",2.0},{"estimatedRemainingSeconds",1.0}});
        Sleep(500);
        writeText(folder / L"progress.json", "this is not json");
        Sleep(500);
        publishProgress(folder, Json{{"version",1},{"stage","prepare"},{"completedFrames",0},
            {"totalFrames",3},{"overallFraction",0.0}});
        Sleep(500);
        publishProgress(folder, Json{{"version",1},{"stage","render"},{"completedFrames",3},
            {"totalFrames",3},{"overallFraction",0.9},{"fps",2.0},{"estimatedRemainingSeconds",0.0}});
        Sleep(300);
        // A sample that omits the counters must not make the visible progress jump
        // back to zero: the last trustworthy values are carried forward.
        publishProgress(folder, Json{{"version",1},{"stage","render"}});
        Sleep(300);
        writeText(resultPath, Json{{"ok",true}}.dump());
        return 0;
    }
    if (mode == L"progress_done") {
        publishProgress(folder, Json{{"version",1},{"stage","done"},{"phase","done"},
            {"completedFrames",2},{"totalFrames",2},{"phaseFraction",1.0},{"overallFraction",1.0},
            {"estimatedRemainingSeconds",0.0}});
        writeText(resultPath, Json{{"ok",true}}.dump());
        return 0;
    }
    if (mode == L"progress_oversize") {
        writeText(folder / L"progress.json", std::string(64u * 1024u + 32u, 'x'));
        Sleep(600);
        publishProgress(folder, Json{{"version",1},{"stage","done"},{"completedFrames",2},
            {"totalFrames",2},{"overallFraction",1.0}});
        Sleep(200);
        writeText(resultPath, Json{{"ok",true}}.dump());
        return 0;
    }
    if (mode == L"progress_types") {
        // Wrongly typed fields (and an out-of-range fraction) must be dropped, not
        // published as measurements; an explicit null estimate is kept.
        publishProgress(folder, Json{{"version",1},{"stage","render"},{"completedFrames",1},
            {"totalFrames",2},{"elapsedSeconds",nullptr},{"fps","fast"},{"overallFraction",1.5},
            {"phaseFraction","half"},{"estimatedRemainingSeconds",nullptr}});
        Sleep(500);
        publishProgress(folder, Json{{"version",1},{"stage","done"},{"completedFrames",2},
            {"totalFrames",2},{"overallFraction",1.0}});
        Sleep(100);
        writeText(resultPath, Json{{"ok",true}}.dump());
        return 0;
    }
    if (mode == L"progress_hang") {
        writeText(folder / L"pid", std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
        publishProgress(folder, Json{{"version",1},{"stage","render"},{"completedFrames",4},
            {"totalFrames",10},{"overallFraction",0.4},{"fps",2.0},{"estimatedRemainingSeconds",3.0}});
        for (;;) Sleep(1000);
    }
    // The three "stale" workers deliberately never publish a terminal sample: a
    // real short export can exit before its own done/failed sample is read, so the
    // parent must publish the real terminal state from the exit code and
    // result.json on top of the last running sample instead of trusting it.
    if (mode == L"progress_stale_ok") {
        publishProgress(folder, Json{{"version",1},{"stage","encode"},{"phase","encode"},
            {"completedFrames",6},{"totalFrames",6},{"elapsedSeconds",1.0},{"phaseFraction",0.5},
            {"overallFraction",0.95},{"fps",6.0},{"estimatedRemainingSeconds",0.25},
            {"estimateScope","encode"}});
        Sleep(600); // long enough for the throttled 200 ms progress read to see it
        writeText(resultPath, Json{{"ok",true}}.dump());
        return 0;
    }
    if (mode == L"progress_stale_fail") {
        publishProgress(folder, Json{{"version",1},{"stage","encode"},{"phase","encode"},
            {"completedFrames",6},{"totalFrames",6},{"elapsedSeconds",1.0},{"phaseFraction",0.5},
            {"overallFraction",0.95},{"fps",6.0},{"estimatedRemainingSeconds",0.25},
            {"estimateScope","encode"}});
        Sleep(600);
        writeText(resultPath, Json{{"ok",false},
            {"error",Json{{"code","encode_failed"},{"message","encoder exploded"}}}}.dump());
        return 1; // the export worker's own exit code for a failed job
    }
    if (mode == L"progress_stale_cancel") {
        writeText(folder / L"pid", std::to_string(static_cast<unsigned long>(GetCurrentProcessId())));
        publishProgress(folder, Json{{"version",1},{"stage","render"},{"phase","render"},
            {"completedFrames",3},{"totalFrames",10},{"elapsedSeconds",0.5},{"phaseFraction",0.3},
            {"overallFraction",0.4},{"fps",6.0},{"estimatedRemainingSeconds",1.5},
            {"estimateScope","render"}});
        for (;;) Sleep(1000);
    }
    if (mode == L"cleanup") {
        // Mimics the export worker: it creates <parent>/physics-export-<token>
        // exclusively, writes <folder>/ownership.json *inside* that directory and
        // only then publishes <folder>/cleanup.json, then either hangs until it is
        // killed or exits. request.json is written by the parent before start();
        // this worker only ever rewrites it in the "forged_request" scenario, to
        // prove the parent uses the target it cached itself.
        if (argc < 6) return 2;
        const std::filesystem::path parent = argv[4];
        const std::wstring scenario = argv[5];
        const auto target = parent / L"out.png";
        const std::wstring token = randomToken();
        const auto tokenText = toUtf8(token);
        const auto temp = parent / (std::wstring(L"physics-export-") + token);
        const auto precious = parent / L"precious_target";
        std::error_code cleanupError;

        const auto seedFrames = [&](const std::filesystem::path& directory) {
            std::filesystem::create_directories(directory / L"frames", cleanupError);
            writeText(directory / L"frames" / L"frame_000000.png", "frame");
            writeText(directory / L"frames" / L"progress.json", "{\"stage\":\"render\"}");
        };
        const auto record = [&](const std::filesystem::path& directory, const std::filesystem::path& recordTarget,
                                const std::filesystem::path& outputParent, const std::string& recordToken) {
            return Json{{"version",1},{"marker","physics-export-cleanup"},{"token",recordToken},
                        {"jobRoot",std::filesystem::absolute(folder).u8string()},
                        {"outputParent",outputParent.u8string()},
                        {"tempDirectory",directory.u8string()},
                        {"target",recordTarget.u8string()}};
        };
        // The marker the real worker leaves inside the directory it created; the
        // parent refuses to delete a directory whose marker is missing or does not
        // agree with the record.
        const auto writeOwnership = [&](const std::filesystem::path& directory, const std::string& markerToken,
                                        const std::filesystem::path& markerTarget) {
            writeText(directory / L"ownership.json",
                Json{{"version",1},{"marker","physics-export-ownership"},{"token",markerToken},
                     {"jobRoot",std::filesystem::absolute(folder).u8string()},
                     {"target",markerTarget.u8string()}}.dump());
        };
        // The order the real worker uses: directory, marker inside it, then record.
        const auto seedRecorded = [&](const std::filesystem::path& directory, const std::filesystem::path& recordTarget,
                                      const std::filesystem::path& outputParent, const std::string& recordToken) {
            seedFrames(directory);
            writeOwnership(directory, recordToken, recordTarget);
            writeText(folder / L"cleanup.json", record(directory, recordTarget, outputParent, recordToken).dump());
        };
        const auto publishLayout = [&](const std::filesystem::path& directory, const std::filesystem::path& keep) {
            writeText(folder / L"layout.json", Json{{"token",tokenText},
                {"temp",directory.u8string()},{"keep",keep.u8string()}}.dump());
        };
        const auto writeRequest = [&](const std::filesystem::path& output) {
            writeText(folder / L"request.json", Json{{"output",output.u8string()}}.dump());
        };
        const auto hang = [] { for (;;) Sleep(1000); };

        if (scenario == L"valid" || scenario == L"abnormal") {
            seedRecorded(temp, target, parent, tokenText);
            publishLayout(temp, {});
            if (scenario == L"abnormal") return 9; // dies without result.json or cleanup
            hang();
        }
        if (scenario == L"success") {
            // A normal successful export: the worker removes its own directory
            // and only then its record, so the parent has nothing left to do.
            seedRecorded(temp, target, parent, tokenText);
            publishLayout(temp, {});
            std::filesystem::remove_all(temp, cleanupError);
            std::filesystem::remove(folder / L"cleanup.json", cleanupError);
            writeText(resultPath, Json{{"ok",true}}.dump());
            return 0;
        }
        if (scenario == L"absent") {
            // A directory that looks exactly like an export temp directory but
            // has no record: without a record the parent must never find it.
            const auto decoy = parent / (std::wstring(L"physics-export-") + randomToken());
            seedFrames(decoy);
            writeOwnership(decoy, toUtf8(randomToken()), target);
            publishLayout(decoy, {});
            hang();
        }
        if (scenario == L"committed") {
            // A successful PNG export commits by renaming the temp directory
            // onto the target and dies before dropping the record: the parent
            // finds no temp directory and must leave the committed target alone.
            const auto committed = parent / L"outdir";
            seedRecorded(temp, committed, parent, tokenText);
            publishLayout(temp, committed);
            std::filesystem::rename(temp, committed, cleanupError);
            if (cleanupError) throw std::runtime_error("cannot commit the temporary directory");
            writeText(resultPath, Json{{"ok",true}}.dump());
            return 0;
        }
        if (scenario == L"forged_parent") {
            // A perfectly formed record that points outside the requested output
            // parent: the target the parent cached itself is what stops it.
            const auto elsewhere = parent / L"elsewhere";
            const auto other = elsewhere / (std::wstring(L"physics-export-") + token);
            seedRecorded(other, elsewhere / L"out.png", elsewhere, tokenText);
            publishLayout(other, {});
            hang();
        }
        if (scenario == L"forged_target") {
            seedRecorded(temp, parent / L"other.png", parent, tokenText);
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"forged_prefix") {
            const auto named = parent / (std::wstring(L"precious-") + token);
            seedRecorded(named, target, parent, tokenText);
            publishLayout(named, {});
            hang();
        }
        if (scenario == L"forged_token") {
            seedRecorded(temp, target, parent, toUtf8(randomToken()));
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"forged_jobroot") {
            seedRecorded(temp, target, parent, tokenText);
            Json payload = record(temp, target, parent, tokenText);
            payload["jobRoot"] = (parent / L"elsewhere").u8string();
            writeText(folder / L"cleanup.json", payload.dump());
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"garbage") {
            seedFrames(temp);
            writeOwnership(temp, tokenText, target);
            writeText(folder / L"cleanup.json", "this is not json");
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"missing_marker") {
            // A well formed record whose directory carries no ownership marker:
            // the directory may not be ours, so it is kept.
            seedFrames(temp);
            writeText(folder / L"cleanup.json", record(temp, target, parent, tokenText).dump());
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"forged_marker") {
            // A record and a marker that disagree (the marker names another
            // token): the directory is kept.
            seedFrames(temp);
            writeOwnership(temp, toUtf8(randomToken()), target);
            writeText(folder / L"cleanup.json", record(temp, target, parent, tokenText).dump());
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"forged_request") {
            // The worker rewrites request.json - a file it can write - so that a
            // naive reader would believe the temporary directory is the export
            // target. The parent must compare against the target it cached before
            // start() and refuse.
            seedFrames(temp);
            writeOwnership(temp, tokenText, temp);
            writeRequest(temp);
            writeText(folder / L"cleanup.json", record(temp, temp, parent, tokenText).dump());
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"no_request") {
            // No request.json existed when the job started (the test wrote none),
            // so there is no cached target to bind against: nothing is deleted,
            // however well the record and the marker agree with each other.
            seedRecorded(temp, target, parent, tokenText);
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"target_is_temp") {
            // The record claims the requested target is the recorded directory
            // itself (the request the parent wrote says otherwise): it may never
            // be deleted, however well formed the record looks.
            seedFrames(temp);
            writeOwnership(temp, tokenText, temp);
            writeText(folder / L"cleanup.json", record(temp, temp, parent, tokenText).dump());
            publishLayout(temp, {});
            hang();
        }
        if (scenario == L"reparse_root") {
            std::filesystem::create_directories(precious, cleanupError);
            writeText(precious / L"keep.txt", "precious");
            if (!createJunction(temp, precious)) { publishLayout({}, {}); return 0; }
            writeText(folder / L"cleanup.json", record(temp, target, parent, tokenText).dump());
            publishLayout(temp, precious);
            hang();
        }
        if (scenario == L"reparse_inside") {
            seedFrames(temp);
            std::filesystem::create_directories(precious, cleanupError);
            writeText(precious / L"keep.txt", "precious");
            if (!createJunction(temp / L"link", precious)) { publishLayout({}, {}); return 0; }
            writeOwnership(temp, tokenText, target);
            writeText(folder / L"cleanup.json", record(temp, target, parent, tokenText).dump());
            publishLayout(temp, precious);
            hang();
        }
        return 2;
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
    {
        // A record left behind by an earlier run could describe a directory this
        // run does not own, so a stale one is refused instead of acted on.
        const auto folder = fresh(L"stale_cleanup");
        std::filesystem::create_directories(folder, error);
        writeText(folder / L"cleanup.json", "{\"version\":1}");
        BackgroundJob job;
        bool threw = false;
        try { job.start({self, L"--worker", L"hang", folder.wstring()}, folder, 5); }
        catch (const std::exception&) { threw = true; }
        check(threw, "start must refuse a pre-existing cleanup.json");
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
        // A worker that publishes no progress (for example the formula compiler)
        // must behave exactly as before and never grow a "progress" key.
        check(!job.state.contains("progress"), "a worker without progress.json must not publish progress");
        check(!job.state.contains("cleanup"), "a worker without a cleanup record must not publish cleanup");
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
        check(!job.state.contains("progress"),
              "a failed worker that published no progress must not grow a progress key");
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
        check(!job.state.contains("progress"),
              "a cancelled worker that published no progress must not grow a progress key");
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

    // --- live progress while the worker is still running --------------------
    {
        const auto folder = fresh(L"progress");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress", folder.wstring(), L"5"}, folder, 0);
        check(job.running(), "the progress worker must run right after start");
        job.state["id"] = "task-progress";
        job.state["output"] = folder.u8string();

        std::vector<ProgressView> history;
        std::vector<ULONGLONG> changeTicks;
        unsigned long long lastCompleted = 0;
        double lastOverall = -1.0;
        bool monotonic = true;
        unsigned repeatedPolls = 0;
        unsigned long long acknowledgedSample = 0;
        const ULONGLONG deadline = GetTickCount64() + 30000;
        for (;;) {
            const bool finished = job.poll();
            const auto view = progressOf(job);
            if (view.present) {
                // The parent publishes the terminal state from the exit code, so
                // that one transition is not a progress.json read and must not be
                // measured against the read throttle.
                const bool live = view.stage != "done" && view.stage != "failed" && view.stage != "cancelled";
                if (view.completed < lastCompleted || view.overall < lastOverall) monotonic = false;
                if (live && (view.completed != lastCompleted || view.overall != lastOverall))
                    changeTicks.push_back(GetTickCount64());
                lastCompleted = view.completed;
                lastOverall = view.overall;
                if (live && view.completed > acknowledgedSample && ++repeatedPolls >= 32) {
                    writeText(folder / (L"ack-" + std::to_wstring(view.completed)), "ready");
                    acknowledgedSample = view.completed;
                    repeatedPolls = 0;
                }
            }
            history.push_back(view);
            if (finished) break;
            check(GetTickCount64() < deadline, "the progress worker did not finish in time");
            Sleep(5);
        }
        check(job.state["status"] == "completed", "the progress worker must complete");
        check(job.state["id"] == "task-progress", "progress must preserve caller metadata");
        check(history.size() > 30, "the test must have polled many times, not just once");
        check(changeTicks.size() >= 2, "progress must show at least two increasing samples while running");
        check(monotonic, "published progress must never move backwards");
        // Reads are throttled to 200 ms: two different samples can therefore never
        // be observed less than ~195 ms apart, however often poll() is called.
        bool throttled = true;
        for (std::size_t index = 1; index < changeTicks.size(); ++index)
            if (changeTicks[index] - changeTicks[index - 1] < 150) throttled = false;
        check(throttled, "progress.json must be read with a throttle, not on every poll");
        const auto final = progressOf(job);
        // The worker's last sample said "render" and never reached 100%; because the
        // result was ok:true, the parent publishes the terminal done state itself.
        check(final.present && final.stage == "done" && final.completed == 5 && final.overall == 1.0,
              "a completed job must end at done/100% even when the last sample said render");
        check(job.state["progress"]["totalFrames"] == 5, "totalFrames must be published");
        const Json raw = Json::parse(readText(folder / L"progress.json"), nullptr, false);
        check(raw.is_object() && raw.value("stage", std::string()) == "render",
              "the worker's own last sample must still be the render one");
    }

    // --- corrupt file and backwards samples are rejected --------------------
    {
        const auto folder = fresh(L"progress_mix");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_mix", folder.wstring()}, folder, 0);
        std::vector<ProgressView> history;
        bool sawFirst = false;
        bool sawCarried = false;
        bool monotonic = true;
        unsigned long long lastCompleted = 0;
        double lastOverall = -1.0;
        const ULONGLONG deadline = GetTickCount64() + 30000;
        for (;;) {
            const bool finished = job.poll();
            const auto view = progressOf(job);
            if (view.present) {
                const bool live = view.stage != "done" && view.stage != "failed" && view.stage != "cancelled";
                // Only the parent's own terminal transition may appear, and only
                // once the job has finished.
                check(live ? view.stage == "render" : (finished && view.stage == "done"),
                      "a corrupt or backwards sample must never be published");
                if (view.completed == 1 && view.overall > 0.299 && view.overall < 0.301) sawFirst = true;
                // A sample without counters or fraction must carry the last
                // trustworthy values forward instead of jumping back to zero.
                if (live && view.completed == 3 && view.overall > 0.899 && view.overall < 0.901)
                    sawCarried = true;
                if (view.completed < lastCompleted || view.overall < lastOverall) monotonic = false;
                lastCompleted = view.completed;
                lastOverall = view.overall;
            }
            history.push_back(view);
            if (finished) break;
            check(GetTickCount64() < deadline, "the mixed-progress worker did not finish in time");
            Sleep(5);
        }
        check(job.state["status"] == "completed", "the mixed-progress worker must still complete");
        check(sawFirst, "the valid sample before the corrupt file must be visible");
        check(monotonic, "a backwards sample must be clamped, never published");
        const auto final = progressOf(job);
        check(final.present && final.stage == "done" && final.completed == 3 && final.overall == 1.0,
              "completion must publish the terminal done state on top of the last trustworthy sample");
        check(sawCarried, "a sample without counters must keep the last trustworthy frame count and fraction");
    }

    // --- the final sample survives an immediately exiting worker ------------
    {
        const auto folder = fresh(L"progress_done");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_done", folder.wstring()}, folder, 30);
        check(pump(job, 20000), "the short progress worker did not finish in time");
        check(job.state["status"] == "completed", "the short progress worker must complete");
        const auto final = progressOf(job);
        check(final.present, "the final progress must be read even when the worker exits at once");
        check(final.stage == "done" && final.completed == 2 && final.overall == 1.0,
              "the final done sample must be the published one");
    }

    // --- wrongly typed progress fields are dropped, never published ---------
    {
        const auto folder = fresh(L"progress_types");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_types", folder.wstring()}, folder, 30);
        bool sanitized = false;
        const ULONGLONG deadline = GetTickCount64() + 20000;
        for (;;) {
            const bool finished = job.poll();
            if (job.state.contains("progress") && job.state["progress"].is_object()) {
                const auto& progress = job.state["progress"];
                if (progress.value("stage", std::string()) == "render") {
                    sanitized = !progress.contains("elapsedSeconds") && !progress.contains("fps") &&
                                !progress.contains("overallFraction") && !progress.contains("phaseFraction") &&
                                progress.contains("estimatedRemainingSeconds") &&
                                progress["estimatedRemainingSeconds"].is_null() &&
                                progress["completedFrames"] == 1 && progress["totalFrames"] == 2;
                }
            }
            if (finished) break;
            check(GetTickCount64() < deadline, "the typed-progress worker did not finish in time");
            Sleep(5);
        }
        check(job.state["status"] == "completed", "the typed-progress worker must complete");
        check(sanitized, "wrongly typed progress fields must be dropped and a null estimate kept");
        const auto final = progressOf(job);
        check(final.present && final.stage == "done" && final.completed == 2, "the valid sample must follow");
    }

    // --- an oversized progress file is refused, not parsed ------------------
    {
        const auto folder = fresh(L"progress_oversize");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_oversize", folder.wstring()}, folder, 30);
        bool premature = false;
        bool finished = false;
        const ULONGLONG early = GetTickCount64() + 400;
        while (GetTickCount64() < early) {
            if (job.poll()) { finished = true; break; }
            if (progressOf(job).present) { premature = true; break; }
            Sleep(5);
        }
        check(!premature, "a progress file over 64 KiB must be ignored");
        if (!finished) check(pump(job, 20000), "the oversize-progress worker did not finish in time");
        check(job.state["status"] == "completed", "the oversize-progress worker must complete");
        const auto final = progressOf(job);
        check(final.present && final.stage == "done" && final.completed == 2,
              "the valid sample after the oversized one must be published");
    }

    // --- cancel keeps the last progress and publishes the terminal state -----
    {
        const auto folder = fresh(L"progress_cancel");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_hang", folder.wstring()}, folder, 0);
        job.state["id"] = "task-cancel-progress";
        const DWORD pid = waitForPidFile(folder / L"pid", 10000);
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (!progressOf(job).present) {
            check(!job.poll(), "the hanging progress worker must not complete");
            check(GetTickCount64() < deadline, "the hanging worker never published progress");
            Sleep(5);
        }
        job.cancel();
        check(job.state["status"] == "cancelled", "cancel must publish status cancelled");
        check(job.state["id"] == "task-cancel-progress", "cancel must preserve caller metadata");
        const auto final = progressOf(job);
        check(final.present && final.stage == "cancelled",
              "cancel must publish the terminal cancelled stage on top of the last sample");
        check(final.completed == 4, "cancel must keep the last measured frame counter");
        check(final.overall > 0.399 && final.overall < 0.401, "the last fraction must be preserved");
        const auto& progress = job.state["progress"];
        check(!progress.contains("fps"), "a cancelled job must drop the stale throughput");
        check(progress.contains("estimatedRemainingSeconds") && progress["estimatedRemainingSeconds"].is_null(),
              "a cancelled job must drop the stale estimate");
        check(waitUntilDead(pid, 5000), "cancel must terminate the worker");
    }

    // --- the parent publishes the terminal state the worker never wrote -------
    // Each of these workers ends on a *running* sample (encode/render) and never
    // publishes a terminal one. The parent knows the real outcome from the exit
    // code and result.json, so the state the editor reads must still be terminal.
    {
        const auto folder = fresh(L"progress_stale_ok");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_stale_ok", folder.wstring()}, folder, 0);
        job.state["id"] = "task-stale-ok";
        bool sawEncode = false;
        bool finished = false;
        const ULONGLONG deadline = GetTickCount64() + 30000;
        while (!finished) {
            finished = job.poll();
            const auto view = progressOf(job);
            if (view.present && view.stage == "encode") sawEncode = true;
            if (!finished) {
                check(GetTickCount64() < deadline, "the stale-progress success worker did not finish in time");
                Sleep(5);
            }
        }
        check(sawEncode, "the running encode sample must be visible while the job still runs");
        check(job.state["status"] == "completed", "the stale-progress success worker must complete");
        check(job.state["id"] == "task-stale-ok", "the terminal sync must preserve caller metadata");
        const auto final = progressOf(job);
        check(final.present && final.stage == "done",
              "a completed job must end at stage done even when the worker's last sample said encode");
        check(final.completed == 6 && final.overall == 1.0,
              "an ok result must pin the counters and the fraction to 100%");
        const auto& progress = job.state["progress"];
        check(progress.contains("phase") && progress["phase"] == "done", "the terminal phase must follow the stage");
        check(progress.contains("phaseFraction") && progress["phaseFraction"] == 1.0,
              "done must carry a complete phase fraction");
        check(progress.contains("estimatedRemainingSeconds") && progress["estimatedRemainingSeconds"] == 0.0,
              "done must carry a zero estimate");
        check(progress.contains("estimateScope") && progress["estimateScope"].is_null(),
              "done must have no estimate scope, exactly like the worker's own done sample");
        const Json raw = Json::parse(readText(folder / L"progress.json"), nullptr, false);
        check(raw.is_object() && raw.value("stage", std::string()) == "encode",
              "the worker's own last sample must still say encode: the parent publishes the terminal state itself");
    }
    {
        const auto folder = fresh(L"progress_stale_fail");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_stale_fail", folder.wstring()}, folder, 0);
        bool finished = false;
        const ULONGLONG deadline = GetTickCount64() + 30000;
        while (!finished) {
            finished = job.poll();
            if (!finished) {
                check(GetTickCount64() < deadline, "the stale-progress failure worker did not finish in time");
                Sleep(5);
            }
        }
        check(job.state["status"] == "failed", "an ok:false result with exit code 1 must fail the job");
        check(job.state["error"].get<std::string>().find("encoder exploded") != std::string::npos,
              "the structured worker error message must reach state.error");
        const auto final = progressOf(job);
        check(final.present && final.stage == "failed",
              "a failed job must end at stage failed even when the worker's last sample said encode");
        check(final.completed == 6, "a failure must keep the last measured frame counter");
        check(final.overall > 0.949 && final.overall < 0.951, "a failure must keep the last measured fraction");
        const auto& progress = job.state["progress"];
        check(progress.contains("phase") && progress["phase"] == "failed", "the terminal phase must follow the stage");
        check(!progress.contains("fps"), "a failed job must drop the stale throughput");
        check(progress.contains("estimatedRemainingSeconds") && progress["estimatedRemainingSeconds"].is_null(),
              "a failed job must drop the stale estimate");
        check(progress.contains("estimateScope") && progress["estimateScope"].is_null(),
              "a failed job must drop the stale estimate scope");
        const std::string message = progress.value("message", std::string());
        check(message.find("encoder exploded") != std::string::npos,
              "a failed progress must carry the real reason, not the last running sample: " + message);
    }
    {
        const auto folder = fresh(L"progress_stale_cancel");
        BackgroundJob job;
        job.start({self, L"--worker", L"progress_stale_cancel", folder.wstring()}, folder, 0);
        const DWORD pid = waitForPidFile(folder / L"pid", 10000);
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (!progressOf(job).present) {
            check(!job.poll(), "the stale-progress cancel worker must not complete");
            check(GetTickCount64() < deadline, "the stale-progress cancel worker never published progress");
            Sleep(5);
        }
        job.cancel();
        check(job.state["status"] == "cancelled", "cancel must publish status cancelled");
        const auto final = progressOf(job);
        check(final.present && final.stage == "cancelled",
              "cancel must end at stage cancelled even when the worker's last sample said render");
        check(final.completed == 3 && final.overall > 0.399 && final.overall < 0.401,
              "cancel must keep the last measured counter and fraction");
        const auto& progress = job.state["progress"];
        check(!progress.contains("fps"), "a cancelled job must drop the stale throughput");
        check(progress.contains("estimatedRemainingSeconds") && progress["estimatedRemainingSeconds"].is_null(),
              "a cancelled job must drop the stale estimate");
        check(waitUntilDead(pid, 5000), "cancel must terminate the worker");
    }

    // --- leftover export temp directory cleanup -----------------------------
    // The export worker renders into <output parent>/physics-export-<token> and
    // dies with its process tree, so its own cleanup never runs. These cases
    // drive the record it leaves behind in every lifecycle and check that no
    // path it names is ever deleted unless every binding is proven.
    struct CleanupRun {
        std::filesystem::path folder;
        std::filesystem::path parent;
        CleanupLayout layout;
        Json state;
    };
    const auto cleanupCase = [&](const std::wstring& scenario, const std::wstring& lifecycle, bool seedTarget) {
        CleanupRun run;
        run.folder = fresh(L"cleanup_" + scenario);
        run.parent = fresh(L"cleanup_" + scenario + L"_out");
        std::filesystem::create_directories(run.folder, error);
        check(!error, "cannot create the run folder: " + error.message());
        std::filesystem::create_directories(run.parent, error);
        check(!error, "cannot create the output parent: " + error.message());
        if (seedTarget) writeText(run.parent / L"out.png", "existing target");
        if (scenario != L"no_request") {
            // The request the editor writes before start(): this is the only
            // trustworthy target, because the worker can rewrite the file once it
            // runs. It is written here, before start(), exactly like the editor
            // does it.
            const auto requestTarget = scenario == L"committed" ? run.parent / L"outdir" : run.parent / L"out.png";
            writeText(run.folder / L"request.json", Json{{"output",requestTarget.u8string()}}.dump());
        }
        {
            BackgroundJob job;
            job.start({self, L"--worker", L"cleanup", run.folder.wstring(), run.parent.wstring(), scenario},
                      run.folder, 0);
            job.state["id"] = "task-cleanup";
            run.layout = waitForLayout(run.folder, 10000);
            if (lifecycle == L"cancel") {
                job.cancel();
                run.state = job.state;
            } else if (lifecycle == L"destructor") {
                // The destructor performs the cleanup; the job is gone afterwards.
            } else {
                check(lifecycle == L"exit", "unknown cleanup lifecycle");
                check(pump(job, 30000), "the cleanup worker did not finish in time");
                run.state = job.state;
            }
        }
        return run;
    };

    // A cancelled export removes exactly the directory it recorded, keeps the
    // requested target and reports what it did.
    {
        const auto run = cleanupCase(L"valid", L"cancel", true);
        check(!std::filesystem::exists(run.layout.temp), "cancel must remove the recorded export temp directory");
        check(std::filesystem::exists(run.parent / L"out.png"), "cancel must keep the requested target");
        check(run.state["status"] == "cancelled", "cancel must still publish status cancelled");
        check(run.state["id"] == "task-cleanup", "cleanup must preserve caller metadata");
        check(cleanupOf(run.state).value("status", std::string()) == "removed",
              "the removed directory must be reported");
        check(cleanupOf(run.state).value("directory", std::string()) == run.layout.temp.u8string(),
              "the reported directory must be the recorded one");
        check(!std::filesystem::exists(run.folder / L"cleanup.json"),
              "the record must be dropped once the directory is gone");
    }

    // The destructor runs the same cleanup when the editor shuts down.
    {
        const auto run = cleanupCase(L"valid", L"destructor", true);
        check(!std::filesystem::exists(run.layout.temp), "the destructor must remove the recorded temp directory");
        check(std::filesystem::exists(run.parent / L"out.png"), "the destructor must keep the requested target");
    }

    // A worker that dies without writing result.json is cleaned up as well.
    {
        const auto run = cleanupCase(L"abnormal", L"exit", true);
        check(run.state["status"] == "failed", "a worker that dies without a result must fail the job");
        check(!std::filesystem::exists(run.layout.temp), "an abnormally dead worker must still be cleaned up");
        check(cleanupOf(run.state).value("status", std::string()) == "removed", "the removal must be reported");
        check(std::filesystem::exists(run.parent / L"out.png"), "the target must survive");
    }

    // A normal success is untouched: the worker cleaned up and dropped its own
    // record, so the job publishes no cleanup result at all.
    {
        const auto run = cleanupCase(L"success", L"exit", true);
        check(run.state["status"] == "completed", "the self-cleaning worker must complete");
        check(!run.state.contains("cleanup"), "a worker that cleaned up itself must leave state unchanged");
        check(!std::filesystem::exists(run.layout.temp), "the worker's own cleanup must have removed the directory");
        check(std::filesystem::exists(run.parent / L"out.png"), "the target must survive");
    }

    // A committed PNG export renames the temp directory onto the target and dies
    // before dropping the record: there is nothing left to delete, and the
    // committed target (with the native progress.json inside it) must survive.
    {
        const auto run = cleanupCase(L"committed", L"exit", false);
        const auto committed = run.parent / L"outdir";
        check(run.state["status"] == "completed", "the committing worker must complete");
        check(std::filesystem::exists(committed / L"frames" / L"progress.json"),
              "the committed output must keep the native progress.json");
        check(std::filesystem::exists(committed / L"frames" / L"frame_000000.png"),
              "the committed frames must survive");
        check(cleanupOf(run.state).value("status", std::string()) == "nothing",
              "a committed export has no directory left to clean");
        check(!std::filesystem::exists(run.layout.temp), "the temp directory was renamed onto the target");
    }

    // Without a record nothing is deleted: the parent must never scan the output
    // parent for directories that look like leftovers.
    {
        const auto run = cleanupCase(L"absent", L"cancel", true);
        check(std::filesystem::exists(run.layout.temp), "a directory no record describes must never be deleted");
        check(std::filesystem::exists(run.layout.temp / L"frames" / L"frame_000000.png"),
              "and its contents must never be deleted");
        check(!run.state.contains("cleanup"), "no record means no cleanup result at all");
        check(run.state["status"] == "cancelled", "the job must still be cancelled");
        check(std::filesystem::exists(run.parent / L"out.png"), "the target must survive");
    }

    // A record that does not prove which directory it owns is refused - broken
    // bindings, a missing or forged ownership marker, a worker that rewrote
    // request.json, or no request at all - and the directory, its contents, the
    // record and the target all survive.
    for (const wchar_t* scenarioName : { L"forged_parent", L"forged_target", L"forged_prefix",
                                         L"forged_token", L"forged_jobroot", L"garbage",
                                         L"missing_marker", L"forged_marker", L"forged_request",
                                         L"no_request", L"target_is_temp" }) {
        const std::wstring scenario = scenarioName;
        const std::string name = toUtf8(scenario);
        const auto run = cleanupCase(scenario, L"cancel", true);
        check(std::filesystem::exists(run.layout.temp), "a refused cleanup must leave the directory: " + name);
        check(std::filesystem::exists(run.layout.temp / L"frames" / L"frame_000000.png"),
              "and its contents: " + name);
        check(cleanupOf(run.state).value("status", std::string()) == "kept",
              "a refused cleanup must be reported as kept: " + name);
        check(!cleanupOf(run.state).value("reason", std::string()).empty(),
              "a refusal must carry a reason: " + name);
        check(std::filesystem::exists(run.folder / L"cleanup.json"),
              "a refused record must stay for inspection: " + name);
        check(std::filesystem::exists(run.parent / L"out.png"), "the target must survive: " + name);
    }

    // A worker that rewrote request.json must not be able to move the cleanup
    // target: the record it wrote names the temporary directory itself, and the
    // parent still refuses it. The file the worker forged is left as evidence.
    {
        const auto run = cleanupCase(L"forged_request", L"cancel", true);
        const Json forged = Json::parse(readText(run.folder / L"request.json"), nullptr, false);
        check(forged.is_object() && forged.value("output", std::string()) == run.layout.temp.u8string(),
              "the forged_request worker must have rewritten request.json to the temporary directory");
        check(std::filesystem::exists(run.layout.temp / L"frames" / L"progress.json"),
              "the forged request must not make the parent delete anything");
    }

    // A reparse point is never removed and never traversed.
    {
        const auto run = cleanupCase(L"reparse_root", L"cancel", true);
        if (run.layout.temp.empty()) {
            std::cout << "note: directory junctions are unavailable here; the reparse-point cases were skipped\n";
        } else {
            // GetFileAttributesW reports the link's own attributes for a
            // junction; std::filesystem::symlink_status maps a mount point to a
            // directory here, so the reparse attribute is what says "the link
            // itself is still there".
            const DWORD attributes = GetFileAttributesW(run.layout.temp.c_str());
            check(attributes != INVALID_FILE_ATTRIBUTES &&
                      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
                  "a directory that is a reparse point must never be removed");
            check(std::filesystem::exists(run.layout.keep / L"keep.txt"), "its target must survive");
            check(cleanupOf(run.state).value("status", std::string()) == "kept",
                  "a reparse point must be reported as kept");
        }
    }
    {
        const auto run = cleanupCase(L"reparse_inside", L"cancel", true);
        if (run.layout.temp.empty()) {
            std::cout << "note: directory junctions are unavailable here; the reparse-point cases were skipped\n";
        } else {
            check(!std::filesystem::exists(run.layout.temp), "the recorded directory must still be removed");
            check(std::filesystem::exists(run.layout.keep / L"keep.txt"),
                  "a junction inside the recorded directory must never be followed");
            check(cleanupOf(run.state).value("status", std::string()) == "removed", "the removal must be reported");
            check(std::filesystem::exists(run.parent / L"out.png"), "the target must survive");
        }
    }

    removeTaskOwnedDirectory(root);
    std::cout << "PASS: background job idle/validation/stale guards, metadata preservation, quoting, "
                 "redirection, NUL stdin, completed/failed/timeout/cancel, process tree kill and destructor cleanup, "
                 "live progress while running (throttled, monotonic, corrupt/oversized rejected), the terminal "
                 "state published by the parent from the exit code and result.json when the worker's last sample "
                 "is still a running stage or absent (done/100% only on ok:true, failed and cancelled keep the "
                 "measured counters and fraction and drop the stale rate and estimate, workers without progress "
                 "keep no progress key), leftover export temp directory cleanup (removed on cancel, "
                 "worker death and destruction only after the process tree was confirmed gone and this job's own "
                 "handles were closed, with a bounded retry for a directory Windows still reports as open; forged, "
                 "out-of-bounds, reparse-point, target-named, marker-less or marker-mismatched records, records "
                 "checked against a request.json the worker rewrote, and runs without a cached request target "
                 "refused and reported; committed targets and records without a binding left untouched)\n";
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

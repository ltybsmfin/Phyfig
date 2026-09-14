#pragma once
// Writable data and read-only resources are separate concerns:
//   dataRoot     - journals, build/jobs scratch, export work; must be writable
//   resourceRoot - tools/ and assets/ shipped with the source tree; read-only
// Data candidates are validated with a real create+write+flush+delete probe,
// never with exists() alone. Resources are found by a bounded upward search
// (at most 8 ancestors), never by a recursive disk scan.
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace physics_editor {
struct RuntimePaths {
    std::filesystem::path dataRoot;      // writable: journal, build/jobs, scratch
    std::filesystem::path resourceRoot;  // contains tools/render_formula.py + assets/formulas/formulas.json; empty when not found
    bool dataFromFallback = false;       // project directory was rejected and %TEMP% was used
    bool resourcesFound = false;
    std::vector<std::string> dataProbes;      // human-readable probe results, for logs/tests
    std::vector<std::string> resourceProbes;
};

namespace runtime_paths_detail {

inline std::string text(const std::filesystem::path& path) {
    return path.u8string();
}

inline std::filesystem::path executableDirectory() {
    wchar_t buffer[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    if (length == 0 || length >= 32768) return {};
    return std::filesystem::path(buffer).parent_path();
}

inline std::filesystem::path temporaryBase() {
    wchar_t buffer[32768]{};
    const DWORD length = GetTempPathW(32768, buffer);
    if (length == 0 || length >= 32768) return {};
    return std::filesystem::path(buffer);
}

// Real probe: create the directory, open a file, write, flush, close, delete.
inline bool probeWritableDirectory(const std::filesystem::path& directory, std::string& detail) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { detail = text(directory) + ": cannot create the directory (" + error.message() + ")"; return false; }
    const auto probe = directory / std::filesystem::u8path(
        "write-probe-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".tmp");
    {
        std::ofstream stream(probe, std::ios::binary | std::ios::trunc);
        if (!stream) { detail = text(directory) + ": cannot open a probe file for writing"; return false; }
        stream.write("p", 1);
        stream.flush();
        if (!stream) { detail = text(directory) + ": cannot flush a probe file"; return false; }
    }
    std::filesystem::remove(probe, error);
    if (error) { detail = text(directory) + ": cannot delete the probe file (" + error.message() + ")"; return false; }
    detail = text(directory) + ": writable";
    return true;
}

inline bool hasResourcePair(const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::is_regular_file(
               root / std::filesystem::u8path("tools") / std::filesystem::u8path("render_formula.py"), error) &&
           std::filesystem::is_regular_file(
               root / std::filesystem::u8path("assets") / std::filesystem::u8path("formulas") /
                   std::filesystem::u8path("formulas.json"), error);
}

inline bool sameDirectory(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ea, eb;
    const auto ca = std::filesystem::weakly_canonical(a, ea);
    const auto cb = std::filesystem::weakly_canonical(b, eb);
    return !ea && !eb && ca == cb;
}

inline std::string join(const std::vector<std::string>& items) {
    std::string result;
    for (size_t i = 0; i < items.size(); ++i) { if (i) result += "; "; result += items[i]; }
    return result;
}

inline RuntimePaths& storage() { static RuntimePaths paths; return paths; }
inline bool& readyFlag() { static bool ready = false; return ready; }

} // namespace runtime_paths_detail

// explicitDataDir: when provided, the requested path is used as given. Failure
// throws with the path in the message and never silently falls back.
inline RuntimePaths initializeRuntimePaths(std::optional<std::filesystem::path> explicitDataDir = std::nullopt) {
    RuntimePaths result;
    const auto cwd = std::filesystem::current_path();
    const auto exeDir = runtime_paths_detail::executableDirectory();

    if (explicitDataDir) {
        const auto requested = std::filesystem::absolute(*explicitDataDir).lexically_normal();
        std::string detail;
        if (!runtime_paths_detail::probeWritableDirectory(requested, detail))
            throw std::runtime_error("--data-dir " + runtime_paths_detail::text(requested) + " is not usable: " + detail +
                ". The explicit path is never silently replaced by a fallback.");
        result.dataProbes.push_back("explicit --data-dir " + detail);
        // The data root itself is not enough: journal and job scratch directories
        // must be probed individually, or a pre-existing same-named file or a
        // read-only child would fail later with a generic journal error.
        std::string historyDetail, jobsDetail;
        const bool historyOk = runtime_paths_detail::probeWritableDirectory(
            requested / std::filesystem::u8path("history"), historyDetail);
        result.dataProbes.push_back("explicit history " + historyDetail);
        const bool jobsOk = runtime_paths_detail::probeWritableDirectory(
            requested / std::filesystem::u8path("build") / std::filesystem::u8path("jobs"), jobsDetail);
        result.dataProbes.push_back("explicit jobs " + jobsDetail);
        if (!historyOk || !jobsOk)
            throw std::runtime_error("--data-dir " + runtime_paths_detail::text(requested) +
                " is not usable: " + historyDetail + "; " + jobsDetail +
                ". The explicit path is never silently replaced by a fallback.");
        result.dataRoot = requested;
    } else {
        if (runtime_paths_detail::sameDirectory(cwd, exeDir)) {
            result.dataProbes.push_back(runtime_paths_detail::text(cwd) +
                ": rejected because it is the executable installation directory");
        } else {
            std::string historyDetail, jobsDetail;
            const bool historyOk =
                runtime_paths_detail::probeWritableDirectory(cwd / std::filesystem::u8path("history"), historyDetail);
            result.dataProbes.push_back("history " + historyDetail);
            if (historyOk) {
                const bool jobsOk = runtime_paths_detail::probeWritableDirectory(
                    cwd / std::filesystem::u8path("build") / std::filesystem::u8path("jobs"), jobsDetail);
                result.dataProbes.push_back("jobs " + jobsDetail);
                if (jobsOk) result.dataRoot = cwd;
            }
        }
        if (result.dataRoot.empty()) {
            const auto base = runtime_paths_detail::temporaryBase();
            if (!base.empty()) {
                const auto fallback = base / std::filesystem::u8path("physics-editor") /
                    std::filesystem::u8path(std::to_string(GetCurrentProcessId()) + "-" +
                                            std::to_string(GetTickCount64()));
                std::string historyDetail, jobsDetail;
                const bool historyOk = runtime_paths_detail::probeWritableDirectory(
                    fallback / std::filesystem::u8path("history"), historyDetail);
                result.dataProbes.push_back("temp history " + historyDetail);
                const bool jobsOk = runtime_paths_detail::probeWritableDirectory(
                    fallback / std::filesystem::u8path("build") / std::filesystem::u8path("jobs"), jobsDetail);
                result.dataProbes.push_back("temp jobs " + jobsDetail);
                if (historyOk && jobsOk) { result.dataRoot = fallback; result.dataFromFallback = true; }
            } else {
                result.dataProbes.push_back("temporary directory: GetTempPathW failed");
            }
        }
        if (result.dataRoot.empty()) {
            const auto base = runtime_paths_detail::temporaryBase();
            throw std::runtime_error("Cannot open the operation journal: no writable data directory. Project directory " +
                runtime_paths_detail::text(cwd) + ", temporary base " +
                (base.empty() ? std::string("<unavailable>") : runtime_paths_detail::text(base)) + ". Probes: " +
                runtime_paths_detail::join(result.dataProbes) + ". Pass --data-dir with a writable directory.");
        }
    }

    std::vector<std::filesystem::path> starts;
    if (!cwd.empty()) starts.push_back(cwd);
    if (!exeDir.empty() && !runtime_paths_detail::sameDirectory(cwd, exeDir)) starts.push_back(exeDir);
    for (const auto& start : starts) {
        auto current = start;
        for (int depth = 0; depth < 8 && !current.empty(); ++depth) {
            result.resourceProbes.push_back(runtime_paths_detail::text(current));
            if (runtime_paths_detail::hasResourcePair(current)) {
                result.resourceRoot = current;
                result.resourcesFound = true;
                break;
            }
            const auto parent = current.parent_path();
            if (parent == current || parent.empty()) break;
            current = parent;
        }
        if (result.resourcesFound) break;
    }

    runtime_paths_detail::storage() = result;
    runtime_paths_detail::readyFlag() = true;
    return runtime_paths_detail::storage();
}

inline const RuntimePaths& runtimePaths() {
    if (!runtime_paths_detail::readyFlag())
        throw std::logic_error(
            "Runtime paths are not initialized; call physics_editor::initializeRuntimePaths() before runtimePaths()");
    return runtime_paths_detail::storage();
}

inline bool runtimePathsReady() { return runtime_paths_detail::readyFlag(); }

} // namespace physics_editor

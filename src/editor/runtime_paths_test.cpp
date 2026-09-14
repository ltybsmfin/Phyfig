#include "runtime_paths.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

std::filesystem::path makeTempWorkspace() {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    check(!error, "Cannot read the temporary directory");
    for (unsigned i = 0; i < 64; ++i) {
        const auto candidate = base / ("physics-runtime-test-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(i));
        if (std::filesystem::create_directory(candidate, error)) return candidate;
        error.clear();
    }
    throw std::runtime_error("Cannot create a temporary test workspace");
}

struct TempTree {
    std::filesystem::path path;
    ~TempTree() { std::error_code error; std::filesystem::remove_all(path, error); }
};

struct CwdGuard {
    std::filesystem::path previous;
    CwdGuard() : previous(std::filesystem::current_path()) {}
    ~CwdGuard() { std::error_code error; std::filesystem::current_path(previous, error); }
};
} // namespace

int main() {
    try {
        const auto root = makeTempWorkspace();
        TempTree cleanup{root};

        // 1. An explicit --data-dir is used as given and probed for real.
        {
            const auto data = root / "explicit-data";
            const auto paths = physics_editor::initializeRuntimePaths(data);
            check(paths.dataRoot == data, "Explicit data root was not used");
            check(!paths.dataFromFallback, "Explicit data root reported a fallback");
            check(std::filesystem::is_directory(data / "history"), "History probe did not create the directory");
            check(std::filesystem::is_directory(data / "build/jobs"), "Jobs probe did not create the directory");
            check(paths.dataProbes.size() == 3, "Explicit root/history/jobs probe results were not recorded");
        }

        // 1b. An explicit root whose history child is an existing file must fail
        // with that path, instead of a generic journal error later.
        {
            const auto data = root / "explicit-broken";
            std::filesystem::create_directories(data);
            { std::ofstream stream(data / "history", std::ios::binary); stream << "not a directory\n"; }
            bool threw = false;
            try {
                physics_editor::initializeRuntimePaths(data);
            } catch (const std::runtime_error& e) {
                threw = true;
                const std::string message = e.what();
                check(message.find("explicit-broken") != std::string::npos,
                      "Error message does not include the explicit data root");
                check(message.find("history") != std::string::npos,
                      "Error message does not name the failing history probe");
                check(message.find("fallback") != std::string::npos,
                      "Error message does not state that no fallback is applied");
            }
            check(threw, "An explicit --data-dir with a blocked history child did not fail");
        }

        // 2. An unusable explicit path fails loudly and never silently falls back.
        {
            const auto file = root / "not-a-directory";
            { std::ofstream stream(file, std::ios::binary); stream << "x"; }
            bool threw = false;
            try {
                physics_editor::initializeRuntimePaths(file / "child");
            } catch (const std::runtime_error& e) {
                threw = true;
                check(std::string(e.what()).find("not-a-directory") != std::string::npos,
                      "Error message does not include the requested path");
                check(std::string(e.what()).find("fallback") != std::string::npos,
                      "Error message does not state that no fallback is applied");
            }
            check(threw, "An unusable --data-dir did not fail");
        }

        // 3. Resource discovery walks upward at most 8 ancestors from cwd.
        {
            const auto tree = root / "resource-tree";
            std::filesystem::create_directories(tree / "tools");
            std::filesystem::create_directories(tree / "assets/formulas");
            { std::ofstream s(tree / "tools/render_formula.py"); s << "#\n"; }
            { std::ofstream s(tree / "assets/formulas/formulas.json"); s << "{}\n"; }
            {
                // `near`/`far` are empty object-like macros in the Win32 headers
                // (windef.h) that runtime_paths.hpp pulls in, so the locals must
                // not use those names.
                const auto nearDir = tree / "a/b/c/d/e/f/g"; // tree is 7 levels above g
                std::filesystem::create_directories(nearDir);
                CwdGuard guard;
                std::filesystem::current_path(nearDir);
                const auto paths = physics_editor::initializeRuntimePaths(root / "resource-data");
                check(paths.resourcesFound, "Resource pair was not found by the upward search");
                check(paths.resourceRoot == tree, "Resource root was not the tree root");
            }
            {
                const auto farDir = tree / "a/b/c/d/e/f/g/h"; // tree is 8 levels above h: outside the bound
                std::filesystem::create_directories(farDir);
                CwdGuard guard;
                std::filesystem::current_path(farDir);
                const auto paths = physics_editor::initializeRuntimePaths(root / "resource-data");
                check(paths.resourceRoot != tree, "Resource search exceeded the 8-ancestor bound");
            }
        }

        // 4. A blocked project data root falls back to a per-process %TEMP% root.
        {
            const auto blocked = root / "blocked";
            std::filesystem::create_directories(blocked);
            { std::ofstream s(blocked / "history"); s << "not a directory\n"; } // fails the probe without touching ACLs
            CwdGuard guard;
            std::filesystem::current_path(blocked);
            const auto paths = physics_editor::initializeRuntimePaths(std::nullopt);
            check(paths.dataFromFallback, "A blocked project data root did not fall back");
            check(paths.dataRoot.string().find("physics-editor") != std::string::npos,
                  "Fallback data root is not under the physics-editor temporary area");
            check(std::filesystem::is_directory(paths.dataRoot / "history"),
                  "Fallback history probe did not create the directory");
            TempTree fallbackCleanup{paths.dataRoot};
        }

        std::cout << "PASS: runtime paths separate writable data from read-only resources; explicit paths fail "
                     "loudly; a blocked project root falls back to a per-process temp root\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}

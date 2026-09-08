# Vendored dependencies

`imgui/` contains the core Dear ImGui sources and the Win32 / Vulkan backends from
the official **v1.91.9b** tag. Upstream source files are unmodified. Only files
needed to build the kinematics demo and the license are included.

- Upstream: https://github.com/ocornut/imgui
- Release: https://github.com/ocornut/imgui/releases/tag/v1.91.9b
- Archive: https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip
- Archive SHA-256: `FD37507C8476A6D14CC7C4B352401F31BCBD0F0D995D35390811E968C466F46E`
- License: MIT, see `imgui/LICENSE.txt`

The build uses these local sources and performs no network downloads.
`json/json.hpp` is the unmodified nlohmann/json **v3.12.0** single header,
downloaded from https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp.
SHA-256: `AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63`.
License: MIT, see `json/LICENSE.MIT`. Used by the editor document and command layer.

Microsoft YaHei is read from the Windows system font directory at runtime;
no Windows font file is copied into this repository or redistributed.

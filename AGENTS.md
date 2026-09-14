# Project collaboration notes

- This is a learning and experimentation project using C++ and Vulkan. Respond to the user in Chinese.
- Directly point out actual terminology mistakes, design problems, and feasibility limits, with reasons and a concrete alternative. Do not invent mistakes or confuse a deliberate learning simplification with a bug.
- Keep each graphics example under its own `examples/NN_name/` directory, with its own CMake target, shaders, and runtime output directory. Avoid building a shared rendering engine before repeated examples establish a real need.
- Use the checked-in CMake presets and MSVC toolchain. Do not commit machine-specific installation paths or generated build files.
- Launch the maintained editor through `run-editor.ps1` (or `run-editor-mcp.ps1` for a managed stdio MCP session); both build current sources first. The canonical binaries are under `build/vs2022/bin/editor/{Debug,Release}`. Never use an old executable from a review/verification directory as the normal editor. Keep historical implementations in Git; remove temporary review worktrees after review.
- When rendering or resource lifetime changes, build and run the affected example with validation and synchronization validation. Check resize, minimize/restore, and shutdown when relevant. Report the scope actually verified.
- Keep the Chinese README and learning notes consistent with the implementation, including prerequisites, controls, and intentional limitations.

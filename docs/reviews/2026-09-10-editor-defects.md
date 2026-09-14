# 编辑器缺陷验收清单 · 2026-09-10

状态更新（2026-09-14）：Debug 与 Release 全量 19 项回归均通过；`mcp/test_defect_acceptance.py` 在两种配置下通过 R1/R2/A2/A4/U4；A1/A3/U5 的运行断言在 `mcp/test_runtime_assets.py`，U2/U3 由 `editor_smoke` 的真实输入覆盖。限制范围照旧明示。

范围：编辑器缺陷修复与文档校正（工程令牌资源指纹修复，以及 README / docs 验收文案）。

编号严格沿用目标表中的 `R1–R5`、`A1–A5`、`U1–U6`，不新增替代编号。R 组为渲染与排版，A 组为 API / 自动化，U 组为交互 / 工程语义。另有两个“不是缺陷、是设计选择”的健壮性项单列。

状态约定：

- **已通过测试**：正式构建/运行中，对应用例实际通过。
- **限制（已文档化）**：有意保留的行为边界或设计约束。
- **未合并 / 未引入**：只存在于旧分支、或本轮明确不做。

权威验证结果（2026-09-14 实测）：

- **Debug**：完整构建成功；全量 19 项通过，`build/vs2022/Testing/Temporary/LastTest.log`（2026-09-10 16:04–16:06）逐项 `Test Passed`，含 `editor_formula_worker` 8.21 s、`editor_stdio` 19.64 s、`editor_runtime_assets` 26.95 s、`editor_background_job` 1.57 s。`editor_smoke` 曾在更早轮次因 JSON 空值初始化失败，修复后复测通过。
- **Release**：`cmake --preset vs2022 -B build/verify-runtime` 与 `cmake --build build/verify-runtime --config Release` 成功；`ctest --test-dir build/verify-runtime -C Release --output-on-failure` 退出码 0，全量 19/19 通过，67.38 s（`build/verify-runtime/Testing/Temporary/LastTest.log`，2026-09-14 16:57–16:58），含 `editor_formula_worker` 8.49 s、`editor_stdio` 16.72 s、`editor_runtime_assets` 22.42 s、`editor_mcp` 7.62 s、`editor_background_job` 1.57 s。五个 smoke 的验证与同步计数同为 0 错误 0 警告。
- **`mcp/test_defect_acceptance.py`**：Debug 与 Release 均退出码 0，通过 R1/R2/A2/A4/U4——2K 请求 57.4206 px → 实际 58 px，2560 px PNG 存在且非单色，工作进程日志 `Validation errors: 0; warnings: 0`；U4 以 121 s × 30 fps = 3630 帧触发明确拒绝（错误信息含 `3600`、无输出文件、文档时长保持 121 s）。证据 `build/defect-acceptance/acceptance-2a472c9e9e7f44dbb25e984692bb7d50.log`（Debug）、`acceptance-35716ad5fae24287aff6be85e588c294.log`（Release），样张 `r2-frames-<uuid>/frame_000000.png`。
- **U2/U3 真实 GUI 输入**：断言已编译进 Debug 与 Release 二进制（`apps/editor/editor.cpp::smokeInput` / `smokeAfterFrame`），两种配置的 `editor_smoke` 均通过。
- **命令执行**：最终 Release 全量命令由外部正式审批权限执行；本机沙箱内 shell 写 `build/` 受限，与产品行为无关。

## 渲染与排版（R）

| 编号 | 目标表描述 | 现状与证据 | 状态 |
| --- | --- | --- | --- |
| R1 | 字体图集无 U+2070–U+209F 上下标字形，`m₁`/`v₂` 渲染成 `m?`，静默失败无提示 | 交互图集补充区已含 `{0x2070,0x209F}`；启动探测主字体缺字并为缺字合并回退字体；无字体可绘制的字符给出警告（`src/platform/font_support.hpp` 补充区、`planCoverage`、`missingGlyphsForText`） | CPU 辅助测试通过（platform_font_support）；下标/nonBMP 警告经 Debug 与 Release 的 test_defect_acceptance 通过 |
| R2 | 离屏导出复用 18 px 字体图集，2K 文字是位图放大、明显发软 | 导出改为按实际绘制字符的按需字形列表，并按请求提高光栅字号，夹取到 `[8, min(256, 设备上限)]`（`src/platform/font_support.hpp::textCodepoints/clampRasterPixelSize`） | 已通过测试（platform_font_support + Debug/Release 的 test_defect_acceptance：2560×1788 PNG 请求 57.4206 px → 实际 58 px 字形）。样张下标正确；公式高倍仍受 R3 限制，未声称无损 |
| R3 | 超出 `referenceZoom` 的缩放会放大位图字形/公式遮罩，画面变软（固有代价） | 已文档化（`runtime-formulas-and-export.md`、`editor.md` 整体缩放段） | 限制（已文档化） |
| R4 | 组件实例 `scale` 不改变文字/公式字号，实例放大后标签相对变小 | 已文档化（`components-and-history.md`：实例等比缩放不改变字号与线宽） | 限制（已文档化） |
| R5 | CJK 无法进 LaTeX（preamble 无 CJK 包），公式图元只能放数学，混排标题必须拆成“文字 + 公式” | 设计约束，已文档化（`runtime-formulas-and-export.md`、`editor.md` 公式段） | 限制（设计约束） |

## API / 自动化（A）

| 编号 | 目标表描述 | 现状与证据 | 状态 |
| --- | --- | --- | --- |
| A1 | 没有接口读画布像素尺寸/视图参数，导出快照只能反解 | `get_view_state` 以物理像素暴露 `canvasWidth/canvasHeight`、`origin`、`pan`，`zoom` 与 `referenceZoom` 的单位为物理像素/米，并附带 `framebufferScale`（`apps/editor/editor.cpp::frameViewJson`）；运行时断言：`canvasReady`、正尺寸、`viewportSource=presented`、`presented.source=presented` 与 `view.source=live` 分离、`origin` 两分量、`zoom>0`（`mcp/test_runtime_assets.py`） | 已通过测试（Debug 与 Release 的 `editor_runtime_assets`） |
| A2 | `export_animation` 要求目标 scope 正在显示且画布已绘制过，纯脚本/无窗口无法导出 | `view_mode=fit` 由 `Editor::fitView(scope,w,h)` 为该作用域独立取景，不读取窗口或首帧；缺少记录视图时提示改用 fit（`apps/editor/editor_jobs.cpp`） | 已通过测试（Debug 与 Release 的 test_defect_acceptance：未呈现 component scope 的 `current` 明确拒绝，`fit` 成功且前端 scope 不变） |
| A3 | 导出必须显式给 `width/height`，无“与画布一致”选项，且按比例留白 | `width=height=0` 表示匹配当前画布，匹配时按偶数向下取整；也可显式偶数尺寸或用 fit 独立取景（`apps/editor/editor_jobs.cpp`、`mcp/server.py`）；运行时断言：`fit` 产出请求尺寸且 `viewport.mode=fit/scope=scene`，`current` 匹配画布时宽高为偶数且 ≥16，`fit` 配 `0/0` 明确拒绝（`mcp/test_runtime_assets.py`） | 已通过测试（Debug 与 Release 的 `editor_runtime_assets`） |
| A4 | `load_project` 后立刻显示未保存（星号），再保存一次才干净 | 加载结束以当前内容设定 `savedToken_`，布局/选择规范化不计入令牌；`editor_project` 覆盖“加载后不脏” | 已通过测试（editor_project + Debug/Release 的 test_defect_acceptance：save/change/load 后两次帧查询仍 clean） |
| A5 | 新增 MCP 工具对已连接客户端不可见，会话启动固定工具列表，需重连 | 在当前实现下，客户端在会话启动时取得固定工具列表，新增工具要刷新/重连后才可见；已文档化，作为当前保留限制。不断言协议层无法推送工具列表变化 | 限制（当前实现/客户端行为） |

## 交互 / 工程语义（U）

| 编号 | 目标表描述 | 现状与证据 | 状态 |
| --- | --- | --- | --- |
| U1 | 画布比窗口大（960×640 窗口 → 画布 1070×747），侧栏与播放条跑到屏幕外 | 启动位置按 `GetMonitorInfoW().rcWork` 工作区夹取（`src/platform/vulkan_host.cpp`）；`placeWindowInWorkArea` 的 CPU 用例覆盖常规工作区、小于期望尺寸、负坐标副屏、最小尺寸被压低与退化 1×1 工作区（`src/platform/font_support_test.cpp`，`platform_font_support`）；本机窗口生命周期由 smoke 覆盖（日志：窗口 1440×960 @ 560,284，工作区 2560×1528） | 已通过测试（工作区 CPU 用例 + 本机窗口生命周期，Debug 与 Release 的 smoke 均通过）；保留限制：不构成完整逐显示器 DPI 矩阵 |
| U2 | 平移/缩放会改 `layout`，而 `projectToken` 含 layout，一动视图就变脏 | `layout` 已从并发令牌排除，仅作为呈现状态保存/加载；`editor_project` 覆盖“仅 layout 不脏”；`editor_smoke` 注入真实画布滚轮缩放与中键拖动后，`get_view_state` 的 zoom/origin/pan 确实变化，而 `get_project.dirty` 仍为 false（`apps/editor/editor.cpp::smokeInput/smokeAfterFrame`） | 已通过测试（`editor_project` + `editor_smoke` 真实输入，Debug 与 Release 均通过） |
| U3 | 颜色连续调整产生多条历史记录，撤销粒度碎 | `beginColorEdit/commitColorEdit/cancelColorEdit`：控件松开或选择/范围切换时提交或取消，一次连续调整只产生一条历史（`apps/editor/editor.cpp`）；`editor_smoke` 点击打开真实 ColorEdit4 弹窗、在弹窗内按住拖动三帧后松开，断言颜色确已改变、revision 只 +1、`history.lastId` 只 +1，随后 undo 恢复原色 | 已通过测试（`editor_smoke` 真实输入，Debug 与 Release 均通过） |
| U4 | 长动画曾静默压缩时长（>3600 帧）；主干已改为明确拒绝，旧分支仍有 | `frames=ceil(duration*fps)` 要求 `1..3600`，超过即拒绝，不改变速度（`apps/editor/editor_jobs.cpp`）；`mcp/test_defect_acceptance.py::check_frame_limit` 以 121 s × 30 fps = 3630 帧触发拒绝，错误信息含 `3600`、无输出文件、文档时长保持 121 s | 已通过测试（Debug 与 Release 的 `test_defect_acceptance.py`）；旧分支未合并 |
| U5 | 旧分支导出中改窗口导致资源越界 → `DEVICE_LOST`；编码器缺失会终止编辑器 | 主干以隔离进程重写导出与公式编译，失败不退出前端；运行时断言：编码器缺失时导出 `failed`、工作进程 `worker.log` 保留、编辑器进程仍存活，导出期间缩放/最小化恢复继续，取消导出不退出前端（`mcp/test_runtime_assets.py`）；旧导出分支不可整体合并（见 `2026-09-09-branches.md`） | 已通过测试（Debug 与 Release 的 `editor_runtime_assets`）；旧分支未合并 |
| U6 | 旧分支单摆资产 `pend_radial = F_T=mg\cos\theta` 缺适用条件，主干未带入，需重做 | 本轮不引入单摆资产。若重做，绳张力径向通式为 `T = mg cosθ + mv²/l`；`T = mg cosθ` 只在瞬时速度为零时成立 | 未引入 |

## 两处“可更健壮”（设计选择，不是缺陷）

1. **强依赖可写工作目录**：编辑器启动要写 `<cwd>/history` 与 `<cwd>/build/jobs`。现支持显式 `--data-dir`；显式目录不可用时**明确失败、不静默替换**，自动模式下才回退到可写目录并在 `[runtime]` 行报告；场景与资源目录分别解析。状态：**已通过测试（editor_runtime_paths）**。
2. **在线接口不限于命名管道**：编辑器侧 `--stdio` JSON 行传输已实现（`apps/editor/main.cpp`、`src/editor/stdio_transport.cpp`），与 smoke/capture/export 模式互斥，协议占用 stdout、诊断走 stderr。它经 Python MCP bridge 封装后对外就是标准 MCP 协议，因此**不是“非完整 MCP”缺陷**；native 侧只负责 JSON 行传输。MCP 服务端 stdio 接入已经 MCP 端到端验证通过（`editor_stdio` 在 Debug 与 Release 全量中均通过），不再待接入。

## 本轮附带修复（不属于上表编号）

工程并发令牌的资源指纹修复不在目标表编号内，单独记录：

- 原指纹只混合 `name/width/height/latex`，同名同尺寸只改 `rle` 不改变令牌。
- 现 `mixValue()` 对每个 JSON 值加类型标签，字符串与容器加长度，空 array / 空 object / 空字符串 / null 不再彼此混同；这里的类型与长度标记消除的是此前确定性的结构性混淆（例如同为空容器、空串与 null 被折叠成同一个哈希输入），并不承诺 FNV-1a 64 位绝无碰撞，理论碰撞概率仍然存在。RLE 载荷与扩展字段全部参与；FNV-1a 64 位 offset basis 已修正为 `14695981039346656037`。
- 资源 API 收敛为 `const Json& resources() const` 单一只读入口加 `void setResources(Json)` 整份提交，删除可写 getter。`setResources` 与工程加载使指纹缓存失效，令牌按内容而非计数器变化；只读访问既不改令牌也不致脏。引用无法逃逸修改，缓存不会被绕过。
- 测试覆盖：`setResources` 同名同尺寸改 RLE → 令牌变化、复原恢复；只读遍历不改变令牌、不置脏；空 array/object/null 不再混同（结构混淆已消除，未声称无碰撞）；保存后不脏、重载后不脏。
- 状态：**已通过测试（editor_project）**；新增局部变量已重命名，消除 C4456 shadow 警告，测试逻辑未变。

## 验证状态总览

- **Debug**：完整构建成功；全量 19 项通过（`build/vs2022/Testing/Temporary/LastTest.log`，2026-09-10 16:04–16:06），五个 smoke 的验证与同步计数均为 `Validation errors: 0; warnings: 0`。
- **Release**：完整构建成功；`ctest --test-dir build/verify-runtime -C Release --output-on-failure` 退出码 0，全量 19/19 通过，67.38 s（`build/verify-runtime/Testing/Temporary/LastTest.log`，2026-09-14 16:57–16:58），五个 smoke 的验证与同步计数同为 0 错误 0 警告。
- **已由测试覆盖**：R1（下标/nonBMP 警告）、R2（按需高分辨率字形 57.4206→58 px；公式高倍无损仍受 R3 限制，未声称无损）、A1/A3（`mcp/test_runtime_assets.py` 运行断言）、A2、A4、U1（工作区 CPU 用例 + 本机窗口生命周期，非完整 DPI 矩阵）、U2/U3（`editor_smoke` 真实滚轮/中键输入与真实取色器拖动）、U4（`test_defect_acceptance.py` 超帧拒绝）、U5（隔离导出进程、编码器缺失与取消不退出前端）、`--data-dir`、stdio MCP 端到端、资源指纹（附带修复）。
- **范围与限制**：仅本机 Windows 10.0.22631 / NVIDIA RTX 4060 Laptop GPU 与当前驱动；不构成完整逐显示器 DPI 矩阵，未验证其他 GPU/驱动与跨平台。字体图集构建与重建已纳入本轮窗口及离屏导出回归。最终 Release 全量命令由外部正式审批权限执行（本机沙箱内写 `build/` 受限，与产品行为无关）。

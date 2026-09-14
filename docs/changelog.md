# 开发日志

## 统一编辑器入口 · 2026-09-14

- 新增 `build-editor.ps1` 作为唯一共用构建入口：按 checked-in `vs2022` 预设配置，再按 `release`（默认）/`debug` 构建预设以 `--target physics_editor` 增量构建；cmake 退出码非 0 立即停止，调用方不会启动旧的可执行文件。
- `run-editor.ps1` 保留 `-DebugBuild`/`-Project` 兼容，启动前先调用同一入口；固定启动 `build/vs2022/bin/editor/<配置>/physics_editor.exe`，构建失败提示先保存并关闭已打开的编辑器窗口（不自动结束进程），并把编辑器退出码返回给调用者；不提供跳过编译的回退路径。
- 新增 `run-editor-mcp.ps1`：同一构建入口，然后运行 `build/mcp-venv/Scripts/python.exe mcp/server.py --workspace <仓库根> --editor-transport stdio --start-editor <刚构建的 exe>`；构建进度与诊断只写 stderr，stdout 只承载 MCP 协议，Python 的标准输入输出直接继承（不重定向、不缓冲）；缺少 `build/mcp-venv` 时提示先运行 `.\mcp\setup.ps1`。stdio 传输启动自己的可见编辑器窗口，不附着旧命名管道。
- VS Code 的 **Physics editor (Debug)** 配置新增 preLaunchTask，调用同一构建入口；新增 `.vscode/tasks.json`（Debug/Release 两个构建任务），未改动通用 CMake 目标配置。
- 文档统一推荐 `run-editor.ps1` 启动（自动增量编译），说明 Debug/Release 是同一份源码的两个配置、`build/verify-runtime` 是历史验证用的临时构建目录而非常用入口、历史实现留在 Git 而构建产物不进 Git。
- 正式 Release 目录重新构建后通过 `editor_smoke`、`editor_mcp`、`editor_stdio` 三项回归（26.12 s）；统一 MCP 入口在 Windows PowerShell 5.1 与 PowerShell 7 下均通过真实构建、协议初始化、工具发现及可见窗口检查。构建失败不启动旧程序、退出码透传另经隔离替身检查；PowerShell / JSON 语法与差异检查通过。
- 移除已审查且无未提交修改的 `build/review-uniform` 工作树，删除 `build/editor-only` 与 `build/verify-runtime` 中的重复编辑器程序，保留日志和导出证据；历史分支及其提交仍由 Git 保存。日常编辑器产物只保留正式目录的同源 Debug / Release 两种配置。

## 验收完成 · 2026-09-14

- Debug 与 Release 全量 19 项回归均通过：Release `ctest --test-dir build/verify-runtime -C Release --output-on-failure` 退出码 0、19/19、67.38 s（`build/verify-runtime/Testing/Temporary/LastTest.log`，16:57–16:58）；Debug 19/19（`build/vs2022/Testing/Temporary/LastTest.log`）。两种配置均含 `editor_formula_worker`、`editor_stdio`、`editor_runtime_assets`、`editor_background_job`，五个 smoke 的验证与同步计数均为 0 错误 0 警告。
- `mcp/test_defect_acceptance.py` 在 Debug 与 Release 均退出码 0，通过 R1/R2/A2/A4/U4：2K 离屏光栅请求 57.4206 px → 实际 58 px，2560 px PNG 非单色；3630 帧导出被明确拒绝、无输出文件且时长不变。证据 `build/defect-acceptance/acceptance-2a472c9e9e7f44dbb25e984692bb7d50.log`（Debug）、`acceptance-35716ad5fae24287aff6be85e588c294.log`（Release）。
- U2/U3 的真实 GUI 输入断言（`editor_smoke` 滚轮/中键平移后工程仍 clean、真实取色器拖动只写一条历史且 undo 复原）在 Debug 与 Release 均通过；A1/A3/U5 的运行断言在 `mcp/test_runtime_assets.py`，U1 由 `placeWindowInWorkArea` 的工作区 CPU 用例与本机窗口生命周期覆盖，不称完整逐显示器 DPI 矩阵。
- 校正验收文档中过时或过强的表述：类型标签与长度标记只消除此前确定性的结构混淆（同为空容器、空串与 null 折叠成同一哈希输入），不承诺 FNV-1a 64 位无碰撞；早期 `editor_smoke` 失败的原因是 JSON 空值初始化，不是空指针；A5 按当前实现描述（新增工具需刷新/重连）。
- 保留约束不变：R3 超出 `referenceZoom` 的位图放大、R4 组件实例固定字号、R5 公式图元仅数学（中文需拆成文字 + 公式）、U6 旧错误单摆资产 `T = mg cosθ` 仍未引入、旧导出分支不可整体合并。

## 编辑器缺陷修复 · 2026-09-10

- 工程并发令牌的资源指纹改为完整覆盖：名称、尺寸、LaTeX 源、RLE 载荷及扩展字段一起参与，同名同尺寸只改 RLE 也会改变令牌；每个值带类型标签、字符串与容器带长度，空 array/空 object/空字符串/null 不再彼此混同——这一步消除的是此前确定性的结构性混淆，不承诺 FNV-1a 64 位绝无碰撞；FNV-1a 64 位 offset basis 修正。
- 资源 API 收敛为 `const Json& resources() const` 只读入口加 `void setResources(Json)` 整份提交，删除可写 getter；提交或加载工程时使指纹缓存失效，令牌按内容而非计数器变化。只读访问不改令牌、不置脏，每帧脏检查保持 O(1)。视图缩放/平移/布局仍不计入令牌。
- 验收清单见 [编辑器缺陷验收清单](reviews/2026-09-10-editor-defects.md)：按 R1–R5 / A1–A5 / U1–U6 标注状态。上一轮 editor/platform 定向回归通过（Release 58.15 s，含 `editor_formula_worker`；`editor_smoke` 修复后复测通过），Release/Debug 的 `mcp/test_defect_acceptance.py` 覆盖 R1/R2/A2/A4 并通过；该记录写入时 U2/U3 真实 GUI 输入 smoke 尚未完成，最终结果见 2026-09-14 记录。

## v0.4 · 2026-09-09

- 主干移植整体缩放，文字、公式、箭头和线条与几何共用缩放比例，工程保存字号基准。
- 新增运行时数学 LaTeX 输入与后台编译；源文本和不可变遮罩随工程保存，恢复历史不要求重新排版。
- 人类/MCP 共用异步任务接口；公式编译后的修改进入独立作用域撤销记录，期间并发修改会拒绝提交。
- PNG/MP4/GIF 导出使用冻结场景和独立进程中的固定尺寸 Vulkan 图像，可取消，编码失败不退出前端。
- 采样按真实 fps 保持时长，超帧数上限明确拒绝；原子提交输出，保留原有文件。
- 审查修正字体图集区域重建、TeX 紧页面裁切和 PNG 长度码 258 边界；新增资源/窗口/导出回归。
- 分支未整体合并：缩放选择性移植、导出重新实现；单摆等数据分支的审查结论见 [记录](reviews/2026-09-09-branches.md)。

## 主干修复 · 2026-09-09

- 公式库新增条目不再阻止旧工程打开；同名遮罩变化、缺失和重复仍拒绝。加载旧工程后保留完整安装库，避免再次保存时遗漏新资源。
- 在工作区交换前校验可选 `referenceZoom`，防止后续界面解析失败造成半加载状态。
- MCP 回归按公式资源与 LaTeX 清单逐项比对，不再写死 28 个资产。
- 本轮在 main 上修复；功能分支另行审查，尚未合并。


## v0.3 · 2026-09-08

- 布局/动画工作区、可拖动分隔条、紧凑对象列表和文件菜单。
- 有限表达式语言、命名物理量、分段公式、阶跃/线性/平滑关键帧与曲线编辑。
- 独立 .physproj 容器保存场景、组件草稿/已关闭草稿归档、动画、完整审计、撤销/重做、布局与公式遮罩。
- MCP 新增工程读写和动画采样，沿用统一动画事务。
- 运行时公式错误不会终止渲染循环。范围与限制见 [新功能说明](animation-and-projects.md)。

## 2026-09-08：v0.2 参数化组件与历史回溯

- 新增圆弧及旋转矩形；组件定义包含局部图元、数值参数、范围、绑定表达式和校验约束。主场景实例可整体移动/旋转/等比缩放。
- 新增主场景与组件草稿标签页，分别保存、撤销、记录历史；发布为一条主场景事务，内部逐次编辑仍留在草稿日志。
- 操作历史在画布下方显示，可用列表或滑块回溯只读状态；恢复旧状态是新的可撤销事务。运动时间和编辑修订分开。
- MCP 增加显式 scene/component scope、组件生命周期与历史回溯接口；实际呈现状态包含文档 scope。共 18 个工具。
- 用真实“斜面接圆弧”题目建立可复现的参数化资产。R/L/倾角变化保持相切和同高关系；沿轨道动力学留待后续模型。
- 建立本地 Git 基线、开发指南、验收计划、设计决策与测试记录。具体证据见 [验证记录](verification.md)，使用见 [组件与历史](components-and-history.md)。

兼容性：场景 version 仍为 1，旧的图元和命令继续支持；组件为新增类型与字段。旧版编辑器不能打开含未知新图元的场景，应使用 v0.2。当前组件定义仅支持静态图元及数值公开参数，不支持嵌套。

## 基线 v0.1

原生 Vulkan 编辑器、9 类图元、匀加速运动/绑定、独立学习示例、LaTeX 资源、统一 UI/MCP 编辑命令、可查看的操作日志与磁盘 JSONL。历史列表尚不能拖动回溯；尚无参数化组件。

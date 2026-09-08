# 物理示意编辑器 v0.2

> v0.3 更新：[动画工作区与独立工程](animation-and-projects.md)。顶部改为菜单和工作区；工程保存包含场景、草稿和历史。下文保留基础文档/组件机制，场景 JSON 是交换格式。


原生 C++ / Vulkan 桌面程序。可用鼠标编排运动示意，也可通过 MCP 操作同一个正在运行的场景。原有立方体与运动学 demo 仍是独立目标。

![实际 Vulkan 界面](editor-component-preview.png)

新增功能与完整流程见 [参数化组件和历史回溯](components-and-history.md)。

## 启动

在项目根目录执行：

```powershell
cmake --preset vs2022
cmake --build --preset release --target physics_editor
.\run-editor.ps1
```

调试版使用 `cmake --build --preset debug --target physics_editor` 和 `./run-editor.ps1 -DebugBuild`。VS Code 选择新增的 **Physics editor (Debug)** 配置；Visual Studio 将 `physics_editor` 设为启动项目。可以用 `-DBUILD_01_CUBE=OFF -DBUILD_02_KINEMATICS=OFF` 单独配置编辑器。

## 编辑流程

1. 从右侧“添加图元…”菜单添加质点、线段、箭头、圆、圆弧、矩形、折线、地面、文字或公式。
2. 点击画布图形或对象列表进行选择。拖动图形会暂停动画并修改初始位置，一次拖动是一条可撤销命令。锁定对象仍可在属性面板编辑。
3. 右侧输入数值或文字后按 Enter 提交。`x/y` 是初始位置；线段和箭头的 `dx/dy` 是相对端点。折线顶点可在“完整属性 JSON”中修改。
4. 给图元添加 `vx/vy/ax/ay` 匀加速运动，或绑定到另一个独立对象。速度箭头使用目标的瞬时速度；其他绑定表示位置跟随。不能同时拥有自己的运动和绑定，也不允许绑定链。
5. 使用播放、时间轴和“解题步骤”预览。对象的“出现步骤”决定何时显示，步骤从 0 编号。“步骤数”减少到已有对象步骤以下时会拒绝，应先修改对象。
6. 使用文件菜单保存或打开 `.physproj` 工程；`.json` 路径仅用于当前文档导入/导出。文档导入、删除、修改都可撤销/重做；工程打开先验证再切换，不属于文档内撤销；关闭有未保存修改的窗口会提示。运行时的播放进度不写入场景文件，重新打开从当前合法进度预览，可点击“|<”。

中键平移、滚轮缩放、“适应画布”居中显示当前内容。Ctrl+Z / Ctrl+Y 撤销重做，Ctrl+S 保存整个工程。完整 JSON 编辑区保存未识别的扩展字段，核心结构错误会拒绝提交。颜色控件的连续调整目前产生多条历史记录；拖动画布只产生一条。

## 单位与物理含义

场景坐标使用米，时间使用秒，正 x 向右、正 y 向上。速度是 m/s，加速度是 m/s²。每个独立运动对象按解析式计算：

\[
\mathbf r(t)=\mathbf r_0+\mathbf v_0t+\tfrac12\mathbf at^2,
\qquad \mathbf v(t)=\mathbf v_0+\mathbf at.
\]

速度箭头长度使用单独的比例，不与位移共用量纲；地面只是示意图元，不会自动产生碰撞。默认上抛场景将时长设为返回地面的时间；改变初始条件后需要同步调整时长、文字和公式。这一版不自动证明文字与运动参数一致，也不解析自然语言题目、不自动求解或执行通用物理仿真。

公式采用真实 LaTeX 排版缓存，可选 28 个公式资源。新增表达式修改 [公式资源](../assets/formulas/README.md) 并重新生成、编译；没有任意 LaTeX 的实时输入控件。文字字号、线宽与公式显示大小使用屏幕尺寸，便于缩放场景时阅读。

## MCP

初始化独立 Python 环境：

```powershell
.\mcp\setup.ps1
```

注册到本机 Codex（需已经编译 Release）：

```powershell
.\mcp\setup.ps1 -RegisterCodex
```

服务名 `physics-editor`，使用官方 MCP Python SDK 1.30.0 的 stdio 传输。服务可自动启动编辑器；宿主结束 MCP 进程时可能一并回收自动启动的窗口，因此退出或重载服务前应先保存场景。需要独立使用窗口时，先手动运行 `run-editor.ps1`，MCP 会连接已有窗口。Codex 重新加载 MCP 配置后才会发现新增工具；接入方式见 [官方文档](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)。

提供 22 个工具，基础 12 个为：`get_catalog`、`get_scene`、`apply_commands`、`replace_scene`、`undo`、`redo`、`set_playback`、`save_scene`、`load_scene`、`show_editor`、`get_view_state`、`get_history`。新增 6 个组件与历史回溯工具见 [接口说明](components-and-history.md#mcp-合同)。文档工具接受 scope=scene/component，默认 scene；不根据当前标签页猜测范围。先发现能力和读取场景；修改时提交最近读取的 `expected_revision`。不同客户端同改场景时，旧修订被拒绝，不会默默覆盖。调用超时后先查询状态，不能直接重发修改。

### MCP 操作时显示原生窗口

默认勾选“跟随 MCP 显示”。MCP 修改场景或时间轴时，编辑器会显示窗口、从最小化状态恢复，并使用与人类操作相同的画布渲染。没有另建一份无界面的场景。Windows 可能限制应用抢占键盘焦点，因此恢复显示不承诺始终抢占前台输入。

`show_editor` 可显式请求显示窗口。`get_view_state` 返回可见/最小化状态、已呈现帧数、最近已呈现画布的 `presentedRevision` 和 `presentedTime`；这能区分“命令已接受”和“新状态已提交呈现”，不表示显示器完成物理扫描。取消“跟随 MCP 显示”后可以自行最小化窗口，后台请求仍正常响应。

### 人和 MCP 的统一操作历史

![独立历史面板的实际渲染截图](editor-component-history.png)

点击顶部“操作历史”查看当前文档的会话记录。滑块和成功修改记录可进入历史只读预览，恢复此状态会生成新的可撤销操作；历史时间轴与物理秒数时间轴分开。创建、修改、删除、移动、排序、场景设置共用相同的编辑命令；保存/打开、撤销/重做以及主动播放/定位请求也有日志。每条记录包含序号、UTC 时间、来源、原始命令参数、前后修订、播放状态、执行结果；失败请求附带原因，撤销/重做关联原操作序号。

人类可直接撤销 MCP 的修改，MCP 也可撤销人类的修改。批量命令按一次事务记录和撤销；画布拖动在松开鼠标时提交一条 move。鼠标位置变化、查询、自动播放的每一帧不写入日志。颜色及时间轴滑块仍按每次实际提交记录，尚未合并整个滑动手势。

日志自动写入启动目录的 `history/session-*.jsonl`，通常位于项目根目录，首行保存会话初始场景，后续逐行保存操作；加载文件时还保存解析后的文档，避免日志只依赖一个可能变化的文件名。每条记录刷新到文件，文件不会随应用正常退出删除。磁盘写入失败会给出明确提示，内存操作仍可继续，应保存场景并检查日志状态。

界面和 `get_history(after, limit)` 分页查询完整审计，单页最多 100 条。工程保存完整审计及有界撤销快照，重开后可继续撤销；独立 JSONL 仍用于会话恢复检查。很大的请求在界面/API 摘要中省略参数，完整参数保存在工程和磁盘日志中。编辑历史状态保留最近 101 个，撤销最多 100 步。

来源 `human` 表示界面入口，`mcp` 表示 MCP 入口，`system` 表示系统操作。MCP 也可能由人类客户端发起，所以这个字段不冒充真实身份认证。

例如，读取修订号后，用 `apply_commands` 添加一支箭头：

```json
{
  "expected_revision": 0,
  "commands": [{
    "op": "create", "id": "force-arrow", "type": "arrow", "name": "辅助箭头",
    "properties": {"x": 3, "y": 4, "dx": 2, "dy": 0}
  }]
}
```

多个相关命令放进同一个数组，只产生一条历史记录。可读回文档确认结果，再移动时间轴检查运动；MCP 不通过模拟鼠标来实现编辑。保存/打开限制在配置的工作区内；没有 shell 执行、任意代码或远程监听工具。本地管道只允许当前所有者访问并拒绝远程客户端。

可手动连接已有编辑器，隔离不同实例：

```powershell
.\build\vs2022\bin\editor\Release\physics_editor.exe --pipe lesson-a
.\build\mcp-venv\Scripts\python.exe mcp/server.py --pipe lesson-a
```

同一管道名只允许一个编辑器，避免误连。标准输出只用于 MCP；原生程序日志走服务的标准错误。

## 验证

```powershell
cmake --build --preset debug
ctest --preset debug --verbose
.\build\mcp-venv\Scripts\python.exe mcp/test_integration.py
```

核心测试覆盖批量原子性、修订冲突、撤销/重做、解析运动、引用完整性、无效值、中文路径往返和注册新图元/命令。UI smoke 注入真实 ImGui 鼠标事件，验证添加和拖动，再覆盖窗口缩放、最小化恢复和关闭。MCP 测试通过官方客户端连接真实原生编辑器，验证工具发现和实际编辑，不使用假的场景后端。

`mcp/demo_client.py` 是可读的官方 SDK 客户端示例，会连接/启动编辑器、读取场景并定位时间。`scenes/vertical-throw.json` 是通过 MCP 保存的默认上抛场景，可直接在编辑器中打开。

## 尚未包含

任意多个同时打开的文档、自动恢复、嵌套组件/层级、通用约束求解、碰撞、沿曲线轨道的动力学、逐字推导动画、任意 LaTeX 即时排版、插件热加载、导出视频或运行时截图 MCP 工具。现在的目标是一个能编辑、保存并接受程序控制的可靠小型基础版本。

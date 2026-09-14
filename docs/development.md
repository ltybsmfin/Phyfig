# 长期开发指南

## 文档分工

- `README.md`：项目入口、启动、编译、常用链接。
- `docs/editor.md`：用户操作与 MCP 使用指南。
- `docs/editor-architecture.md`：当前实现的模块边界和扩展入口。
- `docs/roadmap.md`：本轮目标、验收标准与后续工作。
- `docs/decisions/`：影响长期维护的设计决策及原因。
- `docs/changelog.md`：已完成的行为变化、兼容性说明和已知限制。
- `docs/verification.md`：执行过的测试、环境和证据；不把计划写成验证事实。
- `docs/learning-notes.md`：物理、图形学与术语的学习解释。

## Git 工作方式

项目使用本地 Git 版本管理。当前已安装的作者配置沿用用户设置。不自动创建远程仓库或推送；远程备份可另行指定。

功能尽量按可审阅的职责提交：基线、核心模型、界面/MCP、验证与文档。提交前检查 `git diff --check`、暂存文件列表及相关测试；不能把生成目录、会话日志或驱动缓存提交。第三方来源与许可随源码保留，依赖升级需要独立说明。

工程数据与运行数据分开：`assets/` 和 `scenes/` 中的示例进入 Git；`build/`、`history/` 和个人场景不自动进入 Git。公式缓存是可复现的必要构建资源，因此连同生成器、源公式和摘要一起管理。

## 开发与验证

编辑器入口统一走脚本，脚本内部就是对 checked-in 预设的增量构建：

```powershell
.\build-editor.ps1        # 只构建 physics_editor（Release；-DebugBuild 切 Debug）
.\run-editor.ps1          # 增量编译后启动编辑器
.\run-editor.ps1 -DebugBuild
.\run-editor-mcp.ps1      # 增量编译后以 stdio 传输启动 Python MCP 服务
```

`run-editor.ps1`、`run-editor-mcp.ps1` 和 VS Code 的编辑器任务共用 `build-editor.ps1`：先 `cmake --preset vs2022` 配置，再按 `release`/`debug` 预设增量构建 `physics_editor`；cmake 退出码非 0 就停止，绝不启动旧的可执行文件。规范产物在 `build/vs2022/bin/editor/<配置>/`，评审或验证用的临时目录（如 `build/verify-runtime`）只是历史构建位置，不作为日常入口；历史实现留在 Git，产物不进 Git。

完整构建与测试仍直接使用预设：

```powershell
cmake --preset vs2022
cmake --build --preset debug
ctest --preset debug --output-on-failure
cmake --build --preset release --target physics_editor physics_document_test physics_component_test vulkan_kinematics kinematics_model_test
ctest --test-dir build/vs2022 -C Release -R 'editor|kinematics' --output-on-failure
```

原生 UI、文档与管道的变化分别使用数学/模型测试、真实 ImGui 输入 smoke 和官方 MCP 客户端集成测试。修改窗口/渲染生命周期时，必须覆盖呈现、缩放、最小化恢复与资源清理。测试要验证用户可观察行为，不能只确认 JSON 中多了一个字段。

## 兼容性纪律

持久化标识使用稳定字符串与显式格式版本。改变类型/命令含义时，新增版本或迁移，不能让旧场景静默变形。组件公开参数有类型、范围、默认值及单位；内部图元 ID 应稳定，不能将数组序号当作永久引用。

扩展保持分层：几何/参数求值放核心层；鼠标命中和绘制放视图层；MCP 调用同一业务入口。避免在渲染回调里写文件或改变文档，避免根据 UI 焦点推断 MCP 的目标文档。

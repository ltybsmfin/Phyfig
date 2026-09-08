# LaTeX 数学公式排版与渲染

建议把需求表述为：“使用 LaTeX 数学语法排版并渲染公式，正确显示上下标、分式、根号、希腊字母和矢量箭头。”LaTeX 是这里使用的排版语法/系统；本机安装的是 **MiKTeX 24.1**，它是包含 TeX/LaTeX 引擎及宏包的发行版，并不是名为“mitex”的单一公式控件。[MiKTeX 官方介绍](https://miktex.org/about)

例如，`\vec v_0` 表示带初始下标的速度矢量，`v_y` 表示带符号的竖直速度分量；它们与速率（速度大小）的含义不同。`\frac{1}{2}gt^2` 会排成真正的分式与上标，不会把源码中的斜杠、花括号直接显示出来。

## 本次实现

1. `formulas.json` 保存 28 个命名公式的 LaTeX 源码。
2. `generate.py` 使用本机 MiKTeX `pdflatex` 和 `amsmath` 排版。
3. pypdfium2 将排版结果转为透明度蒙版，生成 `formula_cache.hpp`。公式以白色蒙版保存，可按草稿或暗色界面着色。
4. 应用把蒙版加入现有 ImGui 字体图集，由 Vulkan 后端显示；上下标、矢量符号和分式均来自真实排版结果。

这次生成已在本机运行成功，28 个公式的排版 PDF 与检查用 PNG 位于项目 `build/latex-formulas/`。最初尝试的 `dvipng` 在本机未完成，因此最终生成流程改用 `pdflatex + pypdfium2`，运行程序不依赖 dvipng。

正常 C++ 构建与运行不需要 Python 或 MiKTeX，直接使用已保存的缓存。缓存按 2 倍显示分辨率生成，属于高分辨率位图，不是可无限放大的矢量图。公式保持符号形式，数值读数由界面实时更新；目前没有“用户输入任意 LaTeX 即时预览”或逐帧重新排版。

## 修改公式

在装有 `pdflatex`、Python 的 Pillow 与 pypdfium2 包的环境中执行：

```powershell
python assets/formulas/generate.py
cmake --build --preset debug --target physics_editor vulkan_kinematics
```

MiKTeX 需要已安装 `amsmath` 和标准 Computer Modern 字体。本次使用已有本地宏包完成生成；脚本禁用自动安装和 shell escape，仅用于可信的项目源公式。缓存记录源码 SHA-256，CMake 配置会检查它，避免源公式修改后仍显示旧缓存。

以后如果确实需要实时修改公式或无损缩放，再考虑异步排版缓存、SVG/曲线图形输出及相应渲染后端，不把这些复杂性提前加入当前 demo。

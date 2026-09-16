# LaTeX 数学公式排版与渲染

建议把需求表述为：“使用 LaTeX 数学语法排版并渲染公式，正确显示上下标、分式、根号、希腊字母和矢量箭头。”LaTeX 是这里使用的排版语法/系统；本机安装的是 **MiKTeX 24.1**，它是包含 TeX/LaTeX 引擎及宏包的发行版，并不是名为“mitex”的单一公式控件。[MiKTeX 官方介绍](https://miktex.org/about)

例如，`\vec v_0` 表示带初始下标的速度矢量，`v_y` 表示带符号的竖直速度分量；它们与速率（速度大小）的含义不同。`\frac{1}{2}gt^2` 会排成真正的分式与上标，不会把源码中的斜杠、花括号直接显示出来。

## 内置 demo 公式库

1. `formulas.json` 保存 28 个命名公式的 LaTeX 源码。
2. `generate.py` 使用本机 MiKTeX `pdflatex` 和 `amsmath` 排版。
3. pypdfium2 将排版结果转为透明度蒙版，生成 `formula_cache.hpp`。公式以白色蒙版保存，可按草稿或暗色界面着色。
4. 同一次生成还写出 `formula_writing_plan.hpp`：每个公式的**书写顺序计划**，供 Write 手写入场按结构/字形推进（分式先分数线→分子→分母、嵌套递归、外层从左到右、底数后紧跟上下标、根号与被开方连续、算子先于它的上下限、箭头/帽子跟在底数之后）。它只重新排序/分组掩码里已有的墨迹连通域，不改变任何像素；`formula_cache.hpp` 逐字节不变。
5. 应用把蒙版加入现有 ImGui 字体图集，由 Vulkan 后端显示；上下标、矢量符号和分式均来自真实排版结果。

这次生成已在本机运行成功，28 个公式的排版 PDF 与检查用 PNG 位于项目 `build/latex-formulas/`。最初尝试的 `dvipng` 在本机未完成，因此最终生成流程改用 `pdflatex + pypdfium2`，运行程序不依赖 dvipng。

正常 C++ 构建与运行不需要 Python 或 MiKTeX，直接使用已保存的缓存。缓存按 2 倍显示分辨率生成，属于高分辨率位图，不是可无限放大的矢量图。公式保持符号形式，数值读数由界面实时更新；编辑器另有运行时公式输入和后台编译功能，见 [公式与导出](../../docs/runtime-formulas-and-export.md)；不是逐帧重新排版。

## 修改内置公式（普通用户新增公式无需此步骤）

在装有 `pdflatex`、Python 的 Pillow 与 pypdfium2 包的环境中执行：

```powershell
python assets/formulas/generate.py
cmake --build --preset debug --target physics_editor vulkan_kinematics
```

MiKTeX 需要已安装 `amsmath` 和标准 Computer Modern 字体。本次使用已有本地宏包完成生成；脚本禁用自动安装和 shell escape，仅用于可信的项目源公式。缓存记录源码 SHA-256，CMake 配置会检查它，避免源公式修改后仍显示旧缓存。

生成器按 LF 写出两个头文件（`.gitattributes` 把缓存标记为 `-text` 的精确字节资产），所以在 Windows 上重新生成 `formula_cache.hpp` 不会整文件变成 CRLF。`tools/formula_writing_plan.py` 负责计划推导：

* **字形按对象标识归属**：`get_textobj(i).raw` 的地址与 `page.get_objects()` 对象地址一一对应，包围盒重叠也不会抢错字形；没有所属对象的合成空白直接过滤。
* 字形取 text page 的**紧**字符框（`get_charbox(..., loose=False)`，即墨迹范围而不是会盖住被开方区域的 advance 框），规则线取 page object 的精确 bounds。
* **主体优先**：常见算子的上下限、向量箭头与帽子即使先出现在内容流里，也会排到主体之后。该规则依赖几何关系，不保证所有跨行装饰；整式上横线可能仍先于内容出现。
* **分式**：分子/分母是"从分数线到最近墨迹远边"的连通带；同侧墨迹被多条分数线认领时，"某侧只有这一个候选"的分数线优先，其次内层优先；嵌套必须是严格单向的（间隙 <= 2 × 最近间隙），因此 aligned 里上下两行各自的分式互不认领。逐 texel 归属仍取"包含该 texel 的最小框"，所以根号框不会吞掉被开方字形。

已知限制：`\overbrace` 这类比内容更高的装饰会被当成主体；整式上横线与所在分子一起输出；不做完整 TeX 语法解析。任何一步不可靠时该公式就不带计划，渲染与显示照常。改了生成器后必须重新运行 `python assets/formulas/generate.py` 才能刷新内置计划表。

运行时新增公式已经采用异步排版和工程内资产保存（计划随资产一起存进 `.physproj`）；SVG/曲线图形输出与无损矢量导出尚未实现。

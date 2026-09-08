# 参数化组件资产

`incline-arc.json` 是可独立保存/加载的组件定义；`scenes/incline-arc.json` 是含该定义快照和一个实例的完整场景。

从根目录运行 `python assets/components/generate_incline.py` 可重新生成这两个示例文件。生成器只用于维护该示例；运行时依赖通用 C++ 参数求值器，不运行 Python，也不按组件 ID 硬编码图形。

手工修改示例后再运行生成器会重新生成示例，请把自己的组件另存为新文件。格式、参数约束、坐标、限制与编辑方法见 [组件说明](../../docs/components-and-history.md)。

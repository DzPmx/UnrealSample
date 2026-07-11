# FoliageBaker

FoliageBaker 是一个 Unreal Editor 植被远景代理烘焙插件，用于从 Static Mesh 的指定 LOD 生成 Billboard、Cross Cards 和 BillboardClouds 资产。

插件位于 [`Plugins/FoliageBaker`](Plugins/FoliageBaker)。更完整的参数和输出契约请参阅[插件 README](Plugins/FoliageBaker/README.md)。

当前版本为实验性编辑器工具，不包含运行时模块。

## 功能状态

| 功能 | 状态 | 当前行为 |
| --- | --- | --- |
| Single Billboard | 可用 | 从用户指定的 `+X`、`-X`、`+Y` 或 `-Y` 方向正交拍摄，只生成和烘焙一个面 |
| Cross Cards | 可用 | 生成 2–5 个在 180° 内等角度分布的垂直相交平面，每个角度独立裁切并烘焙正反面 |
| Impostor | 未实现 | 当前只有标签和占位界面，不生成资产 |
| BillboardClouds | 可用 | 使用 K-Means 生成自适应平面云，可选独立树干 Cross Cards |

四种功能统一位于 `Tools > Foliage Baker` 工具窗口中，标签顺序为 Single Billboard、Cross Cards、Impostor、BillboardClouds。

## 核心规则

- 只接受 Static Mesh。
- Source LOD 可由用户选择，不固定使用 LOD0。
- Single Billboard 和 Cross Cards 的平面穿过 Static Mesh 本地原点，也就是资产 Pivot。
- Single Billboard 使用一个固定轴向视角；Cross Cards 对每个平面分别烘焙正反面。
- BillboardClouds 只保留 K-Means 技术路径。
- 已实现的烘焙路径不执行源材质的 WPO 或 Displacement。
- 纹理分辨率硬上限为 4096。
- 支持批量烘焙和重新烘焙。

## 纹理输出

### BaseColor / Opacity

- RGB 保存 Base Color。
- Single Billboard 和 Cross Cards 的 A 通道保存覆盖与树干/树叶分类：背景为 0，树干为 0.5，树叶为 1。
- BillboardClouds 的 A 通道保存源 Opacity Mask，树干与树叶分类保存在代理网格 UV2 中。

### Normal / Depth

- RGB 保存 object/local-space Normal。
- A 保存线性深度。
- 同一次代理烘焙的所有视角共享同一组深度范围。
- 全局最近点映射为 0，全局最远点映射为 1，未覆盖像素为 1。

### Mix

可选 Mix 纹理的 RGBA 依次保存 Occlusion、Roughness、Metallic 和 Emission。

### Atlas

- Single Billboard 和 Cross Cards 始终按每个视角的有效 Alpha 范围裁切。
- 可选移除 Atlas 外围完全未使用的行列。
- Tile 内未覆盖区域使用最近有效像素向边界扩张 RGB，不需要固定 Padding Pixel 参数，也不改变 UV 利用率。

## 材质模板

插件只接受 Material Instance Constant 模板，不创建或修改材质图。生成的纹理会写入用户指定的参数名称，默认参数为：

- `ColorOpacity`
- `NormalMask`
- `Mix`

插件内置 Single Billboard、Cross Cards、BillboardClouds 和预留 Impostor 的材质及材质实例资产。

## 网格与资产输出

支持以下网格输出方式：

1. 创建独立 Static Mesh。
2. 追加到源 Static Mesh 的 LOD。
3. 替换源 Static Mesh 的指定 LOD。

替换目标不能与 Source LOD 相同。生成网格不创建碰撞和 Lightmap UV，并使用完整编辑器构建流程生成距离场派生数据。

纹理、材质实例和代理网格支持可配置命名。单个 Static Mesh 的资产写入使用事务式快照；烘焙失败时会恢复已有资产并删除本次未提交的新资产。

## 使用方法

1. 启用 `FoliageBaker` 插件。
2. 在 Unreal Editor 中打开 `Tools > Foliage Baker`。
3. 选择功能标签。
4. 在 Content Browser 选择一个或多个 Static Mesh，并添加到工具列表。
5. 设置 Source LOD、网格输出方式、纹理输出和 Material Instance Constant 模板。
6. 点击对应功能的 Bake 按钮。

## 模块结构

- `FoliageBakerCore`：公共网格、Atlas、材质烘焙和原子资产写入。
- `FoliageBakerCards`：Single Billboard 与 Cross Cards。
- `FoliageBakerBillboardClouds`：K-Means BillboardClouds。
- `FoliageBakerEditor`：统一工具窗口和功能标签。

## 当前限制

- Impostor 尚未实现。
- 不接受 Skeletal Mesh 或其他非 Static Mesh 输入。
- 不生成材质图、碰撞或 Lightmap UV。
- 当前没有自动视觉误差验收样本或阈值。

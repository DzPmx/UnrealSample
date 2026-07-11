# FoliageBaker

FoliageBaker 是一个 Unreal Editor 插件，用于从 Static Mesh 的指定 LOD 烘焙远景植被代理。插件使用统一工具窗口承载不同烘焙方式，并由公共 Core 模块负责网格提取、材质属性烘焙、Atlas 处理和资产写入。

当前版本为实验性编辑器工具，不包含运行时模块。

## 功能状态

| 功能 | 状态 | 当前行为 |
| --- | --- | --- |
| Single Billboard | 可用 | 从 `+X`、`-X`、`+Y` 或 `-Y` 方向进行一次正交拍摄，生成一个穿过资产 Pivot 的垂直平面，只烘焙一个面 |
| Cross Cards | 可用 | 生成 2–5 个在 180° 内等角度分布的垂直相交平面，每个角度独立裁切并烘焙正反面 |
| Impostor | 未实现 | 工具中仅保留标签和占位界面，不生成资产 |
| BillboardClouds | 可用 | 使用 K-Means 生成自适应平面云，可选独立的树干 Cross Cards |

## 输入与拍摄规则

- 只接受 Static Mesh。
- Source LOD 可以选择，不要求固定使用 LOD0。
- Single Billboard 和 Cross Cards 使用所选 Source LOD 的全部三角形进行固定视角正交投影。
- Single Billboard 和 Cross Cards 的平面穿过 Static Mesh 本地原点，也就是资产 Pivot。
- Cross Cards 的每个角度分别计算投影范围、可见关系和 Alpha 裁切。
- BillboardClouds 只保留 K-Means 技术路径。
- 已实现的烘焙路径读取未执行顶点变形的 Source LOD，并通过 Unreal MaterialBaking 导出材质属性；WPO 和 Displacement 在导出代理中为 0。
- 材质图中与颜色、透明度或其他属性直接相关的时间和风参数不会被统一禁用，只有顶点位移不参与烘焙。

## 纹理输出

纹理分辨率范围为 256–4096，硬上限为 4K。Single Billboard 默认 1024，Cross Cards 和 BillboardClouds 默认 2048。

### BaseColor / Opacity

- RGB 保存 Base Color。
- Single Billboard 和 Cross Cards 的 A 通道在源材质透明度裁切后保存分类：背景为 0，树干为 0.5，树叶为 1。
- 树干分类使用材质实例名称或父材质名称关键字，默认关键字为 `Trunk`。
- BillboardClouds 的 A 通道保存源 Opacity Mask，树干与叶片分类保存在代理网格 UV2 中。

### Normal / Depth

- RGB 保存 object/local-space Normal。
- A 保存线性深度。
- 同一次代理烘焙的所有视角共享同一组深度范围。
- 全局最近的所选 LOD 几何点映射为 0，全局最远点映射为 1，未覆盖像素为 1。

### Mix

Mix 输出为可选项，RGBA 分别保存：

1. Occlusion
2. Roughness
3. Metallic
4. Emission

### Atlas 优化

- Single Billboard 和 Cross Cards 始终按每个视角的有效 Alpha 范围裁切，`Per-View Alpha Crop Guard` 控制额外保留边界。
- Single Billboard 默认开启 `Trim Unused Atlas Space`；Cross Cards 可按需开启。启用后允许输出块对齐的矩形纹理。
- BillboardClouds 可以在最终打包前按每个平面的 Alpha 外边界裁切。
- Atlas Tile 内未覆盖区域使用最近的有效像素向外填充 RGB，不依赖固定 Padding Pixel 参数，也不会改变代理 UV 的有效占用范围。

## 材质模板

插件只接受 Material Instance Constant 模板，不创建或修改材质图。烘焙完成后会复制模板，并把已启用的纹理写入用户指定的纹理参数名称。

内置默认模板：

- Single Billboard：`/FoliageBaker/Materials/MR_Foliage_Billboard`
- Cross Cards：`/FoliageBaker/Materials/MR_Foliage_Cross`
- BillboardClouds：`/FoliageBaker/Materials/MR_Foliage_BillboardClouds`

默认纹理参数名称：

- `ColorOpacity`
- `NormalMask`
- `Mix`

这些名称都可以在 Material 分类中修改。

## 网格输出

每个可用功能支持三种输出方式：

1. 创建独立 Static Mesh 资产。
2. 追加到源 Static Mesh 的 LOD。
3. 替换源 Static Mesh 的指定 LOD。

替换模式下，目标 LOD 不能与 Source LOD 相同，以便保留重新烘焙所需的输入几何。

生成的代理网格：

- 不生成碰撞。
- 不生成 Lightmap UV。
- 使用完整编辑器网格构建流程生成距离场派生数据。
- 保留源 Static Mesh 的本地坐标系和 Pivot 语义。

## 资产位置与重新烘焙

- 独立代理网格生成在源 Static Mesh 所在目录。
- Texture 和 Material 文件夹相对于源 Static Mesh 目录的父目录计算。
- 默认 Texture 文件夹为 `Textures`，Material 文件夹为 `Materials`。
- 纹理、材质实例和代理网格名称的前缀与后缀都可以配置。
- 批量烘焙按每个 Static Mesh 独立执行。
- 每个 Static Mesh 的资产写入使用事务式快照：失败时恢复本次修改前的已有资产，并删除本次创建但未提交的资产。

## 使用方法

1. 在 Unreal Editor 主菜单中打开 `Tools > Foliage Baker`。
2. 选择 `Single Billboard`、`Cross Cards` 或 `BillboardClouds` 标签。
3. 在 Content Browser 中选择一个或多个 Static Mesh，然后点击 `Add Content Browser Selection`，也可以直接在 Mesh 列表中指定资产。
4. 设置 Source LOD、网格输出方式和功能参数。
5. 选择 Material Instance Constant 模板并确认纹理参数名称。
6. 点击当前功能的 Bake 按钮。

## 模块结构

- `FoliageBakerCore`：公共网格、Atlas、材质烘焙和原子资产写入服务。
- `FoliageBakerCards`：Single Billboard 与 Cross Cards 的设置、界面和请求组装。
- `FoliageBakerBillboardClouds`：K-Means BillboardClouds 功能。
- `FoliageBakerEditor`：统一工具窗口、功能标签和 Impostor 占位界面。

## 当前限制

- Impostor 尚未实现。
- 不接受 Skeletal Mesh 或其他非 Static Mesh 输入。
- 不生成材质图。
- 不生成碰撞和 Lightmap UV。
- 当前没有自动视觉误差验收样本或阈值。

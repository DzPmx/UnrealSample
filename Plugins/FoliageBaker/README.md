# FoliageBaker

FoliageBaker 是一个 Unreal Editor 插件，用于从 Static Mesh 的指定 LOD 烘焙远景植被代理。插件使用统一工具窗口承载不同烘焙方式，并由公共 Core 模块负责网格提取、源材质遮罩与属性烘焙、Atlas 处理和资产写入。

当前版本为实验性编辑器工具，不包含运行时模块。

## 功能状态

| 功能 | 状态 | 当前行为 |
| --- | --- | --- |
| Single Billboard | 可用 | 从 `+X`、`-X`、`+Y` 或 `-Y` 方向进行一次正交拍摄，生成一个穿过资产 Pivot 的垂直平面，只烘焙一个面 |
| Cross Cards | 可用 | 生成 2–5 个在 180° 内等角度分布的垂直相交平面，每个角度独立裁切并烘焙正反面 |
| Impostor | 可用 | 使用 3×3–8×8 八面体方向网格采样上半球或完整球面，每帧通过共享 Masked RDG 深度选择可见片元，生成固定 Atlas 和单张自动裁边 Sprite 代理 |
| BillboardClouds | 可用 | 使用 K-Means 生成自适应平面云，可选独立的树干 Cross Cards |

## 输入与拍摄规则

- 只接受 Static Mesh。
- Source LOD 可以选择，不要求固定使用 LOD0。
- Single Billboard 和 Cross Cards 使用所选 Source LOD 的全部三角形进行固定视角正交投影。
- Single Billboard 和 Cross Cards 的平面穿过 Static Mesh 本地原点，也就是资产 Pivot。
- Cross Cards 的每个角度分别计算投影范围、可见关系和 Alpha 裁切。
- Impostor 使用固定 N×N 八面体方向网格。上半球采用半八面体映射，完整球面采用带下半球折叠的完整八面体映射；虚拟八面体只负责方向编码，不作为最终代理几何。插件扫描所选 LOD 的全部唯一顶点及全部采样视角，在每个视角的 U、V 和拍摄深度轴上计算一个共享的紧致半径；在不超过原 Sphere Radius 范围的前提下，尽可能为 Tile 四边各保留 2 px。所有视角共享这个正方形投影范围和深度范围，以匹配 UE Impostor 材质的单一 `Default Mesh Size` 语义。代理初始生成在本地 XY 平面，正面朝向 +Z，再由父材质的 WPO 在运行时朝向观察方向。
- BillboardClouds 只保留 K-Means 技术路径；每个平面和正反面分别烘焙，主投影与 Crack Reduction 投影在同一个逐 Tile 深度目标中竞争，胜出的同一片元同时提供 BaseColor、Normal、Source Triangle ID、Depth 和 Mix。
- 已实现的烘焙路径读取未执行顶点变形的 Source LOD。BaseColor、Object Normal、Source Triangle ID，以及 Mix 中的 AO、Roughness、Metallic、Emission 都使用 Core 的编辑器 RDG Masked 材质路径，并由同一个逐 Tile 深度目标确定胜出片元。WPO 和 Displacement 在导出代理中为 0。
- 材质图中与颜色、透明度或其他属性直接相关的时间和风参数不会被统一禁用，只有顶点位移不参与烘焙。

## 纹理输出

纹理分辨率范围为 256–4096，硬上限为 4K。Single Billboard 默认 1024，Cross Cards、Impostor 和 BillboardClouds 默认 2048。

### BaseColor / Auxiliary

- RGB 保存 Base Color。
- Single Billboard、Cross Cards、Impostor 和 BillboardClouds 的 A 通道都保存由最终可见覆盖生成的整株 Union SDF：外部为 `0`，轮廓为 `0.5`，内部为 `1`。
- `Opacity SDF Range` 控制从轮廓到完全内部或外部的像素距离，默认 16 px。SDF 不增加 Padding，也不扩大平面、UV 或 Atlas Tile。
- BillboardClouds 的树干与叶片分类继续保存在代理网格 UV2 中。

四种功能都可以直接使用 BaseColor A 计算覆盖：

```hlsl
float Coverage = smoothstep(AlphaThreshold - EdgeWidth, AlphaThreshold + EdgeWidth, UnionSDF);
```

### Normal / Auxiliary

- RGB 保存 object/local-space Normal。
- Single Billboard 和 Cross Cards 的 A 通道保存可见源材质分类：背景为 `0`、树干为 `0.5`、树叶为 `1`。
- 树干分类使用材质实例名称或父材质名称关键字，默认关键字为 `Trunk`。
- Impostor 的 A 通道保存与 UE ImpostorBaker 一致的共享范围线性深度：最近点为 `1`、最远点为 `0`、未覆盖像素为 `0.5`。
- BillboardClouds 的 A 通道保存所有视角共享范围的线性深度：全局最近点为 `1`、全局最远点为 `0`、未覆盖像素为 `1`。

Single Billboard 和 Cross Cards 的分类 Mask：

```hlsl
float LeafMask = Coverage * step(0.75, NormalMaskA);
float TrunkMask = Coverage * step(0.25, NormalMaskA) * (1.0 - step(0.75, NormalMaskA));
```

### Mix

Mix 输出为可选项，RGBA 分别保存：

1. Occlusion
2. Roughness
3. Metallic
4. Emission

四个通道分别通过线性 Masked 属性 Pass 计算，并与 BaseColor、Normal 和 Source Triangle ID 复用同一逐 Tile 深度结果；最终由 Core 打包为一张 RGBA 纹理。

### Atlas 优化

- Single Billboard 和 Cross Cards 始终按每个视角的原始有效覆盖范围裁切，`Per-View Alpha Crop Guard` 单独控制额外保留边界。`Opacity SDF Range` 不增加 Padding，也不扩大平面、UV 或 Atlas Tile。
- Single Billboard 默认开启 `Trim Unused Atlas Space`；Cross Cards 可按需开启。启用后允许输出块对齐的矩形纹理。
- Impostor 使用固定的 N×N 正方形 Tile 网格。所有视角使用同一个按几何顶点自动计算的紧致正方形拍摄范围；它不是逐帧 Alpha 裁切，透明叶片 Card 的空白角仍会参与范围计算。方向帧、Depth、代理网格和运行时重建始终共用同一个尺寸。
- BillboardClouds 可以在最终打包前按每个平面的 Alpha 外边界裁切；SDF 只在最终 Tile 内生成，不参与裁切，也不改变 Tile 尺寸。
- Atlas Tile 内未覆盖区域使用最近的有效像素向外填充 RGB，不依赖固定 Padding Pixel 参数，也不会改变代理 UV 的有效占用范围。

## 材质模板

插件不提供硬编码的材质模板默认值。四种功能的 `Parent Material Instance` 必须分别在 `Editor Preferences > Plugins` 的 Foliage Baker 设置中配置。

插件不创建或修改材质图。烘焙完成后会新建一个 Material Instance Constant，以 Editor Preferences 中选择的 MI 作为 Parent，并只在生成的子实例上写入烘焙纹理和必要的运行时参数。重新烘焙会复用生成资产，但会先清除旧的本地纹理、数值、Static Switch 和 Base Property Override，避免残留模板副本数据。

默认纹理参数名称：

- `ColorOpacity`
- `NormalMask`
- `PackedMasks_1`

这些名称都可以在 Material 分类中修改。

Impostor 默认附加运行时参数：

- `FramesXY`：正方形 Atlas 共用的行列数标量。UE 原函数链将它同时用于 X/Y 帧寻址，因此当前只支持 N×N，不支持不等行列。
- `Default Mesh Size`：自动计算的共享紧致半径的两倍，也是所有视角共用的正方形拍摄边长。
- `Pivot Offset`：所选 LOD 本地 Bounds Center 相对于源资产 Pivot 的偏移。
- `UpperHemisphereOnlyImpostor`：上半球模式启用，完整球面模式关闭。

`NormalMask.A` 默认作为深度输入，因为迁移后的父材质保持 `UseDepthTexture` 关闭。启用 Mix 输出时，`PackedMasks_1` 的 R/G/B 分别通过父材质静态开关连接到 Ambient Occlusion、Roughness 和 Metallic；A 仍保存 Emission，但 UE 原 `Impostor_ChannelPackMask_Switches` 不负责把该通道路由到 Emissive。

Impostor 材质保持 `ColorOpacity` 为 Color 采样，`NormalMask` 和 `PackedMasks_1` 为 Linear Color 采样。`FrameBlendWeights_TextureObject` 使用 `Linear Sampling` Static Bool 在编译期选择三帧 Color 或 Linear Color 采样路径：Base Color 调用关闭，Normal/Depth 与 Packed Masks 调用开启，不产生运行时分支。`NormalMask` 纹理保持 `sRGB=false`，不会在烘焙端进行预编码；未覆盖参数分别使用线性的 `T_Default_NormalMask` 和 `T_Default_BlackLinear` 作为默认纹理。

运行时材质先把当前观察方向编码到八面体网格，确定虚拟网格中的三角形并取得三个相邻帧及重心权重。三个帧分别建立自己的投影平面和 UV，不共用投影结果；Normal A 先用于一次深度反投影，再以修正 UV 混合 BaseColor、SDF、Normal 和 Depth。最终代理是一张锁定当前观察方向的 Sprite，ISM/HISM 使用 Instance & Particle Space 计算后再转换为 World Space。

Normal A 的深度沿相机到模型的拍摄射线映射，使用 `lerp(-SharedCaptureHalfExtent, SharedCaptureHalfExtent, Depth)` 解码；`Default Mesh Size` 等于 `SharedCaptureHalfExtent × 2`。

## 网格输出

每个可用功能在单个 Mesh 的几何和贴图烘焙成功后弹出统一的 Mesh Output 对话框，确认后才提交本次资产事务。关闭或取消对话框会回滚该 Mesh 本次尚未提交的贴图、材质和网格修改。可选方式为：

1. 创建独立 Static Mesh 资产。
2. 追加到源 Static Mesh 的 LOD。
3. 替换源 Static Mesh 的指定 LOD。
4. 替换源 Static Mesh 的最后一个 LOD；当最后一个 LOD 是 LOD0，或最后一个 LOD 正是本次 Source LOD 时，该选项不可用。

批量烘焙会按 Mesh 逐个弹出确认，保证不同 LOD 数量的源模型分别解析自己的 Last LOD。Mesh 面板不再保存持久化的输出模式或替换索引。

在追加模式下，四个已实现功能使用独立 Metadata Key 记录各自上次生成的目标 LOD：

- `FoliageBaker.SingleBillboardLOD`
- `FoliageBaker.CrossCardsLOD`
- `FoliageBaker.ImpostorLOD`
- `FoliageBaker.BillboardCloudsLOD`

重新烘焙同一功能时会更新记录的 LOD；只有记录不存在或对应 LOD 已失效时才追加新的 LOD。

替换模式下，目标 LOD 不能与 Source LOD 相同，以便保留重新烘焙所需的输入几何。

生成的代理网格：

- 不生成碰撞。
- 不生成 Lightmap UV。
- 使用完整编辑器网格构建流程生成距离场派生数据。
- 保留源 Static Mesh 的本地坐标系和 Pivot 语义。
- Impostor 按 UE ImpostorBaker 的 MeshCutout 契约，将每个视角的覆盖面积下采样到 16×16，再跨视角加法合并并以 `0.25` 阈值追踪四个象限，固定生成 1 个中心点、8 个轮廓点和 8 个三角形。代理位置缩放为共享紧致半径的 1/10；UV0 使用相同的 0.001/0.995 inset，并除以 `FramesXY × 10`，由运行时材质恢复完整尺寸。几何保留源资产 Pivot，独立代理复用所选 LOD 的局部 Bounds，保持与追加到源模型 LOD 时相同的 WPO 剔除范围。

## 资产位置与重新烘焙

- 独立代理网格生成在源 Static Mesh 所在目录。
- Texture 和 Material 文件夹相对于源 Static Mesh 目录的父目录计算。
- 默认 Texture 文件夹为 `Textures`，Material 文件夹为 `Materials`。
- 纹理、材质实例和代理网格名称的前缀与后缀都可以配置。
- 创建独立资产时，四个已实现功能都会复用并更新同路径、同名称、同类型的现有 Texture2D、Material Instance Constant 和 Static Mesh；不会创建带编号的副本。同名对象类型不匹配时停止烘焙并报错。
- 批量烘焙按每个 Static Mesh 独立执行。
- 每个 Static Mesh 的资产写入使用事务式快照：失败时恢复本次修改前的已有资产，并删除本次创建但未提交的资产。

## 使用方法

1. 在 Unreal Editor 主菜单中打开 `Tools > Foliage Baker`。
2. 选择 `Single Billboard`、`Cross Cards`、`Impostor` 或 `BillboardClouds` 标签。
3. 在 Content Browser 中选择一个或多个 Static Mesh，然后点击 `Add Content Browser Selection`，也可以直接在 Mesh 列表中指定资产。
4. 设置 Source LOD 和功能参数。
5. 在 Editor Preferences 中为当前功能配置 Parent Material Instance，并在工具中确认纹理参数名称。
6. 点击当前功能的 Bake 按钮；烘焙完成后在弹窗中选择网格输出方式。

## 模块结构

- `FoliageBakerCore`：公共网格、Atlas、源材质解析与属性烘焙、原子资产写入服务。
- `FoliageBakerEditorCommon`：四种功能共用的编辑器工具壳层，包括 Details 面板、Mesh 队列、撤销事务、CanBake 前置条件、批处理进度、结果汇总与 Content Browser 同步。
- `FoliageBakerCards`：Single Billboard 与 Cross Cards 的设置、请求组装和功能专属烘焙逻辑。
- `FoliageBakerImpostor`：半球/全球八面体方向采样、固定网格 Atlas、单 Sprite 代理与资产组装。
- `FoliageBakerBillboardClouds`：K-Means BillboardClouds 功能。
- `FoliageBakerEditor`：数据驱动的统一工具窗口、四种功能标签、菜单和 Editor Preferences 注册。

## 当前限制

- 不接受 Skeletal Mesh 或其他非 Static Mesh 输入。
- 不生成材质图。
- 不生成碰撞和 Lightmap UV。
- 当前没有自动视觉误差验收样本或阈值。

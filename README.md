# FoliageBaker

FoliageBaker 是一个实验性的 Unreal Editor 插件，用于把 Static Mesh 的指定 LOD 烘焙成远景植被代理。它提供 Billboard、Cross Cards、MultiBillboard、BillboardClouds 和 Impostor 五类代理，并共享同一套固定帧 WPO、Masked 材质捕获、Atlas、Mip、资产更新和 LOD 写入流程。

插件只包含 Editor 模块。运行时的朝向、视角选择、UV 选择、法线还原和深度解码由父材质负责；工具不会自动改写父材质图。

## 快速开始

1. 在 `Editor Preferences > Plugins` 中打开对应的 Foliage Baker 设置，配置默认 Parent Material Instance。
2. 打开 `Tools > Foliage Baker`。
3. 选择代理模式，从 Content Browser 添加一个或多个 Static Mesh。
4. 设置 `Source LOD Index`、纹理分辨率策略和输出通道。
5. 如有需要，在当前工具面板覆盖 Editor Preferences 提供的默认材质模板。
6. 点击 Bake。
7. 为每个输入选择独立代理 Mesh、追加 LOD、插入 LOD或替换 LOD。

Editor Preferences 只提供新工具会话的默认值。当前面板保存会话状态并可以覆盖默认值；关闭并重新打开工具后，会话设置重新创建，Source Static Mesh 队列被清空。

## 代理模式

| 模式 | 用途与输出 |
| --- | --- |
| Billboard | 固定水平视角的轻量代理，支持单视角、单片双视角和双片双视角 |
| Cross Cards | 在 180° 内均匀生成 2–5 个垂直交叉平面，并分别烘焙正反面 |
| MultiBillboard | 按材质关键词提取叶片，恢复连通组件并聚类，为每个簇生成多层平行 Billboard |
| BillboardClouds | 通过 K-Means 生成自适应平面云，可把树干与枝干分流到固定垂直 Cross Cards |
| Impostor | 使用 3×3–8×8 八面体视角网格烘焙上半球或完整球面，并生成保守裁边代理 |

### Billboard Mode

| Billboard Mode | 拍摄 | 输出几何 | 运行时数据 |
| --- | --- | --- | --- |
| `Single Plane - One View` | 所选水平轴的一个视角 | 一张 Quad | 主要使用 UV0 |
| `Single Plane - Two Views` | 主视角与绕本地 `+Z` 旋转 90° 的第二视角 | 一张 Quad | UV0 = 主视角，UV1 = 第二视角，UV2.xy = 主拍摄方向 |
| `Double Planes - Two Views` | 与单片双视角相同 | 两张重叠平行 Quad | UV0 = 各平面的 Tile，UV1.xy = 局部拍摄方向，UV2.x = 平面编号 `0/1` |

两个 Two Views 模式始终使用固定的上下 Atlas 布局：

```text
┌──────────────────┐
│ 主视角 Tile      │  PlaneIndex 0
├──────────────────┤
│ +90° 视角 Tile   │  PlaneIndex 1
└──────────────────┘
```

初次打包以及 Alpha Crop 后的重新打包都使用该布局。Auto Texel Size 按 `max(TileWidth, TileHeight × 2)` 计算双视角 Atlas 预算。

`Single Plane - Two Views` 只拍摄这两个正方向，不额外拍摄它们的负方向。负方向如何复用或镜像现有 Tile 由专用父材质决定。

两个视角共享同一套代理范围与保守 Alpha Crop。各视角法线在写入 Atlas 前转换到自身的 `Facing/Right/Up` 拍摄基，使专用父材质可以使用统一的 Billboard 法线解码。

### Cross Cards

- 所有平面穿过源 Static Mesh 的局部原点。
- `Two-Sided (UV0 / UV1)`：每个方向生成一张 Quad，UV0 为正面，UV1 为背面。
- `Separate One-Sided Faces`：正反面分别生成绕序相反的 Quad，各自使用 UV0。

### MultiBillboard

- `Leaf Material Keywords` 匹配材质实例或父材质名称，默认关键词为 `Leaf`。
- 叶片先按共享顶点恢复为连通组件，再在 Source LOD 局部空间聚类。
- 每个簇沿 Capture Axis 分配到多个深度层；空层不会生成平面。
- UV0 保存 Atlas Tile。
- UV1.xy 保存相对簇中心的平面内偏移。
- UV2.x 保存沿拍摄法线的有符号层间距。
- `Include Reduced Trunk` 默认开启。
- `Trunk Triangle Percentage` 默认 `0.5`。

保留的树干与枝干通过 Unreal Engine Static Mesh Reducer 减面，并保留源材质及源网格中全部有效 UV 通道。

### BillboardClouds

- 使用 K-Means 生成自适应代理平面，目标平面数默认 `64`。
- `Scaled Envelope-Clipped Projection` 默认开启。
- Trunk Cards 默认开启，生成 4 个垂直平面，Atlas 权重默认 `1.5x`。
- 正面 Tile 写入 UV0；启用双面烘焙时，背面 Tile 写入 UV1。
- 父材质使用 `TwoSidedSign` 选择正反面。

### Impostor

- `Frame Grid Size` 支持 3–8，默认 `4`。
- 支持上半球和完整球面视角分布。
- 使用固定帧 WPO 几何的 Sphere Radius 计算正方形捕获直径。
- 捕获范围按 Tile 分辨率增加 2 px Guard。
- Auto Texel Size 使用相同的带 Guard 直径，避免 WPO 超出原始 Bounds 后发生裁切。

## 公共烘焙规则

### Source LOD

- 输入只接受 Static Mesh。
- 几何、Bounds、材质槽、材质分类和材质捕获均来自所选 Source LOD，不强制使用 LOD0。
- 队列中的每个 Mesh 都必须包含所选 LOD。
- 工具读取 Static Mesh 资产，不读取关卡实例，因此不会采样真实实例上的 `PerInstanceCustomData`。

### 固定帧 World Position Offset

WPO 是默认的底层捕获行为，不需要单独开启：

- 使用源材质 Shader 在 GPU 上计算。
- `GameTime = 0`，`RealTime = 0`。
- WPO 后的顶点同时用于 Bounds、代理生成和正式材质捕获。
- 正式捕获不会再次叠加同一份位移。
- 任一顶点分量为 `NaN`、`+Inf` 或 `-Inf` 时，包含该顶点的整个三角形被排除。
- 如果所有三角形都被排除，当前 Mesh 的 Bake 失败。

### Masked 材质捕获

共享 GPU 路径从同一个深度胜出片元获取：

- Base Color
- Opacity Mask
- Normal
- Ambient Occlusion
- Roughness
- Metallic
- Emission
- Source Triangle ID

Opacity Mask、Opacity Mask Clip Value、Early Opacity Mask、自定义双面朝向和 WPO 会参与捕获。Pixel Depth Offset 与材质 Displacement 不参与捕获；输出深度来自 WPO 后的真实几何位置。

### 烘焙时覆盖 Static Switch

启用 `Override Static Switches During Bake` 后，工具为所选 LOD 实际使用的每个源材质创建临时子 Material Instance，并让固定帧 WPO 与 Atlas 捕获使用同一组 Global Static Switch 覆盖值。

- 源材质资产不会被修改。
- 只覆盖材质中实际存在的 Global Static Switch。
- 缺失的 Switch 产生警告，其余覆盖继续执行。

## 分辨率与 Atlas

默认分辨率策略为 `Auto - Texels Per Meter`：

| 设置 | 默认值 |
| --- | --- |
| `Target Texels Per Meter` | `20 texels/m` |
| `Minimum Atlas Resolution` | `64` |
| `Maximum Atlas Resolution` | `4096` |

`20 texels/m` 表示源网格局部空间中的 1 米约使用 20 个 Texel，与 `5 cm/texel` 等价。数值越大，目标细节越高。

Auto 会在最小与最大分辨率之间选择满足目标密度的最小 2 次幂预算：

- Billboard、Cross Cards 与 MultiBillboard 先按目标密度测量各平面，再打包所有 Tile。
- 两个 Two Views Billboard 固定为上下布局，因此高度预算始终包含两张 Tile。
- BillboardClouds 对每个代理平面执行相同测量；平面数量和 Trunk Card 权重共同占用预算。
- Impostor 按单个视角 Tile 的世界空间密度选择总预算，再按 `Frame Grid Size` 对齐到固定网格。
- 达到最大分辨率后不再继续放大，因此目标密度是期望值而不是无限保证。

`Manual` 模式保留直接指定 Atlas 分辨率的工作流。

### Alpha Crop、Trim 与 Padding

Cards 模式按可见 Alpha 范围测量每个视角，`Alpha Crop Guard` 控制保守边界。需要共享几何范围的正反面或成组视角会合并裁切范围。

`Trim Unused Atlas Space` 只控制最终 Atlas 外圈：

- 开启：紧密移除未使用的外部行列，允许输出块对齐的非 2 次幂矩形。
- 关闭：根据已使用 Tile 的整体范围分别向上取 2 次幂并居中，可输出 `512×1024` 一类矩形。

两种模式都会对 UV Island 的 RGB 执行外扩 Padding。关闭 Trim 时，Padding 会填充 Tile 外的剩余 Atlas 像素；Mip 按 Tile 独立处理，避免黑边和相邻视角串色。

默认行为：

- Billboard：Trim 开启。
- Cross Cards：Trim 关闭。
- MultiBillboard：Trim 开启。
- BillboardClouds：Alpha Crop 开启。
- Impostor：保持固定 N×N 视角网格，不执行逐帧 Trim。

### 语义 Mip

- Cards 与 BillboardClouds 的分类 Alpha 保持背景 `0`、树干 `0.5`、树叶 `1`，不会直接平均成无意义灰度。
- Cards 与 BillboardClouds 按 Tile 独立生成 Mip。
- Impostor Normal 先平均再重新进行八面体编码。
- Impostor Mask 重新分类，Depth 保持连续过滤。

## 输出纹理与 UV 契约

### Billboard、Cross Cards、MultiBillboard

| 纹理 | 通道 |
| --- | --- |
| `ColorOpacity` | RGB = Base Color；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| `NormalMask` | RGB = Object/Local Space Normal；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| `Mix`（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |
| `UpperHemisphereL1Visibility`（Billboard 可选） | RGB = 重映射后的 L1 方向系数；A = 常数项 |

Two Views Billboard 的 `NormalMask.RGB` 使用每视角的 capture-frame normal，而不是普通 Object/Local Space Normal。

### BillboardClouds

| 纹理 | 通道 |
| --- | --- |
| `ColorOpacity` | RGB = Base Color；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| `NormalMask` | RGB = Object/Local Space Normal；A = 全部平面共享范围的线性深度 |
| `Mix`（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |

### Impostor

| 纹理 | 通道 |
| --- | --- |
| `ColorOpacity` | RGB = Base Color；A = 整株可见覆盖生成的 SDF |
| `NormalMask` | RG = 八面体编码 Object/Local Space Normal；B = 树干 `0.5` / 树叶 `1`；A = 共享范围线性深度 |
| `PackedMasks_1`（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |

Impostor 父材质还需要读取：

- `FramesXY`
- `Default Mesh Size`
- `Pivot Offset`
- `UpperHemisphereOnlyImpostor`

关闭 Mix 输出时，工具从最终可见像素分别计算叶片与树干的 Roughness、Specular 平均值，并写入对应 Scalar 参数。没有有效样本的分类不会写入参数。

## L1 Visibility

Billboard 可以额外烘焙上半球 L1 自遮挡可见性。默认参数为：

- 12 个上半球方向样本。
- 512 最大系数纹理边长。
- 1024 内部 Masked Shadow Map 最大边长。
- 每个接收点使用固定 5×5 PCF。

基础解码：

```hlsl
float3 Cxyz = VisibilityTexture.rgb * 2.0 - 1.0;
float C0 = VisibilityTexture.a;
float Visibility = saturate(C0 + dot(Cxyz, LightDirectionBaked));
```

`LightDirectionBaked` 是从接收点指向光源、并逆变换回烘焙局部基的单位方向。工具只生成并设置 `UpperHemisphereL1Visibility` 纹理参数，不会修改父材质图。

## 材质模板

每种模式都需要 Parent Material Instance：

- Editor Preferences 保存默认模板。
- 当前工具面板可以覆盖默认模板。
- `Single Plane - Two Views` 使用独立模板。
- `Double Planes - Two Views` 使用独立模板。
- 生成的 Material Instance Constant 以当前模板为 Parent。

插件内容中包含以下常用模板：

| 用途 | 资产 |
| --- | --- |
| Single Plane - One View | `/FoliageBaker/Materials/MR_Foliage_SingleBillboard` |
| Single Plane - Two Views | `/FoliageBaker/Materials/MR_Foliage_SingleBillboard_TwoView` |
| Double Planes - Two Views | `/FoliageBaker/Materials/MR_Foliage_DoubleBillboard` |
| Cross Cards | `/FoliageBaker/Materials/MR_Foliage_Cross` |
| BillboardClouds | `/FoliageBaker/Materials/MR_Foliage_BillboardClouds` |
| Impostor | `/FoliageBaker/Materials/MR_Foliage_Impostor` |

默认纹理参数名：

| 模式 | Base Color | Normal | Mix |
| --- | --- | --- | --- |
| Billboard / Cross Cards / MultiBillboard | `ColorOpacity` | `NormalMask` | `Mix` |
| BillboardClouds | `ColorOpacity` | `NormalMask` | `Mix` |
| Impostor | `ColorOpacity` | `NormalMask` | `PackedMasks_1` |

## 资产更新与输出位置

当输出路径已存在时，工具要求选择：

- `Update Existing`：原地更新整组 Texture、Material Instance 和独立代理 Mesh。
- `Create New`：为整组输出使用同一个可用版本号。
- `Cancel`：取消当前资产写入。

更新已有 Material Instance 时，工具只替换当前及历史记录中的 FoliageBaker 自有参数，并清理失效的旧工具参数。人工添加或修改的非工具参数继续保留，Parent 更新为当前模板。

同一资产组使用一个写入事务。任一步失败或取消时，已有资产恢复到写入前状态，本次创建的资产被回滚。

默认资产位置规则：

- 独立代理 Static Mesh 与源 Static Mesh 放在同一目录。
- Texture 与 Material 目录相对于源 Mesh 目录的父目录计算，例如 `/Game/Trees/Meshes` 对应 `/Game/Trees/Textures` 与 `/Game/Trees/Materials`。
- `Place Assets Near Replaced LOD Assets` 默认开启。
- Replace LOD 时，材质优先放到目标 LOD 使用的材质附近，纹理优先放到这些材质引用的最近纹理目录。
- 无法解析现有位置时回退到配置目录。
- 源 Mesh 名称以 `SM_` 开头时，生成 Texture 与 Material 名称会移除该前缀。
- 代理材质槽名称与实际 Material 资产名称保持一致。

目录、前缀、后缀和材质参数名均可在对应模式中配置。

## Mesh 与 LOD 写入

每个 Mesh 完成捕获后会弹出统一输出对话框：

1. `Create Separate Mesh Asset`
2. `Add To Source Mesh LODs`
3. `Insert After LOD`
4. `Replace LOD`
5. `Replace Last LOD`

写入规则：

- 插入位置不能早于 Source LOD。
- 替换目标必须位于 Source LOD 之后。
- Add 优先更新该功能之前记录的生成 LOD；没有记录时追加。
- Insert 会同步移动后续 LOD、Section、Base LOD、Min LOD、Collision LOD 与 FoliageBaker 元数据索引。
- Replace 会清理旧目标 LOD 的功能元数据。
- Replace 会移除只被旧目标 LOD 使用、且不再被其他 LOD 引用的材质槽。
- 非 LOD0 代理不会改写 Source Mesh 的整体 Bounds。
- 代理不生成碰撞和 Lightmap UV。

## 模块边界

| 模块 | 职责 |
| --- | --- |
| `FoliageBakerCore` | Source Mesh、固定帧 WPO、Masked 捕获、Atlas、Mip、资产事务与 LOD 写入 |
| `FoliageBakerEditorCommon` | 通用工具面板、批处理、LOD 输出与资产冲突对话框 |
| `FoliageBakerCards` | Billboard、Cross Cards、MultiBillboard |
| `FoliageBakerImpostor` | 八面体方向采样、Impostor Atlas 与代理 |
| `FoliageBakerBillboardClouds` | K-Means 平面云与 Trunk Cards |
| `FoliageBakerEditor` | 统一窗口、菜单、模式切换与 Editor Preferences |

## 当前限制

- 仅支持 Static Mesh，不支持 Skeletal Mesh。
- 父材质必须实现对应模式的 UV、WPO、法线和深度解码。
- WPO 只烘焙 `GameTime = 0`、`RealTime = 0` 的固定形态，不保留动画。
- 不读取关卡实例的 `PerInstanceCustomData`。
- Pixel Depth Offset 与材质 Displacement 不参与捕获。
- 不生成碰撞和 Lightmap UV。
- 当前没有自动化视觉误差基准或画面对比验收。

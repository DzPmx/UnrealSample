# FoliageBaker

FoliageBaker 是一个实验性的 Unreal Editor 插件，用于把 Static Mesh 的指定 LOD 烘焙成远景植被代理。插件提供统一工具窗口，并共享同一套源网格读取、Masked 材质捕获、Atlas 处理、LOD 写入和资产事务流程。

插件仅包含 Editor 模块，不提供运行时模块，也不会自动创建或修改父材质图。

## 功能

| 模式 | 输出 |
| --- | --- |
| Billboard | `Single Plane` 从一个水平轴拍摄；`Double Planes` 从相差 90° 的两个水平轴拍摄并生成两张运行时朝向相机的平面 |
| Cross Cards | 生成 2–5 个在 180° 内均匀分布的垂直交叉平面，分别烘焙正反面 |
| Impostor | 使用 3×3–8×8 八面体方向网格拍摄上半球或完整球面，并生成单张保守裁边代理 |
| MultiBillboard | 按材质关键词提取树叶、识别连通组件、执行空间聚类，并为每个簇生成多层平行 Billboard |
| BillboardClouds | 使用 K-Means 生成自适应平面云，可额外把树干分流到固定 Cross Cards |

MultiBillboard 可以保留非树叶几何，并使用 Unreal Engine 的 Static Mesh Reducer 对树干和枝干减面。减面后的几何保留源材质和源 UV 通道；有多少套有效 UV 就保留多少套。

## 快速开始

1. 在 `Editor Preferences > Plugins` 中打开对应的 Foliage Baker 设置，为功能配置默认 Parent Material Instance。
2. 在 Unreal Editor 主菜单中打开 `Tools > Foliage Baker`。
3. 选择 `Billboard`、`Cross Cards`、`Impostor`、`MultiBillboard` 或 `BillboardClouds`。
4. 从 Content Browser 添加一个或多个 Static Mesh。
5. 选择 `Source LOD Index`，调整烘焙参数，并确认当前工具面板中的 Parent Material Instance。
6. 点击 Bake。
7. 烘焙完成后，为每个 Mesh 选择独立代理、追加 LOD、插入 LOD 或替换 LOD。

Editor Preferences 提供初始默认值。每个工具面板持有独立的临时设置，可以为当前工具会话覆盖材质模板，不会强制继续使用 Preferences 中的模板。

## 源网格与材质捕获

- 输入只接受 Static Mesh。
- Source LOD 可以选择，不要求使用 LOD0。
- 几何、投影范围、材质分类和材质捕获都来自所选 Source LOD。
- Base Color、Opacity Mask、Normal、AO、Roughness、Metallic、Emission 和 Source Triangle ID 通过共享的 GPU Masked 材质路径获取；同一个深度胜出片元为各输出提供数据。
- 源材质的 Opacity Mask、Opacity Mask Clip Value、Early Opacity Mask、自定义 Two Sided 朝向和 World Position Offset 会参与捕获。
- WPO 使用源材质 Shader 在 GPU 上求值，`GameTime = 0`、`RealTime = 0`。投影范围使用固定帧 WPO 后的顶点，正式捕获再由同一材质路径对原始顶点应用一次 WPO，避免重复变形，同时防止越界几何被 Atlas 裁掉。
- Pixel Depth Offset 和材质 Displacement 不参与烘焙；捕获深度保持为 WPO 后的几何深度。
- 工具读取 Static Mesh 资产，不读取关卡实例，因此不会采样真实实例上的 PerInstanceCustomData。

### 烘焙时覆盖 Static Switch

每个模式都可以启用 `Override Static Switches During Bake`：

- 为所选 LOD 实际引用的每个源材质创建临时子 Material Instance。
- 只覆盖配置中存在的 Global Static Switch。
- 缺失的参数会产生警告，但不会修改源材质资产。
- 临时覆盖同时用于固定帧 WPO 求值和材质 Atlas 捕获。

## 纹理分辨率

默认分辨率模式是 `Auto - World Texel Size`：

- `Target World Texel Size` 默认 `5 cm/texel`。
- `Minimum Atlas Resolution` 默认 `64`。
- `Maximum Atlas Resolution` 默认 `4096`。
- Auto 会在最小值和最大值之间选择满足目标世界空间 Texel 尺度的最小 2 次幂预算。
- `Manual Atlas Resolution` 保留手工指定最大分辨率的工作方式。

`cm/texel` 是线性尺度。例如 `5 cm/texel` 表示纹理上一个 Texel 对应模型空间约 5 cm；它不是面积单位。Atlas 中包含多个视角或多个平面时，所有 Tile 共享同一总预算，因此最终能达到的实际密度还会受视角数量、Tile 数量和 4096 上限影响。

不同模式的分辨率处理：

- Billboard、Cross Cards 和 MultiBillboard 在 Auto 模式下先按目标密度测量每个平面，再打包 Atlas。
- BillboardClouds 对每个代理平面执行相同的目标密度测量；平面数量过多时会受到最大 Atlas 分辨率限制。
- Impostor 以每个视角 Tile 的世界空间 Texel 尺度选择总 Atlas 预算。实际 Atlas 按 `Frame Grid Size` 对齐，所有视角保持相同的正方形 Tile。

## Atlas 裁切、Padding 与 Mip

Billboard、Cross Cards 和 MultiBillboard 始终按可见 Alpha 范围裁切每个视角，`Alpha Crop Guard` 控制预留边界。

`Trim Unused Atlas Space` 的行为：

- 开启：紧密移除 Atlas 外圈未使用的行列，输出尺寸允许为块对齐的非 2 次幂矩形。
- 关闭：按已使用 UV Tile 的整体范围取尺寸，宽高分别向上取 2 次幂并居中，可得到 `512×1024` 之类的矩形输出。
- 无论是否 Trim，UV Island 的 RGB 都会使用最近的有效像素外扩并填满剩余区域，生成的每级 Mip 也执行独立 Tile Padding，不保留黑色 RGB 空洞。

默认值：

- Single Plane / Double Planes Billboard：Trim 开启。
- Cross Cards：Trim 关闭。
- MultiBillboard：Trim 开启。
- BillboardClouds：Alpha Crop 开启，使用固定的正方形 Atlas 预算。
- Impostor：固定 N×N 正方形 Tile 网格，不执行逐帧 Trim。

Cards 与 BillboardClouds 默认启用语义 Mask Mip：

- BaseColor/Opacity 的 Alpha 始终保持背景 `0`、树干 `0.5` 或树叶 `1`。
- Mip 不会把树干/树叶分类直接平均成无意义的灰度。
- 每个 Tile 独立生成 Mip，避免相邻视角串色。

Impostor 的 Normal/Mask/Depth Mip 使用专用通道语义：八面体法线按源样本重新平均并编码，树干/树叶 Mask 重新分类，Depth 保留连续过滤结果。

## 纹理通道

### Billboard、Cross Cards 与 MultiBillboard

| 纹理 | 通道 |
| --- | --- |
| ColorOpacity | RGB = Base Color；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| NormalMask | RGB = Object/Local Space Normal；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| Mix（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |
| UpperHemisphereL1Visibility（Billboard 可选） | RGB = 重映射后的 L1 方向系数；A = L1 常数项 |

Double Planes 会把两个视角的法线分别转换到自身的 `Facing/Right/Up` 拍摄基，使专用父材质可以使用同一套 Billboard 法线解码。

关闭 Mix 时，工具会从最终可见像素分别计算树叶和树干的 Roughness、Specular 平均值，并写入生成材质的 `LeafRoughness`、`LeafSpecular`、`TrunkRoughness` 和 `TrunkSpecular` 参数。没有有效样本的分类不会写入参数。

### BillboardClouds

| 纹理 | 通道 |
| --- | --- |
| ColorOpacity | RGB = Base Color；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| NormalMask | RGB = Object/Local Space Normal；A = 所有平面共享范围的线性深度 |
| Mix（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |

正面 Tile 写入 UV0，启用双面烘焙时背面 Tile 写入 UV1。父材质通过 `TwoSidedSign` 选择对应视角。

### Impostor

| 纹理 | 通道 |
| --- | --- |
| ColorOpacity | RGB = Base Color；A = 整株可见覆盖生成的 SDF |
| NormalMask | RG = 八面体编码 Object/Local Space Normal；B = 树干 `0.5` / 树叶 `1`；A = 共享范围线性深度 |
| PackedMasks_1（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |

Impostor 所有视角共享一个基于固定帧 WPO 顶点 Sphere Radius 的正方形捕获直径。捕获范围按 Tile 分辨率额外保留 2 px Guard，Auto Texel Size 也使用这个带 Guard 的直径选择分辨率。运行时材质通过以下参数还原视角：

- `FramesXY`
- `Default Mesh Size`
- `Pivot Offset`
- `UpperHemisphereOnlyImpostor`

## Billboard 几何约定

### Single / Double Planes

- Single Plane 从 `+X`、`-X`、`+Y` 或 `-Y` 拍摄一个视角。
- Double Planes 以选择的方向为主轴，并绕本地 `+Z` 旋转 90° 得到第二个拍摄方向。
- Double Planes 的 UV0 保存各自 Atlas Tile，UV1.xy 保存本地拍摄方向，UV2.x 保存平面编号 `0/1`。

### Cross Cards

- 平面穿过源 Static Mesh 的本地原点。
- `Two-Sided (UV0 / UV1)` 每个方向生成一个 Quad：UV0 为正面，UV1 为背面。
- `Separate One-Sided Faces` 为正反面分别生成绕序相反的 Quad，并把各自 Tile 写入 UV0。

### MultiBillboard

- `Leaf Material Keywords` 匹配源材质实例或其父材质名称，默认关键词为 `Leaf`。
- 叶片先按共享顶点恢复为连通组件，再在 Source LOD 本地空间聚类。
- 每个簇沿 Capture Axis 划分为多个深度层，空层不生成平面。
- UV0 保存 Atlas Tile；UV1.xy 保存顶点相对簇中心的平面内偏移；UV2.x 保存沿拍摄法线的有符号层间距。
- `Include Reduced Trunk` 默认开启，`Trunk Triangle Percentage` 默认 `0.5`。

### BillboardClouds

- K-Means 目标平面数默认 `64`。
- `Scaled Envelope-Clipped Projection` 默认开启，用于减少相邻投影造成的裂缝。
- Trunk Cards 默认开启，默认生成 4 个垂直平面。
- `Double Sided Bake Mode` 可以只为树干、只为 Billboard 或为全部平面生成背面 Tile，默认 `All Planes`。

## L1 Visibility

Billboard 可以额外烘焙上半球 L1 自遮挡可见性：

- 默认 12 个上半球方向样本。
- 系数纹理最大边默认 512。
- 内部 Masked Shadow Map 最大边默认 1024。
- 每个接收点使用固定 5×5 PCF 后拟合四个 L1 系数。

最基础的解码形式：

```hlsl
float3 Cxyz = VisibilityTexture.rgb * 2.0 - 1.0;
float C0 = VisibilityTexture.a;
float Visibility = saturate(C0 + dot(Cxyz, LightDirectionBaked));
```

`LightDirectionBaked` 必须是从接收点指向光源、并逆变换回烘焙时本地基的单位方向。插件只生成并设置 `UpperHemisphereL1Visibility` 纹理参数，不修改父材质图。

## 材质模板与重新烘焙

每种模式都要求一个 Parent Material Instance：

- Editor Preferences 保存默认模板。
- 当前工具面板可以覆盖该模板。
- Double Planes 使用独立的 `Double Planes Parent Material Instance`。
- 生成的 Material Instance Constant 以所选模板为 Parent。

默认纹理参数：

| 模式 | Base Color | Normal | Mix |
| --- | --- | --- | --- |
| Billboard / Cross Cards / MultiBillboard | `ColorOpacity` | `NormalMask` | `Mix` |
| BillboardClouds | `ColorOpacity` | `NormalMask` | `Mix` |
| Impostor | `ColorOpacity` | `NormalMask` | `PackedMasks_1` |

这些参数名都可以在工具中修改。

当同路径资产已存在时，工具会列出冲突资产，并要求选择：

- `Update Existing`：原地更新整组 Texture、Material Instance 和独立代理 Mesh。
- `Create New`：为整组输出选择同一个可用版本号，保留现有资产。
- `Cancel`：不写入当前资产。

更新已有 Material Instance 时：

- 工具只替换当前及历史记录中的 FoliageBaker 自有参数。
- 人工添加或修改的非工具参数继续保留。
- 已失效的旧工具参数会被清理。
- Parent 会更新为当前工具选择的模板。

整个资产组使用同一事务。任一步失败或用户取消时，已有资产恢复到写入前状态，本次新建资产会被回滚。

## 资产位置与命名

- 独立代理 Static Mesh 写在源 Static Mesh 所在目录。
- 默认 Texture 和 Material 目录相对于源 Mesh 目录的父目录计算。例如 `/Game/Trees/Meshes` 默认对应 `/Game/Trees/Textures` 和 `/Game/Trees/Materials`。
- `Place Assets Near Replaced LOD Assets` 默认开启。选择 Replace LOD 时，生成材质优先放到被替换 LOD 所用材质附近，纹理优先放到这些材质引用的最近 Texture 目录；无法解析时回退到配置目录。
- 源 Mesh 名称以 `SM_` 开头时，生成 Texture 和 Material 名称会移除该前缀。
- 生成 Mesh 的代理材质槽名称与实际 Material 资产名称一致。

所有目录、前缀、后缀和材质参数名称都可以在对应模式的设置中修改。

## 网格输出与 LOD

每个 Mesh 烘焙完成后会弹出统一输出对话框：

1. `Create Separate Mesh Asset`
2. `Add To Source Mesh LODs`
3. `Insert After LOD`
4. `Replace LOD`
5. `Replace Last LOD`

约束与更新规则：

- 插入位置不能早于 Source LOD；替换目标必须位于 Source LOD 之后。
- Add 模式会优先更新该功能之前记录的生成 LOD，否则追加新 LOD。
- 插入 LOD 时会同步调整后续 LOD、Section、Base LOD、Min LOD、Collision LOD 和 FoliageBaker 功能元数据索引。
- Replace LOD 时会移除只被旧目标 LOD 单独使用、且不再被其他 LOD 引用的材质槽。
- Replace LOD 会清理目标位置原有的 FoliageBaker 功能元数据，再记录当前功能。
- 生成的非 LOD0 代理不会改写 Source Mesh 的整体 Bounds，避免 Static Mesh 预览和场景摆放出现浮空。
- 代理不生成碰撞和 Lightmap UV。

## 模块

| 模块 | 职责 |
| --- | --- |
| `FoliageBakerCore` | Source Mesh、固定帧 WPO、Masked 材质捕获、Atlas、Mip、资产与 LOD 写入 |
| `FoliageBakerEditorCommon` | 通用工具面板、批处理、输出选择和资产冲突对话框 |
| `FoliageBakerCards` | Billboard、Cross Cards、MultiBillboard |
| `FoliageBakerImpostor` | 八面体方向采样、Impostor Atlas 和代理 |
| `FoliageBakerBillboardClouds` | K-Means 平面云与 Trunk Cards |
| `FoliageBakerEditor` | 统一窗口、菜单、模式切换和 Editor Preferences 注册 |

## 当前限制

- 仅支持 Static Mesh，不支持 Skeletal Mesh。
- 不生成或修改父材质图；父材质必须自行实现对应模式的 UV、WPO、法线和深度解码。
- WPO 只烘焙 `GameTime = 0`、`RealTime = 0` 的固定形态，不保留动画。
- 不读取关卡实例的 PerInstanceCustomData。
- Pixel Depth Offset 和材质 Displacement 不参与捕获。
- 不生成碰撞和 Lightmap UV。
- 当前没有自动化的视觉误差基准或画面对比验收。

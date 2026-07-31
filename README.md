# FoliageBaker

FoliageBaker 是一个实验性的 Unreal Editor 插件，用于把 Static Mesh 的指定 LOD 烘焙成远景植被代理。它把源网格读取、固定帧 WPO、Masked 材质捕获、Atlas、Mip、资产更新和 LOD 写入收敛到一套共享流程，并在同一个工具窗口中提供五种代理模式。

插件只包含 Editor 模块，不提供运行时模块。运行时的朝向、UV 选择、法线还原和深度解码由父材质负责；插件不会自动改写父材质图。

## 支持的代理

| 模式 | 几何与采样 |
| --- | --- |
| Billboard | `Single Plane` 拍摄一个水平视角；`Double Planes` 拍摄相差 90° 的两个水平视角，并输出两张运行时朝向相机的平面 |
| Cross Cards | 在 180° 内均匀生成 2–5 个垂直交叉平面，每个方向分别拍摄正面和背面 |
| Impostor | 使用 3×3–8×8 八面体方向网格拍摄上半球或完整球面，并生成单张保守裁边代理 |
| MultiBillboard | 按材质关键词提取叶片，恢复连通组件并进行空间聚类，为每个簇生成多层平行 Billboard |
| BillboardClouds | 使用 K-Means 生成自适应平面云，可把树干和枝干分流到固定垂直 Cross Cards |

MultiBillboard 可以保留非叶片几何，并通过 Unreal Engine Static Mesh Reducer 对树干和枝干减面。减面部分保留源材质以及源网格现有的全部有效 UV 通道。

## 使用

1. 在 `Editor Preferences > Plugins` 中打开对应的 Foliage Baker 设置，配置该模式的默认 Parent Material Instance。
2. 从主菜单打开 `Tools > Foliage Baker`。
3. 选择一个代理模式。
4. 从 Content Browser 添加一个或多个 Static Mesh。
5. 选择 `Source LOD Index`，检查分辨率、输出通道和当前面板中的材质模板。
6. 点击 Bake。
7. 为每个输入 Mesh 选择独立代理、追加 LOD、插入 LOD 或替换 LOD。

Editor Preferences 是新工具会话的默认值；当前工具面板可以覆盖材质模板和其他设置。关闭并重新打开工具后会重新创建会话设置，并清空待处理的 Source Static Mesh 列表。

## 烘焙模型

### Source LOD

- 输入只接受 Static Mesh。
- 几何、Bounds、材质槽、材质分类和材质捕获都来自所选 Source LOD，不强制使用 LOD0。
- 每个待处理 Mesh 都必须包含所选 LOD。
- 工具读取 Static Mesh 资产，不读取关卡实例，因此不会采样真实实例上的 PerInstanceCustomData。

### Masked 材质捕获

共享 GPU 路径从同一个深度胜出片元获取以下数据：

- Base Color
- Opacity Mask
- Normal
- Ambient Occlusion
- Roughness
- Metallic
- Emission
- Source Triangle ID

源材质的 Opacity Mask、Opacity Mask Clip Value、Early Opacity Mask、自定义双面朝向和 WPO 会参与捕获。Pixel Depth Offset 与材质 Displacement 不参与捕获，输出深度来自 WPO 后的真实几何位置。

### World Position Offset

WPO 是默认的底层捕获行为，不需要单独开启：

- 使用源材质 Shader 在 GPU 上计算。
- `GameTime = 0`，`RealTime = 0`。
- WPO 后顶点用于计算拍摄范围，正式捕获不会再次叠加同一份位移。
- 任一顶点分量为 `NaN`、`+Inf` 或 `-Inf` 时，包含该顶点的整个三角形会从 Bounds、代理生成和材质捕获中排除。
- 如果所有三角形都被排除，当前 Mesh 的 Bake 失败。

Impostor 使用固定帧 WPO 几何的 Sphere Radius 计算正方形捕获直径，并按 Tile 分辨率加入 2 px Guard。Auto Texel Size 使用相同的带 Guard 拍摄直径，避免 WPO 越出原始 Bounds 时发生 Atlas 裁切。

### 烘焙时覆盖 Static Switch

启用 `Override Static Switches During Bake` 后，工具会为所选 LOD 实际使用的每个源材质创建临时子 Material Instance，并在固定帧 WPO 与 Atlas 捕获中使用同一组 Global Static Switch 覆盖值。

- 源材质资产不会被修改。
- 只覆盖材质中实际存在的 Global Static Switch。
- 缺失的 Switch 会产生警告，其余覆盖继续执行。

## 分辨率与 Texel Density

项目默认使用 `Auto - Texels Per Meter`：

| 设置 | 项目默认值 |
| --- | --- |
| `Target Texels Per Meter` | `20 texels/m` |
| `Minimum Atlas Resolution` | `64` |
| `Maximum Atlas Resolution` | `4096` |

`texels/m` 是线性密度：`20 texels/m` 表示源网格局部空间中的 1 米约使用 20 个 Texel，与原来的 `5 cm/texel` 完全等价。数值越大，目标细节越高。Auto 会在最小值与最大值之间选择满足目标密度的最小 2 次幂预算；到达最大分辨率后不再继续放大。

各模式对预算的使用方式不同：

- Billboard、Cross Cards 与 MultiBillboard 先按目标密度测量每个平面，再把所有 Tile 打包到同一 Atlas。
- BillboardClouds 对每个代理平面执行相同测量；平面数量和 Trunk Card 权重会共同占用 Atlas 预算。
- Impostor 按单个视角 Tile 的世界空间密度选择总预算，再按 `Frame Grid Size` 对齐为固定的正方形 Tile 网格。

因此，目标密度是期望值而不是无限保证。视角或平面过多时，最终密度会受 4096 上限约束。`Manual` 模式保留直接指定最大 Atlas 分辨率的工作流。

## Atlas、Padding 与 Mip

### Cards

Billboard、Cross Cards 与 MultiBillboard 始终按可见 Alpha 范围测量每个视角，`Alpha Crop Guard` 控制预留边界。需要共享几何范围的正反面或成组视角会合并为保守裁切范围。

`Trim Unused Atlas Space` 只控制最终 Atlas 外圈：

- 开启：紧密移除未使用的外部行列，结果允许是块对齐的非 2 次幂矩形。
- 关闭：按已使用 Tile 的整体范围计算宽高，分别向上取 2 次幂并居中，结果可以是 `512×1024` 这类矩形。

两种模式都会对 UV Island 的 RGB 执行外扩 Padding。关闭 Trim 时，Padding 会填满 Tile 之外的剩余 Atlas 像素；各级 Mip 也按 Tile 独立处理，避免黑边和相邻视角串色。

项目行为为：

- Billboard：默认 Trim 开启。
- Cross Cards：默认 Trim 关闭。
- MultiBillboard：默认 Trim 开启。

### BillboardClouds

`Enable Alpha Crop` 控制是否按每个代理平面的可见 Alpha 外边界缩小 Tile，`Alpha Crop Guard` 控制保留像素。项目默认开启 Alpha Crop，最终使用正方形 Atlas 预算。

### Impostor

Impostor 保持固定 N×N 正方形视角网格，不执行逐帧 Trim。`Opacity SDF Range` 控制 Base Color Alpha 中 SDF 从轮廓到完全内外的像素距离，它不会额外扩展视角 Tile。

### 语义 Mip

Cards 与 BillboardClouds 默认启用语义 Mask Mip：

- 分类 Alpha 保持背景 `0`、树干 `0.5` 或树叶 `1`，不会直接平均成无意义灰度。
- 每个 Tile 独立生成 Mip。

Impostor 的 Normal/Mask/Depth Mip 按通道语义处理：法线重新平均后再进行八面体编码，Mask 重新分类，Depth 保持连续过滤。

## 输出纹理契约

### Billboard、Cross Cards、MultiBillboard

| 纹理 | 通道 |
| --- | --- |
| `ColorOpacity` | RGB = Base Color；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| `NormalMask` | RGB = Object/Local Space Normal；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| `Mix`（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |
| `UpperHemisphereL1Visibility`（Billboard 可选） | RGB = 重映射后的 L1 方向系数；A = 常数项 |

Double Planes 会把两个视角的法线分别转换到自身的 `Facing/Right/Up` 拍摄基，使两个视角可以使用同一个 Billboard 法线解码。

### BillboardClouds

| 纹理 | 通道 |
| --- | --- |
| `ColorOpacity` | RGB = Base Color；A = 背景 `0`、树干 `0.5`、树叶 `1` |
| `NormalMask` | RGB = Object/Local Space Normal；A = 全部平面共享范围的线性深度 |
| `Mix`（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |

正面 Tile 写入 UV0。启用双面烘焙时，背面 Tile 写入 UV1，父材质使用 `TwoSidedSign` 选择对应视角。`Double Sided Bake Mode` 可以作用于树干平面、Billboard 平面或全部平面；本项目默认只为 Trunk Cards 烘焙背面。

### Impostor

| 纹理 | 通道 |
| --- | --- |
| `ColorOpacity` | RGB = Base Color；A = 整株可见覆盖生成的 SDF |
| `NormalMask` | RG = 八面体编码 Object/Local Space Normal；B = 树干 `0.5` / 树叶 `1`；A = 共享范围线性深度 |
| `PackedMasks_1`（可选） | RGBA = Occlusion、Roughness、Metallic、Emission |

运行时父材质还需要读取：

- `FramesXY`
- `Default Mesh Size`
- `Pivot Offset`
- `UpperHemisphereOnlyImpostor`

关闭 Mix 输出时，工具会从最终可见像素分别计算叶片与树干的 Roughness、Specular 平均值，并写入对应 Scalar 参数。没有有效样本的分类不会写入参数。

## 代理几何约定

### Billboard

- Single Plane 从 `+X`、`-X`、`+Y` 或 `-Y` 拍摄。
- Double Planes 以所选方向为主轴，并绕本地 `+Z` 旋转 90° 得到第二个方向。
- Double Planes 的 UV0 保存 Atlas Tile，UV1.xy 保存局部拍摄方向，UV2.x 保存平面编号 `0/1`。

### Cross Cards

- 所有平面穿过源 Static Mesh 的局部原点。
- `Two-Sided (UV0 / UV1)` 每个方向生成一个 Quad，UV0 为正面，UV1 为背面。
- `Separate One-Sided Faces` 为正反面分别生成绕序相反的 Quad，各自使用 UV0。

### MultiBillboard

- `Leaf Material Keywords` 匹配材质实例或其父材质名称，默认关键词为 `Leaf`。
- 叶片先按共享顶点恢复为连通组件，再在 Source LOD 局部空间聚类。
- 每个簇沿 Capture Axis 分配到多个深度层，空层不会生成平面。
- UV0 保存 Atlas Tile，UV1.xy 保存相对簇中心的平面内偏移，UV2.x 保存沿拍摄法线的有符号层间距。
- `Include Reduced Trunk` 默认开启，`Trunk Triangle Percentage` 默认 `0.5`。

### BillboardClouds

- K-Means 目标平面数项目默认 `64`。
- `Scaled Envelope-Clipped Projection` 项目默认开启。
- Trunk Cards 项目默认开启，生成 4 个垂直平面，Atlas 权重为 `1.5x`。

## L1 Visibility

Billboard 可以额外烘焙上半球 L1 自遮挡可见性。项目默认参数为 12 个方向样本、512 最大系数纹理边长和 1024 内部 Masked Shadow Map 最大边长；每个接收点使用固定 5×5 PCF 后拟合四个 L1 系数。

基础解码形式：

```hlsl
float3 Cxyz = VisibilityTexture.rgb * 2.0 - 1.0;
float C0 = VisibilityTexture.a;
float Visibility = saturate(C0 + dot(Cxyz, LightDirectionBaked));
```

`LightDirectionBaked` 是从接收点指向光源、并逆变换回烘焙局部基的单位方向。插件只生成并设置 `UpperHemisphereL1Visibility` 纹理参数，不会修改父材质图。

## 材质模板与重新烘焙

每种模式都需要 Parent Material Instance：

- Editor Preferences 保存默认模板。
- 当前工具面板可以覆盖默认模板。
- Double Planes 使用独立的 `Double Planes Parent Material Instance`。
- 生成的 Material Instance Constant 以当前模板为 Parent。

默认参数名：

| 模式 | Base Color | Normal | Mix |
| --- | --- | --- | --- |
| Billboard / Cross Cards / MultiBillboard | `ColorOpacity` | `NormalMask` | `Mix` |
| BillboardClouds | `ColorOpacity` | `NormalMask` | `Mix` |
| Impostor | `ColorOpacity` | `NormalMask` | `PackedMasks_1` |

当输出路径已存在时，工具会列出冲突资产，并要求选择：

- `Update Existing`：原地更新整组 Texture、Material Instance 和独立代理 Mesh。
- `Create New`：为整组输出使用同一个可用版本号。
- `Cancel`：取消当前资产写入。

更新已有 Material Instance 时，工具只替换当前及历史记录中的 FoliageBaker 自有参数，并清理已失效的旧工具参数。人工添加或修改的非工具参数继续保留，Parent 更新为当前选择的模板。

同一资产组使用一个写入事务。任一步失败或取消时，已有资产恢复到写入前状态，本次创建的资产被回滚。

## 资产位置与命名

- 独立代理 Static Mesh 与源 Static Mesh 放在同一目录。
- 默认 Texture 与 Material 目录相对于源 Mesh 目录的父目录计算。例如 `/Game/Trees/Meshes` 对应 `/Game/Trees/Textures` 与 `/Game/Trees/Materials`。
- `Place Assets Near Replaced LOD Assets` 默认开启。Replace LOD 时，材质优先放到目标 LOD 使用的材质附近，纹理优先放到这些材质引用的最近纹理目录；无法解析时回退到配置目录。
- 源 Mesh 名称以 `SM_` 开头时，生成 Texture 与 Material 名称会移除该前缀。
- 代理材质槽名称与实际 Material 资产名称保持一致。

目录、前缀、后缀和材质参数名都可以在对应模式中配置。

## Mesh 与 LOD 输出

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
- 插入会同步移动后续 LOD、Section、Base LOD、Min LOD、Collision LOD 与 FoliageBaker 元数据索引。
- Replace 会清理旧目标 LOD 的功能元数据，并移除只被旧目标 LOD 使用、且不再被其他 LOD 引用的材质槽。
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
- 父材质必须自行实现对应模式的 UV、WPO、法线与深度解码。
- WPO 只烘焙 `GameTime = 0`、`RealTime = 0` 的固定形态，不保留动画。
- 不读取关卡实例的 PerInstanceCustomData。
- Pixel Depth Offset 与材质 Displacement 不参与捕获。
- 不生成碰撞和 Lightmap UV。
- 当前没有自动化视觉误差基准或画面对比验收。

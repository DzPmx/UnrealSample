# UnrealSample Billboard Clouds Demo

本仓库当前 Demo 版本包含一个 Unreal Editor 插件：

`Plugins/BillboardClouds`

它用于从选中的 Static Mesh 生成 Billboard Clouds / card proxy，用少量 plane 和一张 atlas 近似原模型。当前主要面向树、灌木、枝叶等植被类资产的远景简化。

## 功能概览

- 支持三种生成技术：
  - Paper 1 - Plane-Space Greedy Cover
  - Paper 2 - K-Means Best-Fit Planes
  - God of War - Greedy Card Capture
- 支持按材质关键字将树干/树枝从 Billboard Clouds 输入中排除，单独生成 2 到 4 张垂直交叉插片。
- 支持固定 atlas 分辨率，自动缩放 tile 并做矩形 packing，提高贴图利用率。
- 支持可选双面拍片：
  - Off
  - Trunk Cards Only
  - Billboard Planes Only
  - All Planes
- 支持 Masked 材质输出，默认 `Opacity Mask Clip Value = 0.333`。
- 支持源材质 alpha/cutout mask 采样，并对 atlas tile 做 RGB padding，减少透明边缘发黑。
- 支持 UV 通道约定：
  - `UV0`：正面 atlas
  - `UV1`：背面 atlas，未开启双面拍片的 plane 会镜像 `UV0`
  - `UV2`：trunk/leaf 分类点，trunk 为 `(0,0)`，billboard/leaf 为 `(1,0)`

## 原理简述

### Paper 1 - Plane-Space Greedy Cover

第一篇论文的方法把候选平面离散到 plane space 中，用方向和距离寻找能够覆盖最多三角形的平面。每轮选择当前最优 plane，移除已覆盖三角形，再继续寻找下一张 billboard。

当前实现保留了以下核心路径：

- 根据误差容忍度判断三角形是否能被某个 plane 近似。
- 在离散 normal/rho 空间中搜索高密度 plane。
- 对选中的 plane 做对象空间投影矩形，生成 proxy quad。
- 用源三角形投影到 quad 的 atlas tile 中生成颜色和 opacity mask。

### Paper 2 - K-Means Best-Fit Planes

第二篇论文的方法更偏向预算驱动：给定目标 plane 数量，用 K-Means 风格的聚类把三角形分配给若干 best-fit plane。

当前实现包含：

- 目标 plane 数量控制。
- 三角形到 plane 的距离度量。
- 聚类分配与 best-fit plane refit。
- 可选 crack reduction：
  - Off
  - Paper Exact - envelope intersection
  - Boundary Aware - 更适合 alpha-card 植被的边界感知补缝

### God of War - Greedy Card Capture

God of War 风格路径是逐张 card 贪心选择：

- 候选 card 由方向和距离定义。
- 每张候选 card 吸附一层厚切片，只有距离 card plane 足够近的三角形可以被压扁到该 card 上。
- card 评分基于可捕获三角形投影面积，越大且形变越小越好。
- 最优 card 认领三角形，重复直到没有可认领内容。
- 最终对已生成 card 做一次 face reclaim，让三角形切换到更合适的 close card，减少局部空洞。

## 树干/树枝交叉插片

有些树干或大树枝不适合用 Billboard Clouds 聚类。插件提供了独立 trunk cards 流程：

- 通过材质实例名或父材质名中的关键字识别 trunk/branch 三角形。
- 匹配到的三角形会从 Billboard Clouds 输入中完全排除。
- 这些三角形会单独生成 2 到 4 张垂直交叉插片。
- 插片中心使用模型原点，避免只按 trunk bounds 中心放置导致偏移。
- 可选择是否只给 trunk cards 或所有 plane 开启双面拍片。

## 双面拍片

默认情况下，普通 two-sided 材质会让 plane 的背面复用正面内容，这对非对称树干或枝叶可能会把错误方向的内容镜像到背面。

当前 Demo 支持额外生成背面 atlas tile：

- 正面写入 `UV0`
- 背面写入 `UV1`
- 生成材质通过 `TwoSidedSign` 判断当前面向：
  - front face 采样 `UV0`
  - back face 采样 `UV1`

如果某个 plane 没有启用双面拍片，则 `UV1` 会镜像 `UV0`，行为等价于旧的 two-sided 复用。

## 贴图与 UV

生成的 atlas 使用固定方形分辨率，由设置项 `TextureAtlasResolution` 控制。tile 尺寸不再手动设置，而是根据所有 plane 的对象空间尺寸自动缩放，并通过矩形 packing 尽量提高利用率。

日志中会输出：

- atlas 分辨率
- largest tile
- packed tile usage
- front tiles
- back tiles
- painted pixels

注意：`painted pixels` 不是 atlas 矩形利用率。树叶 cutout 天然有大量透明区域，所以判断布局利用率应优先看 `packed tile usage`。

## 使用方法

1. 启动 Unreal Editor。
2. 在 Content Browser 中选择一个或多个 `Static Mesh`。
3. 打开菜单：

   `Tools > Billboard Clouds > Create Plane Proxy Meshes`

4. 插件会在源 mesh 所在目录生成：
   - `*_BillboardCloudProxy`
   - `*_BillboardCloudAtlas`
   - `*_BillboardCloudMaterial`

生成完成后，Content Browser 会自动同步到新创建的资产。

## 主要设置

设置位于：

`Project Settings > Plugins > Billboard Clouds`

常用参数：

- `Technique`
  - 选择 Paper 1、Paper 2 或 God of War 路径。
- `Relative Error`
  - 基于 mesh bounds 半径的相对误差。
- `Minimum Error Cm`
  - 最小对象空间误差。
- `KMeans Plane Count`
  - K-Means 模式下的目标 plane 数量。
- `God Of War Geodesic Subdivisions`
  - God of War 模式的候选方向精度。
- `God Of War Candidate Spacing Multiplier`
  - God of War 模式的候选 plane 距离采样间隔。
- `Enable Trunk Cards`
  - 是否启用 trunk/branch 单独交叉插片。
- `Trunk Card Plane Count`
  - trunk cards 数量，范围 2 到 4。
- `Trunk Card Material Keywords`
  - 用于识别 trunk/branch 材质的关键字。
- `Texture Atlas Resolution`
  - 生成 atlas 的固定分辨率。
- `Texture Tile Padding Pixels`
  - tile 周围透明 padding。
- `Double Sided Bake Mode`
  - 控制哪些 plane 额外拍背面。

## 当前 Demo 约束

- 这是 Editor 插件流程，不是运行时生成系统。
- 输出材质当前以 Masked atlas 为主。
- 生成质量依赖源 mesh 的材质、UV、alpha mask 和用户设置。
- 不同植被资产需要针对 plane 数量、误差、trunk 关键字和双面拍片模式做调参。

## 推荐调参顺序

1. 先确认 trunk/branch 是否需要从 Billboard Clouds 输入中排除。
2. 调整 `Technique`。
3. 调整误差或 K-Means plane 数量。
4. 检查 atlas 日志中的 `packed tile usage`。
5. 对非对称 trunk/branch 开启 `Trunk Cards Only` 双面拍片。
6. 对枝叶背面明显错误的资产，再尝试 `Billboard Planes Only` 或 `All Planes`。

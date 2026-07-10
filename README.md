# Billboard Clouds Plugin V1.0

Unreal Editor plugin path:

`Plugins/BillboardClouds`

This plugin generates billboard-card proxy meshes from selected Static Mesh assets. It is designed for distant-view simplification of vegetation assets such as trees, trunks, branches, and foliage cards.

V1.0 focuses on an editor-side, asset-generation workflow:

- Select one or more Static Mesh assets.
- Run the Billboard Clouds tool.
- Generate atlas textures and a copied material instance.
- Output the generated proxy geometry either as a separate Static Mesh asset, as a new LOD on the source Static Mesh, or as a replacement for an existing source Static Mesh LOD.

## Core Features

- Three generation techniques:
  - `Paper 1 - Plane-Space Greedy Cover`
  - `Paper 2 - K-Means Best-Fit Planes`
  - `God of War - Greedy Card Capture`
- Optional trunk/branch cross-card pass.
- Optional front/back atlas baking for two-sided planes.
- Fixed atlas resolution with automatic tile scaling and rectangle packing.
- Optional alpha-aware tile crop that removes transparent outer borders before final packing.
- RGB padding and tile-border padding to reduce dark transparent edges.
- Unreal material-output baking for source BaseColor, OpacityMask, Normal, Occlusion, Roughness, Metallic, and Emission into billboard atlas tiles.
- Render-data normal/tangent extraction used as the source tangent basis for material-output normal baking.
- User-provided material instance workflow. The plugin copies your configured material instance and assigns generated textures to known texture parameters.
- Mesh output modes:
  - separate proxy Static Mesh asset
  - append generated proxy geometry as a new source Static Mesh LOD
  - replace an existing source Static Mesh LOD
- Optional atlas outputs controlled from settings:
  - `ColorOpacity`
  - `NormalMask`
  - `Mix`

## Generation Techniques

### Paper 1 - Plane-Space Greedy Cover

The first-paper path searches discretized plane space. A plane is evaluated by the source triangles it can approximate within the configured object-space error.

Current implementation includes:

- normal/rho plane-space search
- adaptive refinement
- greedy best-plane selection
- object-space projection footprint
- compactness weighting for texture-friendly clusters
- valid-zone clipped texture projection

### Paper 2 - K-Means Best-Fit Planes

The second-paper path uses a budget-driven K-Means style assignment/refit process.

Current implementation includes:

- target plane count
- nearest-plane assignment by triangle-to-plane distance
- symmetric covariance best-fit plane refit
- coverage-centroid relaxation
- minimum-coverage cluster replacement
- optional crack reduction:
  - `Off`
  - `Scaled Envelope-Clipped Projection`

`Scaled Envelope-Clipped Projection` clips cross-plane projection fragments against a scaled neighbor envelope before GPU material baking. Lower scale values reduce overfill on alpha-card foliage.

### God of War - Greedy Card Capture

The God of War style path greedily selects one card at a time.

Current implementation includes:

- geodesic hemisphere candidate directions
- candidate plane spacing based on the closeness/error metric
- thick-slice triangle capture
- projected-area card scoring
- greedy face claiming
- final face reclaim to move triangles to a better close card before texture baking
- ortho-style card bounds and closeness-clipped projection behavior

## Trunk Cards

Some trunks or large branches are not good Billboard Clouds candidates. V1.0 can route matching material triangles out of the Billboard Clouds input and generate fixed vertical cross cards instead.

Settings:

- `Enable Trunk Cards`
- `Trunk Card Plane Count`
  - range: `2` to `4`
  - `2` = cross card
  - `3` = three-way star
  - `4` = four-way star
- `Trunk Card Atlas Scale`
  - `1.5x` or `2.0x`
  - increases trunk cross-card tile weight during packing
- `Trunk Card Material Keywords`
  - matches material instance names and parent material names

Matched trunk/branch triangles are fully excluded from the Billboard Clouds clustering input. Trunk cards are centered at the source mesh origin rather than only the trunk bounds center.

`Trunk Card Atlas Scale` increases trunk card tile resolution weight during packing. The final atlas resolution remains fixed by `Texture Atlas Resolution`; the packer gives trunk cards more pixels and correspondingly compresses foliage billboard tiles.

## Two-Sided Baking

By default, a two-sided material can reuse one side of a plane on the back face. This is cheap, but it mirrors the same captured content.

V1.0 supports optional separate back-side atlas baking:

- `Off`
- `Trunk Cards Only`
- `Billboard Planes Only`
- `All Planes`

UV convention:

- `UV0`: front-side atlas tile
- `UV1`: back-side atlas tile
- `UV2`: trunk/leaf point mask
  - trunk = `(0, 0)`
  - billboard/leaf = `(1, 0)`

If a plane does not have a baked back-side tile, `UV1` mirrors the `UV0` layout.

## Material Workflow

V1.0 does not generate a material graph.

Instead, configure your own material instance in:

`Tools > Billboard Clouds > BillboardCloudsTools > Material > Billboard Material Template`

The plugin duplicates that material instance for every generated proxy and assigns the enabled atlas textures to these texture parameters:

- `ColorOpacity`
- `NormalMask`
- `Mix`

The template material is responsible for sampling those parameters correctly. This lets the project keep its own shading model, material functions, switches, and rendering policy.

## Atlas Outputs

Atlas output is controlled from:

`Tools > Billboard Clouds > BillboardCloudsTools > Texture > Atlas Outputs`

### BaseColorOpacity

Setting: `Bake Base Color Opacity Atlas`

Parameter: `ColorOpacity`

Channels:

- RGB = source base color
- A = evaluated source material `OpacityMask`

Default: enabled.

BaseColor/Opacity bake behavior:

- Uses Unreal material baking to evaluate the source material's final `BaseColor` and `OpacityMask` outputs directly into each billboard tile.
- The temporary bake mesh preserves source UV channels, render-data vertex colors, normals, tangents, and binormal signs, so material functions, Material Attributes, layered blends, parameter-driven mixes, vertex-color masks, and tangent-space texture sampling are evaluated by the material shader.
- Atlas merge resolves overlapping projected fragments per card side with a far-to-near painter order plus per-pixel tile depth, so BaseColor/Opacity/Normal/Mix follow the same visible source fragment.
- If the GPU `OpacityMask` export is not usable for a masked material, opacity falls back to a projected evaluated/direct opacity source.

### NormalMask

Setting: `Bake Normal Mask Atlas`

Parameter: `NormalMask`

Stores object-space normal as encoded XYZ. The generated texture is linear and uses vector-style compression.

Default: enabled.

Normal bake behavior:

- Uses Unreal material baking to evaluate the source material's final `MP_Normal` output for each billboard tile.
- The bake requests tangent-space normal output. For materials authored with world-space normals, Unreal's material baker first converts the material normal to tangent space.
- During atlas write, the plugin rasterizes the same tile triangles into a per-pixel source TBN basis map, then converts the baked tangent-space `MP_Normal` back to object/local-space XYZ before storing `NormalMask`.
- The temporary bake mesh and the atlas decode use the same side-aware source render-data TBN basis, so trunk cards, billboard cards, tangent-space normal maps, and world-space material normals all go through the same `MP_Normal` path.
- Normal writes share the same per-pixel tile depth resolve used by `ColorOpacity`, which prevents overlapped trunk/branch fragments from mixing color from one source fragment with normal from another.
- Front and back atlas tiles are baked with opposite capture rays. Back-side bakes reverse the temporary bake mesh winding so material graphs using `TwoSidedSign` can evaluate the opposite side. For two-sided tangent-space-normal source materials, source backfaces flip the final decoded object-space normal after tangent-to-object conversion, matching Unreal's final normal flip order.
- Source Static Mesh LOD render-data normals (`VertexTangentZ`), tangents, binormal signs, and UV channels are preserved on the temporary bake mesh so tangent-space normal maps, material functions, Material Attributes, and parameter blends feed into `MP_Normal` normally.
- If the source material does not connect a Normal input, the baked default tangent normal `(0,0,1)` is converted through the source render-data TBN basis, so `NormalMask` falls back to the source mesh's interpolated render normals instead of a flat purple normal.
- `NormalMask` is therefore a material-output normal atlas converted to object/local space, not a raw source render-normal atlas.

### Mix

Setting: `Bake Mix Atlas`

Parameter: `Mix`

Channels:

- R = Occlusion
- G = Roughness
- B = Metallic
- A = Emission

Default: disabled.

Mix bake behavior:

- Uses Unreal material baking to evaluate source material `AmbientOcclusion`, `Roughness`, `Metallic`, and `EmissiveColor` outputs directly into each billboard tile.
- Shares the same per-card side projection and per-pixel tile depth resolve as `ColorOpacity` and `NormalMask`.
- Uses fallback defaults when a property is inactive or unavailable:
  - Occlusion = `1.0`
  - Roughness = `0.5`
  - Metallic = `0.0`
  - Emission = `0.0`

The generated Mix texture is linear mask data.

## Alpha-Aware Tile Crop

Setting:

- `Enable Alpha Aware Tile Crop`
- `Alpha Aware Tile Crop Guard Pixels`

When enabled, the tool runs a pre-bake alpha pass, finds the painted alpha bounds of each tile, crops the proxy plane rectangle to that outer painted region, repacks the atlas, rebuilds the proxy mesh, and then runs the final bake.

This improves per-tile usage when a tile has transparent outer borders. It does not fill or reuse transparent holes inside a tile, because atlas packing still packs rectangular billboard tiles.

## Usage

1. Enable the `BillboardClouds` plugin.
2. Run `Tools > Billboard Clouds > BillboardCloudsTools` to open the editor tool panel.
3. Add source meshes through the panel's `Source Static Meshes` array, or select meshes in the Content Browser and click `Add Content Browser Selection`.
4. Configure the material template, generation technique, mesh output, and atlas outputs directly in the panel.
5. Click `Bake`.

Generated textures and material instances use the configurable `Asset` paths and naming rules. Paths are relative to the parent of the source mesh folder, and the Content Browser syncs to the generated or modified assets after generation.

For a source mesh at `/Game/Trees/StaticMeshes/SM_Tree`, the default outputs are:

- `/Game/Trees/StaticMeshes/SM_Tree_BillboardCloudProxy` when `Mesh Output Mode` is `Create Separate Mesh Asset`
- `/Game/Trees/Textures/T_SM_Tree_DA`
- `/Game/Trees/Textures/T_SM_Tree_NR`
- `/Game/Trees/Textures/T_SM_Tree_M`
- `/Game/Trees/Materials/MI_SM_Tree`

Only enabled atlas outputs are generated.

Generated atlases use streamed mip chains rather than `NoMipmaps`. `ColorOpacity`, `NormalMask`, and `Mix` use the `World`, `WorldNormalMap`, and `WorldSpecular` texture groups respectively. All three atlases use BC7 compression; `ColorOpacity` remains sRGB while `NormalMask` and `Mix` remain linear. For masked proxy templates, the `ColorOpacity` alpha mip chain preserves coverage using the template material's opacity-mask clip value.

Asset output is staged as one operation. Textures, the copied material instance, and a separate proxy mesh are registered only after the complete bundle succeeds. If texture/material/mesh creation fails, staged assets are removed; a failed source-LOD append also restores the original source-model count and package dirty state.

## Asset Output

The `Asset` category in `BillboardCloudsTools` configures:

- texture and material output folders relative to the parent of the source mesh folder
- texture prefix
- BaseColor/Opacity, Normal, and Mix suffixes
- material instance prefix and optional suffix

Folder values may contain nested relative paths. Invalid package paths stop the bake with an explicit error instead of falling back to the source mesh directory.

## Mesh Output

Mesh output is controlled from:

`Tools > Billboard Clouds > BillboardCloudsTools > Mesh Output`

### Create Separate Mesh Asset

Setting: `Mesh Output Mode = Create Separate Mesh Asset`

The tool creates a new `*_BillboardCloudProxy` Static Mesh asset next to the source mesh. This is the default V1.0 behavior.

### Add To Source Mesh LODs

Setting: `Mesh Output Mode = Add To Source Mesh LODs`

The tool appends the generated proxy geometry as a new LOD on the selected source Static Mesh. The copied material instance is added or updated in the source mesh material slots as `BillboardProxy`, and the generated LOD section uses that slot.

### Replace Source Mesh LOD

Settings:

- `Mesh Output Mode = Replace Source Mesh LOD`
- `Replace Source LOD Index`

The tool replaces an existing source Static Mesh LOD with the generated proxy geometry. The target LOD must already exist. The tool does not create missing replacement LODs. The copied material instance is added or updated in the source mesh material slots as `BillboardProxy`, and the replacement LOD section uses that slot.

When replacing an existing LOD, the existing LOD ScreenSize is preserved.

## Important Settings

- `Texture Output Folder Name` / `Material Output Folder Name`
  - Relative output paths under the parent of the source mesh folder.
- `Texture Name Prefix` and texture suffix settings
  - Configure `T_`, `_DA`, `_NR`, and `_M` naming.
- `Material Instance Name Prefix` / `Material Instance Name Suffix`
  - Configure generated material instance naming; the defaults produce `MI_<MeshName>`.
- `Technique`
  - Selects Paper 1, Paper 2, or God of War path.
- `Mesh Output Mode`
  - Selects separate mesh asset, append-to-source LOD, or replace-source LOD output.
- `Replace Source LOD Index`
  - Existing source mesh LOD index replaced when using replace mode.
- `Relative Error`
  - Relative object-space error based on source mesh bounds radius.
- `Minimum Error Cm`
  - Absolute minimum object-space error.
- `KMeans Plane Count`
  - Target plane budget for K-Means mode.
- `KMeans Crack Reduction Mode`
  - Controls Paper 2 crack reduction behavior.
- `God Of War Geodesic Subdivisions`
  - Candidate direction density for God of War mode.
- `God Of War Candidate Spacing Multiplier`
  - Candidate plane distance spacing multiplier.
- `Enable Trunk Cards`
  - Routes matching trunk/branch material triangles into fixed cross-card planes.
- `Trunk Card Plane Count`
  - Number of trunk planes, from `2` to `4`.
- `Trunk Card Atlas Scale`
  - Gives trunk cross-card atlas tiles a `1.5x` or `2.0x` packing weight.
- `Trunk Card Material Keywords`
  - Material-name keywords used to identify trunk/branch triangles.
- `Texture Atlas Resolution`
  - Fixed square atlas resolution.
- `Source Material Bake Resolution`
  - Resolution used by the fallback/evaluation path for source-material UV-space data, mainly opacity fallback and alpha analysis. Final atlas channels are baked per billboard tile.
- `Texture Tile Padding Pixels`
  - Transparent padding around each tile.
- `Enable Alpha Aware Tile Crop`
  - Crops transparent outer borders from each tile before final atlas packing.
- `Alpha Aware Tile Crop Guard Pixels`
  - Extra pixels retained around the alpha-painted bounds during crop.
- `Double Sided Bake Mode`
  - Controls which planes receive separate back-side atlas tiles.
- `Bake Base Color Opacity Atlas`
  - Generates `ColorOpacity`.
- `Bake Normal Mask Atlas`
  - Generates `NormalMask`.
- `Bake Mix Atlas`
  - Generates `Mix`.
- `Billboard Material Template`
  - Material instance to duplicate and assign textures into.

## Log Output

The tool report includes:

- selected algorithm
- source triangle count
- output plane count
- atlas size
- largest tile size
- packed tile usage
- front/back tile count
- painted pixel count
- alpha-aware cropped plane count
- readable source texture count
- alpha policy
- normal material-output bake information
- generated asset paths
- mesh output target and source LOD index when source mesh LOD output is enabled

For texture packing, prefer `packed tile usage` over `painted pixels` when judging layout efficiency. Vegetation cutout textures naturally contain many transparent pixels.

## Notes

- This is an editor asset-generation plugin, not a runtime generation system.
- Final atlas channels are generated through Unreal material property baking. Material functions, Material Attributes, parameter blends, tangent-space normal maps, WPO, and Custom nodes are evaluated by the material baker where supported by Unreal's editor baking path.
- The opacity fallback path can still read direct opacity textures/parameters when the GPU `OpacityMask` export is unusable for a masked material.
- The generated proxy quality depends on source geometry, UVs, material setup, alpha masks, and the selected technique/settings.
- The material template is project-owned. The plugin only duplicates it and assigns generated texture parameters.

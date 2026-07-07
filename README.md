# Billboard Clouds Plugin V1.0

Unreal Editor plugin path:

`Plugins/BillboardClouds`

This plugin generates billboard-card proxy meshes from selected Static Mesh assets. It is designed for distant-view simplification of vegetation assets such as trees, trunks, branches, and foliage cards.

V1.0 focuses on an editor-side, asset-generation workflow:

- Select one or more Static Mesh assets.
- Run the Billboard Clouds tool.
- Generate a proxy Static Mesh, atlas textures, and a copied material instance.

## Core Features

- Three generation techniques:
  - `Paper 1 - Plane-Space Greedy Cover`
  - `Paper 2 - K-Means Best-Fit Planes`
  - `God of War - Greedy Card Capture`
- Optional trunk/branch cross-card pass.
- Optional front/back atlas baking for two-sided planes.
- Fixed atlas resolution with automatic tile scaling and rectangle packing.
- RGB padding and tile-border padding to reduce dark transparent edges.
- Render-data normal extraction, object-space normal atlas baking, and source normal-map baking when the source material explicitly connects a normal texture.
- User-provided material instance workflow. The plugin copies your configured material instance and assigns generated textures to known texture parameters.
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
  - `Paper Exact - Envelope Intersection`
  - `Boundary Aware - Envelope Boundary Band`

For alpha-card vegetation, `Boundary Aware` is usually safer than strict paper-style envelope projection because it limits extra projections to boundary regions.

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
- `Trunk Card Material Keywords`
  - matches material instance names and parent material names

Matched trunk/branch triangles are fully excluded from the Billboard Clouds clustering input. Trunk cards are centered at the source mesh origin rather than only the trunk bounds center.

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

`Project Settings > Plugins > Billboard Clouds > Billboard Material Template`

The plugin duplicates that material instance for every generated proxy and assigns the enabled atlas textures to these texture parameters:

- `ColorOpacity`
- `NormalMask`
- `Mix`

The template material is responsible for sampling those parameters correctly. This lets the project keep its own shading model, material functions, switches, and rendering policy.

## Atlas Outputs

Atlas output is controlled from:

`Project Settings > Plugins > Billboard Clouds > Texture | Atlas Outputs`

### BaseColorOpacity

Setting: `Bake Base Color Opacity Atlas`

Parameter: `ColorOpacity`

Channels:

- RGB = source base color
- A = source opacity/cutout mask

Default: enabled.

### NormalMask

Setting: `Bake Normal Mask Atlas`

Parameter: `NormalMask`

Stores object-space normal as encoded XYZ. The generated texture is linear and uses vector-style compression.

Default: enabled.

Normal bake behavior:

- Uses Static Mesh render-data vertex normals/tangents when available.
- If the source material explicitly connects a normal texture to the material Normal input, that tangent-space normal is sampled and converted to object space.
- If no explicit normal texture is connected, the bake falls back to source render normals.

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

- Reads directly connected texture samples or texture parameters from the source material properties when available.
- Uses fallback defaults when no readable source is found:
  - Occlusion = `1.0`
  - Roughness = `0.5`
  - Metallic = `0.0`
  - Emission = `0.0`

The generated Mix texture is linear mask data.

## Usage

1. Enable the `BillboardClouds` plugin.
2. Create or select a material instance template.
3. Configure the template in:

   `Project Settings > Plugins > Billboard Clouds > Billboard Material Template`

4. Configure the generation technique and atlas outputs.
5. Select one or more `Static Mesh` assets in the Content Browser.
6. Run:

   `Tools > Billboard Clouds > Create Plane Proxy Meshes`

Generated assets are created next to the source mesh and the Content Browser syncs to them after generation.

Typical outputs:

- `*_BillboardCloudProxy`
- `*_BillboardCloudAtlas`
- `*_BillboardCloudNormalAtlas`
- `*_BillboardCloudMixAtlas`
- `*_BillboardCloudMaterialInstance`

Only enabled atlas outputs are generated.

## Important Settings

- `Technique`
  - Selects Paper 1, Paper 2, or God of War path.
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
- `Trunk Card Material Keywords`
  - Material-name keywords used to identify trunk/branch triangles.
- `Texture Atlas Resolution`
  - Fixed square atlas resolution.
- `Texture Tile Padding Pixels`
  - Transparent padding around each tile.
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
- readable source texture count
- alpha policy
- normal source information
- generated asset paths

For texture packing, prefer `packed tile usage` over `painted pixels` when judging layout efficiency. Vegetation cutout textures naturally contain many transparent pixels.

## Notes

- This is an editor asset-generation plugin, not a runtime generation system.
- Source material graph evaluation is intentionally limited to explicit texture and common parameter reads. Complex procedural material logic is not fully evaluated.
- The generated proxy quality depends on source geometry, UVs, material setup, alpha masks, and the selected technique/settings.
- The material template is project-owned. The plugin only duplicates it and assigns generated texture parameters.

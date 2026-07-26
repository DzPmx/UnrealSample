# Texture Baker

`TextureBaker` is a standalone Unreal Engine Editor plugin for baking
mesh-derived fields into textures. It has no dependency on `FoliageBaker` and
contains no runtime module.

The first baker type is **SH Thickness**.

## Current scope

- Input: one or more standard non-Nanite `UStaticMesh` or `USkeletalMesh`
  assets, source LOD0 only.
- LOD0 reduction must be disabled.
- Bake space: Tangent Space or Local Space for StaticMesh; Tangent Space only
  for SkeletalMesh.
- Bake UV: UV0 or UV1.
- Bake backend: CPU reference path or GPU Compute Shader path.
- Texture resolution: fixed choices of 256, 512, 1024, or 2048.
- UV reuse is the default and never modifies the source mesh.
- Optional XAtlas regeneration replaces only the selected UV channel, or adds
  a missing UV1, directly on the selected StaticMesh or SkeletalMesh after a
  successful bake.
- Output: one tangent-space or local-space affine L1 coefficient texture per
  source asset.
- Optional coefficient-range remapping improves RGBA8 utilization after the
  normal bounds-normalized bake.
- Source format: linear `TSF_BGRA8`.
- Cooked compression: BC7 with alpha preserved.
- Mips: normal texture-group mip generation remains enabled.
- No bake metadata is written to either asset.

The default high-quality settings are:

```text
Bake backend       GPU
Bake space         Tangent Space
Bake UV            UV1
Resolution        1024
Sphere directions 256
Subpixel grid       4  (16 samples per pixel)
Padding            16 texels
Coefficient remap   On
```

The displayed ray estimate is a worst case before empty UV texels are removed.
Large jobs warn and remain cancellable; they are not rejected by a fixed ray
budget.

## Bake backends

Both backends implement the same geometric, coefficient-space, filtering,
padding, encoding, and asset-output contract:

- **CPU** uses UE's CPU mesh AABB tree and double-precision watertight ray
  tests. It is the reference path.
- **GPU Compute** builds the same working mesh into a CPU-side binary BVH,
  uploads that BVH and the required render normal/TBN data, and traverses it
  in an SM5 Compute Shader. It does not require hardware ray tracing or DXR.

GPU work is submitted one bounded batch at a time. A batch contains at most
4096 surface samples, and its readback fence is polled without synchronously
waiting on the render thread. The existing texture filter and RGBA8 encoder
run after all coefficients have been read back. The GPU ray tests use
single-precision arithmetic, so texels close to triangle edges can differ
slightly from the CPU result while retaining the same coefficient meaning.
GPU mode requires an active SM5-or-newer rendering RHI; it does not silently
fall back to CPU when explicitly selected.

For a multi-mesh bake, all source LOD0 meshes are placed directly into one
ray-query mesh using their authored asset-local positions and identity
transforms. There is no component or instance transform input. Every target
is rasterized through its own selected UV channel into its own texture, while
its thickness rays can hit triangles from any mesh in the group. The combined
geometry is uploaded once by the GPU backend.

## Geometry contract

The farthest-hit thickness kernel accepts:

- open surfaces;
- multiple shells and disconnected components;
- boundary and non-manifold splits;
- inconsistent winding;
- cavities, intersections, and air gaps.

For a surface point `P`, render normal `N`, and direction `w`, the ray origin
and Unity-sample clip distances are:

```text
O = P - normalize(N) * 1.1 cm

m     = max(abs(w.x), abs(w.y), abs(w.z))
NearT = 1 cm / m
FarT  = 30000 cm / m
```

The result is the farthest two-sided triangle intersection between `NearT` and
`FarT`, or zero when there is no qualifying hit. It is not `SolidLength`:
inside intervals are not paired or summed, and gaps can contribute to the
camera-to-farthest-hit distance.

This reproduces the geometric behavior of the referenced Unity sample's
two-sided cubemap pass with `ZTest Always` and `BlendOp Max`. The UE
implementation uses continuous ray/triangle intersections instead of a
128-by-128 cubemap.

**Assumption / needs confirmation:** one Unity unit is mapped to one meter,
which produces the centimeter constants above. These constants are a sample
compatibility choice, not a general scale-independent thickness definition.

Zero-area and duplicate source triangles do not stop the bake. The working
DynamicMesh omits them because they do not change farthest-hit distance.
Duplicate faces using separate UV regions are not guaranteed to receive
independent texture coverage. Unreferenced vertices are ignored. A warning
reports skipped source triangles.

## Bake-space contract

Tangent Space asks Unreal to prepare the standard non-Nanite LOD0 float render
tangent basis from the current Build Settings:

- Recompute Normals and Recompute Tangents are supported.
- MikkTSpace and Compute Weighted Normals keep their Unreal behavior.
- UV0 regeneration occurs before the final tangent build so MikkTSpace reads
  the UV state that will be committed.
- `BuildScale3D` and the StaticMesh legacy tangent-scaling mode are applied to
  the working geometry and TBN.
- Temporary built normals, tangents, and binormal signs are never committed.

StaticMesh Local Space stores the SH directional coefficients in the mesh
asset's local/object axes. It does not use TBN to rotate sample directions.
The runtime query direction must be transformed into mesh local space before
evaluating `C0 + dot(Cxyz, D)`.

SkeletalMesh supports Tangent Space only. Geometry and thickness are baked
from the LOD0 reference pose. At runtime the tangent basis follows skinning,
so the coefficient directions deform with the surface, but the baked
thickness values do not update when joints bend.

Only normal/TBN data referenced by final working triangles is validated.
Local Space requires a valid render normal for ray-origin offset. Tangent
Space additionally treats non-finite, zero, parallel, or singular tangent
data as a hard error because a tangent-space direction cannot be evaluated
from it.

A render TBN discontinuity inside a continuous Bake UV chart is a quality
warning, not a bake error. Coefficients on the two sides are expressed in
different frames, so bilinear or mip filtering can produce local seam
artifacts.

UE's `XAtlasWrapper` accepts positions and triangle indices but exposes no
forced TBN-seam input. Regeneration therefore creates a valid non-overlapping
atlas but does not claim to cut every final render TBN seam. Any uncovered,
materially different seam is counted and reported after the final render TBN
is built.

## Bake UV contract

With regeneration disabled:

- the selected UV must already exist;
- the source mesh, MeshDescription, UVs, and Build Settings are not modified;
- Padding dilates the output texture but cannot add chart spacing.

With regeneration enabled:

- XAtlas replaces the selected channel, or adds missing UV1;
- other UV channels, skeletal attributes, and imported normal/tangent
  attributes are preserved;
- the change is committed only after the texture bake succeeds;
- the StaticMesh or SkeletalMesh change is transactional.

UV validation applies only to non-degenerate, unique working triangles:

- values must be finite;
- values must remain inside `[0,1]`, with a small numerical tolerance;
- non-degenerate 3D triangles need non-degenerate UV area;
- overlap larger than one quarter of a mip-0 texel is a hard conflict;
- smaller positive-area slivers are warnings.

Meaningful overlap remains a hard error because one texture texel cannot store
different coefficient fields for two unrelated surfaces. Mirrored, stacked,
or tiled material UVs generally require regeneration or a separate Bake UV.

`Padding` may be zero. It is exact mip-0 output dilation. With XAtlas it is
also passed to the approximate-resolution packer, so final chart spacing is
approximate. Finite padding cannot prevent unrelated charts from meeting in
the smallest mips.

## StaticMesh lightmap interaction

Reuse mode stops if automatic lightmap generation overwrites the selected
channel, because the stored source UV would not be the final rendered UV.

Regeneration stops if the selected UV feeds automatic generation of another
channel, because replacing it would also change that derived layout.

If automatic lightmap generation writes into the regenerated Bake UV, the
confirmation dialog explicitly reports that Texture Baker will disable that
Build Setting during the same transactional commit. The tool also warns when
the StaticMesh Light Map Coordinate Index selects the Bake UV.

## L1 convention

For normalized farthest-hit thickness `f(d)`:

```text
C0   = mean(f)
Cxyz = 3 * mean(f*d)
```

Logical texture channels:

```text
R = Cx
G = Cy
B = Cz
A = C0
```

The direction set is a deterministic antipodally paired Fibonacci sphere.

Given a linear texture sample `S`:

```text
Cxyz = S.rgb * 2 - 1
C0   = S.a

NormalizedThickness = max(0, C0 + dot(Cxyz, DirectionInBakeSpace))
```

The RGBA8 mapping follows the referenced shader convention. UNORM8, BC7,
bilinear filtering, and mips all make the reconstructed field approximate.

`ThicknessScaleCm` is the combined built working-geometry bounds diagonal plus
the 1.1 cm origin offset. It normalizes every output in the group with the same
scale. By request, it is not stored as metadata. Materials that need physical
distance rather than normalized thickness must receive the same scale through
a project-owned parameter.

When **Remap SH range for RGBA8** is enabled, all filtered floating-point
coefficient images in the group are measured before quantization. One linear
gain shared by every output and all four coefficients is applied to `Cx`,
`Cy`, `Cz`, and `C0`:

```text
DirectionalGain = 0.8 / max(abs(Cxyz))
ConstantGain    = 0.9 / max(C0)
Gain            = min(DirectionalGain, ConstantGain)
```

Constraints whose measured maximum is zero are ignored when selecting the
gain.

This places signed RGB within stored values `0.1-0.9`, keeps RGB zero at
`0.5`, and keeps `C0` within `0-0.9`. An all-zero image remains unchanged.
The material decode formula does not change. The common gain preserves the
directional SH shape and remains compatible with linear filtering. Outputs
from the same group remain comparable, but coefficient magnitude cannot be
compared across separately run bakes. The gain is intentionally not stored as
metadata.

## Editor workflow

1. Open **Tools > Texture Baker**.
2. Choose **SH Thickness**, select a StaticMesh or SkeletalMesh in the first
   thumbnail asset slot, and use **Add Mesh** for additional group members.
3. Select the CPU or GPU Compute backend.
4. Select Tangent Space or Local Space. SkeletalMesh locks this to Tangent
   Space.
5. Select UV0 or UV1.
6. Leave regeneration unchecked to reuse the channel without modifying the
   mesh, or enable it to replace/add the selected channel.
7. Optionally enable coefficient remapping to improve RGBA8 range usage.
8. Configure quality and press **Bake**.
9. Review warnings, including the identity-local placement contract, and
   confirm.
10. Save each generated `_SHThickness_L1_TS_UV0`,
   `_SHThickness_L1_TS_UV1`, `_SHThickness_L1_LS_UV0`, or
   `_SHThickness_L1_LS_UV1` texture.
11. Save the source meshes only when regeneration modified them.

Read-only `/Engine/` meshes can be baked with regeneration disabled; their
textures are created under `/Game/TextureBaker`. Regeneration of `/Engine/`
assets remains unsupported.

Preparation and XAtlas run on the game thread. CPU ray evaluation runs on a
worker thread. GPU mode collects the exact map-baker surface samples on the
worker, submits batched Compute Shader work to the render thread, reads the
coefficients back, and runs the same texture filtering and RGBA8 encoding on
the worker. Cancellation, invalid numerical output, asset creation failure, or
a bake-relevant change to any source prevents the prepared UV set from being
committed. Group UV changes use one editor transaction.

## Hard errors

The tool stops for:

- unsupported Nanite or reduced LOD0 paths;
- missing editable source LOD0;
- Local Space selected for SkeletalMesh;
- reuse of a missing UV channel;
- non-finite referenced geometry or zero/non-finite Build Scale;
- missing or unusable final render normal/TBN required by the selected space;
- non-finite, out-of-range, or zero-area Bake UV data on a working triangle;
- texture-resolution-significant UV overlap;
- an XAtlas result the current UV writer cannot represent;
- conflicting automatic lightmap generation described above;
- GPU mode without an active SM5 rendering RHI;
- non-finite ray or coefficient data;
- source changes during an active job;
- texture or commit failures.

## Warnings

The tool allows but reports:

- open, multi-shell, boundary, or non-manifold topology;
- duplicate or zero-area source triangles omitted by the working mesh;
- sub-texel UV overlap slivers;
- continuous Bake UV across materially different render TBN seams;
- zero Padding;
- additional LODs;
- UV0 replacement side effects;
- lightmap-coordinate sharing;
- Build Settings changed with explicit confirmation;
- large estimated ray counts.

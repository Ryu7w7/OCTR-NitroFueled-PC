# Fur/hair materials missing (open)

Symptom: every fur-covered body (Crash included) is absent; rigid parts draw. Not readbacks, not
in-place command decoding, not the 1D-texture view (all tried; the 1D view change is kept as it
is correct on its own). Vulkan validation (BB_VK_VALIDATION=1, run 22) on the fur pipeline:

- The fur geometry shader (one program, cache hash 0xdcc4d960) declares inputs at locations 0..14
  (60 components) plus gl_in {Position, PointSize, ClipDistance} (7): 67 > this GPU's
  maxGeometryInputComponents 64 (VUID-RuntimeSpirv-Location-06272). Upstream shadPS4 0.19 has a
  fur regression on AMD iGPUs (issue #4413, 2026-10-08) and the same recompiler code.
- Its export (vertex) stage writes only locations 0..6 (ESGS itemsize 32 dwords); no cached vertex
  module writes more than 10 locations. So GS reads of locations 7..14 are not an interface the
  game intends (VUID-RuntimeSpirv-OpEntryPoint-08743).
- Hypothesis (next step): in shader_recompiler/ir/passes/ring_access_elimination.cpp,
  HwStage::Geometry, a ring read whose soffset bucket is >= gs_info.in_vertex_data_size belongs to
  the next input vertex (ESGS layout: component-major, 256 bytes per component, vertices
  consecutive). Map `vertex += bucket / itemsize; bucket %= itemsize` before deriving the
  attribute. That makes the GS declare 7 locations (32 components) and match its ES.
  Then delete user/cache/CUSA13795/*.spv (cached modules keep the old interface) and retest with
  validation on. If the GS legitimately needs more than 64 components, drop the unused PointSize/
  ClipDistance members of gl_in in the backend (spirv_emit_context.cpp) to save 3.
- Also seen: VUID-VkGraphicsPipelineCreateInfo-layout-07988 (vertex-stage descriptors flagged
  fragment-only / missing from the layout) in 20 pipeline creations; check whether it is confined
  to these GS pipelines after the fix.

## Outcome (2026-10-09, run 25)

The ring-read mapping now folds a slot at or past `in_vertex_data_size` onto the next input
vertex (ring_access_elimination.cpp, HwStage::Geometry) and logs any read past slot 32. With the
SPIR-V cache cleared, fur renders (user report: "looks fine now"). In that run the geometry shader
compiled without any read past slot 32, i.e. the good variant; the validation run before it had
the same shader reading slots 32..60. The slot base therefore comes from draw-time data folded
into soffset, so both variants exist and the fold covers the bad one. If fur ever goes missing
again, look for "sacredfire GS ring read" lines and VUID-RuntimeSpirv-Location-06272.

## Follow-up (run 26/27): the on-disk shader cache

With the fix in place, a launch that reuses `user/cache/CUSA13795` (SPIR-V modules named
`<hash>_<permutation index>.spv`, serialized pipelines) shows the fur broken again without
recompiling the geometry shader; a launch with that directory empty compiles it (log: VERT_ITEMSIZE
warning, no out-of-vertex reads) and fur is right. User-confirmed twice. run.sh therefore clears the
cache per CTR launch. Root cause to find: permutation indices are assigned in first-seen order per
run, so `<hash>_1.spv` may hold a different specialization (plain VS vs export stage of the GS
pipeline) than the one the pipeline cache expects; check vk_pipeline_cache.cpp's cache load against
the `.meta` specialization records.

## 1D images lowered to 2D (run 31)

The minimal form of the upstream change (image type Dim2D with height 1, coordinate/offset/
derivative padding, size queries, e2D views and images) compiles and runs without the RADV crash
seen with the full #5133 backport; the interim "keep 1D images apart" rule is reverted. It fixed
the adventure hub sky (a colour LUT built from height-1 render targets).

## Root cause of the cache breakage (2026-10-09)

`HwGeometryRuntimeInfo::operator==` (shader_recompiler/runtime_info.h) compared the geometry
stage's outputs, invocations, output vertices, input primitive, output primitives and the copy
shader hash, but not `in_vertex_data_size` (the ESGS ring item size), `out_vertex_data_size` or
`mode`; it also tested `num_invocations && other.num_invocations` instead of comparing them. The
ring-read fold above uses `in_vertex_data_size` to decide which input vertex a ring slot belongs
to, so the compiled SPIR-V depends on it while the specialization said two such shaders were the
same. Within a session, whichever item size compiled first served every later draw of that program
(fur right or wrong depending on draw order: "ok wtf now crash's fur is broken again" / "ok now
its fine again"); with the on-disk cache the preloaded module decided it for the whole session.
The comparison now covers every field the recompiler reads; the compute stage's `shared_memory_size`
(read by the shared-memory passes) joins its comparison for the same reason. Cache versions bumped
(binary 8: the fold and the 1D lowering changed codegen without a bump; meta 8, pipeline key 6).

The specialization fix alone did not cure the cached case (run 44: fur wrong with the cache
kept, no geometry shader recompiled). Second cause, in the preload path
(vk_pipeline_serialization.cpp, LoadShaderMeta): the pipeline's fetch shader was assigned from
every stage's metadata, and stages load in SwStage order (fragment, tessellation control and
evaluation, vertex, geometry). For a pipeline with a geometry stage the geometry metadata, which
has no fetch shader, cleared the one the vertex stage had just set, so every preloaded GS pipeline
was built without vertex attributes. The runtime path (RefreshGraphicsKey's bind_stage) only
takes a fetch shader from a stage that has one; the preload path now does the same.

Run 45 (cache kept, both fixes): fur right ("fur looks fine now"). run.sh keeps the cache from
here on; BB_CLEAR_SHADER_CACHE=1 clears it.

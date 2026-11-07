# Task and mesh graphics pipeline libraries

With `EDEN_NX2_GPL=1` on a device supporting both graphics pipeline libraries and
task/mesh shaders, native task/mesh pipelines use the same shader-library cache as
ordinary graphics pipelines. They link three parts: pre-rasterization shaders
(task and mesh together), fragment shader, and fragment output. There is no
vertex-input library. Fast and optimized links use the same three parts.

The shader stages retain their captured execution state, resource specialization,
and task/mesh interfaces. Library identities include the final emitted SPIR-V,
descriptor layouts, push-constant range, relevant graphics state and render pass.
Task/mesh stages are distinguished from the vertex/tessellation slots holding
them. All libraries and the final layout use the same mesh-aware push-constant
range, including the fragment library and the monolithic fallback layout.

Existing disk-cache boot replay can therefore seed reusable task/mesh libraries
for later graphics-state combinations. This does not reconstruct unrecorded
environments from archive code, nor guarantee that a new shader or specialization
is already compiled. The archive warm-up format and its VS/FS admission are
unchanged. In particular, matching native shader hashes alone does not prove
library reuse. A first library build can still stall, and failed GPL construction
continues through the existing monolithic fallback.

## Targeted validation

Set `EDEN_NX2_PIPELINE_TRACE_HASH` to exactly 16 hexadecimal digits to trace one
complete graphics-pipeline key. For example:

```text
EDEN_NX2_GPL=1 EDEN_NX2_PIPELINE_TRACE_HASH=20acc41b19b908cf
```

This diagnostic is off by default. Its budget is independent of the general
128-entry pipeline job/wait quotas. It records boot admission and result, build
completion (including synchronous boot builds), first runtime lookup, and library
lookup outcomes (`built`, `hit`, `waited`) with success and the producer pipeline.
Each library event includes its actual key digest and the final SPIR-V hashes of
its stages. The 256-event budget emits a `trace_limit=256` notice when exhausted;
a library event can include up to five additional stage lines. Targeted waits
retain the `pipeline_wait` log format.

The library-key digest includes a process-local render-pass handle. Compare it
within one process; SPIR-V hashes can be compared across processes with matching
device/profile/emission settings. A successful library hit with a different
producer pipeline demonstrates reuse across pipeline keys. It does not by itself
measure a frame-time improvement. A successful boot result followed by a ready
first lookup instead demonstrates replay of the exact pipeline.

Run `[mesh_pipeline_library]` and `[mesh_pipeline_key]` unit tests after building.
GPU validation additionally needs an authentic recorded pipeline, Vulkan
validation-layer output, actual library-hit evidence and gameplay comparison.
Check both GPL and monolithic behavior with the same captured inputs. A successful
unit test or pipeline build alone is not a gameplay pass.

# Mesh programs without task payload reads

Mesh programs whose only live ISBE input reads use byte address zero read the workgroup index.
Previously, SPIR-V emission also loaded the task payload before selecting that index. It declared
`TaskPayloadWorkgroupEXT` even for Mesh-only pipelines. The installed validation layer reported
VUID 10883 when no Task stage was present.

Shader info now records possible payload reads after constant propagation and dead-code elimination.
An immediate zero read returns the workgroup index directly. Mesh programs without any remaining
nonzero or dynamic reads omit the payload declaration. Task programs always retain it for their
output writes and task epilogue. Captured payload sizes and dynamic-address behavior are unchanged.
Mesh-only programs with real payload reads still require investigation; this change does not invent
a missing Task stage or establish a performance improvement.

Upstream nx2 `0857e8b4a792ac99b87af6bb6f329127aa7df683` retains the unconditional declaration.
The reviewed diffs of [PR 684](https://github.com/Djihads80/Eglantine/pull/684) and
[PR 472](https://github.com/Djihads80/Eglantine/pull/472) change Task payload initialization and
Task output reads/packing, respectively; neither removes unused Mesh payload declarations.

Run the `[mesh_payload]` unit tests. They emit real SPIR-V from optimized IR for no reads, live zero,
constant-folded zero, dead nonzero, live nonzero, and dynamic reads, at both 64 and 800 words. They
check payload storage types, variables, array lengths, loads, and the workgroup-index extraction.
Set `EDEN_MESH_PAYLOAD_TEST_OUTPUT` to an empty scratch directory to export all 16 modules, then run
`spirv-val --target-env vulkan1.2 <module.spv>` on each. This validates individual modules; Vulkan
pipeline validation must separately check Mesh-only and Task+Mesh combinations.

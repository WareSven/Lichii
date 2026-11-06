# CPU exclusive-read experiment

This temporary branch tests a shorter x64 exclusive-monitor critical section. It must not be
merged to nx2 with its experiment switch. The initial state is the reference implementation.

The reference holds the global monitor lock while an inline guest exclusive load calls back
into memory. That callback can wait for the GPU. Other cores then spin on the same monitor.
The candidate publishes its reservation under the lock, releases it, loads the value, and
saves that value in the owning core's slot. Exclusive stores still take the global lock and
invalidate matching reservations. A concurrent store therefore invalidates the pending read
even when the load subsequently observes the new value. Saving the value never republishes
the reservation. Only 8/16/32/64-bit inline reads participate; 128-bit reads and C++ monitor
callbacks keep the reference behavior.

Enable the test package with `EDEN_NX2_EXCLUSIVE_READ_EXPERIMENT=1` and the normal loopback
debug server. The A64 JIT samples the mode once per participating load into its own state.
Changing the switch during a callback cannot change which of that load's two unlock sites
runs. Without the environment variable, no experiment pointer is provided to the JIT.

The debug method `cpu_exclusive_read_experiment` accepts no parameters to inspect state, or
`{"enabled": true}` / `{"enabled": false}` to select candidate/reference. `status` also reports
the selected mode and its generation. A reply acknowledges selection, not the retirement of
every in-flight read. Exclude transitions and warm-up from performance samples.

For A/B/A testing, keep the same running session, position, camera, graphics settings, and
clock settings. Capture the fixed scene before timing and again before allowing movement.
Collect reference, candidate, then reference windows with boundary timestamps and present
counts. The native counter counts guest PresentClientBuffer requests, not interpolated FSR3
frames or actual display refreshes. Assess normal game speed and graphics separately.

Upstream reviewed before this experiment: Eglantine PR #717 at
`90343821cf782499cfa7aa622797c85273153bbb`. That proposal removes the store lock and publishes
the reservation after loading; it is not the algorithm tested here. No performance gain or
60 FPS result is established by compilation or CPU unit tests alone.

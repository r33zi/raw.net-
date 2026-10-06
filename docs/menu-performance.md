# Menu rendering and data collection

The DX9 renderer consumes immutable snapshots published by a single collection
worker. Scans, driver reads, skeleton reconstruction, and rescan requests run on
that worker. Settings are copied before each collection; collection never holds
the UI request mutex while doing work. Projection, camera, vertices, and counters
are published together. The render thread has its own projection state. Frames
older than 250 ms or captured at another viewport size are not used for world
visuals or targeting. Mouse targeting is suspended while the menu is open.

Collection runs at most once per 33 ms, with no catch-up queue. Hidden/minimized
windows and lost devices suspend collection and rendering while continuing to
pump messages. Shutdown joins collection before releasing its dependencies.
An in-flight driver call still has to return before shutdown can finish.

DX9 now requests hardware vertex processing, with a software fallback. It does
not allocate an unused depth buffer. Static icon textures are uploaded during
initialization to the managed pool and survive device resets. Font upload is
also completed before the first frame, and a failed font lock releases its texture. Failed resets are
retried before starting another frame. This addresses identified blocking work
without a DX11 migration; actual FPS and latency improvements need Windows
measurement. Menu FPS is shown in the ESP tab.

## Offset status

The pasted manager/feature addresses are not entries in this repository. Repeated
values across unrelated features do not establish successful resolution. They
must not be substituted into `offsets.h` without the matching executable build,
module base/size, dumper source, and signature/read diagnostics.

The supplied HUD 118144515 metadata and executable hash are documented in
[build-118144515.md](build-118144515.md). They are not a claim that offsets are
current. The legacy runtime decoders remain unverified for that build. No offset
values or decoder algorithms were replaced in this change.

## Portable checks

From the repository root:

```sh
g++ -std=c++20 -pthread -Wall -Wextra -Werror -pedantic -Ireverse tests/async_snapshot_test.cpp -o /tmp/async_snapshot_test
timeout 15s /tmp/async_snapshot_test
g++ -std=c++20 -Wall -Wextra -Werror -pedantic -Ireverse tests/build_118144515_test.cpp -o /tmp/build_118144515_test
/tmp/build_118144515_test
```

The worker test blocks collection and verifies that reading the previous frame
and changing settings still work, that old frames remain immutable, that shutdown
joins work and wakes the timer, and that failure/restart are observable.

## Windows validation still required

Build `reverse.sln` with the declared VS 2022 v143/Windows SDK toolchain, Release
x64. With the matching game and driver, compare menu FPS/input responsiveness
while scans run and while skeleton display is enabled. Verify rescan, resizing,
minimize/restore, Alt-Tab, device loss/reset, popup clicks beyond the menu bounds,
icon display after resizing, the weather intensity slider, and shutdown during
collection. No Windows runtime measurements are available from the Linux sandbox.

# ESP rendering investigation

## Fixed in this patch

- Position reads now share the skeleton reader's indexed component resolver,
  including discovered list/index offsets and read/index validation.
- Camera and live positions are sampled after skeleton discovery and label
  resolution. The frame's age starts before those final reads, so a slow final
  read still expires normally. Cached actors have their origins refreshed even
  between capture windows. Bone poses have a separate age limit.
- Box and bone projection accepts finite points in front of the camera outside
  the viewport. ImGui clips their drawing. Screen inclusion still gates labels,
  snaplines and targeting. A box smaller than four pixels no longer suppresses
  the entity's other visuals.
- Missing head/neck bones use the existing height estimate for boxes instead of
  projecting the world origin. Failed/nonfinite camera reads and empty matrices
  are rejected; an invalid configured camera pointer clears its old address.
- The fifth overlay diagnostic line exposes pipeline/worker state, camera read
  status, sample age, projected actor count, and available bone count. “Valid”
  means basic read/finite checks passed, not that the build's decoder is verified.

## Remaining build dependency

HUD 118144515 metadata is documented in [build-118144515.md](build-118144515.md).
The existing runtime camera offsets, paired-position reader, capture signatures,
and skeleton palette discovery still need verification against the running
executable. This patch does not supply the missing encoded camera/owner decoders
or infer palette layouts from metadata. It cannot guarantee ESP works on that
build without live diagnostics.

## Forum research (2026-10-06)

Search results identified these relevant UnknownCheats discussions:

- [W2S](https://www.unknowncheats.me/forum/rainbow-six-siege/733723-w2s.html)
- [Camera works, need a live actor pointer](https://www.unknowncheats.me/forum/rainbow-six-siege/773335-camera-live-actor-pointer.html)

Direct requests to both returned HTTP 403/Cloudflare blocks. Only search snippets
were accessible; no post bodies or replacement offsets were verified or adopted.
The web search API also failed authentication. Code changes above are based on
repository inspection.

## Validation

```sh
g++ -std=c++20 -Wall -Wextra -Werror -pedantic -Ireverse tests/overlay_projection_test.cpp -o /tmp/overlay_projection_test
/tmp/overlay_projection_test
python3 tests/esp_readers_test.py
python3 tests/esp_initialization_test.py
```

The reader test compiles selected production functions against synthetic memory;
it does not compile the Windows application or exercise the driver. The
initialization test also compiles production startup code with synthetic discovery
and allocation results, covering GameManager-only startup, capture fallback,
and the failure messages when neither source is available.

Build Release x64 with the declared Windows/MSVC/DirectX SDK toolchain, then
check boxes at viewport edges, moving actors during capture gaps, and enabling
skeletons during discovery. Collect the startup console and overlay counters if
ESP remains absent. A failed pipeline indicates initialization/capture discovery;
unavailable camera indicates pointer/read/matrix failure; coordinate rejections
indicate the actor/component position path. A high sample age indicates stalled
reads. None of these Windows/live checks can run in the Linux sandbox.

## Initialization failures

`pipeline failed | worker stopped` means initialization returned before the
worker was started. `camera not sampled` distinguishes this from a failed camera
read. The overlay and ESP menu now show the startup failure reason: PE headers,
code-section reads, missing or ambiguous entity anchors, multiple entity targets,
or capture allocation. Entity discovery and capture allocation failures prevent
startup only when the GameManager list layout is also unavailable. A configured
GameManager path can initialize without capture; its live reads still require
validation. Consult the corresponding startup console messages.

The reported startup log successfully cached the code section, then found 36
entity-call anchors and two candidate calls. The scanner rejected the ambiguous
anchor before capture or camera sampling could start. The same log reported
0/12 HUD 118144515 entry signatures matching and no configured view/camera
pointer signature matches. This does not establish the running build number or
prove why bytes differ (for example, a different build or runtime code changes).

Do not raise the anchor limit or select an arbitrary call to suppress this
failure. Matching executable bytes and the current game build are required to
verify a replacement entity signature and the camera decoding path. Collect a
dump of the running module together with the `[BUILD]`, `[SCAN]`, and
`[ENTITY-SCAN]` log lines. Live box rendering remains unverified until those
build-specific paths are resolved and exercised with the Windows game/driver.

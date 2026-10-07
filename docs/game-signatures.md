# Supplied GameManager signatures

`reverse/game_signatures.h` contains the supplied manager, entity, and skeleton
patterns. Startup resolves them from the cached executable `.text` section and
prints `[LAYOUT]` results. GameManager/ProfileManager resolve signed RIP-relative
operands to **pointer slots**; structure patterns resolve offsets, not addresses.
`Skeleton.TransformArray` has a signed one-byte displacement. The marked ✓
EntityList/TeamId variants take priority; alternatives are used only if the
preferred pattern is absent. Conflicting operand results are rejected. The
marked ✗ EntityList.Array(v4) LEA pattern is excluded.

Actor collection reads `*GameManager -> *EntityList -> *EntityList.Array`, with
an int32 count capped at 512 and pointer-sized entries. It checks read success,
pointer ranges, actor vtables, and stable manager/list/count/array values around
the read. This assumes the pointer-list layout implied by the supplied names;
confirm it against the running build. Successful empty lists clear old actors.
The existing capture path remains available when the list cannot be read.
Initialization can proceed with the manager layout even without a capture target.

The other supplied fields are resolved and logged; their object ownership,
encoding, and value interpretation have not been verified against this build.
They are not silently substituted into the existing indexed-component, stencil,
or skeleton-palette readers. The new patterns do not describe equivalents for
camera, position, round, or capture signatures, so those remain separate.

## Additional global signatures

The supplied `g_world`, `g_names`, and `g_objects` signatures are scanned for
startup diagnostics only. Each uses `match + 7 + signed_disp32_at_3`.
`g_world` starts with MOV and resolves to a pointer slot. The two LEA patterns
resolve to direct addresses; the scanner does not dereference them. Logs label
these as `pointer-slot` and `direct-address` respectively.

Their source game/build is unconfirmed. These names are commonly associated
with Unreal Engine, while this application targets `RainbowSix.exe`. A byte
match alone does not establish a compatible engine or object layout. They do
not replace GameManager, the actor list, world coordinates, or camera readers.
Using them requires a verified world-to-actor chain and component/camera layout.

## Wrong box position

Box horizontal bounds now include both the head and feet projections, avoiding
cutting off a head displaced sideways by leaning or perspective. This does not
validate the source world coordinates. The current paired-position heuristic,
owner transforms, and camera decoding still need live verification. A skeleton
component offset alone does not establish a world-position field or transform.

Collect `[LAYOUT]`, `[R6]`, and `[POS]` startup/runtime lines, the exact game build,
and a screenshot of misplaced boxes. A verified world-position path and camera
layout are required to finish diagnosing wrong coordinates; do not substitute
a movement-component pointer or encoded matrix field for a world-space vector.

## Portable checks

```sh
g++ -std=c++20 -Wall -Wextra -Werror -pedantic -Ireverse tests/game_signatures_test.cpp -o /tmp/game_signatures_test
/tmp/game_signatures_test
python3 tests/esp_readers_test.py
g++ -std=c++20 -Wall -Wextra -Werror -pedantic -Ireverse tests/overlay_projection_test.cpp -o /tmp/overlay_projection_test
/tmp/overlay_projection_test
```

A Windows Release x64 build and a live game/driver session are still required.
Synthetic memory checks cannot verify these signatures against the executable.

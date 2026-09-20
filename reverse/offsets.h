#pragma once

#include <cstdint>

namespace OFFSETS
{
    // Addresses captured from RainbowSix.exe are stored as RVAs. The runtime
    // module base is always used so ASLR cannot invalidate a captured base.
    static constexpr uintptr_t DumpGameBase = 0x7FF66C070000ULL;

    static constexpr uintptr_t ActorPatchRva = 0x00CFCE5B;
    static constexpr uintptr_t CameraPatchRva = 0x0E6A4795;
    static constexpr uintptr_t CodeCaveOneRva = 0x10D73294;
    static constexpr uintptr_t CodeCaveTwoRva = 0x10D78DF4;

    static constexpr uintptr_t ActorTrampolineRva = 0x000080D2;
    static constexpr uintptr_t ActorMovRva = 0x00CFCE57;
    static constexpr uint8_t ActorMovBytes[] = { 0x48, 0x89, 0x15 };
    static constexpr bool ActorPatternIsTypeA = true;

    static constexpr uintptr_t CameraMovRva = 0x0E6A4779;
    static constexpr uintptr_t CameraTrampolineRva = 0x00052192;
    static constexpr uint32_t CameraCaptureRegisterIndex = 2; // supplied as r2

    static constexpr uintptr_t ViewMatrixRva = 0x11FB8EF0;
    static constexpr uintptr_t ViewBlockAddress = 0x00000000;
    static constexpr uintptr_t CameraPositionOffset = 0x190;
    static constexpr uintptr_t ViewProjectionOffset = 0x250;

    static constexpr const char* CameraOneSignature =
        "48 8B 0D ?? ?? ?? ?? 4C 8B 01 41 FF 90 D8 00 00 00 F3 0F 10";
    static constexpr const char* CameraTwoSignature =
        "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 D8 00 00 00 F3 0F 10";
    static constexpr const char* GameManagerSignature =
        "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 FF 90";
    static constexpr const char* EntitySignature =
        "FF 91 E0 00 00 00 8B B8 10 01 00 00";
    static constexpr const char* ViewMatrixSignature =
        "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 83 C4 58 41 5D 41 5C 41 5F 5E 5B 5F 5D";
    static constexpr const char* ViewAnchorSignature =
        "A4 70 7D BF 00 00 00 00 00 00 00 00 00 00 A0 40 "
        "00 00 A0 C0 00 00 00 00 00 00 00 00 CD CC 4C 3F "
        "00 00 00 3F 00 00 80 3E";
}

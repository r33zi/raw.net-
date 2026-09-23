#pragma once

#include <cstdint>

namespace OFFSETS
{
    // Addresses captured from RainbowSix.exe are stored as RVAs. The runtime
    // module base is always used so ASLR cannot invalidate a captured base.
    static constexpr uintptr_t DumpGameBase = 0x7FF66C070000ULL;

    static constexpr uintptr_t CameraPatchRva = 0x0E6A4795;
    static constexpr uintptr_t CodeCaveOneRva = 0x10D73294;
    static constexpr uintptr_t CodeCaveTwoRva = 0x10D78DF4;

    static constexpr uintptr_t ActorTrampolineRva = 0x000080D2;

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
    static constexpr const char* PositionSignature =
        "00 00 00 00 00 00 00 00 00 00 80 3F 00 00 00 00 "
        "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 00 00 00 00 "
        "9D 99 99 3E 00 00 00 00 00 00 00 00";
    static constexpr uintptr_t PositionXOffset = 0x10;
    static constexpr uintptr_t PositionYOffset = 0x14;
    static constexpr uintptr_t PositionZOffset = 0x18;
    static constexpr const char* InGameFlagSignature =
        "F6 C1 07 45 0F B6 ?? 45 0F 44 ?? 41 83 E2 01 44 89 15 ?? ?? ?? ??";
    static constexpr int InGameFlagInstructionOffset = 15;
    static constexpr const char* ActorCallerSignature =
        "65 ?? 8B ?? 25 58 00 00 00 ?? 8B ?? ?? ?? 8D ?? ?? ?? ?? 00 ?? C1 ?? 03";
    static constexpr const char* EntityFunctionCallsSignature =
        "C7 05 ?? ?? ?? ?? 00 00 01 00";
    static constexpr const char* EntitySignature =
        "FF 91 E0 00 00 00 8B B8 10 01 00 00";
    static constexpr const char* ViewMatrixSignature =
        "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 83 C4 58 41 5D 41 5C 41 5F 5E 5B 5F 5D";
    static constexpr const char* ViewAnchorSignature =
        "A4 70 7D BF 00 00 00 00 00 00 00 00 00 00 A0 40 "
        "00 00 A0 C0 00 00 00 00 00 00 00 00 CD CC 4C 3F "
        "00 00 00 3F 00 00 80 3E";
}

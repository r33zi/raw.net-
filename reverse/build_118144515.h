#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Findings for this executable only. RVAs must be added to the queried module
// base; member offsets must be added to the corresponding object, never base.
namespace Build118144515 {
static constexpr uint32_t HUD = 118144515;
static constexpr const char* ExecutableSHA256 =
    "F88F5265402690E6525BA48A4F2BC4BF677D1B278FF09B669C90DAD617130E92";
static constexpr uint32_t TLSIndexGlobalRVA = 0x125017BC;

namespace VTables {
static constexpr uint32_t LocalControllerRVA = 0x11B04278;
static constexpr uint32_t AIControllerRVA = 0x11B03E78;
static constexpr uint32_t OwnerRVA = 0x11A21018;
static constexpr uint32_t OperatorRVA = 0x11A42D88;
static constexpr uint32_t SkeletonRVA = 0x11C9CC30;
static constexpr uint32_t CameraRVA = 0x119F3EE0;
}
namespace Controller {
static constexpr uint32_t OwnerEncodedField = 0x30;
static constexpr uint32_t LocalCameraEncodedField = 0x3E8;
static constexpr uint32_t OwnerAccessorVTableSlot = 0x108;
}
namespace Owner {
static constexpr uint32_t ComponentTable = 0xD8;
static constexpr uint32_t ComponentCountDescriptor = 0xE0;
static constexpr uint32_t ComponentCountMask = 0x3FFFFFFF;
static constexpr size_t ComponentPointerStrideBytes = 8;
static constexpr uint32_t OperatorSelectorByte = 0x212;
static constexpr uint32_t SkeletonSelectorByte = 0x1FB;
static constexpr uint32_t MatrixInlineMarkerU16 = 0x6E;
static constexpr uint32_t MatrixInlineField = 0x30; // marker != 0
static constexpr uint32_t MatrixExternalEncodedField = 0x30; // marker == 0
}
namespace OperatorComponent {
static constexpr uint32_t ParentOwnerEncodedField = 0x18;
}
namespace SkeletonComponent {
static constexpr uint32_t ResourceCandidateA = 0x128;
static constexpr uint32_t ResourceCandidateB = 0x168;
}
namespace SkeletonResource {
static constexpr uint32_t JointHashIndexMap = 0x50;
static constexpr uint32_t JointMapCountDescriptor = 0x58;
static constexpr size_t JointMapStrideBytes = 8;
static constexpr uint32_t Topology = 0x70;
static constexpr uint32_t TopologyCountDescriptor = 0x78;
static constexpr size_t TopologyStrideBytes = 28;
}
namespace PosePalette {
// Model space. Resolve named joints through EACH resource's hash/index map,
// then transform model positions with the current owner's matrix for world space.
static constexpr size_t RecordStrideBytes = 32;
static constexpr uint32_t PositionFloat3 = 0x00;
static constexpr uint32_t QuaternionFloat4 = 0x10;
}
namespace CameraObject {
// The object and output state are distinct objects. Slots are byte offsets.
static constexpr uint32_t GetterVTableSlot = 0x90;
static constexpr uint32_t GetterCodeRVA = 0x02926A80;
static constexpr uint32_t GetterReturnedSubobject = 0x190;
}
namespace CameraOutputState {
// These encoded fields require decoding before use as matrices/diagnostics.
static constexpr uint32_t RigidMatrixEncoded = 0x50;
static constexpr uint32_t ViewMatrixEncoded = 0x1F0;
static constexpr uint32_t ProjectionMatrixEncoded = 0x10;
static constexpr uint32_t ViewProjectionEncoded = 0x150;
static constexpr uint32_t ViewportFloat4XYWH = 0x310;
static constexpr uint32_t DiagnosticPositionEncoded = 0x190;
static constexpr uint32_t DiagnosticQuaternionEncodedQwordA = 0x280;
static constexpr uint32_t DiagnosticQuaternionEncodedQwordB = 0x288;
}

struct CodeEntry {
    const char* name;
    uint32_t rva;
    const char* signature;
};
// Match at the listed entry, not at an arbitrary hit elsewhere in the module.
static constexpr CodeEntry CodeEntries[] = {
    {"local_controller_consumer", 0x0EB3ED50,
     "41 55 56 41 54 41 57 57 41 56 53 55 48 81 EC 88 00 00 00 4C 89 CB 48 89 D6"},
    {"controller_owner_accessor", 0x00678B90,
     "41 55 41 54 53 41 56 41 57 57 56 55 48 83 EC 10 48 89 54 24 08 49 89 C8"},
    {"controller_camera_link_consumer", 0x08332880,
     "57 41 56 56 55 41 55 41 54 41 57 53 48 83 EC 48 44 0F 29 7C 24 30 44 0F 29 54 24 20"},
    {"camera_output_manager_consumer", 0x006E24E0,
     "41 57 57 41 55 41 54 41 56 55 53 56 48 81 EC 98 00 00 00 44 0F 29 A4 24 80 00 00 00"},
    {"owner_transform_consumer", 0x01CDA6F0,
     "53 55 57 56 41 57 41 54 41 56 41 55 48 83 EC 78 44 89 C7 49 89 CF 45 31 C0"},
    {"operator_selector_consumer", 0x01D65DA0,
     "55 48 83 EC 30 48 89 CD 0F B6 82 12 02 00 00 48 3D FF 00 00 00 0F 84 ?? ?? ?? ??"},
    {"pose_updater", 0x003BB660,
     "57 56 41 55 55 41 57 53 41 54 41 56 48 81 EC 28 02 00 00 44 0F 29 B4 24 00 02 00 00"},
    {"pose_parent_helper", 0x003D6830,
     "56 41 55 41 54 55 53 57 41 57 41 56 48 81 EC 08 01 00 00 0F 29 7C 24 70"},
    {"camera_pose_producer", 0x00E017E0,
     "53 41 55 41 56 55 57 56 41 54 41 57 48 83 EC 18 49 89 C9 F3 0F 10 84 24 80 00 00 00"},
    {"view_projection_producer", 0x00D27CF0,
     "41 56 56 41 55 53 55 41 54 41 57 57 48 81 EC 28 02 00 00 0F 29 B4 24 80 01 00 00"},
    {"operator_state_getter_not_controller", 0x07B147A0,
     "57 44 8B 15 ?? ?? ?? ?? 65 48 8B 14 25 58 00 00 00 4A 8B 14 D2 48 8B 92 50 FC 00 00 48 89 F8"},
    {"camera_rigid_getter_leaf", CameraObject::GetterCodeRVA,
     "48 8D 81 90 01 00 00 C3"}
};

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Read(address, destination, byteCount) returns true only on a complete read.
// A match checks entry bytes only; it does NOT verify the executable SHA-256.
template <typename Read>
bool ValidateCodeEntry(const CodeEntry& entry, uint64_t moduleBase,
    uint64_t moduleSize, Read read) {
    if (!moduleBase || !entry.signature) return false;
    const size_t length = std::strlen(entry.signature);
    if (!length || length % 3 != 2) return false;
    const size_t count = length / 3 + 1;
    uint8_t bytes[64]{};
    if (count > sizeof(bytes) || entry.rva >= moduleSize ||
        count > moduleSize - entry.rva || moduleSize > UINT64_MAX - moduleBase)
        return false;
    if (!read(moduleBase + entry.rva, bytes, count)) return false;
    for (size_t i = 0; i < count; ++i) {
        const char* token = entry.signature + i * 3;
        if (i + 1 < count && token[2] != ' ') return false;
        if (token[0] == '?' && token[1] == '?') continue;
        const int hi = HexDigit(token[0]), lo = HexDigit(token[1]);
        if (hi < 0 || lo < 0 || bytes[i] != (hi * 16 + lo)) return false;
    }
    return true;
}
}

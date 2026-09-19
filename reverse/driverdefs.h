#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winioctl.h>
#include <cstdint>
#include <TlHelp32.h>
#include <d3d9.h>
#include <dwmapi.h>
#include <xmmintrin.h>
#include <mutex>
#include <iostream>

#pragma comment(lib, "dwmapi.lib")

#ifndef _NTDEF_CUSTOM_
#define _NTDEF_CUSTOM_
typedef struct _NT_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} NT_UNICODE_STRING, *PNT_UNICODE_STRING;
#endif

typedef long NT_STATUS_T;

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NT_STATUS_T)(s)) >= 0)
#endif

typedef struct _NT_IO_STATUS_BLOCK {
    NT_STATUS_T Status;
    ULONG_PTR   Information;
} NT_IO_STATUS_BLOCK, *PNT_IO_STATUS_BLOCK;

typedef struct _NT_OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PNT_UNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} NT_OBJECT_ATTRIBUTES, *PNT_OBJECT_ATTRIBUTES;

#define NT_OBJ_CASE_INSENSITIVE         0x00000040UL
#define NT_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020UL
#define NT_FILE_NON_DIRECTORY_FILE      0x00000040UL
#define NT_SYNCHRONIZE                  0x00100000UL
#define NT_GENERIC_READ                 0x80000000UL
#define NT_GENERIC_WRITE                0x40000000UL
#define NT_FILE_SHARE_READ              0x00000001UL
#define NT_FILE_SHARE_WRITE             0x00000002UL
#define NT_FILE_OPEN                    0x00000001UL

typedef NT_STATUS_T (__stdcall *pfnNtCreateFile)(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PNT_OBJECT_ATTRIBUTES ObjectAttributes, PNT_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
    ULONG ShareAccess, ULONG CreateDisposition,
    ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);

typedef NT_STATUS_T (__stdcall *pfnNtDeviceIoControlFile)(
    HANDLE FileHandle, HANDLE Event,
    PVOID ApcRoutine, PVOID ApcContext,
    PNT_IO_STATUS_BLOCK IoStatusBlock,
    ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength);

typedef NT_STATUS_T (__stdcall *pfnNtClose)(HANDLE Handle);
typedef void        (__stdcall *pfnRtlInitUnicodeString)(PNT_UNICODE_STRING, PCWSTR);

struct NtApi {
    pfnNtCreateFile            fnNtCreateFile          = nullptr;
    pfnNtDeviceIoControlFile   fnNtDeviceIoControlFile = nullptr;
    pfnNtClose                 fnNtClose               = nullptr;
    pfnRtlInitUnicodeString    fnRtlInitUnicodeString  = nullptr;

    bool Init() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        fnNtCreateFile          = (pfnNtCreateFile)         GetProcAddress(ntdll, "NtCreateFile");
        fnNtDeviceIoControlFile = (pfnNtDeviceIoControlFile)GetProcAddress(ntdll, "NtDeviceIoControlFile");
        fnNtClose               = (pfnNtClose)              GetProcAddress(ntdll, "NtClose");
        fnRtlInitUnicodeString  = (pfnRtlInitUnicodeString) GetProcAddress(ntdll, "RtlInitUnicodeString");
        return fnNtCreateFile && fnNtDeviceIoControlFile && fnNtClose && fnRtlInitUnicodeString;
    }

    HANDLE OpenDevice(const wchar_t* ntPath) {
        NT_UNICODE_STRING uStr{};
        fnRtlInitUnicodeString(&uStr, ntPath);
        NT_OBJECT_ATTRIBUTES oa{};
        oa.Length     = sizeof(oa);
        oa.ObjectName = (PNT_UNICODE_STRING)&uStr;
        oa.Attributes = NT_OBJ_CASE_INSENSITIVE;
        NT_IO_STATUS_BLOCK iosb{};
        HANDLE hFile = INVALID_HANDLE_VALUE;
        NT_STATUS_T st = fnNtCreateFile(
            &hFile,
            NT_GENERIC_READ | NT_GENERIC_WRITE | NT_SYNCHRONIZE,
            &oa, &iosb, nullptr, 0,
            NT_FILE_SHARE_READ | NT_FILE_SHARE_WRITE,
            NT_FILE_OPEN,
            NT_FILE_SYNCHRONOUS_IO_NONALERT | NT_FILE_NON_DIRECTORY_FILE,
            nullptr, 0);
        return NT_SUCCESS(st) ? hFile : INVALID_HANDLE_VALUE;
    }

    bool Ioctl(HANDLE hDev, ULONG code,
               void* inBuf,  ULONG inLen,
               void* outBuf, ULONG outLen,
               ULONG*        bytesReturned = nullptr,
               NT_STATUS_T*  ntStatus      = nullptr)
    {
        NT_IO_STATUS_BLOCK iosb{};
        NT_STATUS_T st = fnNtDeviceIoControlFile(
            hDev, nullptr, nullptr, nullptr,
            &iosb, code,
            inBuf, inLen,
            outBuf, outLen);
        if (bytesReturned) *bytesReturned = (ULONG)iosb.Information;
        if (ntStatus)      *ntStatus      = st;
        return NT_SUCCESS(st);
    }
};

inline NtApi& GetNtApi() {
    static NtApi inst;
    static bool  inited = inst.Init();
    (void)inited;
    return inst;
}

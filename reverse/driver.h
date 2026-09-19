#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <iostream>
#include <functional>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "driverdefs.h"

#define IOCTL_BASE              0x8000
#define CTL(code)               CTL_CODE(IOCTL_BASE, code, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_READ_MEMORY       CTL(0x800)
#define IOCTL_WRITE_MEMORY      CTL(0x801)
#define IOCTL_GET_BASE_ADDRESS  CTL(0x802)
#define IOCTL_GET_PEB           CTL(0x803)
#define IOCTL_ALLOC_MEMORY      CTL(0x804)
#define IOCTL_FREE_MEMORY       CTL(0x805)
#define IOCTL_PROTECT_MEMORY    CTL(0x806)
#define IOCTL_GET_MODULE_BASE   CTL(0x807)
#define IOCTL_QUERY_PROCESS     CTL(0x808)
#define IOCTL_READ_PHYS         CTL(0x809)
#define IOCTL_WRITE_PHYS        CTL(0x80A)
#define IOCTL_QUEUE_USER_APC          CTL(0x80B)
#define IOCTL_GET_THREAD              CTL(0x80C)
#define IOCTL_CREATE_REMOTE_THREAD    CTL(0x80D)
#define IOCTL_HIJACK_THREAD           CTL(0x80E)
#define IOCTL_SPRAY_USER_APC          CTL(0x80F)

#pragma pack(push, 1)
struct KReadRequest    { uint32_t ProcessId; uint64_t Address; uint64_t Buffer; uint64_t Size; };
struct KWriteRequest   { uint32_t ProcessId; uint64_t Address; uint64_t Buffer; uint64_t Size; };
struct KBaseRequest    { uint32_t ProcessId; wchar_t ModuleName[256]; uint64_t BaseAddress; uint64_t ModuleSize; };
struct KPebRequest     { uint32_t ProcessId; uint64_t PebAddress; };
struct KAllocRequest   { uint32_t ProcessId; uint64_t Address; uint64_t Size; uint32_t AllocationType; uint32_t Protect; };
struct KFreeRequest    { uint32_t ProcessId; uint64_t Address; uint64_t Size; uint32_t FreeType; };
struct KProtectRequest { uint32_t ProcessId; uint64_t Address; uint64_t Size; uint32_t NewProtect; uint32_t OldProtect; };
struct KQueryProcessRequest { uint32_t ProcessId; uint64_t Wow64; uint64_t PebAddress; uint64_t MainModuleBase; wchar_t ImagePath[512]; };
struct KPhysRequest    { uint64_t PhysicalAddress; uint64_t Buffer; uint64_t Size; };
struct KApcRequest     { uint32_t ProcessId; uint32_t ThreadId; uint64_t Routine; uint64_t Argument; };
struct KGetThreadRequest { uint32_t ProcessId; uint32_t ThreadId; };
struct KCreateThreadRequest { uint32_t ProcessId; uint64_t StartAddress; uint64_t Argument; uint32_t ThreadId; };
struct KHijackThreadRequest { uint32_t ProcessId; uint32_t ThreadId; uint64_t TrampolineVA; uint64_t OrigRipOffset; uint64_t OutOriginalRip; };
#pragma pack(pop)

static std::mutex g_driverMtx;

class Driver
{
public:
    UINT ProcessId;

    Driver() : ProcessId(0), m_handle(INVALID_HANDLE_VALUE), m_lastError(0) {}
    ~Driver() { Close(); }

    const bool Init(const BOOL PhysicalMode = FALSE) {
        (void)PhysicalMode;
        printf("[DRV] Init() called, PhysicalMode=%d\n", PhysicalMode);

        auto& nt = GetNtApi();
        printf("[DRV] NtApi initialized: NtCreateFile=%p, NtDeviceIoControlFile=%p\n",
            (void*)nt.fnNtCreateFile, (void*)nt.fnNtDeviceIoControlFile);

        printf("[DRV] Trying NtCreateFile on \\\\Device\\\\WdmDiagSvc...\n");
        m_handle = nt.OpenDevice(L"\\Device\\WdmDiagSvc");
        printf("[DRV] NtCreateFile result: handle=0x%p\n", m_handle);

        if (m_handle == INVALID_HANDLE_VALUE) {
            printf("[DRV] NtCreateFile failed, trying CreateFileW on \\\\.\\\\WdmDiagSvc...\n");
            m_handle = CreateFileW(
                L"\\\\.\\WdmDiagSvc",
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            printf("[DRV] CreateFileW result: handle=0x%p (err=%lu)\n", m_handle, GetLastError());
        }

        bool success = m_handle != INVALID_HANDLE_VALUE;
        printf("[DRV] Init() %s\n", success ? "SUCCESS" : "FAILED");
        return success;
    }

    void Close() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            GetNtApi().fnNtClose(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }
    DWORD LastError() const { return m_lastError; }

    const bool Attach(DWORD ProcessID) {
        this->ProcessId = ProcessID;
        return true;
    }

    NTSTATUS ReadProcessMemory(uint64_t src, void* dest, uint32_t size) {
        std::scoped_lock lock(g_driverMtx);
        if (m_handle == INVALID_HANDLE_VALUE) return -1;
        KReadRequest req{};
        req.ProcessId = ProcessId;
        req.Address   = src;
        req.Size      = size;
        ULONG ret = 0;
        bool ok = Ioctl(IOCTL_READ_MEMORY, &req, (ULONG)sizeof(req), dest, (ULONG)size, &ret);
        m_lastError = ok ? 0 : GetLastError();
        static int readCount = 0;
        static int failCount = 0;
        readCount++;
        if (!ok) failCount++;
        if (readCount <= 5 || (failCount > 0 && readCount <= 20)) {
            printf("[DRV] ReadMemory(PID=%u, addr=0x%llX, sz=%u) = %s (ret=%lu)\n",
                ProcessId, (unsigned long long)src, size, ok ? "OK" : "FAIL", ret);
        }
        return ok ? 0 : -1;
    }

    NTSTATUS WriteProcessMemory(PVOID src, PVOID dest, DWORD size) {
        std::scoped_lock lock(g_driverMtx);
        size_t total = sizeof(KWriteRequest) + size;
        auto* buf = static_cast<uint8_t*>(malloc(total));
        if (!buf) return -1;
        KWriteRequest hdr{};
        hdr.ProcessId = ProcessId;
        hdr.Address   = (uint64_t)dest;
        hdr.Size      = size;
        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), src, size);
        bool ok = Ioctl(IOCTL_WRITE_MEMORY, buf, (ULONG)total, nullptr, 0);
        m_lastError = ok ? 0 : GetLastError();
        free(buf);
        return ok ? 0 : -1;
    }

    const uint64_t GetModuleBase(const wchar_t* ModuleName = L"", uint64_t* outModSize = nullptr) {
        std::scoped_lock lock(g_driverMtx);
        if (m_handle == INVALID_HANDLE_VALUE) {
            printf("[DRV] GetModuleBase called but driver not open!\n");
            return 0;
        }
        KBaseRequest req{};
        req.ProcessId = ProcessId;
        if (ModuleName) {
            wcsncpy_s(req.ModuleName, ModuleName, 255);
        }
        ULONG ret = 0;
        bool ok = Ioctl(IOCTL_GET_BASE_ADDRESS, &req, sizeof(req), &req, sizeof(req), &ret);
        m_lastError = ok ? 0 : GetLastError();
        if (outModSize) *outModSize = req.ModuleSize;
        printf("[DRV] GetModuleBase(PID=%u, module='%ls') = %s, base=0x%llX, size=0x%llX (ioctl ret=%lu, err=%lu)\n",
            ProcessId, ModuleName ? ModuleName : L"(null)",
            ok ? "OK" : "FAIL",
            (unsigned long long)req.BaseAddress,
            (unsigned long long)req.ModuleSize,
            ret, m_lastError);
        return ok ? req.BaseAddress : 0;
    }

    uint64_t GetPeb() {
        std::scoped_lock lock(g_driverMtx);
        KPebRequest req{};
        req.ProcessId = ProcessId;
        ULONG ret = 0;
        bool ok = Ioctl(IOCTL_GET_PEB, &req, sizeof(req), &req, sizeof(req), &ret);
        m_lastError = ok ? 0 : GetLastError();
        return ok ? req.PebAddress : 0;
    }

    uint64_t AllocMemory(size_t size,
                         uint32_t allocType = MEM_COMMIT | MEM_RESERVE,
                         uint32_t protect   = PAGE_EXECUTE_READWRITE,
                         uint64_t base      = 0) {
        std::scoped_lock lock(g_driverMtx);
        KAllocRequest req{};
        req.ProcessId      = ProcessId;
        req.Address        = base;
        req.Size           = size;
        req.AllocationType = allocType;
        req.Protect        = protect;
        ULONG ret = 0;
        bool ok = Ioctl(IOCTL_ALLOC_MEMORY, &req, sizeof(req), &req, sizeof(req), &ret);
        m_lastError = ok ? 0 : GetLastError();
        return ok ? req.Address : 0;
    }

    bool FreeMemory(uint64_t address, size_t size = 0, uint32_t freeType = MEM_RELEASE) {
        std::scoped_lock lock(g_driverMtx);
        KFreeRequest req{};
        req.ProcessId = ProcessId;
        req.Address   = address;
        req.Size      = size;
        req.FreeType  = freeType;
        bool ok = Ioctl(IOCTL_FREE_MEMORY, &req, sizeof(req), nullptr, 0);
        m_lastError = ok ? 0 : GetLastError();
        return ok;
    }

    bool ProtectMemory(uint64_t address, size_t size,
                       uint32_t newProtect, uint32_t* oldProtect = nullptr) {
        std::scoped_lock lock(g_driverMtx);
        KProtectRequest req{};
        req.ProcessId  = ProcessId;
        req.Address    = address;
        req.Size       = size;
        req.NewProtect = newProtect;
        ULONG ret = 0;
        bool ok = Ioctl(IOCTL_PROTECT_MEMORY, &req, sizeof(req), &req, sizeof(req), &ret);
        m_lastError = ok ? 0 : GetLastError();
        if (oldProtect) *oldProtect = req.OldProtect;
        return ok;
    }

    bool QueryProcess(KQueryProcessRequest& out) {
        std::scoped_lock lock(g_driverMtx);
        out.ProcessId = ProcessId;
        ULONG ret = 0;
        bool ok = Ioctl(IOCTL_QUERY_PROCESS, &out, sizeof(out), &out, sizeof(out), &ret);
        m_lastError = ok ? 0 : GetLastError();
        return ok;
    }

    const UINT GetProcessThreadNumByID(DWORD dwPID)
    {
        HANDLE hProcessSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hProcessSnap == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32 pe32 = { 0 };
        pe32.dwSize = sizeof(pe32);
        BOOL bRet = ::Process32First(hProcessSnap, &pe32);
        while (bRet)
        {
            if (pe32.th32ProcessID == dwPID)
            {
                ::CloseHandle(hProcessSnap);
                return pe32.cntThreads;
            }
            bRet = ::Process32Next(hProcessSnap, &pe32);
        }
        ::CloseHandle(hProcessSnap);
        return 0;
    }

    HANDLE Handle() const { return m_handle; }

private:
    HANDLE m_handle;
    DWORD  m_lastError;

    bool Ioctl(ULONG code,
               void* inBuf, ULONG inLen,
               void* outBuf, ULONG outLen,
               ULONG* ret = nullptr)
    {
        return GetNtApi().Ioctl(m_handle, code, inBuf, inLen, outBuf, outLen, ret);
    }
};

Driver* driver = new Driver;

template <typename T>
T read(const uintptr_t address)
{
    T buffer{};
    driver->ReadProcessMemory(address, &buffer, sizeof(T));
    return buffer;
}

template <typename T>
T write(const uintptr_t address, T buffer)
{
    driver->WriteProcessMemory((PVOID)&buffer, (PVOID)address, sizeof(T));
    return buffer;
}

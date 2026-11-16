#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <openvr_driver.h>

#include "cave_bytes.h"

namespace {
    FILE* g_log;
    std::mutex g_log_lock;

    void Log(const char* fmt, ...) {
        char b[700];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lk(g_log_lock);

        if (!g_log) {
            char dir[MAX_PATH];

            if (GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof dir)) {
                std::string d = std::string(dir) + "\\FrameDragFix";
                CreateDirectoryA(d.c_str(), nullptr);
                g_log = fopen((d + "\\framedragfix.log").c_str(), "a");
            }
        }
        if (g_log) {
            SYSTEMTIME t;
            GetLocalTime(&t);

            fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", t.wYear, t.wMonth,
                t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, b);
            fflush(g_log);
        }

        if (vr::VRDriverLog())
            vr::VRDriverLog()->Log((std::string("framedragfix: ") + b).c_str());
    }

    struct Pattern {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> mask;
        explicit Pattern(const char* s) {
            while (*s) {
                while (*s == ' ')
                    s++;

                if (!*s)
                    break;

                if (*s == '?') {
                    bytes.push_back(0);
                    mask.push_back(0);
                }
                else {
                    bytes.push_back(
                        (uint8_t)strtoul(std::string(s, 2).c_str(), nullptr, 16));
                    mask.push_back(1);
                }

                s += 2;
            }
        }

        // offset of the only match, -1 for none, -2 for more than 1
        ptrdiff_t Find(const std::vector<uint8_t>& data) const {
            ptrdiff_t at_offset = -1;
            size_t n = bytes.size();
            for (size_t i = 0; i + n <= data.size(); i++) {
                if (data[i] != bytes[0])
                    continue;

                size_t k = 1;
                while (k < n && (!mask[k] || data[i + k] == bytes[k]))
                    k++;

                if (k < n)
                    continue;
                if (at_offset >= 0)
                    return -2;

                at_offset = (ptrdiff_t)i;
            }
            return at_offset;
        }
    };

    uint32_t U32(const std::vector<uint8_t>& data, size_t at_offset) {
        uint32_t v;
        memcpy(&v, &data[at_offset], 4);
        return v;
    }

    int32_t I32(const std::vector<uint8_t>& data, size_t at_offset) {
        int32_t v;
        memcpy(&v, &data[at_offset], 4);
        return v;
    }

    // CSceneGraphRenderer's visit of CRemoteTransform from the call that gets the
    // headset's matrix (to [rsp+20h]):
    //   call [rbx+30h] / mov rsi,[rsp+68h] / mov rbx,[rsp+60h] / test al,al / jz
    //   fail movups xmm0,[rsp+20h] / movups xmm1,[rsp+30h] / movups [rdi+CUR],xmm0
    //   / movups xmm0,[rsp+40h] movups [rdi+CUR+10h],xmm1 / movups
    //   [rdi+CUR+20h],xmm0 / add rsp,50h / pop rdi / ret
    const Pattern kRemote("ff 53 30 48 8b 74 24 68 48 8b 5c 24 60 84 c0 74 ?? 0f "
        "10 44 24 20 0f 10 4c 24 30 0f 11 87 ?? ?? ?? ?? "
        "0f 10 44 24 40 0f 11 8f ?? ?? ?? ?? 0f 11 87 ?? ?? ?? "
        "?? 48 83 c4 50 5f c3");
    const size_t kRemoteSite = 17;
    const size_t kRemoteCur = 30;
    const size_t kRemoteCur1 = 42;
    const size_t kRemoteCur2 = 49;

    // the visit of CTransform where a transform's parent is the raw tracking space
    // (parent-origin Raw):
    //   cmp ecx,ebx / jnz .. / movups xmm0,[rdi+RAW] / movups [rdi+CUR],xmm0 /
    //   movups xmm1,[rdi+RAW+10h] movups xmm0,[rdi+RAW+20h] / jmp ..
    const Pattern kRaw("3b cb 0f 85 ?? ?? ?? ?? 0f 10 87 ?? ?? ?? ?? 0f 11 87 ?? "
        "?? ?? ?? 0f 10 8f ?? ?? ?? ?? 0f 10 87 ?? ?? ?? ?? eb");
    const size_t kRawRaw = 11;
    const size_t kRawCur = 18;
    const size_t kRawRaw1 = 25;
    const size_t kRawRaw2 = 32;

    // filling a VRLink_LaserMouseOverlay from an overlay record (r14):
    // setup of the loop over the record's 12 transforms & first instruction:
    //   xor r15d,r15d / mov ecx,[rbx+38h] / lea r13,TABLE / lea rbp,[r15-1] / nops
    //   / movss xmm6,[r14+r15*4+18h]
    const Pattern kLaser("45 33 ff 8b 4b 38 4c 8d 2d ?? ?? ?? ?? 49 8d 6f ff 0f 1f "
        "40 00 66 0f 1f 84 00 00 00 00 00 "
        "f3 43 0f 10 74 be 18");
    const size_t kLaserTable = 9;
    const size_t kLaserLoop = 30;

    const size_t kStub1 = 0x000;
    const size_t kStub2 = 0x040;
    const size_t kCode = 0x100;
    const size_t kData = 0x800;
    const size_t kCaveSize = 0x1000; // cave johnson here, my cave's HUGE! The lab boys sure did a good job at this!

    struct CaveData {
        int have;
        float headset_standing[12];
    };

    void Put(std::vector<uint8_t>& vec, std::initializer_list<uint8_t> b) {
        vec.insert(vec.end(), b);
    }
    void Put32(std::vector<uint8_t>& vec, int64_t x) {
        int32_t y = (int32_t)x;
        vec.insert(vec.end(), (uint8_t*)&y, (uint8_t*)&y + 4);
    }

    struct Compositor {
        DWORD pid = 0;
        HANDLE hdl = nullptr;
        uint64_t cave = 0;
        bool gave_up = false;
        CaveData sent{};
        bool sent_any = false;

        void Close() {
            if (hdl)
                CloseHandle(hdl);
            *this = Compositor();
        }
    };

    DWORD FindCompositor() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32W entry{ sizeof entry };
        DWORD pid = 0;
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok;
            ok = Process32NextW(snapshot, &entry))
            if (!_wcsicmp(entry.szExeFile, L"vrcompositor.exe")) {
                pid = entry.th32ProcessID;
                break;
            }

        CloseHandle(snapshot);
        return pid;
    }

    bool Freeze(DWORD pid, std::vector<HANDLE>* threads, const uint64_t ranges[][2],
        int n) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        THREADENTRY32 entry{ sizeof entry };
        bool clear = true;
        for (BOOL ok = Thread32First(snapshot, &entry); ok;
            ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid)
                continue;

            HANDLE threadHdl = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                FALSE, entry.th32ThreadID);
            if (!threadHdl)
                continue;

            if (SuspendThread(threadHdl) == (DWORD)-1) {
                CloseHandle(threadHdl);
                continue;
            }
            threads->push_back(threadHdl);

            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(threadHdl, &ctx))
                for (int i = 0; i < n; i++)
                    if (ctx.Rip > ranges[i][0] && ctx.Rip < ranges[i][1])
                        clear = false;
        }
        CloseHandle(snapshot);
        return clear;
    }

    void Unfreeze(std::vector<HANDLE>* threads) {
        for (HANDLE threadHdl : *threads) {
            ResumeThread(threadHdl);
            CloseHandle(threadHdl);
        }

        threads->clear();
    }

    bool Write(HANDLE hdl, uint64_t at_offset, const void* p, size_t n) {
        DWORD old;
        if (!VirtualProtectEx(hdl, (void*)at_offset, n, PAGE_EXECUTE_READWRITE,
            &old))
            return false;

        SIZE_T done = 0;
        bool ok =
            WriteProcessMemory(hdl, (void*)at_offset, p, n, &done) && done == n;
        VirtualProtectEx(hdl, (void*)at_offset, n, old, &old);
        FlushInstructionCache(hdl, (void*)at_offset, n);

        return ok;
    }

    bool Patch(Compositor* comp) {
        HMODULE mod = NULL;
        DWORD need = 0;
        if (!EnumProcessModulesEx(comp->hdl, &mod, sizeof mod, &need, LIST_MODULES_64BIT))
            return false; // starting

        uint64_t base = (uint64_t)mod;
        uint8_t hdr[0x1000];
        SIZE_T read = 0;
        if (!ReadProcessMemory(comp->hdl, (void*)base, hdr, sizeof hdr, &read) || read != sizeof hdr)
            return false;

        uint32_t pe = *(uint32_t*)(hdr + 0x3c);
        if (pe > sizeof hdr - 0x200 || memcmp(hdr + pe, "PE\0\0", 4)) {
            Log("no PE header in compositor");
            return true;
        }
        uint16_t opt = *(uint16_t*)(hdr + pe + 20);
        uint32_t image_size = *(uint32_t*)(hdr + pe + 24 + 56);
        const uint8_t* sec = hdr + pe + 24 + opt;
        uint32_t text_size = *(uint32_t*)(sec + 8),
            text_rva = *(uint32_t*)(sec + 12);
        uint64_t text = base + text_rva;
        std::vector<uint8_t> code(text_size);
        if (!ReadProcessMemory(comp->hdl, (void*)text, code.data(), code.size(),
            &read) ||
            read != code.size())
            return false;

        ptrdiff_t remote = kRemote.Find(code), raw = kRaw.Find(code),
            laser = kLaser.Find(code);
        if (remote < 0 || raw < 0 || laser < 0) {
            Log("could not find matching code to patch in vrcompositor.exe (remote "
                "%s, raw %s, laser %s): bailing",
                remote >= 0 ? "found"
                : remote == -1 ? "missing"
                : "unknown",
                raw >= 0 ? "found"
                : raw == -1 ? "missing"
                : "unknown",
                laser >= 0 ? "found"
                : laser == -1 ? "missing"
                : "unknown");
            return true;
        }
        uint32_t cur = U32(code, remote + kRemoteCur);
        uint32_t raw_off = U32(code, raw + kRawRaw);
        if (U32(code, remote + kRemoteCur1) != cur + 0x10 ||
            U32(code, remote + kRemoteCur2) != cur + 0x20 ||
            U32(code, raw + kRawCur) != cur ||
            U32(code, raw + kRawRaw1) != raw_off + 0x10 ||
            U32(code, raw + kRawRaw2) != raw_off + 0x20) {
            Log("unexpected matrix layout: bailing");
            return true;
        }
        uint64_t remote_at = text + remote + kRemoteSite, laser_at = text + laser,
            laser_loop = laser_at + kLaserLoop;
        uint64_t table = laser_at + kLaserTable + 4 + I32(code, laser + kLaserTable);

        uint64_t cave = 0;
        for (uint64_t a = (base + image_size + 0xffff) & ~0xffffull;
            a < base + 0x70000000ull && !cave; a += 0x10000)
            cave = (uint64_t)VirtualAllocEx(comp->hdl, (void*)a, kCaveSize,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE);
        if (!cave) {
            Log("no memory found to write patch in (error %lu)", GetLastError());
            return true;
        }

        std::vector<uint8_t> page(kCaveSize, 0xcc);
        std::vector<uint8_t> s;
        if (sizeof kCaveCode > kData - kCode) {
            Log("cave code too large");
            return true;
        }
        memcpy(&page[kCode], kCaveCode, sizeof kCaveCode);
        memset(&page[kData], 0, kCaveSize - kData);
        
        // clang-format off
        // 1: RemoteVisit(cave, current, headset matrix, raw origin)
        Put(s, { 0x48, 0x8d, 0x0d }); Put32(s, (int64_t)(kData - (kStub1 + s.size() + 4)));             // lea rcx, data
        Put(s, { 0x48, 0x8d, 0x97 }); Put32(s, cur);                                                    // lea rdx, [rdi+CUR]
        Put(s, { 0x4c, 0x8d, 0x44, 0x24, 0x20 });                                                       // lea r8, [rsp+20h]
        Put(s, { 0x4c, 0x8d, 0x8f }); Put32(s, raw_off);                                                // lea r9, [rdi+RAW]
        Put(s, { 0x48, 0x83, 0xec, 0x20 });                                                             // sub rsp, 20h
        Put(s, { 0xe8 }); Put32(s, (int64_t)(kCode + kRemoteVisit) - (int64_t)(kStub1 + s.size() + 4)); // call RemoteVisit
        Put(s, { 0x48, 0x83, 0xc4, 0x70, 0x5f, 0xc3 });                                                 // add rsp, 70h / pop rdi / ret
        memcpy(&page[kStub1], s.data(), s.size());
        s.clear();

        // 2: r14 = LaserRecord(cave, r14) & loop setup
        Put(s, { 0x48, 0x8d, 0x0d }); Put32(s, (int64_t)(kData - (kStub2 + s.size() + 4)));                 // lea rcx, data
        Put(s, { 0x4c, 0x89, 0xf2 });                                                                       // mov rdx, r14
        Put(s, { 0x48, 0x83, 0xec, 0x20 });                                                                 // sub rsp, 20h
        Put(s, { 0xe8 }); Put32(s, (int64_t)(kCode + kLaserRecord) - (int64_t)(kStub2 + s.size() + 4));     // call LaserRecord
        Put(s, { 0x48, 0x83, 0xc4, 0x20 });                                                                 // add rsp, 20h
        Put(s, { 0x49, 0x89, 0xc6 });                                                                       // mov r14, rax
        Put(s, { 0x45, 0x33, 0xff });                                                                       // xor r15d, r15d
        Put(s, { 0x8b, 0x4b, 0x38 });                                                                       // mov ecx, [rbx+38h]
        Put(s, { 0x4c, 0x8d, 0x2d }); Put32(s, (int64_t)table - (int64_t)(cave + kStub2 + s.size() + 4));   // lea r13, table
        Put(s, { 0x49, 0x8d, 0x6f, 0xff });                                                                 // lea rbp, [r15-1]
        Put(s, { 0xe9 }); Put32(s, (int64_t)laser_loop - (int64_t)(cave + kStub2 + s.size() + 4));          // jmp loop
        // clang-format on
        memcpy(&page[kStub2], s.data(), s.size());
        if (!WriteProcessMemory(comp->hdl, (void*)cave, page.data(), page.size(),
            &read) ||
            read != page.size()) {
            Log("could not write into the compositor (error %lu)", GetLastError());
            return true;
        }
        FlushInstructionCache(comp->hdl, (void*)cave, kCaveSize);

        // two jumps
        uint8_t j1[5] = { 0xe9 }, j2[5] = { 0xe9 };
        int32_t r1 = (int32_t)((int64_t)(cave + kStub1) - (int64_t)(remote_at + 5));
        int32_t r2 = (int32_t)((int64_t)(cave + kStub2) - (int64_t)(laser_at + 5));
        memcpy(j1 + 1, &r1, 4);
        memcpy(j2 + 1, &r2, 4);
        const uint64_t ranges[2][2] = { {remote_at, remote_at + 5},
                                       {laser_at, laser_at + 5} };
        bool ok = false;
        for (int attempt = 0; attempt < 50 && !ok; attempt++) {
            std::vector<HANDLE> threads;
            if (Freeze(comp->pid, &threads, ranges, 2)) {
                ok = Write(comp->hdl, laser_at, j2, 5) &&
                    Write(comp->hdl, remote_at, j1, 5);
                Unfreeze(&threads);
                if (!ok) {
                    Log("could not change the compositor's code (error %lu)",
                        GetLastError());
                    return true;
                }
            }
            else {
                Unfreeze(&threads);
                Sleep(2);
            }
        }
        if (!ok) {
            Log("failed to freeze compositor threads after 50 attempts: bailing");
            return true;
        }
        comp->cave = cave;
        Log("compositor %lu changed: remote-transform visit at +%llx, laser records "
            "at +%llx, cave at %llx",
            comp->pid, (unsigned long long)(remote_at - base),
            (unsigned long long)(laser_at - base), (unsigned long long)cave);
        return true;
    }

    bool HeadsetStanding(const std::string& json, float m[12]) {
        // ghetto json parsing is ghetto
        size_t at = json.find("\"standing\"");
        if (at == std::string::npos)
            return false;

        size_t end = json.find('}', at), tr = json.find("\"translation\"", at),
            yw = json.find("\"yaw\"", at);
        if (end == std::string::npos || tr > end || yw > end)
            return false;

        double x, y, z, a;
        size_t b = json.find('[', tr), colon = json.find(':', yw);
        if (b > end || colon > end)
            return false;

        if (sscanf(json.c_str() + b, "[ %lf , %lf , %lf", &x, &y, &z) != 3 ||
            sscanf(json.c_str() + colon + 1, "%lf", &a) != 1)
            return false;

        float c = (float)std::cos(a), s = (float)std::sin(a);
        const float r[12] = { c, 0,         s,  (float)-x, 0, 1,
                             0, (float)-y, -s, 0,         c, (float)-z };
        memcpy(m, r, sizeof r);
        return true;
    }

    class Provider : public vr::IServerTrackedDeviceProvider {
    public:
        vr::EVRInitError Init(vr::IVRDriverContext* ctx) override {
            VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
            Log("loaded");
            run_ = true;
            worker_ = std::thread([this] { Work(); });
            return vr::VRInitError_None;
        }

        void Cleanup() override {
            run_ = false;
            if (worker_.joinable())
                worker_.join();
            VR_CLEANUP_SERVER_DRIVER_CONTEXT();
        }

        const char* const* GetInterfaceVersions() override {
            return vr::k_InterfaceVersions;
        }

        void RunFrame() override {
            ULONGLONG now = GetTickCount64();
            if (now - read_at_ < 250)
                return;

            read_at_ = now;
            CaveData d{};
            auto* props = vr::VRProperties();
            vr::PropertyContainerHandle_t hmd =
                props->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd);
            vr::ETrackedPropertyError err = vr::TrackedProp_Success;
            std::string system = props->GetStringProperty(
                hmd, vr::Prop_ActualTrackingSystemName_String, &err);
            if (err == vr::TrackedProp_Success && system == "vrlink") {
                std::string json = props->GetStringProperty(
                    hmd, vr::Prop_DriverProvidedChaperoneJson_String, &err);
                if (err == vr::TrackedProp_Success &&
                    HeadsetStanding(json, d.headset_standing))
                    d.have = 1;
            }
            std::lock_guard<std::mutex> lock(lock_);
            want_ = d;
        }

        bool ShouldBlockStandbyMode() override { return false; }
        void EnterStandby() override {}
        void LeaveStandby() override {}

    private:
        std::atomic<bool> run_{ false };
        std::thread worker_;
        std::mutex lock_;
        CaveData want_{};
        ULONGLONG read_at_ = 0;

        void Work() {
            Compositor comp;
            ULONGLONG last_checked = 0;
            while (run_) {
                Sleep(50);
                ULONGLONG now = GetTickCount64();
                if (comp.hdl && WaitForSingleObject(comp.hdl, 0) == WAIT_OBJECT_0)
                    comp.Close();

                if (!comp.hdl && now - last_checked >= 500) {
                    last_checked = now;
                    DWORD pid = FindCompositor();
                    if (pid) {
                        comp.hdl = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                            PROCESS_VM_WRITE |
                            PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                            FALSE, pid);
                        comp.pid = pid;

                        if (!comp.hdl)
                            Log("could not open the compositor (error %lu)", GetLastError());
                    }
                }

                if (!comp.hdl)
                    continue;

                if (!comp.cave && !comp.gave_up && now - last_checked >= 0) {
                    if (Patch(&comp) && !comp.cave)
                        comp.gave_up = true;

                    if (!comp.cave)
                        continue;
                }

                if (!comp.cave)
                    continue;
                CaveData want;
                {
                    std::lock_guard<std::mutex> lock(lock_);
                    want = want_;
                }
                if (comp.sent_any && !memcmp(&want, &comp.sent, sizeof want))
                    continue;

                int off = 0;
                SIZE_T got;
                uint64_t data = comp.cave + kData;
                WriteProcessMemory(comp.hdl, (void*)data, &off, sizeof off, &got);
                if (want.have) {
                    WriteProcessMemory(
                        comp.hdl, (void*)(data + offsetof(CaveData, headset_standing)),
                        want.headset_standing, sizeof want.headset_standing, &got);
                    WriteProcessMemory(comp.hdl, (void*)data, &want.have, sizeof want.have,
                        &got);
                    const float* m = want.headset_standing;
                    Log("headset standing space in the raw one: at [%+.3f %+.3f %+.3f], "
                        "turned %+.2f deg",
                        m[3], m[7], m[11], std::atan2(m[2], m[10]) * 57.29577951);
                }
                else if (comp.sent_any) {
                    Log("no streaming headset's chaperone: panels placed the stock way");
                }
                comp.sent = want;
                comp.sent_any = true;
            }

            if (comp.cave) {
                int off = 0;
                SIZE_T got;
                WriteProcessMemory(comp.hdl, (void*)(comp.cave + kData), &off, sizeof off, &got);
            }
            comp.Close();
        }
    };

    Provider g_provider;

} // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name,
    int* ret) {
    if (!strcmp(name, vr::IServerTrackedDeviceProvider_Version))
        return &g_provider;

    if (ret)
        *ret = vr::VRInitError_Init_InterfaceNotFound;

    return nullptr;
}

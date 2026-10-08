// GekkoNavProxy.cpp 02/10/2026. Slot-order proxies of TPP's NavigationSystem S and Pathfinder B for the lifted Survive NavigationController2Impl, and the Path2Impl conversion.
// Facts (all from both exes, S/B vtables live):
//  S: Survive slot k = TPP slot k for k 0..5, Survive 6 has no TPP counterpart, Survive k = TPP k-1 for k 7..14. B: slots 0..5 map 1:1. Survive scope table T and TPP's agree on +0x98/+0xa0.
//  P (T+0x98): Survive Initialize calls [[P+0x110]] slot 0xe8, TPP's twin calls [[P+0x100]] slot 0x90 (a type test, 0xc382 in both: Survive 0x140a70da0, TPP 0x140a012f0); [P+8] slot 0 is the same call in both.
//  Path2Impl: points +8 (16 bytes each, both), segments +0x10 (Survive stride 0x30, flag dword +0x28; TPP stride 0x40, flag dword +0x30), count Survive +0x28 / TPP +0x20; Survive's Reserve
//  (0x141bfb080) also keeps a byte array at +0x18 (n+1) and a u16 array at +0x20 (n), TPP's (0x141cd0660) a byte array at +0x18 (n).
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <vector>
#include "GekkoNavProxy.h"
#include "lift_rt.h"

namespace NavProxy
{
    struct Path { std::uint8_t* obj = nullptr; std::uint8_t* block = nullptr; std::size_t cap = 0; };

    void SyncEntry(void* proxySelf, std::uint32_t id);   // copy of one real table entry into the Survive-layout table (state bytes and converted path)

    namespace
    {
        constexpr std::size_t kReal = 0x70;           // every proxy object keeps the real object here (Survive never reads past +0x20 of S, B or Q)
        constexpr std::uint32_t kMaxPoints = 0x4000;  // corrupt-count guard of the conversion, not behaviour: a real path is a few hundred points at most

        Config g_cfg;
        bool g_ready = false;
        void* g_sVtbl[15];
        void* g_bVtbl[6];
        void* g_qVtbl[160];   // the real SsdNpc Setup calls slot 134 (+0x430, name "Npc") on this object (0x141076b0a)
        alignas(16) std::uint8_t g_sObj[0x80], g_pObj[0x120], g_qObj[0x80];
        std::uint8_t* g_realS = nullptr;

        void Trace(const char* fmt, ...)
        {
            if (!g_cfg.trace) return;
            char buf[768]; va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); g_cfg.trace(buf);
        }

        // Runtime code stubs. thisSwap: rcx = [rcx + kReal], then jump to the real function. Otherwise: Unmapped(rcx, slot, tag), a logged call that returns nothing.
        std::uint8_t* g_stubs = nullptr; std::size_t g_stubUsed = 0;
        std::uint64_t Unmapped(void*, int slot, int tag) { Trace("[navproxy] unmapped %s slot %d called from survive code", tag == 1 ? "S" : tag == 2 ? "Q" : "B", slot); return 0; }
        void* Stub(void* target, bool thisSwap, int slot = 0, int tag = 0)
        {
            if (!g_stubs) { g_stubs = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)); if (!g_stubs) return nullptr; }
            std::uint8_t* p = g_stubs + g_stubUsed;
            std::uint8_t* s = p;
            if (thisSwap) { const std::uint8_t sw[] = { 0x48, 0x8B, 0x49, static_cast<std::uint8_t>(kReal) }; std::memcpy(s, sw, 4); s += 4; }   // mov rcx,[rcx+0x70]
            else { *s++ = 0xBA; std::memcpy(s, &slot, 4); s += 4; *s++ = 0x41; *s++ = 0xB8; std::memcpy(s, &tag, 4); s += 4; }                 // mov edx,slot ; mov r8d,tag
            *s++ = 0x48; *s++ = 0xB8; std::memcpy(s, &target, 8); s += 8;                                                                          // mov rax,target
            *s++ = 0xFF; *s++ = 0xE0;                                                                                                              // jmp rax
            g_stubUsed += (static_cast<std::size_t>(s - p) + 15) & ~static_cast<std::size_t>(15);
            return p;
        }
        void* Fwd(void** realVtbl, int slot) { return Stub(realVtbl[slot], true); }
        void* Miss(int slot, int tag) { return Stub(reinterpret_cast<void*>(&Unmapped), false, slot, tag); }

        bool ReadPtr(const void* at, void** out)
        {
            __try { *out = *static_cast<void* const*>(at); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        struct Rec { bool used = false; std::uint8_t* real = nullptr; alignas(16) std::uint8_t proxy[0x80]; std::uint32_t count = 0; std::vector<std::uint8_t> table; std::vector<Path*> paths; };
        Rec g_recs[8];
        std::uint64_t B_Request(void* self, std::uint64_t id, std::uint64_t req, std::uint64_t r9);
        std::uint64_t B_Cancel(void* self, std::uint64_t id, std::uint64_t, std::uint64_t);
        std::uint64_t B_Recv(void* self, std::uint64_t id, std::uint64_t param, std::uint64_t r9);
        void ReadBytes16(std::uint8_t* d, const std::uint8_t* s) { __try { std::memcpy(d, s, 16); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

        // S slot 0 CreatePathfinder(desc): the real S makes the Pathfinder, the proxy hands Survive a Survive-layout stand-in with the same slot order.
        void* S_Create(void*, const std::uint32_t* desc)
        {
            using F = void* (*)(void*, const void*);
            void** vt = *reinterpret_cast<void***>(g_realS);
            std::uint8_t* b = static_cast<std::uint8_t*>(reinterpret_cast<F>(vt[0])(g_realS, desc));
            void* bv = nullptr;
            if (!b || !ReadPtr(b, &bv) || reinterpret_cast<std::uint64_t>(bv) != g_cfg.bVtbl) { Trace("[navproxy] CreatePathfinder returned %p with vtable %p, expected %llx", b, bv, static_cast<unsigned long long>(g_cfg.bVtbl)); return nullptr; }
            for (Rec& r : g_recs)
            {
                if (r.used) continue;
                std::memset(r.proxy, 0, sizeof(r.proxy));
                for (int k = 0; k < 6; ++k) g_bVtbl[k] = Fwd(static_cast<void**>(bv), k);
                g_bVtbl[0] = reinterpret_cast<void*>(&B_Request);
                g_bVtbl[1] = reinterpret_cast<void*>(&B_Cancel);   // logged forwards of the real slots 1 (cancel/release) and 3 (receive result): they decide whether a request survives
                g_bVtbl[3] = reinterpret_cast<void*>(&B_Recv);
                r.used = true; r.real = b; r.count = desc[0];
                ReadBytes16(r.proxy + 0x10, b + 0x10);   // the world handle B+0x10 (S slot 2 / 4 arguments) from the start, not only after the first Sync
                r.table.assign(static_cast<std::size_t>(r.count) * 0x10, 0);
                r.paths.assign(r.count, nullptr);
                *reinterpret_cast<void***>(r.proxy) = g_bVtbl;
                *reinterpret_cast<void**>(r.proxy + 8) = r.table.data();
                *reinterpret_cast<void**>(r.proxy + kReal) = b;
                Trace("[navproxy] pathfinder %p wrapped as %p (%u ids)", b, r.proxy, r.count);
                return r.proxy;
            }
            Trace("[navproxy] more than 8 pathfinders");
            return nullptr;
        }
        // S slot 1 DeletePathfinder(B'): the real one is deleted through the real S.
        void S_Delete(void*, void* proxyB)
        {
            using F = void (*)(void*, void*);
            for (Rec& r : g_recs)
                if (r.used && r.proxy == proxyB)
                {
                    reinterpret_cast<F>((*reinterpret_cast<void***>(g_realS))[1])(g_realS, r.real);
                    r.used = false; r.real = nullptr; return;
                }
            Trace("[navproxy] DeletePathfinder of an unknown object %p", proxyB);
        }

        // B slot 0 Request(id, request): the real Pathfinder sets the entry's flag bytes (+9 state, +0xa bit 0 and 1 = pending/busy, TPP NavPathfinderImpl), and the controller reads them from the
        // proxy table right after the call (0x141409e24: bit 1 of +0xa decides between the request counter and 0xffff as the handle). A tail-jump stub would leave the copy stale until the next
        // Sync, so slot 0 refreshes those 8 bytes of the entry (+8..+0xf) of that id; the path pointer is converted by Sync as before.
        // The first kMaxLines request lines show what the controller asks of the real pathfinder and what the real pathfinder answers.
        constexpr int kMaxLines = 60;
        int g_reqLines = 0, g_probeLines = 0;
        std::uint64_t ReadU64(const void* p) { std::uint64_t v = 0; __try { v = *static_cast<const std::uint64_t*>(p); } __except (EXCEPTION_EXECUTE_HANDLER) {} return v; }
        // Real entry bytes +8..+0xf of id (state at +9, flags at +0xa), read from the real table.
        std::uint64_t RealEntry(void* real, std::uint64_t id)
        {
            void* t = nullptr;
            if (!ReadPtr(static_cast<std::uint8_t*>(real) + 8, &t) || !t) return ~0ull;
            return ReadU64(static_cast<std::uint8_t*>(t) + static_cast<std::size_t>(id) * 0x10 + 8);
        }
        int g_evLines = 0;
        std::uint64_t B_Cancel(void* self, std::uint64_t id, std::uint64_t, std::uint64_t)
        {
            void* real = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kReal);
            const std::uint64_t before = RealEntry(real, id);
            using F = std::uint64_t (*)(void*, std::uint64_t);
            const std::uint64_t r = reinterpret_cast<F>((*reinterpret_cast<void***>(real))[1])(real, id);
            if (g_evLines < 400) { ++g_evLines; Trace("[navB] CANCEL (survive pc %llx) id %llu entry+8 %016llx -> %016llx", static_cast<unsigned long long>(lift::g_pc), static_cast<unsigned long long>(id), static_cast<unsigned long long>(before), static_cast<unsigned long long>(RealEntry(real, id))); }
            return r;
        }
        std::uint64_t B_Recv(void* self, std::uint64_t id, std::uint64_t param, std::uint64_t r9)
        {
            void* real = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kReal);
            const std::uint64_t before = RealEntry(real, id);
            // The finished search lives in the holder's strategy object (holder = [B+0x60] + id*0x80, strategy = [holder+0x60]); ReceiveResult releases it, so it is read first:
            // +0xb8 goal reached (state 2/3 when set, 4/5 when not), +0x11b path end differs from the request (adds 1 to the state), +0x134 strategy state, +0x138 flag (NavPathfinderImpl.cpp ReceiveResult).
            unsigned sb8 = 0xff, s11b = 0xff, s138 = 0xff; int s134 = -1;
            {
                const std::uint64_t holder = ReadU64(static_cast<std::uint8_t*>(real) + 0x60) + id * 0x80;
                const std::uint64_t st = ReadU64(reinterpret_cast<const void*>(holder + 0x60));
                if (st) { __try { const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(st); sb8 = p[0xb8]; s11b = p[0x11b]; s134 = *reinterpret_cast<const int*>(p + 0x134); s138 = p[0x138]; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            }
            using F = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
            const std::uint64_t r = reinterpret_cast<F>((*reinterpret_cast<void***>(real))[3])(real, id, param, r9);
            // The controller reads the entry state (+9) and the path again from the proxy table right after this call (NavigationController2Impl::Update: the switch on the state after the
            // receive block). TPP shares one table; the proxy keeps a copy, so a stale copy sends the controller into its request case again (Gekko.log: 59 repeated requests).
            SyncEntry(self, static_cast<std::uint32_t>(id));
            std::uint32_t n = 0;
            if (r) { std::uint32_t c = 0; __try { c = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(r) + 0x20); } __except (EXCEPTION_EXECUTE_HANDLER) {} n = c; }
            if (g_evLines < 400) { ++g_evLines; Trace("[navB] RECEIVE (survive pc %llx) id %llu entry+8 %016llx -> %016llx | strat b8 %u 11b %u s134 %d f138 %u | result path %p count %u", static_cast<unsigned long long>(lift::g_pc), static_cast<unsigned long long>(id), static_cast<unsigned long long>(before), static_cast<unsigned long long>(RealEntry(real, id)), sb8, s11b, s134, s138, reinterpret_cast<const void*>(r), n); }
            return r;
        }
        std::uint64_t B_Request(void* self, std::uint64_t id, std::uint64_t req, std::uint64_t r9)
        {
            void* real = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kReal);
            using F = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
            std::uint64_t before = 0;
            const std::uint8_t* rq = reinterpret_cast<const std::uint8_t*>(req);
            for (Rec& rec : g_recs)
            {
                void* t = nullptr;
                if (rec.used && rec.proxy == self && ReadPtr(rec.real + 8, &t) && t) before = ReadU64(static_cast<std::uint8_t*>(t) + static_cast<std::size_t>(id) * 0x10 + 8);
            }
            // Read-only probe of the real NavigationSystem slot 2 (the controller's own "is this point on the nav mesh" query, at 0x141409911: S, &B+0x10, &point) for the start and the goal.
            // Every request is probed. Before 07/10/2026 the probe stopped after 8 requests and the later log lines read -1, which means "not asked", not "off the mesh".
            int onStart = -1, onGoal = -1;
            if (g_realS)
            {
                ++g_probeLines;
                using Q = std::uint8_t (*)(void*, void*, void*);
                const Q q = reinterpret_cast<Q>((*reinterpret_cast<void***>(g_realS))[2]);
                alignas(16) float pt[2][4] = {};
                __try { std::memcpy(pt, rq, 32); onStart = q(g_realS, static_cast<std::uint8_t*>(real) + 0x10, pt[0]) ? 1 : 0; onGoal = q(g_realS, static_cast<std::uint8_t*>(real) + 0x10, pt[1]) ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            const std::uint64_t r = reinterpret_cast<F>((*reinterpret_cast<void***>(real))[0])(real, id, req, r9);
            std::uint64_t after = 0;
            for (Rec& rec : g_recs)
            {
                if (!rec.used || rec.proxy != self || static_cast<std::uint32_t>(id) >= rec.count) continue;
                void* t = nullptr;
                if (ReadPtr(rec.real + 8, &t) && t)
                {
                    __try { std::memcpy(rec.table.data() + static_cast<std::size_t>(id) * 0x10 + 8, static_cast<std::uint8_t*>(t) + static_cast<std::size_t>(id) * 0x10 + 8, 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    after = ReadU64(static_cast<std::uint8_t*>(t) + static_cast<std::size_t>(id) * 0x10 + 8);
                }
            }
            static int offLines = 0;
            if ((onStart == 0 || onGoal == 0) && offLines < 60 && g_cfg.trace) { ++offLines; Trace("[navB] request %d: start on-mesh %d goal on-mesh %d (0 = off the nav mesh)", g_probeLines, onStart, onGoal); }
            if (g_reqLines < kMaxLines && g_cfg.trace)
            {
                ++g_reqLines;
                float v[8] = {}; __try { std::memcpy(v, rq, sizeof(v)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                std::uint8_t d[0x40] = {}; __try { std::memcpy(d, rq, sizeof(d)); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Trace("[navB] Request (survive pc %llx) id %llu start %.2f %.2f %.2f goal %.2f %.2f %.2f desc+20 %llx n28 %u w2c %u w2e %u f30 %.2f f34 %.2f b38 %u | world %016llx %016llx | entry+8 %016llx -> %016llx | ret %llx | S slot 2 on-mesh start %d goal %d",
                      static_cast<unsigned long long>(lift::g_pc), static_cast<unsigned long long>(id), v[0], v[1], v[2], v[4], v[5], v[6], static_cast<unsigned long long>(*reinterpret_cast<std::uint64_t*>(d + 0x20)), *reinterpret_cast<std::uint32_t*>(d + 0x28),
                      *reinterpret_cast<std::uint16_t*>(d + 0x2c), *reinterpret_cast<std::uint16_t*>(d + 0x2e), *reinterpret_cast<float*>(d + 0x30), *reinterpret_cast<float*>(d + 0x34), unsigned(d[0x38]),
                      static_cast<unsigned long long>(ReadU64(static_cast<std::uint8_t*>(real) + 0x10)), static_cast<unsigned long long>(ReadU64(static_cast<std::uint8_t*>(real) + 0x18)),
                      static_cast<unsigned long long>(before), static_cast<unsigned long long>(after), static_cast<unsigned long long>(r), onStart, onGoal);
                Trace("[navB] on-mesh start %d goal %d", onStart, onGoal);
            }
            return r;
        }

        // No C++ objects here: SEH only guards the reads of TPP memory.
        bool CopyPath(Path* d, const std::uint8_t* t, std::uint32_t n)
        {
            __try
            {
                const std::uint8_t* pts = *reinterpret_cast<const std::uint8_t* const*>(t + 8);
                const std::uint8_t* seg = *reinterpret_cast<const std::uint8_t* const*>(t + 0x10);
                const std::uint8_t* flg = *reinterpret_cast<const std::uint8_t* const*>(t + 0x18);
                std::uint8_t* b = d->block;
                std::uint8_t* sg = b + static_cast<std::size_t>(n) * 16;
                std::uint8_t* fl = sg + static_cast<std::size_t>(n) * 0x30;
                std::uint8_t* u16 = fl + ((n + 2) & ~1u);
                std::memcpy(b, pts, static_cast<std::size_t>(n) * 16);
                std::memset(sg, 0, static_cast<std::size_t>(n) * 0x30);
                for (std::uint32_t i = 0; i < n; ++i) *reinterpret_cast<std::uint32_t*>(sg + i * 0x30ull + 0x28) = *reinterpret_cast<const std::uint32_t*>(seg + i * 0x40ull + 0x30);
                std::memcpy(fl, flg, n); fl[n] = 0;
                std::memset(u16, 0, static_cast<std::size_t>(n) * 2);
                *reinterpret_cast<std::uint32_t*>(d->obj + 0x28) = n;
                *reinterpret_cast<void**>(d->obj + 8) = b;
                *reinterpret_cast<void**>(d->obj + 0x10) = sg;
                *reinterpret_cast<void**>(d->obj + 0x18) = fl;
                *reinterpret_cast<void**>(d->obj + 0x20) = u16;
                *reinterpret_cast<void**>(d->obj + 0x38) = b;   // where Survive's Reserve keeps the block
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool ReadCount(const std::uint8_t* t, std::uint32_t* n)
        {
            __try { *n = *reinterpret_cast<const std::uint32_t*>(t + 0x20); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    Path* NewPath()
    {
        Path* p = new Path;
        p->obj = static_cast<std::uint8_t*>(_aligned_malloc(0x80, 16));
        std::memset(p->obj, 0, 0x80);
        if (g_cfg.callLifted) g_cfg.callLifted(0x141bf1e80ull, reinterpret_cast<std::uint64_t>(p->obj), 0, 0, 0);   // Survive's own constructor sets the vtable and clears the fields
        return p;
    }

    bool ConvertPath(Path* d, const std::uint8_t* t)
    {
        std::uint32_t n = 0;
        if (!ReadCount(t, &n) || n > kMaxPoints) { Trace("[navproxy] path %p: bad count %u", t, n); return false; }
        if (n == 0) { *reinterpret_cast<std::uint32_t*>(d->obj + 0x28) = 0; return true; }
        const std::size_t need = static_cast<std::size_t>(n) * (16 + 0x30 + 1 + 2) + 16;
        if (need > d->cap)
        {
            _aligned_free(d->block);
            d->block = static_cast<std::uint8_t*>(_aligned_malloc(need, 16));
            d->cap = need;
        }
        return CopyPath(d, t, n);

    }

    bool AccessorOk()
    {
        static const std::uint8_t want[13] = { 0x48, 0x83, 0xEC, 0x28, 0xE8, 0x47, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x40, 0x08 };
        __try { return std::memcmp(reinterpret_cast<const void*>(0x140bff050ull), want, sizeof(want)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Init(const Config& cfg)
    {
        if (g_ready) return true;
        g_cfg = cfg;
        std::uint8_t* t = static_cast<std::uint8_t*>(cfg.realScope);
        void *s = nullptr, *p = nullptr, *sv = nullptr;
        if (!t || !ReadPtr(t + 0xa0, &s) || !ReadPtr(t + 0x98, &p) || !s || !p || !ReadPtr(s, &sv)) { Trace("[navproxy] scope %p unreadable", t); return false; }
        if (reinterpret_cast<std::uint64_t>(sv) != cfg.sVtbl) { Trace("[navproxy] [S]=%p, expected %llx: slot order unknown, proxies not built", sv, static_cast<unsigned long long>(cfg.sVtbl)); return false; }
        g_realS = static_cast<std::uint8_t*>(s);
        void** rv = static_cast<void**>(sv);
        g_sVtbl[0] = reinterpret_cast<void*>(&S_Create);
        g_sVtbl[1] = reinterpret_cast<void*>(&S_Delete);
        for (int k = 2; k <= 5; ++k) g_sVtbl[k] = Fwd(rv, k);
        g_sVtbl[6] = Miss(6, 1);
        for (int k = 7; k <= 14; ++k) g_sVtbl[k] = Fwd(rv, k - 1);
        std::memset(g_sObj, 0, sizeof(g_sObj));
        *reinterpret_cast<void***>(g_sObj) = g_sVtbl;
        *reinterpret_cast<void**>(g_sObj + kReal) = s;
        // P: +8 is the same object in both games, +0x110 (Survive) / +0x100 (TPP) is the type-test object, Survive slot 29 (+0xe8) = TPP slot 18 (+0x90).
        void *x = nullptr, *q = nullptr, *qv = nullptr;
        if (!ReadPtr(static_cast<std::uint8_t*>(p) + 8, &x) || !ReadPtr(static_cast<std::uint8_t*>(p) + 0x100, &q) || !q || !ReadPtr(q, &qv)) { Trace("[navproxy] P %p unreadable", p); return false; }
        for (int k = 0; k < 160; ++k) g_qVtbl[k] = Miss(k, 2);
        g_qVtbl[29] = Fwd(static_cast<void**>(qv), 18);
        std::memset(g_qObj, 0, sizeof(g_qObj));
        *reinterpret_cast<void***>(g_qObj) = g_qVtbl;
        *reinterpret_cast<void**>(g_qObj + kReal) = q;
        std::memset(g_pObj, 0, sizeof(g_pObj));
        *reinterpret_cast<void**>(g_pObj + 8) = x;
        *reinterpret_cast<void**>(g_pObj + 0x110) = g_qObj;
        g_ready = true;
        Trace("[navproxy] ready: S %p (vtable %p) P %p", s, sv, p);
        return true;
    }
    bool Ready() { return g_ready; }

    void FillScope(std::uint8_t* t)
    {
        if (!g_ready) return;
        *reinterpret_cast<void**>(t + 0xa0) = g_sObj;
        // The host hub the life components read (GekkoLife) already sits at +0x98: the nav proxies add their two fields to it.
        std::uint8_t* hub = *reinterpret_cast<std::uint8_t**>(t + 0x98);
        if (hub) { *reinterpret_cast<void**>(hub + 8) = *reinterpret_cast<void**>(g_pObj + 8); *reinterpret_cast<void**>(hub + 0x110) = *reinterpret_cast<void**>(g_pObj + 0x110); }
        else *reinterpret_cast<void**>(t + 0x98) = g_pObj;
    }

    // One line when the real entry (+8..+0xf, state +9, flags +0xa), the real path object's point count or the converted copy changes for id (capped). Read-only.
    void Diag(int id, int frame)
    {
        if (!g_ready || !g_cfg.trace) return;
        static int lines = 0; static std::uint64_t last[8]; static std::uint32_t lastN[8], lastC[8];
        if (id < 0 || id > 7) return;
        for (Rec& r : g_recs)
        {
            if (!r.used || static_cast<std::uint32_t>(id) >= r.count) continue;
            void* t = nullptr;
            if (!ReadPtr(r.real + 8, &t) || !t) continue;
            const std::uint8_t* e = static_cast<std::uint8_t*>(t) + static_cast<std::size_t>(id) * 0x10;
            const std::uint64_t w = ReadU64(e + 8);
            const std::uint8_t* tp = reinterpret_cast<const std::uint8_t*>(ReadU64(e));
            std::uint32_t n = 0; if (tp) ReadCount(tp, &n);
            const std::uint8_t* pe = r.table.data() + static_cast<std::size_t>(id) * 0x10;
            const std::uint64_t pp = *reinterpret_cast<const std::uint64_t*>(pe);
            std::uint32_t c = 0; if (pp) c = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(pp) + 0x28);
            if (w == last[id] && n == lastN[id] && c == lastC[id]) continue;
            last[id] = w; lastN[id] = n; lastC[id] = c;
            if (lines++ >= 200) continue;
            float p0[4] = {}; if (c) { const std::uint8_t* b = *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(pp) + 8); __try { std::memcpy(p0, b, 16); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
            Trace("[navB] frame %d id %d real entry+8 %016llx (state %u flags %x) real path %p count %u | proxy count %u first point %.2f %.2f %.2f", frame, id, static_cast<unsigned long long>(w), unsigned((w >> 8) & 0xff), unsigned((w >> 16) & 0xff),
                  static_cast<const void*>(tp), n, c, p0[0], p0[1], p0[2]);
        }
    }

    void SyncOne(Rec& r, std::uint32_t i)
    {
        void* t = nullptr;
        if (!ReadPtr(r.real + 8, &t) || !t) return;
        const std::uint8_t* tbl = static_cast<const std::uint8_t*>(t);
        std::uint8_t* e = r.table.data() + i * 0x10ull;
        __try { std::memcpy(e, tbl + i * 0x10ull, 0x10); } __except (EXCEPTION_EXECUTE_HANDLER) { std::memset(e, 0, 0x10); return; }
        const std::uint8_t* tp = *reinterpret_cast<const std::uint8_t**>(e);
        if (!tp) return;
        if (!r.paths[i]) r.paths[i] = NewPath();
        if (!ConvertPath(r.paths[i], tp)) *reinterpret_cast<std::uint32_t*>(r.paths[i]->obj + 0x28) = 0;
        *reinterpret_cast<void**>(e) = r.paths[i]->obj;
    }

    void SyncEntry(void* proxySelf, std::uint32_t id)
    {
        for (Rec& r : g_recs)
            if (r.used && r.proxy == proxySelf && id < r.count) SyncOne(r, id);
    }

    void Sync()
    {
        if (!g_ready) return;
        for (Rec& r : g_recs)
        {
            if (!r.used) continue;
            void* t = nullptr;
            if (!ReadPtr(r.real + 8, &t) || !t) continue;
            std::uint8_t* tbl = static_cast<std::uint8_t*>(t);
            __try { std::memcpy(r.proxy + 0x10, r.real + 0x10, 0x10); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            for (std::uint32_t i = 0; i < r.count; ++i) SyncOne(r, i);
        }
    }
}

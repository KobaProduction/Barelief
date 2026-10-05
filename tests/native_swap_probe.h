#pragma once
#include "core/performance.h"
#include <QDebug>
#if defined(_WIN32)
#include <windows.h>
#include <cstring>
#include <array>

// Optional test-only probe. Patch the Qt Windows plugin's import slot, rather
// than breakpoint/unwind every driver call. Restore the slot before Qt teardown.
class NativeSwapProbe {
    using Swap = BOOL (WINAPI *)(HDC);
    struct Sample { uint64_t start, duration; };
    inline static Swap _original = nullptr;
    inline static DWORD _owner_thread = 0;
    inline static uint64_t _count = 0, _total = 0, _maximum = 0;
    inline static std::array<Sample, 16> _slow{};
    inline static size_t _slow_count = 0;
    void **_slot = nullptr;
    static BOOL WINAPI measured_swap(HDC dc) {
        if (GetCurrentThreadId() != _owner_thread || !core::perf::enabled.load(std::memory_order_relaxed)) return _original(dc);
        const auto start = core::perf::now();
        const auto result = _original(dc);
        const auto duration = core::perf::now() - start;
        ++_count; _total += duration; _maximum = std::max(_maximum, duration);
        if (duration >= 50000000 && _slow_count < _slow.size())
            _slow[_slow_count++] = {start, duration};
        return result;
    }
public:
    bool install() {
        auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(L"qwindows.dll"));
        if (!base) return false;
        const auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return false;
        auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + rva);
        for (; imports->Name; ++imports) {
            if (!imports->OriginalFirstThunk) continue;
            auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->OriginalFirstThunk);
            auto *entries = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++entries) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
                const auto *symbol = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char *>(symbol->Name), "SwapBuffers") != 0) continue;
                auto **slot = reinterpret_cast<void **>(&entries->u1.Function);
                DWORD protection;
                if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return false;
                _original = reinterpret_cast<Swap>(*slot);
                _owner_thread = GetCurrentThreadId();
                InterlockedExchangePointer(slot, reinterpret_cast<void *>(&measured_swap));
                DWORD ignored; VirtualProtect(slot, sizeof(void *), protection, &ignored);
                _slot = slot; return true;
            }
        }
        return false;
    }
    void report() const {
        if (!_slot) return;
        qInfo("NATIVE_SWAP count=%llu mean=%.3fms max=%.3fms", static_cast<unsigned long long>(_count),
            _count ? _total / double(_count) / 1000000.0 : 0, _maximum / 1000000.0);
        for (size_t i = 0; i < _slow_count; ++i)
            qInfo("NATIVE_SWAP_SLOW start_ns=%llu duration_ms=%.3f", static_cast<unsigned long long>(_slow[i].start),
                _slow[i].duration / 1000000.0);
    }
    ~NativeSwapProbe() {
        if (!_slot) return;
        DWORD protection;
        if (VirtualProtect(_slot, sizeof(void *), PAGE_READWRITE, &protection)) {
            InterlockedCompareExchangePointer(_slot, reinterpret_cast<void *>(_original), reinterpret_cast<void *>(&measured_swap));
            DWORD ignored; VirtualProtect(_slot, sizeof(void *), protection, &ignored);
        }
    }
};
#else
class NativeSwapProbe {
public:
    bool install() { return false; }
    void report() const {}
};
#endif

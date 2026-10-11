// =============================================================================
// Gaslightr — main.cpp
// Phase 1 Test Harness:  Registry bypass → Headless DX12 init → Diagnostics
//
// This is a standalone console application (no Photoshop, no UXP).  It exists
// solely to verify that:
//   1. The ALLOW_UNSIGNED registry keys can be written.
//   2. A headless D3D12 device can be created on the NVIDIA adapter.
//   3. The command queue, allocator, list, and fence are operational.
//
// If this harness succeeds, the DX12 foundation is ready for Phase 2
// (Memory Alignment and the Neural Backend Core).
// =============================================================================

#include "DX12Context.h"
#include "nvngx_dlssnr_core.h"   // Struct definitions (not used yet — Phase 4)

#include <cstdio>
#include <cstdlib>

// ── Colour codes for console output ──────────────────────────────────────────
static void SetConsoleGreen() {
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hCon, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}
static void SetConsoleRed() {
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hCon, FOREGROUND_RED | FOREGROUND_INTENSITY);
}
static void SetConsoleYellow() {
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hCon,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}
static void SetConsoleDefault() {
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hCon,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// ── Pretty-print an HRESULT ──────────────────────────────────────────────────
static void PrintHR(HRESULT hr) {
    wprintf(L"  HRESULT = 0x%08lX\n", static_cast<unsigned long>(hr));
}

// ═════════════════════════════════════════════════════════════════════════════
// Entry point
// ═════════════════════════════════════════════════════════════════════════════
int wmain(int /*argc*/, wchar_t* /*argv*/[])
{
    wprintf(L"====================================================\n");
    wprintf(L"  Gaslightr — Phase 1 Scaffolding Test Harness\n");
    wprintf(L"====================================================\n\n");

    // ── Step 1: Assert NVIDIA registry bypass ────────────────────────────
    wprintf(L"[1/3] Asserting NVIDIA ALLOW_UNSIGNED registry bypass...\n");
    {
        auto result = Gaslightr::DX12Context::AssertRegistryBypass();
        if (result) {
            SetConsoleGreen();
            wprintf(L"      PASS — Registry keys written successfully.\n");
            SetConsoleDefault();
        } else {
            SetConsoleYellow();
            wprintf(L"      WARN — Registry write failed.\n");
            wprintf(L"      %s\n", result.detail.c_str());
            PrintHR(result.hr);
            wprintf(L"      The DX12 context will still initialise, but\n");
            wprintf(L"      NGX may reject unsigned binaries in Phase 4.\n");
            wprintf(L"      Try re-running as Administrator.\n");
            SetConsoleDefault();
        }
    }
    wprintf(L"\n");

    // ── Step 2: Initialise headless DX12 ─────────────────────────────────
    wprintf(L"[2/3] Initialising headless DX12 context...\n");
    Gaslightr::DX12Context ctx;
    {
        auto result = ctx.InitializeHeadless();
        if (!result) {
            SetConsoleRed();
            wprintf(L"      FAIL — %s\n", result.detail.c_str());
            PrintHR(result.hr);
            SetConsoleDefault();
            return EXIT_FAILURE;
        }
        SetConsoleGreen();
        wprintf(L"      PASS — Device, Queue, Allocator, List, Fence created.\n");
        SetConsoleDefault();
    }

    // Print adapter info.
    {
        DXGI_ADAPTER_DESC1 desc{};
        if (ctx.GetAdapter()) {
            ctx.GetAdapter()->GetDesc1(&desc);
            wprintf(L"      Adapter : %s\n", desc.Description);
            wprintf(L"      VRAM    : %zu MB\n",
                desc.DedicatedVideoMemory / (1024 * 1024));
            wprintf(L"      Vendor  : 0x%04X\n", desc.VendorId);
        }
    }
    wprintf(L"\n");

    // ── Step 3: GPU fence round-trip ─────────────────────────────────────
    wprintf(L"[3/3] Testing GPU fence synchronisation...\n");
    {
        auto result = ctx.WaitForGPU();
        if (!result) {
            SetConsoleRed();
            wprintf(L"      FAIL — %s\n", result.detail.c_str());
            PrintHR(result.hr);
            SetConsoleDefault();
            return EXIT_FAILURE;
        }
        SetConsoleGreen();
        wprintf(L"      PASS — Fence signal/wait round-trip succeeded.\n");
        SetConsoleDefault();
    }
    wprintf(L"\n");

    // ── Done ─────────────────────────────────────────────────────────────
    SetConsoleGreen();
    wprintf(L"====================================================\n");
    wprintf(L"  Phase 1 COMPLETE.  Headless DX12 environment is\n");
    wprintf(L"  operational.  Ready for Phase 2.\n");
    wprintf(L"====================================================\n");
    SetConsoleDefault();

    // Graceful teardown.
    ctx.Shutdown();

    return EXIT_SUCCESS;
}

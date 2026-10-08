#define WINVER 0x0601
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "hook_defs.h"
#include "jxl_loader.h"
#include "lav_hook.h"

static void LogInfo(const char* fmt, ...) {
    FILE* f = fopen("d3d9_hook.log", "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

// Forward to real d3d9.dll
typedef void* (WINAPI *Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t s_orig_Direct3DCreate9 = NULL;

extern "C" __declspec(dllexport) void* WINAPI Direct3DCreate9(UINT SDKVersion) {
    if (!s_orig_Direct3DCreate9) {
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        strcat(sysPath, "\\d3d9.dll");
        HMODULE hReal = LoadLibraryA(sysPath);
        if (hReal) {
            s_orig_Direct3DCreate9 = (Direct3DCreate9_t)GetProcAddress(hReal, "Direct3DCreate9");
        }
    }
    if (s_orig_Direct3DCreate9) {
        return s_orig_Direct3DCreate9(SDKVersion);
    }
    return NULL;
}

// Active game offsets & function pointers
static const struct GameOffsets* g_pCurrentGame = NULL;
static fn_CreateImageBuffer g_pCreateImageBuffer = NULL;
static fn_OpenExpFile g_pOpenExpFile = NULL;
static fn_CloseExpFile g_pCloseExpFile = NULL;
static fn_ReadExpFile g_pReadExpFile = NULL;
static fn_SeekExpFile g_pSeekExpFile = NULL;
static fn_LoadPNG g_pOrig_LoadPNG = NULL;

// Inline hook trampoline for LoadPNG
static void* Hook_LoadPNG(const char* lpFileName) {
    if (!lpFileName) return NULL;

    // Step 1: Check loose JXL file
    char jxlPath[MAX_PATH];
    strncpy(jxlPath, lpFileName, MAX_PATH - 1);
    jxlPath[MAX_PATH - 1] = '\0';

    char* ext = strrchr(jxlPath, '.');
    if (ext && (_stricmp(ext, ".png") == 0 || _stricmp(ext, ".jpg") == 0)) {
        strcpy(ext, ".jxl");
        DWORD attr = GetFileAttributesA(jxlPath);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            FILE* f = fopen(jxlPath, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long fsize = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (fsize > 0) {
                    uint8_t* buf = (uint8_t*)malloc(fsize);
                    if (buf) {
                        fread(buf, 1, fsize, f);
                        fclose(f);
                        if (IsJxlData(buf, fsize)) {
                            void* surface = DecodeJxlToGameSurface(buf, fsize, g_pCreateImageBuffer);
                            free(buf);
                            if (surface) {
                                return surface;
                            }
                        } else {
                            free(buf);
                        }
                    } else {
                        fclose(f);
                    }
                } else {
                    fclose(f);
                }
            }
        }
    }

    // Step 2: Check resource stream from OpenExpFile
    if (g_pOpenExpFile && g_pReadExpFile && g_pSeekExpFile && g_pCloseExpFile) {
        void* stream = g_pOpenExpFile(lpFileName);
        if (stream) {
            uint8_t header[16] = {0};
            DWORD bytesRead = 0;
            if (g_pReadExpFile(stream, header, 16, &bytesRead) && bytesRead >= 2) {
                if (IsJxlData(header, bytesRead)) {
                    int totalSize = g_pSeekExpFile(stream, 0, FILE_END);
                    g_pSeekExpFile(stream, 0, FILE_BEGIN);
                    if (totalSize > 0) {
                        uint8_t* fileBuf = (uint8_t*)malloc(totalSize);
                        if (fileBuf) {
                            DWORD actualRead = 0;
                            g_pReadExpFile(stream, fileBuf, totalSize, &actualRead);
                            g_pCloseExpFile(stream);
                            void* surface = DecodeJxlToGameSurface(fileBuf, totalSize, g_pCreateImageBuffer);
                            free(fileBuf);
                            if (surface) {
                                return surface;
                            }
                        }
                    }
                }
            }
            g_pSeekExpFile(stream, 0, FILE_BEGIN);
            g_pCloseExpFile(stream);
        }
    }

    // Step 3: Fall back to original LoadPNG
    return g_pOrig_LoadPNG(lpFileName);
}

// Hook installer
static bool InstallInlineHook(void* targetFunc, void* detourFunc, void** ppTrampoline, size_t patchLen) {
    if (!targetFunc || !detourFunc || !ppTrampoline || patchLen < 5) return false;

    void* trampoline = VirtualAlloc(NULL, patchLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return false;

    memcpy(trampoline, targetFunc, patchLen);

    uint8_t* pTrampJump = (uint8_t*)trampoline + patchLen;
    pTrampJump[0] = 0xE9;
    int32_t relToTarget = (int32_t)((uint8_t*)targetFunc + patchLen - (pTrampJump + 5));
    memcpy(pTrampJump + 1, &relToTarget, 4);

    *ppTrampoline = trampoline;

    DWORD oldProtect;
    VirtualProtect(targetFunc, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect);

    uint8_t* pTarget = (uint8_t*)targetFunc;
    pTarget[0] = 0xE9;
    int32_t relToDetour = (int32_t)((uint8_t*)detourFunc - (pTarget + 5));
    memcpy(pTarget + 1, &relToDetour, 4);

    for (size_t i = 5; i < patchLen; i++) {
        pTarget[i] = 0x90;
    }

    VirtualProtect(targetFunc, patchLen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), targetFunc, patchLen);
    FlushInstructionCache(GetCurrentProcess(), trampoline, patchLen + 5);

    return true;
}

static void InitHooks(void) {
    HMODULE hMain = GetModuleHandleA(NULL);
    if (!hMain) return;

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)hMain;
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;

    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((uint8_t*)hMain + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;

    uint32_t entryRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;

    for (size_t i = 0; i < sizeof(g_SupportedGames)/sizeof(g_SupportedGames[0]); i++) {
        if (g_SupportedGames[i].entry_rva == entryRVA) {
            g_pCurrentGame = &g_SupportedGames[i];
            break;
        }
    }

    if (!g_pCurrentGame) {
        g_pCurrentGame = &g_SupportedGames[0];
    }

    g_pCreateImageBuffer = (fn_CreateImageBuffer)g_pCurrentGame->create_image_buf;
    g_pOpenExpFile       = (fn_OpenExpFile)g_pCurrentGame->open_exp_file;
    g_pCloseExpFile      = (fn_CloseExpFile)g_pCurrentGame->close_exp_file;
    g_pReadExpFile       = (fn_ReadExpFile)g_pCurrentGame->read_exp_file;
    g_pSeekExpFile       = (fn_SeekExpFile)g_pCurrentGame->seek_exp_file;

    LogInfo("=== d3d9_hook (LAV Filters Hardware Accelerated Edition) initialized ===");
    LogInfo("Base: %p, EntryPoint RVA: 0x%08X", (void*)hMain, entryRVA);
    LogInfo("Target Game: %s", g_pCurrentGame->game_name);

    // Install inline hook on LoadPNG (6 bytes: 55 8b ec 83 ec 48)
    bool bHooked = InstallInlineHook(
        (void*)g_pCurrentGame->load_png,
        (void*)Hook_LoadPNG,
        (void**)&g_pOrig_LoadPNG,
        6
    );
    LogInfo("LoadPNG hook installed: %s", bHooked ? "SUCCESS" : "FAILED");

    // Install LAV Filters DirectShow hook
    bool bLav = InstallLAVFiltersHook();
    LogInfo("LAV Filters DirectShow hook installed: %s", bLav ? "SUCCESS" : "FAILED");
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        InitHooks();
    }
    return TRUE;
}

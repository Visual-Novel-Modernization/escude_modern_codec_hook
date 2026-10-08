#define WIN32_LEAN_AND_MEAN
#include "lav_hook.h"
#include <windows.h>
#include <dshow.h>
#include <stdio.h>
#include <stdint.h>

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

static void WideToUtf8(const wchar_t* wstr, char* out, int outLen) {
    if (!wstr || outLen <= 0) { if (outLen > 0) out[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out, outLen, NULL, NULL);
    out[outLen - 1] = '\0';
}

static void EnsureLAVManifest(const wchar_t* dirPath) {
    wchar_t manifestPath[MAX_PATH] = { 0 };
    lstrcpyW(manifestPath, dirPath);
    lstrcatW(manifestPath, L"\\LAVFilters.Dependencies.manifest");
    if (GetFileAttributesW(manifestPath) == INVALID_FILE_ATTRIBUTES) {
        static const char s_manifestContent[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
            "  <assemblyIdentity type='win32' name='LAVFilters.Dependencies' version='1.0.0.0' />\r\n"
            "  <file name=\"avutil-lav-61.dll\" />\r\n"
            "  <file name=\"swresample-lav-7.dll\" />\r\n"
            "  <file name=\"avcodec-lav-63.dll\" />\r\n"
            "  <file name=\"avformat-lav-63.dll\" />\r\n"
            "  <file name=\"swscale-lav-10.dll\" />\r\n"
            "  <file name=\"avfilter-lav-12.dll\" />\r\n"
            "  <file name=\"libbluray.dll\" />\r\n"
            "</assembly>\r\n";
        FILE* f = _wfopen(manifestPath, L"wb");
        if (f) {
            fwrite(s_manifestContent, 1, sizeof(s_manifestContent) - 1, f);
            fclose(f);
            LogInfo("[LAV] Auto-generated missing LAVFilters.Dependencies.manifest");
        }
    }
}

// LAV Filters CLSIDs
static const GUID GUID_LAVSplitterSource = { 0xB98D13E7, 0x55DB, 0x4385, { 0xA3, 0x3D, 0x09, 0xFD, 0x1B, 0xA2, 0x63, 0x38 } };
static const GUID GUID_LAVVideo          = { 0xEE30215D, 0x164F, 0x4A92, { 0xA4, 0xEB, 0x9D, 0x4C, 0x13, 0x39, 0x0F, 0x9F } };
static const GUID GUID_LAVAudio          = { 0xE8E73B6B, 0x4CB3, 0x44A4, { 0xBE, 0x99, 0x4F, 0x7B, 0xCB, 0x96, 0xE4, 0x91 } };

typedef HRESULT (WINAPI *PFN_CoCreateInstance)(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID *ppv);
typedef HRESULT (WINAPI *PFN_RenderFile)(IGraphBuilder* pThis, LPCWSTR lpcwstrFile, LPCWSTR lpcwstrPlayList);
typedef HRESULT (STDAPICALLTYPE *PFN_DllGetClassObject)(REFCLSID rclsid, REFIID riid, LPVOID *ppv);

static PFN_CoCreateInstance g_pfnRealCoCreateInstance = NULL;
static PFN_RenderFile        g_pfnRealRenderFile        = NULL;

static void GetLAVPath(const wchar_t* axName, wchar_t* outPath, DWORD maxLen) {
    wchar_t selfDir[MAX_PATH] = { 0 };
    GetModuleFileNameW(GetModuleHandleA(NULL), selfDir, MAX_PATH);
    wchar_t* pSlash = wcsrchr(selfDir, L'\\');
    if (pSlash) *(pSlash + 1) = L'\0';

    // 1. Check ./lav/
    lstrcpyW(outPath, selfDir);
    lstrcatW(outPath, L"lav\\");
    lstrcatW(outPath, axName);
    if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) return;

    // 2. Check ./
    lstrcpyW(outPath, selfDir);
    lstrcatW(outPath, axName);
}

static IBaseFilter* LoadFilterFromAx(const wchar_t* axName, const GUID& clsid) {
    wchar_t axPath[MAX_PATH] = { 0 };
    GetLAVPath(axName, axPath, MAX_PATH);

    wchar_t dirPath[MAX_PATH] = { 0 };
    lstrcpyW(dirPath, axPath);
    wchar_t* pSlash = wcsrchr(dirPath, L'\\');
    if (pSlash) {
        *pSlash = L'\0';
        EnsureLAVManifest(dirPath);
        SetDllDirectoryW(dirPath);
    }

    char u8Path[MAX_PATH * 3] = { 0 };
    WideToUtf8(axPath, u8Path, sizeof(u8Path));
    char u8Name[MAX_PATH * 3] = { 0 };
    WideToUtf8(axName, u8Name, sizeof(u8Name));

    HMODULE hMod = LoadLibraryExW(axPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hMod) {
        LogInfo("[LAV] Failed to load %s (Error: %lu)", u8Path, GetLastError());
        return NULL;
    }

    PFN_DllGetClassObject fnGetClass = (PFN_DllGetClassObject)GetProcAddress(hMod, "DllGetClassObject");
    if (!fnGetClass) {
        LogInfo("[LAV] DllGetClassObject not found in %s", u8Name);
        return NULL;
    }

    IClassFactory* pFactory = NULL;
    HRESULT hr = fnGetClass(clsid, IID_IClassFactory, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        LogInfo("[LAV] DllGetClassObject failed for %s: 0x%08lX", u8Name, hr);
        return NULL;
    }

    IBaseFilter* pFilter = NULL;
    hr = pFactory->CreateInstance(NULL, IID_IBaseFilter, (void**)&pFilter);
    pFactory->Release();
    if (FAILED(hr) || !pFilter) {
        LogInfo("[LAV] CreateInstance failed for %s: 0x%08lX", u8Name, hr);
        return NULL;
    }

    LogInfo("[LAV] Successfully instantiated %s", u8Name);
    return pFilter;
}

static bool CheckModernVideoFile(LPCWSTR origPath, wchar_t* outTargetFile, DWORD maxLen) {
    if (!origPath) return false;

    // Check if origPath is already an MP4/MKV/WebM
    const wchar_t* ext = wcsrchr(origPath, L'.');
    if (ext && (_wcsicmp(ext, L".mp4") == 0 || _wcsicmp(ext, L".webm") == 0 || _wcsicmp(ext, L".mkv") == 0)) {
        if (GetFileAttributesW(origPath) != INVALID_FILE_ATTRIBUTES) {
            lstrcpynW(outTargetFile, origPath, maxLen);
            return true;
        }
    }

    // Check if a companion .mp4, .webm, or .mkv exists
    size_t baseLen = ext ? (size_t)(ext - origPath) : wcslen(origPath);
    const wchar_t* modernExts[] = { L".mp4", L".webm", L".mkv" };
    wchar_t testPath[MAX_PATH];

    for (int i = 0; i < 3; i++) {
        wcsncpy(testPath, origPath, baseLen);
        testPath[baseLen] = L'\0';
        lstrcatW(testPath, modernExts[i]);
        if (GetFileAttributesW(testPath) != INVALID_FILE_ATTRIBUTES) {
            lstrcpynW(outTargetFile, testPath, maxLen);
            return true;
        }
    }

    return false;
}

static HRESULT WINAPI Hook_RenderFile(IGraphBuilder* pThis, LPCWSTR lpcwstrFile, LPCWSTR lpcwstrPlayList) {
    char u8File[MAX_PATH * 3] = { 0 };
    WideToUtf8(lpcwstrFile, u8File, sizeof(u8File));
    LogInfo("[LAV] IGraphBuilder::RenderFile called for: %s", u8File);

    wchar_t modernFile[MAX_PATH] = { 0 };
    if (CheckModernVideoFile(lpcwstrFile, modernFile, MAX_PATH)) {
        char u8Modern[MAX_PATH * 3] = { 0 };
        WideToUtf8(modernFile, u8Modern, sizeof(u8Modern));
        LogInfo("[LAV] Target is modern video: %s, routing through LAV Filters (Hardware Accelerated)", u8Modern);

        IBaseFilter* pSplitter = LoadFilterFromAx(L"LAVSplitter.ax", GUID_LAVSplitterSource);
        IBaseFilter* pVideoDec = LoadFilterFromAx(L"LAVVideo.ax",    GUID_LAVVideo);
        IBaseFilter* pAudioDec = LoadFilterFromAx(L"LAVAudio.ax",    GUID_LAVAudio);

        if (!pSplitter || !pVideoDec || !pAudioDec) {
            LogInfo("[LAV] Failed to load LAV Filters, falling back to native RenderFile");
            if (pSplitter) pSplitter->Release();
            if (pVideoDec) pVideoDec->Release();
            if (pAudioDec) pAudioDec->Release();
            return g_pfnRealRenderFile(pThis, lpcwstrFile, lpcwstrPlayList);
        }

        IFileSourceFilter* pFileSource = NULL;
        HRESULT hr = pSplitter->QueryInterface(IID_IFileSourceFilter, (void**)&pFileSource);
        if (FAILED(hr)) {
            LogInfo("[LAV] QueryInterface IFileSourceFilter failed: 0x%08lX", hr);
            pSplitter->Release();
            pVideoDec->Release();
            pAudioDec->Release();
            return g_pfnRealRenderFile(pThis, lpcwstrFile, lpcwstrPlayList);
        }

        hr = pFileSource->Load(modernFile, NULL);
        pFileSource->Release();
        if (FAILED(hr)) {
            LogInfo("[LAV] IFileSourceFilter::Load failed: 0x%08lX", hr);
            pSplitter->Release();
            pVideoDec->Release();
            pAudioDec->Release();
            return g_pfnRealRenderFile(pThis, lpcwstrFile, lpcwstrPlayList);
        }

        pThis->AddFilter(pSplitter, L"LAV Splitter Source");
        pThis->AddFilter(pVideoDec, L"LAV Video Decoder");
        pThis->AddFilter(pAudioDec, L"LAV Audio Decoder");

        // Render output pins from LAV Splitter
        IEnumPins* pEnum = NULL;
        hr = pSplitter->EnumPins(&pEnum);
        if (SUCCEEDED(hr) && pEnum) {
            IPin* pPin = NULL;
            while (pEnum->Next(1, &pPin, NULL) == S_OK) {
                PIN_DIRECTION dir;
                pPin->QueryDirection(&dir);
                if (dir == PINDIR_OUTPUT) {
                    HRESULT rhr = pThis->Render(pPin);
                    LogInfo("[LAV] Render pin result: 0x%08lX", rhr);
                }
                pPin->Release();
            }
            pEnum->Release();
        }

        pSplitter->Release();
        pVideoDec->Release();
        pAudioDec->Release();

        LogInfo("[LAV] Successfully mounted LAV Filters in FilterGraph! Hardware decoding active.");
        return S_OK;
    }

    return g_pfnRealRenderFile(pThis, lpcwstrFile, lpcwstrPlayList);
}

static void HookGraphBuilder(IGraphBuilder* pGraph) {
    void** pVTable = *(void***)pGraph;
    if (pVTable[13] != (void*)Hook_RenderFile) {
        g_pfnRealRenderFile = (PFN_RenderFile)pVTable[13];
        DWORD oldProtect;
        VirtualProtect(&pVTable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
        pVTable[13] = (void*)Hook_RenderFile;
        VirtualProtect(&pVTable[13], sizeof(void*), oldProtect, &oldProtect);
        LogInfo("[LAV] IGraphBuilder::RenderFile (slot 13) hooked successfully!");
    }
}

static HRESULT WINAPI Hook_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID *ppv) {
    HRESULT hr = g_pfnRealCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (IsEqualGUID(rclsid, CLSID_FilterGraph)) {
            LogInfo("[LAV] CoCreateInstance(CLSID_FilterGraph) intercepted: %p", *ppv);
            HookGraphBuilder((IGraphBuilder*)*ppv);
        }
    }
    return hr;
}

bool InstallLAVFiltersHook(void) {
    HMODULE hOle32 = GetModuleHandleA("ole32.dll");
    if (!hOle32) hOle32 = LoadLibraryA("ole32.dll");
    if (!hOle32) return false;

    uint8_t* pTarget = (uint8_t*)GetProcAddress(hOle32, "CoCreateInstance");
    if (!pTarget) return false;

    // Allocate executable trampoline
    uint8_t* tramp = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    memcpy(tramp, pTarget, 5);
    tramp[5] = 0xE9;
    *(int32_t*)(tramp + 6) = (int32_t)(pTarget + 5) - (int32_t)(tramp + 10);
    g_pfnRealCoCreateInstance = (PFN_CoCreateInstance)tramp;

    DWORD oldProtect;
    VirtualProtect(pTarget, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    pTarget[0] = 0xE9;
    *(int32_t*)(pTarget + 1) = (int32_t)((uint8_t*)Hook_CoCreateInstance) - (int32_t)(pTarget + 5);
    VirtualProtect(pTarget, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), pTarget, 5);

    LogInfo("[LAV] CoCreateInstance hook installed successfully!");
    return true;
}

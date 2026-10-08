#define WINVER 0x0601
#define _WIN32_WINNT 0x0601

#include "mf_video.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <evr.h>
#include <stdio.h>

static const GUID k_GUID_NULL = { 0 };

static volatile bool g_bIsPlaying = false;
static volatile bool g_bStopRequested = false;
static volatile int  g_nVolumePercent = 100;

static HANDLE g_hVideoThread = NULL;
static wchar_t g_wszCurrentFile[MAX_PATH] = { 0 };
static HWND g_hwndParent = NULL;
static HWND g_hwndChild = NULL;

static IMFMediaSession* g_pSession = NULL;
static IMFSimpleAudioVolume* g_pAudioVolume = NULL;

static LRESULT CALLBACK VideoChildWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_KEYDOWN:
            if (g_hwndParent) {
                // Forward input to parent window so game can skip movie
                PostMessageA(g_hwndParent, uMsg, wParam, lParam);
            }
            return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static void EnsureVideoClass(void) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = VideoChildWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"EscudeVideoWnd";
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
}

static HRESULT CreateMediaSource(PCWSTR pszURL, IMFMediaSource** ppSource) {
    IMFSourceResolver* pSourceResolver = NULL;
    IUnknown* pSourceUnk = NULL;
    MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;

    HRESULT hr = MFCreateSourceResolver(&pSourceResolver);
    if (SUCCEEDED(hr)) {
        hr = pSourceResolver->CreateObjectFromURL(
            pszURL,
            MF_RESOLUTION_MEDIASOURCE,
            NULL,
            &ObjectType,
            &pSourceUnk
        );
    }
    if (SUCCEEDED(hr)) {
        hr = pSourceUnk->QueryInterface(IID_PPV_ARGS(ppSource));
    }
    if (pSourceUnk) pSourceUnk->Release();
    if (pSourceResolver) pSourceResolver->Release();
    return hr;
}

static HRESULT CreateTopology(IMFMediaSource* pSource, HWND hwndVideo, IMFTopology** ppTopology) {
    IMFTopology* pTopology = NULL;
    IMFPresentationDescriptor* pPD = NULL;
    DWORD cSourceStreams = 0;

    HRESULT hr = MFCreateTopology(&pTopology);
    if (SUCCEEDED(hr)) hr = pSource->CreatePresentationDescriptor(&pPD);
    if (SUCCEEDED(hr)) hr = pPD->GetStreamDescriptorCount(&cSourceStreams);

    for (DWORD i = 0; i < cSourceStreams && SUCCEEDED(hr); i++) {
        BOOL fSelected = FALSE;
        IMFStreamDescriptor* pSD = NULL;
        hr = pPD->GetStreamDescriptorByIndex(i, &fSelected, &pSD);
        if (SUCCEEDED(hr) && fSelected) {
            IMFMediaTypeHandler* pHandler = NULL;
            GUID guidMajorType;
            hr = pSD->GetMediaTypeHandler(&pHandler);
            if (SUCCEEDED(hr)) hr = pHandler->GetMajorType(&guidMajorType);

            IMFActivate* pSinkActivate = NULL;
            if (SUCCEEDED(hr)) {
                if (guidMajorType == MFMediaType_Audio) {
                    hr = MFCreateAudioRendererActivate(&pSinkActivate);
                } else if (guidMajorType == MFMediaType_Video) {
                    hr = MFCreateVideoRendererActivate(hwndVideo, &pSinkActivate);
                }
            }

            if (SUCCEEDED(hr) && pSinkActivate) {
                IMFTopologyNode* pSourceNode = NULL;
                IMFTopologyNode* pOutputNode = NULL;

                hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &pSourceNode);
                if (SUCCEEDED(hr)) hr = pSourceNode->SetUnknown(MF_TOPONODE_SOURCE, pSource);
                if (SUCCEEDED(hr)) hr = pSourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pPD);
                if (SUCCEEDED(hr)) hr = pSourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pSD);

                if (SUCCEEDED(hr)) hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &pOutputNode);
                if (SUCCEEDED(hr)) hr = pOutputNode->SetObject(pSinkActivate);

                if (SUCCEEDED(hr)) hr = pTopology->AddNode(pSourceNode);
                if (SUCCEEDED(hr)) hr = pTopology->AddNode(pOutputNode);
                if (SUCCEEDED(hr)) hr = pSourceNode->ConnectOutput(0, pOutputNode, 0);

                if (pSourceNode) pSourceNode->Release();
                if (pOutputNode) pOutputNode->Release();
                pSinkActivate->Release();
            }
            if (pHandler) pHandler->Release();
        }
        if (pSD) pSD->Release();
    }

    if (SUCCEEDED(hr)) {
        *ppTopology = pTopology;
        (*ppTopology)->AddRef();
    }
    if (pTopology) pTopology->Release();
    if (pPD) pPD->Release();
    return hr;
}

static DWORD WINAPI VideoThreadProc(LPVOID lpParam) {
    // Isolated COM multithreaded apartment in this worker thread only
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    HWND hParent = g_hwndParent;
    RECT rc = { 0 };
    if (hParent) {
        GetClientRect(hParent, &rc);
    }
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    EnsureVideoClass();

    // Create a pristine child window on top of hParent
    HWND hChild = CreateWindowExW(
        0, L"EscudeVideoWnd", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, width, height,
        hParent, NULL, GetModuleHandle(NULL), NULL
    );
    g_hwndChild = hChild;

    IMFMediaSource* pSource = NULL;
    IMFMediaSession* pSession = NULL;
    IMFTopology* pTopology = NULL;
    IMFSimpleAudioVolume* pVol = NULL;

    HRESULT hr = CreateMediaSource(g_wszCurrentFile, &pSource);
    if (SUCCEEDED(hr)) {
        hr = MFCreateMediaSession(NULL, &pSession);
    }
    if (SUCCEEDED(hr)) {
        g_pSession = pSession;
        hr = CreateTopology(pSource, hChild, &pTopology);
    }
    if (SUCCEEDED(hr)) {
        hr = pSession->SetTopology(0, pTopology);
    }
    if (SUCCEEDED(hr)) {
        PROPVARIANT varStart;
        PropVariantInit(&varStart);
        varStart.vt = VT_EMPTY;
        hr = pSession->Start(&k_GUID_NULL, &varStart);
    }
    if (SUCCEEDED(hr)) {
        MFGetService(pSession, MR_POLICY_VOLUME_SERVICE, IID_PPV_ARGS(&pVol));
        if (pVol) {
            g_pAudioVolume = pVol;
            float fVol = (float)g_nVolumePercent / 100.0f;
            if (fVol < 0.0f) fVol = 0.0f;
            if (fVol > 1.0f) fVol = 1.0f;
            pVol->SetMasterVolume(fVol);
        }

        // Wait loop: block on session events while checking for stop request
        while (!g_bStopRequested) {
            IMFMediaEvent* pEvent = NULL;
            HRESULT evHr = pSession->GetEvent(0, &pEvent);
            if (SUCCEEDED(evHr) && pEvent) {
                MediaEventType met = MEUnknown;
                pEvent->GetType(&met);
                pEvent->Release();

                if (met == MESessionEnded || met == MESessionClosed) {
                    break;
                }
            } else {
                if (FAILED(evHr)) break;
            }
        }
    }

    // Cleanup session and source inside worker thread
    if (pSession) {
        pSession->Stop();
        pSession->Close();
        pSession->Release();
        g_pSession = NULL;
    }
    if (pVol) {
        pVol->Release();
        g_pAudioVolume = NULL;
    }
    if (pTopology) {
        pTopology->Release();
    }
    if (pSource) {
        pSource->Shutdown();
        pSource->Release();
    }

    if (hChild) {
        DestroyWindow(hChild);
        g_hwndChild = NULL;
    }

    MFShutdown();
    CoUninitialize();

    g_bIsPlaying = false;

    // Repaint parent window
    if (hParent) {
        InvalidateRect(hParent, NULL, TRUE);
        UpdateWindow(hParent);
    }

    return 0;
}

bool MFVideo_Play(const wchar_t* pwszFilePath, HWND hwndParent) {
    MFVideo_Stop();

    if (!pwszFilePath || !hwndParent) return false;
    wcsncpy(g_wszCurrentFile, pwszFilePath, MAX_PATH - 1);
    g_wszCurrentFile[MAX_PATH - 1] = L'\0';

    g_hwndParent = hwndParent;
    g_bStopRequested = false;
    g_bIsPlaying = true;

    g_hVideoThread = CreateThread(NULL, 0, VideoThreadProc, NULL, 0, NULL);
    if (!g_hVideoThread) {
        g_bIsPlaying = false;
        return false;
    }

    return true;
}

void MFVideo_Stop(void) {
    if (g_bIsPlaying) {
        g_bStopRequested = true;
    }
    if (g_hVideoThread) {
        WaitForSingleObject(g_hVideoThread, 3000);
        CloseHandle(g_hVideoThread);
        g_hVideoThread = NULL;
    }
    g_bIsPlaying = false;
}

bool MFVideo_IsPlaying(void) {
    return g_bIsPlaying;
}

void MFVideo_SetVolume(int percent) {
    g_nVolumePercent = percent;
    if (g_pAudioVolume) {
        float fVol = (float)percent / 100.0f;
        if (fVol < 0.0f) fVol = 0.0f;
        if (fVol > 1.0f) fVol = 1.0f;
        g_pAudioVolume->SetMasterVolume(fVol);
    }
}

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
static HANDLE g_hVideoThread = NULL;
static HANDLE g_hStopEvent = NULL;
static HANDLE g_hEndedEvent = NULL;
static wchar_t g_wszCurrentFile[MAX_PATH] = { 0 };
static HWND g_hwndParent = NULL;
static HWND g_hwndPopup = NULL;

static IMFMediaSession* g_pSession = NULL;

static LRESULT CALLBACK VideoPopupWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_KEYDOWN:
            // User requested skip
            MFVideo_Stop();
            return 0;
        case WM_SETCURSOR:
            SetCursor(NULL); // Hide cursor during movie
            return TRUE;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static void EnsureVideoClass(void) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = VideoPopupWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"EscudeVideoPopup";
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = NULL;
        RegisterClassW(&wc);
        registered = true;
    }
}

class CSessionCallback : public IMFAsyncCallback {
public:
    CSessionCallback(IMFMediaSession* pSession, HANDLE hEnded)
        : m_nRefCount(1), m_pSession(pSession), m_hEnded(hEnded) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_nRefCount);
    }

    STDMETHODIMP_(ULONG) Release() {
        ULONG uCount = InterlockedDecrement(&m_nRefCount);
        if (uCount == 0) delete this;
        return uCount;
    }

    STDMETHODIMP GetParameters(DWORD* pdwFlags, DWORD* pdwQueue) {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult* pResult) {
        if (!m_pSession) return S_OK;

        IMFMediaEvent* pEvent = NULL;
        HRESULT hr = m_pSession->EndGetEvent(pResult, &pEvent);
        if (SUCCEEDED(hr) && pEvent) {
            MediaEventType met = MEUnknown;
            pEvent->GetType(&met);
            pEvent->Release();

            if (met == MESessionEnded || met == MESessionClosed) {
                g_bIsPlaying = false;
                if (m_hEnded) SetEvent(m_hEnded);
            } else {
                m_pSession->BeginGetEvent(this, NULL);
            }
        }
        return S_OK;
    }

private:
    virtual ~CSessionCallback() {}
    long m_nRefCount;
    IMFMediaSession* m_pSession;
    HANDLE m_hEnded;
};

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
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    IMFMediaSource* pSource = NULL;
    IMFMediaSession* pSession = NULL;
    IMFTopology* pTopology = NULL;
    CSessionCallback* pCallback = NULL;

    HRESULT hr = CreateMediaSource(g_wszCurrentFile, &pSource);
    if (SUCCEEDED(hr)) {
        hr = MFCreateMediaSession(NULL, &pSession);
    }
    if (SUCCEEDED(hr)) {
        g_pSession = pSession;
        pCallback = new CSessionCallback(pSession, g_hEndedEvent);
        pSession->BeginGetEvent(pCallback, NULL);

        hr = CreateTopology(pSource, g_hwndPopup, &pTopology);
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
        HANDLE waitHandles[2] = { g_hStopEvent, g_hEndedEvent };
        WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
    }

    if (pSession) {
        pSession->Stop();
        pSession->Close();
        pSession->Release();
        g_pSession = NULL;
    }
    if (pTopology) {
        pTopology->Release();
    }
    if (pSource) {
        pSource->Shutdown();
        pSource->Release();
    }
    if (pCallback) {
        pCallback->Release();
    }

    MFShutdown();
    CoUninitialize();

    g_bIsPlaying = false;
    return 0;
}

bool MFVideo_Play(const wchar_t* pwszFilePath, HWND hwndParent) {
    MFVideo_Stop();

    if (!pwszFilePath || !hwndParent) return false;
    wcsncpy(g_wszCurrentFile, pwszFilePath, MAX_PATH - 1);
    g_wszCurrentFile[MAX_PATH - 1] = L'\0';

    g_hwndParent = hwndParent;

    EnsureVideoClass();

    // Determine client position and dimensions in screen coordinates
    POINT pt = { 0, 0 };
    ClientToScreen(hwndParent, &pt);
    RECT rc = { 0 };
    GetClientRect(hwndParent, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    // Create a borderless, top-level popup window perfectly matching the game viewport
    g_hwndPopup = CreateWindowExW(
        WS_EX_TOPMOST, L"EscudeVideoPopup", L"",
        WS_POPUP | WS_VISIBLE,
        pt.x, pt.y, width, height,
        hwndParent, NULL, GetModuleHandle(NULL), NULL
    );

    if (!g_hwndPopup) return false;

    ShowWindow(g_hwndPopup, SW_SHOW);
    UpdateWindow(g_hwndPopup);

    g_bIsPlaying = true;

    if (!g_hStopEvent)  g_hStopEvent  = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_hEndedEvent) g_hEndedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    ResetEvent(g_hStopEvent);
    ResetEvent(g_hEndedEvent);

    g_hVideoThread = CreateThread(NULL, 0, VideoThreadProc, NULL, 0, NULL);
    if (!g_hVideoThread) {
        g_bIsPlaying = false;
        DestroyWindow(g_hwndPopup);
        g_hwndPopup = NULL;
        return false;
    }

    return true;
}

void MFVideo_Stop(void) {
    if (g_bIsPlaying) {
        g_bIsPlaying = false;
        if (g_hStopEvent) SetEvent(g_hStopEvent);
    }
    if (g_hVideoThread) {
        WaitForSingleObject(g_hVideoThread, 1500);
        CloseHandle(g_hVideoThread);
        g_hVideoThread = NULL;
    }
    if (g_hwndPopup) {
        DestroyWindow(g_hwndPopup);
        g_hwndPopup = NULL;
        if (g_hwndParent) {
            InvalidateRect(g_hwndParent, NULL, TRUE);
            UpdateWindow(g_hwndParent);
            SetFocus(g_hwndParent);
        }
    }
}

bool MFVideo_IsPlaying(void) {
    return g_bIsPlaying;
}

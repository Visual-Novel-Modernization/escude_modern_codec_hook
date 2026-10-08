#define WINVER 0x0601
#define _WIN32_WINNT 0x0601

#include "mf_video.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <evr.h>
#include <stdio.h>

static const GUID k_GUID_NULL = { 0 };

class CSessionCallback : public IMFAsyncCallback {
public:
    CSessionCallback() : m_nRefCount(1), m_pSession(NULL), m_hwnd(NULL) {}

    void SetSession(IMFMediaSession* pSession, HWND hwnd) {
        m_pSession = pSession;
        m_hwnd = hwnd;
    }

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
        if (FAILED(hr) || !pEvent) return S_OK;

        MediaEventType met = MEUnknown;
        pEvent->GetType(&met);

        if (met == MESessionEnded) {
            MFVideo_Stop();
            if (m_hwnd) {
                // DirectShow EC_COMPLETE compatibility
                PostMessageA(m_hwnd, 1074, 1, 0);
            }
        } else if (met == MESessionClosed) {
            // Closed
        } else {
            // Keep listening
            m_pSession->BeginGetEvent(this, NULL);
        }

        pEvent->Release();
        return S_OK;
    }

private:
    virtual ~CSessionCallback() {}
    long m_nRefCount;
    IMFMediaSession* m_pSession;
    HWND m_hwnd;
};

static bool g_bMFInitialized = false;
static IMFMediaSession* g_pSession = NULL;
static IMFMediaSource* g_pSource = NULL;
static IMFSimpleAudioVolume* g_pAudioVolume = NULL;
static CSessionCallback* g_pCallback = NULL;
static bool g_bIsPlaying = false;
static HWND g_hTargetWnd = NULL;

bool MFVideo_Init(void) {
    if (g_bMFInitialized) return true;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (SUCCEEDED(hr)) {
        g_bMFInitialized = true;
        return true;
    }
    return false;
}

void MFVideo_Shutdown(void) {
    MFVideo_Stop();
    if (g_bMFInitialized) {
        MFShutdown();
        g_bMFInitialized = false;
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

bool MFVideo_Play(const wchar_t* pwszFilePath, HWND hwnd) {
    if (!MFVideo_Init()) return false;
    MFVideo_Stop();

    g_hTargetWnd = hwnd;

    HRESULT hr = CreateMediaSource(pwszFilePath, &g_pSource);
    if (FAILED(hr)) return false;

    hr = MFCreateMediaSession(NULL, &g_pSession);
    if (FAILED(hr)) {
        g_pSource->Release();
        g_pSource = NULL;
        return false;
    }

    g_pCallback = new CSessionCallback();
    g_pCallback->SetSession(g_pSession, hwnd);
    g_pSession->BeginGetEvent(g_pCallback, NULL);

    IMFTopology* pTopology = NULL;
    hr = CreateTopology(g_pSource, hwnd, &pTopology);
    if (SUCCEEDED(hr)) {
        hr = g_pSession->SetTopology(0, pTopology);
        pTopology->Release();
    }

    if (FAILED(hr)) {
        MFVideo_Stop();
        return false;
    }

    PROPVARIANT varStart;
    PropVariantInit(&varStart);
    varStart.vt = VT_EMPTY;
    hr = g_pSession->Start(&k_GUID_NULL, &varStart);
    if (FAILED(hr)) {
        MFVideo_Stop();
        return false;
    }

    // Query volume service
    MFGetService(g_pSession, MR_POLICY_VOLUME_SERVICE, IID_PPV_ARGS(&g_pAudioVolume));

    g_bIsPlaying = true;
    return true;
}

void MFVideo_Stop(void) {
    if (g_pSession) {
        g_pSession->Stop();
        g_pSession->Close();
        g_pSession->Release();
        g_pSession = NULL;
    }
    if (g_pAudioVolume) {
        g_pAudioVolume->Release();
        g_pAudioVolume = NULL;
    }
    if (g_pSource) {
        g_pSource->Shutdown();
        g_pSource->Release();
        g_pSource = NULL;
    }
    if (g_pCallback) {
        g_pCallback->Release();
        g_pCallback = NULL;
    }
    g_bIsPlaying = false;

    // Repaint game window
    if (g_hTargetWnd) {
        InvalidateRect(g_hTargetWnd, NULL, TRUE);
        UpdateWindow(g_hTargetWnd);
    }
}

bool MFVideo_IsPlaying(void) {
    return g_bIsPlaying;
}

void MFVideo_SetVolume(int percent) {
    if (g_pAudioVolume) {
        float fVol = (float)percent / 100.0f;
        if (fVol < 0.0f) fVol = 0.0f;
        if (fVol > 1.0f) fVol = 1.0f;
        g_pAudioVolume->SetMasterVolume(fVol);
    }
}

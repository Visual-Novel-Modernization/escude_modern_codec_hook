#define WINVER 0x0601
#define _WIN32_WINNT 0x0601

#include "av1_player.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <dav1d/dav1d.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static volatile bool g_bIsPlaying = false;
static volatile bool g_bStopRequested = false;

static HANDLE g_hVideoThread = NULL;
static wchar_t g_wszCurrentFile[MAX_PATH] = { 0 };
static HWND g_hwndParent = NULL;
static HWND g_hwndPopup = NULL;

static inline uint8_t clamp8(int val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return (uint8_t)val;
}

static void YUV420_To_BGR24(const uint8_t* yPlane, int yStride,
                            const uint8_t* uPlane, int uStride,
                            const uint8_t* vPlane, int vStride,
                            int width, int height,
                            uint8_t* dstBgr, int dstStride) {
    for (int y = 0; y < height; y++) {
        const uint8_t* py = yPlane + y * yStride;
        const uint8_t* pu = uPlane + (y >> 1) * uStride;
        const uint8_t* pv = vPlane + (y >> 1) * vStride;
        uint8_t* pdst = dstBgr + (height - 1 - y) * dstStride;

        for (int x = 0; x < width; x++) {
            int Y = py[x];
            int U = pu[x >> 1] - 128;
            int V = pv[x >> 1] - 128;

            int c = Y - 16;
            if (c < 0) c = 0;
            int r = (298 * c + 409 * V + 128) >> 8;
            int g = (298 * c - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * c + 516 * U + 128) >> 8;

            pdst[x * 3 + 0] = clamp8(b);
            pdst[x * 3 + 1] = clamp8(g);
            pdst[x * 3 + 2] = clamp8(r);
        }
    }
}

static LRESULT CALLBACK VideoPopupWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_KEYDOWN:
            AV1Player_Stop();
            return 0;
        case WM_SETCURSOR:
            SetCursor(NULL);
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
        wc.lpszClassName = L"EscudeAV1Popup";
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hCursor = NULL;
        RegisterClassW(&wc);
        registered = true;
    }
}

static DWORD WINAPI VideoPlaybackThread(LPVOID lpParam) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_FULL);

    IMFSourceReader* pReader = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(g_wszCurrentFile, NULL, &pReader);
    if (FAILED(hr) || !pReader) {
        g_bIsPlaying = false;
        MFShutdown();
        CoUninitialize();
        return 1;
    }

    // Audio format: PCM
    IMFMediaType* pAudioType = NULL;
    MFCreateMediaType(&pAudioType);
    pAudioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pAudioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    hr = pReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pAudioType);
    pAudioType->Release();

    UINT32 channels = 2, rate = 44100, bits = 16;
    IMFMediaType* pCurrentAudio = NULL;
    if (SUCCEEDED(pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &pCurrentAudio)) && pCurrentAudio) {
        pCurrentAudio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        pCurrentAudio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        pCurrentAudio->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        pCurrentAudio->Release();
    }

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = rate;
    wfx.wBitsPerSample = bits;
    wfx.nBlockAlign = (channels * bits) / 8;
    wfx.nAvgBytesPerSec = rate * wfx.nBlockAlign;

    HWAVEOUT hWaveOut = NULL;
    waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);

    // Initialize dav1d
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = 4;
    Dav1dContext* ctx = NULL;
    dav1d_open(&ctx, &settings);

    // Framebuffer setup
    RECT rc = { 0 };
    GetClientRect(g_hwndPopup, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    HDC hdc = GetDC(g_hwndPopup);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    int bgrStride = 4 * ((width * 24 + 31) / 32);
    uint8_t* bgrBuffer = (uint8_t*)malloc(bgrStride * height);

    Dav1dData videoData = { 0 };
    Dav1dPicture pic = { 0 };
    bool bVideoEOS = false;
    bool bAudioEOS = false;

    DWORD dwStartTime = timeGetTime();
    DWORD dwFrameIndex = 0;
    const double frameIntervalMs = 1000.0 / 30.0; // 30 fps

    while (!g_bStopRequested && !(bVideoEOS && bAudioEOS)) {
        // Feed audio
        if (!bAudioEOS) {
            DWORD aStream = 0, aFlags = 0;
            LONGLONG aTs = 0;
            IMFSample* pAudioSample = NULL;
            hr = pReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &aStream, &aFlags, &aTs, &pAudioSample);
            if (aFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                bAudioEOS = true;
            } else if (pAudioSample) {
                IMFMediaBuffer* pABuf = NULL;
                pAudioSample->ConvertToContiguousBuffer(&pABuf);
                BYTE* pAudioBytes = NULL;
                DWORD cbAudio = 0;
                pABuf->Lock(&pAudioBytes, NULL, &cbAudio);

                WAVEHDR* pHdr = (WAVEHDR*)calloc(1, sizeof(WAVEHDR));
                pHdr->lpData = (LPSTR)malloc(cbAudio);
                memcpy(pHdr->lpData, pAudioBytes, cbAudio);
                pHdr->dwBufferLength = cbAudio;
                waveOutPrepareHeader(hWaveOut, pHdr, sizeof(WAVEHDR));
                waveOutWrite(hWaveOut, pHdr, sizeof(WAVEHDR));

                pABuf->Unlock();
                pABuf->Release();
                pAudioSample->Release();
            }
        }

        // Feed video
        if (videoData.sz == 0 && !bVideoEOS) {
            DWORD vStream = 0, vFlags = 0;
            LONGLONG vTs = 0;
            IMFSample* pVideoSample = NULL;
            hr = pReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &vStream, &vFlags, &vTs, &pVideoSample);
            if (vFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                bVideoEOS = true;
            } else if (pVideoSample) {
                IMFMediaBuffer* pVBuf = NULL;
                pVideoSample->ConvertToContiguousBuffer(&pVBuf);
                BYTE* pVBytes = NULL;
                DWORD cbVideo = 0;
                pVBuf->Lock(&pVBytes, NULL, &cbVideo);

                uint8_t* pCopy = (uint8_t*)malloc(cbVideo);
                memcpy(pCopy, pVBytes, cbVideo);

                pVBuf->Unlock();
                pVBuf->Release();
                pVideoSample->Release();

                dav1d_data_wrap(&videoData, pCopy, cbVideo, (void(*)(const uint8_t*, void*))free, NULL);
            }
        }

        if (videoData.sz > 0) {
            dav1d_send_data(ctx, &videoData);
        }

        if (dav1d_get_picture(ctx, &pic) == 0) {
            // Render frame
            YUV420_To_BGR24(
                (const uint8_t*)pic.data[0], pic.stride[0],
                (const uint8_t*)pic.data[1], pic.stride[1],
                (const uint8_t*)pic.data[2], pic.stride[1],
                pic.p.w, pic.p.h,
                bgrBuffer, bgrStride
            );

            SetDIBitsToDevice(
                hdc, 0, 0, width, height,
                0, 0, 0, height,
                bgrBuffer, &bmi, DIB_RGB_COLORS
            );

            dav1d_picture_unref(&pic);
            dwFrameIndex++;

            // Accurate 30 FPS timing synchronization
            DWORD expectedTime = dwStartTime + (DWORD)(dwFrameIndex * frameIntervalMs);
            DWORD currentTime = timeGetTime();
            if (expectedTime > currentTime) {
                DWORD sleepMs = expectedTime - currentTime;
                if (sleepMs > 50) sleepMs = 50;
                Sleep(sleepMs);
            }
        } else {
            // No picture ready yet, brief yield
            Sleep(2);
        }

        // Pump popup window messages (clicks, keypresses)
        MSG msg;
        while (PeekMessageW(&msg, g_hwndPopup, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Cleanup
    ReleaseDC(g_hwndPopup, hdc);
    free(bgrBuffer);
    dav1d_data_unref(&videoData);
    dav1d_close(&ctx);

    if (hWaveOut) {
        waveOutReset(hWaveOut);
        waveOutClose(hWaveOut);
    }

    pReader->Release();
    MFShutdown();
    CoUninitialize();

    if (g_hwndPopup) {
        DestroyWindow(g_hwndPopup);
        g_hwndPopup = NULL;
    }

    if (g_hwndParent) {
        InvalidateRect(g_hwndParent, NULL, TRUE);
        UpdateWindow(g_hwndParent);
        SetFocus(g_hwndParent);
    }

    g_bIsPlaying = false;
    return 0;
}

bool AV1Player_Play(const wchar_t* pwszFilePath, HWND hwndParent) {
    AV1Player_Stop();

    if (!pwszFilePath || !hwndParent) return false;
    wcsncpy(g_wszCurrentFile, pwszFilePath, MAX_PATH - 1);
    g_wszCurrentFile[MAX_PATH - 1] = L'\0';

    g_hwndParent = hwndParent;

    EnsureVideoClass();

    POINT pt = { 0, 0 };
    ClientToScreen(hwndParent, &pt);
    RECT rc = { 0 };
    GetClientRect(hwndParent, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;

    g_hwndPopup = CreateWindowExW(
        WS_EX_TOPMOST, L"EscudeAV1Popup", L"",
        WS_POPUP | WS_VISIBLE,
        pt.x, pt.y, width, height,
        hwndParent, NULL, GetModuleHandle(NULL), NULL
    );

    if (!g_hwndPopup) return false;

    ShowWindow(g_hwndPopup, SW_SHOW);
    UpdateWindow(g_hwndPopup);

    g_bStopRequested = false;
    g_bIsPlaying = true;

    g_hVideoThread = CreateThread(NULL, 0, VideoPlaybackThread, NULL, 0, NULL);
    if (!g_hVideoThread) {
        g_bIsPlaying = false;
        DestroyWindow(g_hwndPopup);
        g_hwndPopup = NULL;
        return false;
    }

    return true;
}

void AV1Player_Stop(void) {
    if (g_bIsPlaying) {
        g_bStopRequested = true;
    }
    if (g_hVideoThread) {
        WaitForSingleObject(g_hVideoThread, 2000);
        CloseHandle(g_hVideoThread);
        g_hVideoThread = NULL;
    }
    if (g_hwndPopup) {
        DestroyWindow(g_hwndPopup);
        g_hwndPopup = NULL;
    }
    if (g_hwndParent) {
        InvalidateRect(g_hwndParent, NULL, TRUE);
        UpdateWindow(g_hwndParent);
        SetFocus(g_hwndParent);
    }
    g_bIsPlaying = false;
}

bool AV1Player_IsPlaying(void) {
    return g_bIsPlaying;
}

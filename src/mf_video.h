#ifndef MF_VIDEO_H
#define MF_VIDEO_H

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start playback in an isolated worker thread on a dedicated child window over hwndParent
bool MFVideo_Play(const wchar_t* pwszFilePath, HWND hwndParent);
void MFVideo_Stop(void);
bool MFVideo_IsPlaying(void);
void MFVideo_SetVolume(int percent);

#ifdef __cplusplus
}
#endif

#endif // MF_VIDEO_H

#ifndef AV1_PLAYER_H
#define AV1_PLAYER_H

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool AV1Player_Play(const wchar_t* pwszFilePath, HWND hwndParent);
void AV1Player_Stop(void);
bool AV1Player_IsPlaying(void);

#ifdef __cplusplus
}
#endif

#endif // AV1_PLAYER_H

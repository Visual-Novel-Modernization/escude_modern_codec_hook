#ifndef HOOK_DEFS_H
#define HOOK_DEFS_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Function pointer signatures matching the game engine
typedef void** (__cdecl *fn_CreateImageBuffer)(int width, int height, int bpp);
typedef void*   (__cdecl *fn_OpenExpFile)(const char* filename);
typedef void    (__cdecl *fn_CloseExpFile)(void* stream);
typedef BOOL    (__cdecl *fn_ReadExpFile)(void* stream, void* buf, size_t size, DWORD* bytesRead);
typedef int     (__cdecl *fn_SeekExpFile)(void* stream, LONG dist, DWORD origin);

typedef void*   (__cdecl *fn_LoadPNG)(const char* filename);
typedef BOOL    (__cdecl *fn_VideoPlay)(const char* filename);
typedef void*   (__cdecl *fn_VideoStop)(void);
typedef int     (__cdecl *fn_VideoIsPlay)(void);
typedef int     (__cdecl *fn_VideoSetVol)(int volume);

struct GameOffsets {
    const char* game_name;
    uint32_t entry_rva;
    uint32_t load_png;
    uint32_t create_image_buf;
    uint32_t open_exp_file;
    uint32_t close_exp_file;
    uint32_t read_exp_file;
    uint32_t seek_exp_file;
    uint32_t video_play;
    uint32_t video_stop;
    uint32_t video_is_play;
    uint32_t video_set_vol;
    uint32_t hwnd_ptr;
    uint32_t is_playing_ptr;
};

// Target offsets for all 3 games
static const struct GameOffsets g_SupportedGames[] = {
    // Haison (haison.exe)
    {
        "Haison",
        0x000F9429, // 0x4F9429
        0x00470850,
        0x004654C0,
        0x004ECF40,
        0x004ED0A0,
        0x004ED1A0,
        0x004ED130,
        0x0041DEA0,
        0x0041D800,
        0x0041D530,
        0x0041D4C0,
        0x00C31B68,
        0x0058E95C
    },
    // Haison Gaiden (haison_fd.exe)
    {
        "Haison Gaiden",
        0x000F98AC, // 0x4F98AC
        0x00470C70,
        0x004654E0,
        0x004ED3C0,
        0x004ED520,
        0x004ED620,
        0x004ED5B0,
        0x0041DEA0,
        0x0041D800,
        0x0041D530,
        0x0041D4C0,
        0x00C31B68,
        0x0058E95C
    },
    // Haison 2 (haison2.exe)
    {
        "Haison 2",
        0x0009D016, // 0x49D016
        0x0044D590,
        0x00441070,
        0x004916A0,
        0x00491800,
        0x00491900,
        0x00491890,
        0x004151D0,
        0x00414B30,
        0x00414860,
        0x004147F0,
        0x00BD49F8,
        0x005317B4
    }
};

#ifdef __cplusplus
}
#endif

#endif // HOOK_DEFS_H

#ifndef JXL_LOADER_H
#define JXL_LOADER_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include "hook_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

bool IsJxlData(const uint8_t* data, size_t size);
void* DecodeJxlToGameSurface(const uint8_t* data, size_t size, fn_CreateImageBuffer pCreateBuffer);

#ifdef __cplusplus
}
#endif

#endif // JXL_LOADER_H

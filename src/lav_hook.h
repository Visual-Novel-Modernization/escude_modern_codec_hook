#ifndef LAV_HOOK_H
#define LAV_HOOK_H

#include <windows.h>
#include <dshow.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Installs in-process DirectShow LAV Filters hook
bool InstallLAVFiltersHook(void);

#ifdef __cplusplus
}
#endif

#endif // LAV_HOOK_H

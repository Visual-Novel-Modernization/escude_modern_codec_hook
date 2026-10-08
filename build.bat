@echo off
setlocal
echo [*] Building Escu:de Modern Hook (d3d9.dll)...

if defined MINGW_PATH set "PATH=%MINGW_PATH%;%PATH%"
if defined MSYS_PATH set "PATH=%MSYS_PATH%;%PATH%"

if not exist bin mkdir bin

g++ -O2 -shared -o bin\d3d9.dll ^
    src\main.cpp ^
    src\jxl_loader.c ^
    src\mf_video.cpp ^
    -Isrc ^
    -DJXL_STATIC_DEFINE ^
    -static-libgcc -static-libstdc++ ^
    -Wl,--kill-at ^
    -Wl,-Bstatic -ljxl -ljxl_cms -lhwy -lbrotlidec -lbrotlienc -lbrotlicommon -Wl,-Bdynamic ^
    -lmfplat -lmf -lmfuuid -lstrmiids -lole32 -loleaut32 -lshlwapi

if %ERRORLEVEL% EQU 0 (
    echo [+] Build successful: bin\d3d9.dll
    if defined MINGW_PATH if exist "%MINGW_PATH%\libwinpthread-1.dll" (
        copy /Y "%MINGW_PATH%\libwinpthread-1.dll" bin\ >nul 2>&1
        echo [+] Copied libwinpthread-1.dll to bin\
    )
) else (
    echo [!] Build failed with error %ERRORLEVEL%
)

endlocal

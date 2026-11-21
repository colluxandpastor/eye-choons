@echo off
setlocal

rem Detect GCC compiler
where gcc >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    set "GCC=gcc"
) else if exist "%LOCALAPPDATA%\Programs\w64devkit\bin\gcc.exe" (
    set "GCC=%LOCALAPPDATA%\Programs\w64devkit\bin\gcc.exe"
) else (
    echo [ERROR] GCC compiler not found in PATH or standard directory.
    echo Please ensure MinGW-w64 or w64devkit is installed and gcc is in your PATH.
    exit /b 1
)

echo Compiling Eye Choons - iPhone Music Manager in pure C (Zero Apple DLLs)...
"%GCC%" -O2 -mwindows -o EyeChoons.exe main.c apple_conduit.c -lcomctl32 -lsetupapi -lws2_32 -lole32 -lshell32 -lgdi32 -lcomdlg32 -lmsimg32 -lsecur32 -lcrypt32
if %ERRORLEVEL% EQU 0 (
    echo [OK] Successfully built EyeChoons.exe
) else (
    echo [ERROR] Build failed.
    exit /b %ERRORLEVEL%
)

@echo off
setlocal enabledelayedexpansion
::============================================================================
:: build.bat - HAL Engine Build Script (Windows)
::
:: Usage:
::   build              Build Release (auto-detects vcpkg)
::   build debug        Build Debug
::   build clean        Remove build artifacts
::   build release      Build Release + package zip
::   build next         Build ZX Spectrum Next (requires z88dk on PATH)
::   build all          Build Windows Release + Next + package
::   build run          Build Release and launch the game
::   build help         Show this message
::
:: Environment variables (optional):
::   VCPKG_ROOT         Path to vcpkg (default: auto-detect)
::   SDL2_DIR           Manual SDL2 SDK path (if not using vcpkg)
::   Z88DK_PATH         Path to z88dk (for Next builds)
::   VS_GENERATOR       CMake generator (default: auto-detect VS version)
::============================================================================

set "PROJECT_NAME=windgame"
set "BUILD_DIR=build\out"
set "NEXT_BUILD_DIR=build\out_next"
set "DIST_DIR=dist"

:: ---- Parse command ---
set "CMD=%~1"
if "%CMD%"=="" set "CMD=release_build"
if /i "%CMD%"=="help"    goto :help
if /i "%CMD%"=="clean"   goto :clean
if /i "%CMD%"=="debug"   goto :debug
if /i "%CMD%"=="release" goto :release
if /i "%CMD%"=="next"    goto :next
if /i "%CMD%"=="all"     goto :all
if /i "%CMD%"=="run"     goto :run
goto :release_build

:: ========================================================================
:help
:: ========================================================================
echo.
echo  HAL Engine Build Script
echo  =======================
echo  build              Build Release (auto-detects vcpkg)
echo  build debug        Build Debug
echo  build clean        Remove build artifacts
echo  build release      Build Release + package zip
echo  build next         Build ZX Spectrum Next (requires z88dk)
echo  build all          Build Windows Release + Next + package
echo  build run          Build Release and launch
echo  build help         This message
echo.
echo  Set VCPKG_ROOT to your vcpkg install if auto-detect fails.
echo  Set SDL2_DIR for manual SDL2 SDK (no vcpkg).
echo  Set Z88DK_PATH for ZX Spectrum Next builds.
exit /b 0

:: ========================================================================
:detect_vcpkg
:: ========================================================================
if defined VCPKG_ROOT (
    if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
        echo [vcpkg] Using VCPKG_ROOT=%VCPKG_ROOT%
        set "VCPKG_TOOLCHAIN=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
        exit /b 0
    )
)
:: Common install locations
for %%D in (
    "C:\vcpkg"
    "%USERPROFILE%\vcpkg"
    "%LOCALAPPDATA%\vcpkg"
    "C:\src\vcpkg"
    "C:\tools\vcpkg"
) do (
    if exist "%%~D\scripts\buildsystems\vcpkg.cmake" (
        echo [vcpkg] Found at %%~D
        set "VCPKG_ROOT=%%~D"
        set "VCPKG_TOOLCHAIN=%%~D\scripts\buildsystems\vcpkg.cmake"
        exit /b 0
    )
)
echo [vcpkg] Not found. Trying manual SDL2 discovery...
set "VCPKG_TOOLCHAIN="
if defined SDL2_DIR (
    set "SDL2_PREFIX=%SDL2_DIR%"
    echo [SDL2]  Using SDL2_DIR=%SDL2_DIR%
)
exit /b 0

:: ========================================================================
:detect_generator
:: ========================================================================
set "VS_GEN="
:: Try VS 2022 first, then 2019
where cl >nul 2>nul
if %errorlevel%==0 (
    cl 2>&1 | findstr /c:"19.4" >nul && set "VS_GEN=Visual Studio 17 2022"
    cl 2>&1 | findstr /c:"19.3" >nul && set "VS_GEN=Visual Studio 17 2022"
    cl 2>&1 | findstr /c:"19.2" >nul && set "VS_GEN=Visual Studio 16 2019"
)
if defined VS_GENERATOR set "VS_GEN=%VS_GENERATOR%"
if not defined VS_GEN (
    :: Default to Ninja if available, otherwise VS 2022
    where ninja >nul 2>nul
    if %errorlevel%==0 (
        set "VS_GEN=Ninja"
    ) else (
        set "VS_GEN=Visual Studio 17 2022"
    )
)
echo [cmake] Generator: %VS_GEN%
exit /b 0

:: ========================================================================
:configure
:: ========================================================================
call :detect_vcpkg
call :detect_generator
set "GEN_ARGS=-G "%VS_GEN%""
if /i "%VS_GEN%"=="Visual Studio 17 2022" set "GEN_ARGS=-G "%VS_GEN%" -A x64"
if /i "%VS_GEN%"=="Visual Studio 16 2019" set "GEN_ARGS=-G "%VS_GEN%" -A x64"

echo.
echo [cmake] Configuring...
set "TOOLCHAIN_ARG="
if defined VCPKG_TOOLCHAIN set "TOOLCHAIN_ARG=-DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%""
if defined SDL2_PREFIX set "TOOLCHAIN_ARG=-DCMAKE_PREFIX_PATH="%SDL2_PREFIX%""
cmake -B "%BUILD_DIR%" %GEN_ARGS% %TOOLCHAIN_ARG% -S build
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] CMake configure failed.
    echo         Make sure SDL2 is available via vcpkg, SDL2_DIR, or system install.
    exit /b 1
)
exit /b 0

:: ========================================================================
:release_build
:: ========================================================================
echo.
echo ===== Building Release =====
call :configure
if %errorlevel% neq 0 exit /b 1
cmake --build "%BUILD_DIR%" --config Release
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)
echo.
echo [OK] Build complete.
:: Find the exe
for /r "%BUILD_DIR%" %%F in (%PROJECT_NAME%.exe) do (
    echo [OK] %%F
)
exit /b 0

:: ========================================================================
:debug
:: ========================================================================
echo.
echo ===== Building Debug =====
call :configure
if %errorlevel% neq 0 exit /b 1
cmake --build "%BUILD_DIR%" --config Debug
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)
echo.
echo [OK] Debug build complete.
exit /b 0

:: ========================================================================
:release
:: ========================================================================
echo.
echo ===== Building Release + Packaging =====
call :release_build
if %errorlevel% neq 0 exit /b 1
call :package_win
exit /b 0

:: ========================================================================
:next
:: ========================================================================
echo.
echo ===== Building ZX Spectrum Next =====
:: Find z88dk
set "ZCC=zcc"
if defined Z88DK_PATH set "ZCC=%Z88DK_PATH%\bin\zcc"
where "%ZCC%" >nul 2>nul
if %errorlevel% neq 0 (
    echo [ERROR] z88dk not found. Set Z88DK_PATH or add z88dk\bin to PATH.
    exit /b 1
)
echo [z88dk] Using: %ZCC%
if not exist "%NEXT_BUILD_DIR%" mkdir "%NEXT_BUILD_DIR%"
pushd "%NEXT_BUILD_DIR%"
make -f "..\..\build\Makefile.next"
set "RET=%errorlevel%"
popd
if %RET% neq 0 (
    echo [ERROR] Next build failed.
    exit /b 1
)
echo [OK] Next build complete: %NEXT_BUILD_DIR%\game.nex
exit /b 0

:: ========================================================================
:all
:: ========================================================================
call :release_build
if %errorlevel% neq 0 exit /b 1
call :next
:: Package regardless of Next success (may not have z88dk)
call :package_all
exit /b 0

:: ========================================================================
:run
:: ========================================================================
call :release_build
if %errorlevel% neq 0 exit /b 1
echo.
echo [run] Launching...
for /r "%BUILD_DIR%" %%F in (%PROJECT_NAME%.exe) do (
    start "" "%%F"
    exit /b 0
)
echo [ERROR] Could not find %PROJECT_NAME%.exe
exit /b 1

:: ========================================================================
:package_win
:: ========================================================================
echo.
echo [pkg] Packaging Windows release...
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
:: Find exe and SDL2.dll
set "EXE_PATH="
set "DLL_PATH="
for /r "%BUILD_DIR%" %%F in (%PROJECT_NAME%.exe) do set "EXE_PATH=%%F"
for /r "%BUILD_DIR%" %%F in (SDL2.dll) do set "DLL_PATH=%%F"
if not defined EXE_PATH (
    echo [ERROR] Cannot find game.exe
    exit /b 1
)
:: Stage files
set "STAGE=%DIST_DIR%\windgame_win64"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy "%EXE_PATH%" "%STAGE%\" >nul
if defined DLL_PATH copy "%DLL_PATH%" "%STAGE%\" >nul
if exist "README.md" copy "README.md" "%STAGE%\" >nul

:: Create zip (use tar on Win10+ or PowerShell)
set "ZIP_NAME=windgame_win64.zip"
pushd "%DIST_DIR%"
if exist "%ZIP_NAME%" del "%ZIP_NAME%"
powershell -NoProfile -Command "Compress-Archive -Path 'windgame_win64\*' -DestinationPath '%ZIP_NAME%' -Force"
popd
echo [OK] %DIST_DIR%\%ZIP_NAME%
exit /b 0

:: ========================================================================
:package_all
:: ========================================================================
call :package_win
:: Add Next binary if it exists
if exist "%NEXT_BUILD_DIR%\game.nex" (
    echo [pkg] Including Next build...
    set "STAGE=%DIST_DIR%\windgame_next"
    if exist "%STAGE%" rmdir /s /q "%STAGE%"
    mkdir "%STAGE%"
    copy "%NEXT_BUILD_DIR%\game.nex" "%STAGE%\" >nul
    if exist "README.md" copy "README.md" "%STAGE%\" >nul
    pushd "%DIST_DIR%"
    if exist "windgame_next.zip" del "windgame_next.zip"
    powershell -NoProfile -Command "Compress-Archive -Path 'windgame_next\*' -DestinationPath 'windgame_next.zip' -Force"
    popd
    echo [OK] %DIST_DIR%\windgame_next.zip
)
exit /b 0

:: ========================================================================
:clean
:: ========================================================================
echo.
echo ===== Cleaning =====
if exist "%BUILD_DIR%" (
    rmdir /s /q "%BUILD_DIR%"
    echo [OK] Removed %BUILD_DIR%
)
if exist "%NEXT_BUILD_DIR%" (
    rmdir /s /q "%NEXT_BUILD_DIR%"
    echo [OK] Removed %NEXT_BUILD_DIR%
)
if exist "%DIST_DIR%" (
    rmdir /s /q "%DIST_DIR%"
    echo [OK] Removed %DIST_DIR%
)
echo [OK] Clean complete.
exit /b 0

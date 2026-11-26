@echo off
setlocal
rem Builds r6-port on Windows with the MSVC toolchain, without CMake.
rem
rem   build-port-msvc.cmd <disc image> <output directory>
rem
rem The disc image (.chd/.cue/.bin) is the user's own copy of the game. The
rem generated translation unit is written into the output directory, outside
rem this repository, because it is derived from the game's code.

if "%~2"=="" (
    echo usage: build-port-msvc.cmd ^<disc image^> ^<output directory^>
    exit /b 1
)

set "DISC=%~f1"
set "OUT=%~f2"
set "ROOT=%~dp0.."
pushd "%ROOT%"

rem Locate a Visual Studio installation with the C++ workload.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
    echo error: Visual Studio with the C++ workload was not found.
    echo Install "Desktop development with C++" or use the CMake build.
    popd
    exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    popd
    exit /b 2
)

if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /std:c++20 /EHsc /O2 /W3 /Iinclude ^
    /Fo:"%OUT%\\" /Fe:"%OUT%\r6-recomp.exe" ^
    tools\r6-recomp.cpp src\chd_image.cpp
if errorlevel 1 (
    popd
    exit /b 3
)

"%OUT%\r6-recomp.exe" "%DISC%" "%OUT%\translation.cpp"
if errorlevel 1 (
    popd
    exit /b 4
)

cl /nologo /std:c++20 /EHsc /O2 /W3 /bigobj /Iinclude ^
    /DR6_HAS_GENERATED_TRANSLATION=1 /Fo:"%OUT%\\" /Fe:"%OUT%\r6-port.exe" ^
    src\port_main.cpp "%OUT%\translation.cpp" ^
    src\bios_services.cpp src\software_gpu.cpp src\gte.cpp ^
    src\chd_image.cpp src\windows_platform_services.cpp ^
    user32.lib gdi32.lib comdlg32.lib shell32.lib opengl32.lib winmm.lib
if errorlevel 1 (
    popd
    exit /b 5
)

if exist shaders xcopy /y /i /e shaders "%OUT%\shaders" >nul

echo built "%OUT%\r6-port.exe"
echo run:  "%OUT%\r6-port.exe" --disc "%DISC%"
popd
endlocal

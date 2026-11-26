@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > vcvars.log 2>&1
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
if "%~1"=="" (
    echo usage: build-port.cmd ^<generated translation.cpp^> ^<output exe^>
    exit /b 1
)
if "%~2"=="" (
    echo usage: build-port.cmd ^<generated translation.cpp^> ^<output exe^>
    exit /b 1
)
cl /nologo /std:c++20 /EHsc /O2 /W3 /bigobj /Iinclude /DR6_HAS_GENERATED_TRANSLATION=1 /Fe:"%~2" src/port_main.cpp "%~1" src/bios_services.cpp src/software_gpu.cpp src/gte.cpp src/chd_image.cpp src/windows_platform_services.cpp user32.lib gdi32.lib comdlg32.lib shell32.lib opengl32.lib
if errorlevel 1 exit /b 2
echo built %~2

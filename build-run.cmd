@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > vcvars.log 2>&1
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /W3 /bigobj /Iinclude /Fe:build/r6-run.exe src/runner_main.cpp src/mips_interpreter.cpp src/bios_services.cpp src/software_gpu.cpp src/gte.cpp src/chd_image.cpp src/windows_platform_services.cpp user32.lib gdi32.lib comdlg32.lib shell32.lib
if errorlevel 1 exit /b 2
echo built build/r6-run.exe

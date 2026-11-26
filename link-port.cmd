@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > vcvars.log 2>&1
cd /d "%~dp0"
cl /nologo /c /std:c++20 /EHsc /O2 /W3 /Iinclude /DR6_HAS_GENERATED_TRANSLATION=1 /Fo:port_main.obj src/port_main.cpp
if errorlevel 1 exit /b 2
link /nologo /OUT:"%~1" port_main.obj SpiderMan_translation.obj bios_services.obj software_gpu.obj gte.obj chd_image.obj windows_platform_services.obj user32.lib gdi32.lib comdlg32.lib shell32.lib opengl32.lib
if errorlevel 1 exit /b 3
echo linked %~1

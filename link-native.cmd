@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > vcvars.log 2>&1
cd /d "%~dp0"
link /nologo /OUT:build/r6-native.exe native_main.obj SpiderMan_translation.obj mips_interpreter.obj bios_services.obj software_gpu.obj gte.obj chd_image.obj windows_platform_services.obj user32.lib gdi32.lib comdlg32.lib shell32.lib
if errorlevel 1 exit /b 2
echo linked build/r6-native.exe

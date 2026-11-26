@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > vcvars.log 2>&1
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++20 /EHsc /O2 /W4 /Iinclude /Fe:build\r6-recomp.exe tools\r6-recomp.cpp src\chd_image.cpp
if errorlevel 1 exit /b 2
echo built build\r6-recomp.exe

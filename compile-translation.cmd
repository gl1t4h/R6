@echo off
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > vcvars.log 2>&1
cd /d "%~dp0"
cl /nologo /c /std:c++20 /EHsc /O2 /W3 /bigobj /Iinclude /Fo:SpiderMan_translation.obj "%~1" > compile-translation.log 2>&1
if errorlevel 1 exit /b 2
echo compiled SpiderMan_translation.obj

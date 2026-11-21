@echo off
rem Builds gamecache as a proxy for a system DLL.
rem Usage: build.cmd [dllname]   (default: xinput1_4)
rem Needs Visual Studio (Build Tools) with the C++ x64 tools, and Python 3.
setlocal
set PROXY=%~1
if "%PROXY%"=="" set PROXY=xinput1_4
cd /d "%~dp0"
set OUT=%~dp0build\%PROXY%

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo ERROR: vswhere.exe not found - install Visual Studio Build Tools & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if "%VSINSTALL%"=="" (echo ERROR: no Visual Studio with C++ x64 tools found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "proxies\%PROXY%.exports" (
    python -I tools\gen_proxy.py dump %PROXY% || exit /b 1
)
python -I tools\gen_proxy.py gen %PROXY% "%OUT%" || exit /b 1

ml64 /nologo /c /Fo"%OUT%\proxy_stubs.obj" "%OUT%\proxy_stubs.asm" || exit /b 1
rc /nologo /i src /d GC_ORIGINAL_NAME=\"%PROXY%.dll\" /fo"%OUT%\version.res" src\version.rc || exit /b 1
cl /nologo /LD /O2 /W4 /WX /GS /MT /DNDEBUG /Isrc /FI"%OUT%\proxy_exports.h" src\gamecache.c ^
   "%OUT%\proxy_stubs.obj" "%OUT%\version.res" /Fo"%OUT%\gamecache.obj" /Fe"build\%PROXY%.dll" ^
   /link /NOLOGO /DLL /BREPRO kernel32.lib || exit /b 1

del "build\%PROXY%.exp" "build\%PROXY%.lib" 2>nul
echo Built: build\%PROXY%.dll

@echo off
rem Builds sacredfire_dlss.dll with MSVC: build.bat <DLSS SDK folder> <Vulkan include folder>
setlocal
set SDK=%~1
set VKINC=%~2
if "%SDK%"=="" goto usage
if "%VKINC%"=="" goto usage
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc /std:c++20 /LD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%SDK%\include" /I"%VKINC%" ^
   shadps4_dlss_bridge.cpp /Fo"%TEMP%\\" /link "%SDK%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" ^
   advapi32.lib user32.lib legacy_stdio_definitions.lib ^
   /OUT:sacredfire_dlss.dll /IMPLIB:"%TEMP%\sacredfire_dlss.lib" || exit /b 1
echo Built %~dp0sacredfire_dlss.dll
exit /b 0
:usage
echo usage: build.bat ^<DLSS SDK folder^> ^<folder with vulkan\vulkan.h^>
exit /b 2

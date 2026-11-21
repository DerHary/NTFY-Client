@echo off
setlocal
set "WORK=%~dp0..\..\.build\ntfy-client"
set "INNO=%~1"
if not defined INNO if exist "%~dp0..\..\.build\inno\ISCC.exe" set "INNO=%~dp0..\..\.build\inno\ISCC.exe"
if not defined INNO for /f "delims=" %%I in ('where ISCC.exe 2^>nul') do if not defined INNO set "INNO=%%I"
if not defined INNO (
    echo Inno Setup 6.7+ required. Pass the full path to ISCC.exe.
    exit /b 1
)
for %%I in ("%INNO%") do set "INNO=%%~fI"
if not exist "%INNO%" exit /b 1

if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    for /f "delims=" %%I in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%I"
    if not defined VSPATH (
        echo Install Visual Studio Build Tools with C++ and a Windows SDK.
        exit /b 1
    )
    call :init_vs
    if errorlevel 1 exit /b 1
)

if not exist "%WORK%" mkdir "%WORK%"
if not exist "%~dp0..\bin" mkdir "%~dp0..\bin"
pushd "%~dp0" || exit /b 1
rc /nologo /fo "%WORK%\client.res" client.rc
if errorlevel 1 goto :failed
cl /nologo /std:c++17 /permissive- /Zc:__cplusplus /W4 /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE ^
    /Fo"%WORK%\\" /Fe"%WORK%\NtfyWindows.exe" main.cpp config.cpp receiver.cpp settings.cpp "%WORK%\client.res" ^
    /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:NO shell32.lib ole32.lib shlwapi.lib user32.lib gdi32.lib ^
    windowsapp.lib winhttp.lib crypt32.lib powrprof.lib
if errorlevel 1 goto :failed
"%INNO%" /DClientExe="%WORK%\NtfyWindows.exe" setup.iss
if errorlevel 1 goto :failed
popd
exit /b 0

:init_vs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
exit /b %errorlevel%

:failed
popd
exit /b 1

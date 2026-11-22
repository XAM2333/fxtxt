@echo off
rem Build FXTXT and drop the .g1a everywhere it is needed:
rem   out\FXTXT.G1A   local deliverable
rem   H:\FXTXT.G1A   calculator storage (silently skipped when absent)
setlocal
cd /d %~dp0..

set PKG=E:\fxtxtviewer\ref\pkg3\FxStudio-0.2.0-win-x64
set FXCONFIG=Release
set TMP=E:\fxtxtviewer\.tmp
set TEMP=E:\fxtxtviewer\.tmp

python "%PKG%\sdk\native\service\build-native.py" "%PKG%\sdk\native" "E:\fxtxtviewer\work\fxtxt" "E:\fxtxtviewer\work\fxtxt\out" Release
if errorlevel 1 goto fail

copy /y "work\fxtxt\out\artifacts\addin.g1a" "out\FXTXT.G1A" >nul

if exist H:\ (
    copy /y "out\FXTXT.G1A" "H:\FXTXT.G1A" >nul
    if errorlevel 1 echo [build] copy to H: failed & goto fail
    echo [build] copied to H:\FXTXT.G1A
) else (
    echo [build] H: not mounted, skipped
)

for %%A in (out\FXTXT.G1A) do echo [build] ok, %%~zA bytes
exit /b 0

:fail
echo [build] FAILED
exit /b 1

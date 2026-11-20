@echo off
rem Build WLAN_Monitor without Visual Studio IDE.
rem Output: out\x64\Release\WLAN_Monitor.exe (copied to bin\)
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo [ERROR] vcvars64.bat failed
  exit /b 1
)
cd /d "%~dp0"
if not exist out\x64\Release mkdir out\x64\Release

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   src\WLAN_Monitor.cpp ^
   /Fo:out\x64\Release\ ^
   /Fe:out\x64\Release\WLAN_Monitor.exe ^
   /link /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
   wlanapi.lib comctl32.lib shell32.lib user32.lib gdi32.lib
if errorlevel 1 (
  echo [ERROR] build failed
  exit /b 1
)

copy /y out\x64\Release\WLAN_Monitor.exe bin\WLAN_Monitor.exe >nul
echo [OK] out\x64\Release\WLAN_Monitor.exe  copied to bin\

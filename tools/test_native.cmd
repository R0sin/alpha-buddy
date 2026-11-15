@echo off
setlocal
cd /d "%~dp0.."
if not exist ".pio\native-msvc" mkdir ".pio\native-msvc"
if not exist ".pio\libdeps\native\Unity\src\unity.c" (
  echo Unity missing. Install native test dependencies with PlatformIO first.
  exit /b 1
)
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd ".pio\native-msvc"
cl /nologo /std:c++17 /EHsc /W4 /DALPHA_BUDDY_NATIVE /DUNITY_SUPPORT_64 /I..\..\include /I..\libdeps\native\Unity\src ..\..\src\core\Diagnostics.cpp ..\..\src\core\JpegFrameSink.cpp ..\..\src\ptpip\PtpIpProtocol.cpp ..\..\src\sony\SonyLiveViewProbe.cpp ..\..\src\sony\SonyLiveViewStream.cpp ..\libdeps\native\Unity\src\unity.c ..\..\test\test_native\test_native.cpp /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
exit /b %errorlevel%

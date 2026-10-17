@echo off
rem Builds the desktop test tools (fake iPhone sender) into build\tests\.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo || exit /b 1
set R=%~dp0..
set DEPS=%R%\tools\vcpkg_installed\x64-windows-static-md
set OUT=%R%\build\tests
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cd /d "%OUT%\obj"

set CFLAGS=/nologo /c /O2 /MD /W3 /DWIN32 /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS ^
  /D_WINSOCK_DEPRECATED_NO_WARNINGS /DNOHOLD /DPLIST_210 /DPLIST_230 /DOPENSSL_API_COMPAT=0x10101000L ^
  /wd4018 /wd4101 /wd4146 /wd4244 /wd4267 /wd4305 /wd4996 ^
  /I"%R%\src\compat" /I"%R%\src\airplay" /I"%R%\src\airplay\playfair" /I"%R%\src\airplay\llhttp" /I"%DEPS%\include"

cl %CFLAGS% /std:c17 /FI"%R%\src\compat\airserver_force.h" "%R%\src\airplay\*.c" "%R%\src\airplay\playfair\*.c" "%R%\src\airplay\llhttp\*.c" "%R%\src\airplay\mdnsd\*.c" || exit /b 1
cl %CFLAGS% /std:c++17 /EHsc /DNOMINMAX "%R%\test\fakesender\fakesender.cpp" || exit /b 1
rem FFmpeg (avformat/avcodec/swresample) decodes audio files; it also pulls in the Media Foundation,
rem Direct3D and Schannel system libraries it was configured with
link /nologo /out:"%OUT%\fakesender.exe" *.obj /LIBPATH:"%DEPS%\lib" libcrypto.lib plist-2.0.lib openh264.lib ^
  avformat.lib avcodec.lib swresample.lib avutil.lib ^
  mfplat.lib mfuuid.lib strmiids.lib ole32.lib secur32.lib ncrypt.lib windowscodecs.lib ^
  ws2_32.lib iphlpapi.lib crypt32.lib advapi32.lib user32.lib bcrypt.lib || exit /b 1
echo built %OUT%\fakesender.exe

rem the app's H.264 SPS rewrite, checked against FFmpeg's decoder (run build\tests\h264sps_test.exe)
if not exist "%OUT%\obj_sps" mkdir "%OUT%\obj_sps"
cd /d "%OUT%\obj_sps"
cl /nologo /c /O2 /MD /W3 /EHsc /std:c++17 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%R%\src\app" /I"%DEPS%\include" ^
  "%R%\src\app\H264Sps.cpp" "%R%\test\h264sps\h264sps_test.cpp" || exit /b 1
link /nologo /out:"%OUT%\h264sps_test.exe" *.obj /LIBPATH:"%DEPS%\lib" openh264.lib avcodec.lib swresample.lib avutil.lib ^
  mfplat.lib mfuuid.lib strmiids.lib ole32.lib user32.lib bcrypt.lib || exit /b 1
echo built %OUT%\h264sps_test.exe

rem the app's HLS packed-audio to MPEG-TS conversion, checked with FFmpeg (build\tests\hlsaudio_test.exe)
if not exist "%OUT%\obj_hlsaudio" mkdir "%OUT%\obj_hlsaudio"
cd /d "%OUT%\obj_hlsaudio"
cl /nologo /c /O2 /MD /W3 /EHsc /std:c++17 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I"%R%\src\app" /I"%DEPS%\include" ^
  "%R%\src\app\HlsAudio.cpp" "%R%\test\hlsaudio\hlsaudio_test.cpp" || exit /b 1
link /nologo /out:"%OUT%\hlsaudio_test.exe" *.obj /LIBPATH:"%DEPS%\lib" avformat.lib avcodec.lib swresample.lib avutil.lib ^
  mfplat.lib mfuuid.lib strmiids.lib ole32.lib secur32.lib ncrypt.lib crypt32.lib ws2_32.lib user32.lib bcrypt.lib ^
  advapi32.lib || exit /b 1
echo built %OUT%\hlsaudio_test.exe

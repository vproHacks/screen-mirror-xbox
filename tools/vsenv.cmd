@echo off
rem Sets up the Visual Studio 2022 x64 build environment (any edition or the Build Tools, with the
rem C++ tools). Found with vswhere; instances that vswhere reports as incomplete (as on some CI
rem images) and the standard install folders are tried too. Called by the build scripts.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VSDIR=
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -all -products * -version [17.0^,18.0^) -property installationPath`) do if not defined VSDIR set "VSDIR=%%i"
if not defined VSDIR for %%e in (Enterprise Professional Community BuildTools) do if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\%%e"
if not defined VSDIR for %%e in (Enterprise Professional Community BuildTools) do if not defined VSDIR if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e"
if not defined VSDIR (echo Visual Studio 2022 with the C++ tools not found & exit /b 1)
echo using Visual Studio in %VSDIR%
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo

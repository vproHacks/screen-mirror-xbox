@echo off
rem Sets up the Visual Studio 2022 x64 build environment (any edition or the Build Tools, with the
rem C++ tools), found with vswhere. Called by the build scripts.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found: install Visual Studio 2022 or its Build Tools & exit /b 1)
set VSDIR=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -version [17.0^,18.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio 2022 with the C++ tools not found & exit /b 1)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo

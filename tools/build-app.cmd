@echo off
rem Builds the UWP app and its sideload package (build\AppPackages\...).
rem Usage: tools\build-app.cmd [Release|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo || exit /b 1
msbuild "%~dp0..\src\app\ScreenMirrorServer.vcxproj" /nologo /m /v:minimal /p:Configuration=%CONFIG% /p:Platform=x64 ^
  /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly /p:GenerateAppxPackageOnBuild=true %2 %3 %4
exit /b %ERRORLEVEL%

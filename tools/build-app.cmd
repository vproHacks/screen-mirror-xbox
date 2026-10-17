@echo off
rem Builds the UWP app and its sideload package (build\AppPackages\...).
rem Usage: tools\build-app.cmd [Release|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
call "%~dp0vsenv.cmd" || exit /b 1
msbuild "%~dp0..\src\app\ScreenMirrorServer.vcxproj" /nologo /m /v:minimal /p:Configuration=%CONFIG% /p:Platform=x64 ^
  /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly /p:GenerateAppxPackageOnBuild=true %2 %3 %4
exit /b %ERRORLEVEL%

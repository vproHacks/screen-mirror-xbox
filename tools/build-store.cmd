@echo off
rem Builds the Microsoft Store upload package (build\AppPackages\...\*.msixupload).
rem Store packages are not signed locally: Partner Center signs them.
setlocal
call "%~dp0vsenv.cmd" || exit /b 1
msbuild "%~dp0..\src\app\ScreenMirrorServer.vcxproj" /nologo /m /v:minimal /p:Configuration=Release /p:Platform=x64 ^
  /p:AppxBundle=Never /p:UapAppxPackageBuildMode=StoreUpload /p:GenerateAppxPackageOnBuild=true ^
  /p:AppxPackageSigningEnabled=false /p:AppxSymbolPackageEnabled=true %1 %2 %3
exit /b %ERRORLEVEL%

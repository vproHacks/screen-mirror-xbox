@echo off
rem Builds the Microsoft Store upload package (build\AppPackages\...\*.msixupload).
rem Store packages are not signed locally: Partner Center signs them.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -no_logo || exit /b 1
msbuild "%~dp0..\src\app\AirServerXbox.vcxproj" /nologo /m /v:minimal /p:Configuration=Release /p:Platform=x64 ^
  /p:AppxBundle=Never /p:UapAppxPackageBuildMode=StoreUpload /p:GenerateAppxPackageOnBuild=true ^
  /p:AppxPackageSigningEnabled=false /p:AppxSymbolPackageEnabled=true %1 %2 %3
exit /b %ERRORLEVEL%

@echo off
cd /d "%~dp0"

set "VS_PATH=D:\Program Files\Microsoft Visual Studio\2022\Community"
set "MSBUILD=%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe"

for %%f in (*.sln) do set "SLN=%%f"

"%MSBUILD%" "%SLN%" /p:Configuration=Debug /p:Platform=x64 /m
pause
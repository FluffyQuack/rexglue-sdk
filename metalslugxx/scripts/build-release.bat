@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
cmake --preset win-amd64-release -DREXSDK_DIR=../
if errorlevel 1 exit /b 1
cmake --build --preset win-amd64-release
exit /b %errorlevel%
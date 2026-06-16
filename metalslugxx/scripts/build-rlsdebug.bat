@echo off
REM Build the metalslugxx recompiled game from a VS Developer environment.
REM The win-amd64 preset uses bare clang/clang++ + lld-link against MSVC/UCRT,
REM so vcvars64 must populate INCLUDE/LIB/PATH first (see Phase B build-env note).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
cmake --preset win-amd64-relwithdebinfo -DREXSDK_DIR=../
if errorlevel 1 exit /b 1
cmake --build --preset win-amd64-relwithdebinfo
exit /b %errorlevel%

@echo off
rem Builds ragekit with the MSYS2 UCRT64 GCC toolchain (gcc, cmake, ninja from C:\msys64\ucrt64\bin).
setlocal
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"
cd /d "%~dp0"
if not exist build\%CFG% (
    cmake -S . -B build\%CFG% -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
)
cmake --build build\%CFG% || exit /b 1
echo.
echo built build\%CFG%\rk.exe

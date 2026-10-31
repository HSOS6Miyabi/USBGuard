@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake is required. Install Visual Studio 2022 C++ tools and CMake.
  exit /b 1
)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release --parallel || exit /b 1
ctest --test-dir build -C Release --output-on-failure || exit /b 1
if not exist dist mkdir dist
copy /y "build\Release\USBGuardUI.exe" "dist\USBGuardUI.exe" >nul || exit /b 1
copy /y "build\Release\USBGuardMini.exe" "dist\USBGuardMini.exe" >nul || exit /b 1
echo Built and tested. See dist\USBGuardUI.exe and dist\USBGuardMini.exe
exit /b 0

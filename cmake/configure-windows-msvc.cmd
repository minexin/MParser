@echo off
setlocal

where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Run this script from a VS Developer Shell.
  exit /b 1
)

chcp 65001 >nul
cmake --preset windows-msvc-release %*

@echo off
setlocal

where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Run this script from a VS Developer Shell.
  exit /b 1
)

chcp 65001 >nul

set "preset=windows-msvc-release"
set "first=%~1"
if not "%first%"=="" if not "%first:~0,1%"=="-" (
  cmake --preset "%first%" %2 %3 %4 %5 %6 %7 %8 %9
  exit /b %errorlevel%
)

cmake --preset "%preset%" %*

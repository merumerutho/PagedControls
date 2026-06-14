@echo off
REM Build and run the PagedControls host tests.
setlocal
cd /d "%~dp0"
if "%CXX%"=="" set CXX=g++
echo Building PagedControls tests...
%CXX% -std=c++17 -O2 -Wall test_paged_controls.cpp -o test_paged_controls.exe || exit /b 1
echo.
test_paged_controls.exe || exit /b 1

@echo off
REM build.bat -- Build Litesrpent on Windows using MinGW-w64 GCC
REM
REM Usage:
REM   build.bat              -- standard build
REM   build.bat release      -- optimized release build
REM   build.bat clean        -- remove build artifacts
REM   build.bat amalgamate   -- create single-file amalgamation

setlocal enabledelayedexpansion

set PROJECT_ROOT=%~dp0
set SRC=%PROJECT_ROOT%src
set INC=%PROJECT_ROOT%include
set BUILD=%PROJECT_ROOT%build
set OUT=%BUILD%\litesrpent.exe

REM Try to find GCC: bundled portable MinGW first, then PATH
set GCC=
if exist "%PROJECT_ROOT%third_party\mingw64\bin\gcc.exe" (
    set GCC=%PROJECT_ROOT%third_party\mingw64\bin\gcc.exe
    echo Using bundled MinGW: %GCC%
) else (
    where gcc >nul 2>&1
    if %errorlevel% equ 0 (
        set GCC=gcc
        echo Using system GCC
    )
)
if not defined GCC (
    REM Try MSVC
    where cl >nul 2>&1
    if %errorlevel% equ 0 (
        echo Using MSVC cl.exe
        goto :msvc_build
    )
    echo ERROR: No C compiler found. Install MinGW-w64 or MSVC.
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"

set SOURCES=
for %%f in (%SRC%\*.c) do set SOURCES=!SOURCES! "%%f"

set CFLAGS=-I"%INC%" -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -D_CRT_SECURE_NO_WARNINGS
set LIBS=-lm -lkernel32 -luser32 -lgdi32

if "%1"=="clean" (
    echo Cleaning build artifacts...
    if exist "%BUILD%" rd /s /q "%BUILD%"
    echo Done.
    exit /b 0
)

if "%1"=="amalgamate" (
    echo Running amalgamation script...
    bash "%PROJECT_ROOT%tools\amalgamate.sh"
    exit /b %errorlevel%
)

if "%1"=="release" (
    echo Building Litesrpent [RELEASE]...
    set CFLAGS=!CFLAGS! -O2 -DNDEBUG -flto
) else (
    echo Building Litesrpent [DEBUG]...
    set CFLAGS=!CFLAGS! -g -O0
)

echo Compiling...
"%GCC%" %CFLAGS% %SOURCES% -o "%OUT%" %LIBS%
if %errorlevel% neq 0 (
    echo BUILD FAILED
    exit /b 1
)
echo.
echo Build successful: %OUT%
"%OUT%" --version
exit /b 0

:msvc_build
if not exist "%BUILD%" mkdir "%BUILD%"
set SOURCES=
for %%f in (%SRC%\*.c) do set SOURCES=!SOURCES! "%%f"
cl /nologo /I"%INC%" /W3 /D_CRT_SECURE_NO_WARNINGS %SOURCES% /Fe:"%OUT%" /link kernel32.lib user32.lib gdi32.lib
if %errorlevel% neq 0 (
    echo BUILD FAILED
    exit /b 1
)
echo Build successful: %OUT%
"%OUT%" --version
exit /b 0

@echo off
setlocal

REM --- Configuration ---
REM Set the root path for Windows Kits (containing WDK)
set "WDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
REM Set the target architecture (x64, x86, arm64) - You chose x64
set "TARGET_ARCH=x64"
REM Set the build configuration (Release, Debug) - You chose Release
set "BUILD_TYPE=Release"
REM Set the name for your driver project/output file
set "TARGET_NAME=BandwidthThrottler"
REM Set the C source file name (must be in the same directory as this bat file)
set "SOURCE_FILE=BandwidthThrottler.c"
REM --- End Configuration ---

REM --- Internal Setup ---
REM Determine WDK environment type (fre=Release, chk=Debug)
set "WDK_ENV_TYPE=fre"
if /I "%BUILD_TYPE%"=="Debug" set "WDK_ENV_TYPE=chk"

REM Construct expected path to setenv.bat
set "SETENV_PATH=%WDK_ROOT%\bin\setenv.bat"

REM Verify WDK Root and setenv.bat exists
if not exist "%SETENV_PATH%" (
    echo ERROR: WDK setenv.bat not found at "%SETENV_PATH%".
    echo Please verify the WDK_ROOT path at the top of this script.
    goto :eof
)

REM --- Create SOURCES file ---
echo Creating temporary SOURCES file...
(
    echo TARGETNAME=%TARGET_NAME%
    echo TARGETTYPE=DRIVER
    echo TARGETPATH=OBJ
    echo NTKERN=1
    echo WFPKMCLIB=1
    echo TARGETLIBS=$(SDK_LIB_PATH)\uuid.lib $(SDK_LIB_PATH)\rpcrt4.lib
    echo LINKER_FLAGS=/INTEGRITYCHECK
    echo SOURCES=%SOURCE_FILE%
) > SOURCES

if not exist SOURCES (
    echo ERROR: Could not create the SOURCES file. Check permissions.
    goto :eof
)

REM --- Setup WDK Build Environment ---
echo Setting up WDK build environment (%TARGET_ARCH% %WDK_ENV_TYPE% build)...
call "%SETENV_PATH%" %WDK_ROOT% %WDK_ENV_TYPE% %TARGET_ARCH%

REM Check if setenv was successful
if "%ERRORLEVEL%" NEQ "0" (
   echo ERROR: Failed to set up WDK build environment using setenv.bat. Check script output above.
   goto :Cleanup
)
REM Check for a variable typically set by setenv.bat
if not defined BUILD_DEFAULT (
    echo ERROR: WDK environment variable BUILD_DEFAULT not set. setenv.bat might have failed.
    goto :Cleanup
)

REM --- Clean Previous Build ---
echo Cleaning previous build target (%WDK_ENV_TYPE%/%TARGET_ARCH%)...
build -cZ

if "%ERRORLEVEL%" NEQ "0" (
    echo Warning: Cleaning previous build failed (or no previous build existed), continuing anyway...
)

REM --- Run Build ---
echo Starting %BUILD_TYPE% build for %TARGET_ARCH%...
build

REM --- Check Build Result ---
if errorlevel 1 (
    echo.
    echo **************************
    echo * BUILD FAILED      *
    echo **************************
    echo Check the build output above and the log file (e.g., build%WDK_ENV_TYPE%_%TARGET_ARCH%.log)
    if defined O (
      echo Build output might be in: %O%
    ) else {
      echo Build output directory variable 'O' not found.
    }

    goto :Cleanup
) else (
    echo.
    echo **************************
    echo * BUILD SUCCESSFUL     *
    echo **************************
    REM Provide path based on common WDK variables set by setenv.bat
    if defined O (
      echo Driver output directory: %O%
      echo Driver file should be: %O%\%TARGET_NAME%.sys
      if exist "%O%\%TARGET_NAME%.sys" (
          echo Found: "%O%\%TARGET_NAME%.sys"
      ) else (
          echo Warning: Expected driver file not found in output directory.
      )
    ) else (
       echo Build succeeded, but output directory variable 'O' not found.
       echo Driver should be located in the build output directory (e.g., obj%WDK_ENV_TYPE%_%TARGET_ARCH%\%TARGET_ARCH%\%BUILD_TYPE%)
    )
)

:Cleanup
REM Clean up the SOURCES file
if exist SOURCES del SOURCES
echo Temporary SOURCES file deleted.

endlocal
echo.
echo Build process finished.
pause

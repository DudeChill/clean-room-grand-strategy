@echo off
rem One-click launcher for the Windows build.
rem
rem Finds game.exe next to this file, picks a free TCP port (8080 first, then
rem 8081..8089), runs the game in this window, and pauses at the end so the
rem window stays open and any error can be read. Plain cmd.exe, Windows 10/11.

setlocal
cd /d "%~dp0"

set "GAME=%~dp0game.exe"

if not exist "%GAME%" (
    echo.
    echo   game.exe was not found next to play.bat.
    echo   Expected it at: "%GAME%"
    echo   Unzip the whole archive, keeping the files together, and try again.
    echo.
    pause
    exit /b 1
)

rem --- pick a free port: 8080, then 8081..8089 --------------------------------
rem netstat is part of Windows; a port is taken when something is LISTENING on it.
set "NETSTAT_FILE=%TEMP%\hoi_play_netstat_%RANDOM%.txt"
netstat -ano > "%NETSTAT_FILE%" 2>nul

set "PORT="
for /L %%P in (8080,1,8089) do (
    if not defined PORT (
        findstr /C:":%%P " "%NETSTAT_FILE%" | findstr /C:"LISTENING" >nul 2>&1
        if errorlevel 1 set "PORT=%%P"
    )
)
del "%NETSTAT_FILE%" >nul 2>&1

if not defined PORT (
    echo.
    echo   No free TCP port in the range 8080-8089.
    echo   Close whatever is using them and run play.bat again.
    echo.
    pause
    exit /b 1
)

echo.
echo   Starting the game on http://127.0.0.1:%PORT%
echo   Your browser should open by itself. If it does not, type that address in.
echo   Keep this window open while you play; close it to stop the game.
echo.

"%GAME%" --play --port %PORT%
set "CODE=%ERRORLEVEL%"

echo.
if "%CODE%"=="0" (
    echo   The game has stopped.
) else (
    echo   The game exited with error code %CODE%.
    echo   The last message above says what went wrong.
)
echo.
pause

endlocal & exit /b %CODE%

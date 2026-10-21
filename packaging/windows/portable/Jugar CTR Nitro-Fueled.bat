@echo off
rem CTR Nitro-Fueled PC sin el launcher (el launcher, "Crash Team Racing Nitro-Fueled.exe", hace lo mismo con ventana).
rem El juego mergeado va en .\CUSA13795-1.21 (o define BB_GAME_DIR).
rem Menu del port dentro del juego: tecla Insert o L3+R3 en el mando.
setlocal
cd /d "%~dp0"
if "%BB_GAME_DIR%"=="" set "BB_GAME_DIR=%~dp0CUSA13795-1.21"
set "PYEXE=python"
set "PYARGS="
if exist "%~dp0python\python.exe" (
    set "PYEXE=%~dp0python\python.exe"
) else (
    where py >nul 2>nul && set "PYEXE=py" && set "PYARGS=-3"
)
"%PYEXE%" %PYARGS% -X utf8 run.py %*
if %errorlevel% neq 0 (
    echo.
    echo El juego se cerro con un error. Revisa last_run.log y LEEME.txt.
    pause
)

@echo off
rem Crea .\CUSA13795-1.21 a partir de tu dump base (.\CUSA13795) y la update 1.21 (.\CUSA13795-patch).
rem Usa enlaces duros: NO duplica espacio, pero las tres carpetas deben estar en la misma unidad.
rem Necesita Python 3 (py --version). Solo hay que hacerlo una vez.
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if %errorlevel%==0 (set "PY=py -3") else (set "PY=python")
%PY% tools\merge_update.py "%~dp0CUSA13795" "%~dp0CUSA13795-patch" "%~dp0CUSA13795-1.21"
if %errorlevel% neq 0 (
    echo.
    echo Fallo el mergeado. Revisa que existan las carpetas CUSA13795 y CUSA13795-patch junto a este .bat.
    pause
) else (
    echo.
    echo Listo: carpeta CUSA13795-1.21 creada. Ya puedes abrir Jugar CTR Nitro-Fueled.bat.
    pause
)

@echo off
rem Hellwalker: lanzador transaccional (mantiene la carpeta del juego 100%% vanilla en reposo).
rem Acepta flags opcionales: -BuildCache, -ForceRebuild, -Restore
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0hellwalker_session.ps1" %*
if errorlevel 1 (
    echo.
    pause
)
endlocal

@echo off
rem Double-clique ce fichier pour compiler Reddit3DS.cia (via WSL, rien a installer).
where wsl >NUL 2>NUL
if errorlevel 1 (
    echo WSL n'est pas installe. Ouvre PowerShell en administrateur et tape : wsl --install
    pause
    exit /b 1
)
wsl --cd "%~dp0" -e bash ./build.sh
if errorlevel 1 (
    echo.
    echo ECHEC de la compilation.
) else (
    echo.
    echo OK : Reddit3DS.cia est dans ce dossier.
)
pause

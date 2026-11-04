@echo off
rem Por que o jogo fica mais lento com janela. Na tomada, launcher fechado, sem usar o notebook. Uns 16 minutos.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rodar.ps1" -Set 11 -Exe "%~dp0gt4-t2.exe"
pause

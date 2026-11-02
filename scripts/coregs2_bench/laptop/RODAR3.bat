@echo off
rem Terceiro benchmark do CoreGS 2.0 no notebook. Deixe na tomada, feche o launcher e o jogo. Demora uns 15 minutos.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rodar.ps1" -Set 3
pause

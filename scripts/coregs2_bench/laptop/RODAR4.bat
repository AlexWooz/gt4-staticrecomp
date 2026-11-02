@echo off
rem Quarto benchmark do CoreGS 2.0 no notebook. Deixe na tomada, feche o launcher e o jogo. Demora uns 25 minutos.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rodar.ps1" -Set 4
pause

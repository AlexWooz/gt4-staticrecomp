@echo off
rem O video atras da escolha da pista, decodificador antigo e novo. Na tomada, launcher fechado, sem usar o notebook. Uns 8 minutos.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rodar.ps1" -Set 13 -Exe "%~dp0gt4-t2.exe"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rodar.ps1" -Set 13 -Exe "%~dp0gt4-t3.exe"
pause

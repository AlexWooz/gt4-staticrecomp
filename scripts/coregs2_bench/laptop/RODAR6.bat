@echo off
rem Mede quanto custa apresentar na tela no notebook. Na tomada, launcher fechado, sem usar o notebook. Uns 12 minutos.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rodar.ps1" -Set 8
pause

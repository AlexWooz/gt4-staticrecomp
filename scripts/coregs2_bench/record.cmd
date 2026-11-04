@echo off
rem Runs record-input.sh from PowerShell or cmd: record.cmd <name>
setlocal
pushd "%~dp0"
"C:\Program Files\Git\bin\bash.exe" ./record-input.sh %*
popd

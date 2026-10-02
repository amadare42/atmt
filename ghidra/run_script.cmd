@echo off
rem ghidra/run_script.cmd - run one of ghidra\scripts against the analysed project (a few seconds).
rem
rem   ghidra\run_script.cmd decompile.java 0x0053c0b0
rem   ghidra\run_script.cmd find_strings.java save%03d
rem
rem The analysis comes from run_import.cmd (-noanalysis here). Same environment: GHIDRA_ROOT, and
rem optionally GHIDRA_PROJECT_DIR and JAVA_HOME. Output goes to the console; redirect it to a file
rem (ghidra\logs\ is ignored by git).
setlocal
if "%GHIDRA_ROOT%"=="" (echo set GHIDRA_ROOT to your Ghidra folder first & exit /b 1)
if "%GHIDRA_PROJECT_DIR%"=="" set "GHIDRA_PROJECT_DIR=%~dp0proj"
if "%JAVA_HOME%"=="" for /d %%d in ("%GHIDRA_ROOT%\jdk-*") do set "JAVA_HOME=%%d"
call "%GHIDRA_ROOT%\support\analyzeHeadless.bat" "%GHIDRA_PROJECT_DIR%" cs1 -process ed8.exe -noanalysis -scriptPath "%~dp0scripts" -postScript %*
endlocal

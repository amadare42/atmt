@echo off
rem ghidra/run_import.cmd - analyse ed8.exe into a Ghidra project, headless (takes minutes).
rem
rem   set GHIDRA_ROOT=C:\path\to\ghidra_12.x_PUBLIC
rem   ghidra\run_import.cmd
rem
rem Needs GHIDRA_ROOT. The game's exe is expected at ghidra\ed8.exe (copy it from your install; it is
rem not part of the repository). Optional: GHIDRA_PROJECT_DIR (default ghidra\proj), JAVA_HOME (default
rem the JDK that ships inside the Ghidra release folder, jdk-*).
setlocal
if "%GHIDRA_ROOT%"=="" (echo set GHIDRA_ROOT to your Ghidra folder first & exit /b 1)
if "%GHIDRA_PROJECT_DIR%"=="" set "GHIDRA_PROJECT_DIR=%~dp0proj"
if "%JAVA_HOME%"=="" for /d %%d in ("%GHIDRA_ROOT%\jdk-*") do set "JAVA_HOME=%%d"
if not exist "%GHIDRA_PROJECT_DIR%" mkdir "%GHIDRA_PROJECT_DIR%"
call "%GHIDRA_ROOT%\support\analyzeHeadless.bat" "%GHIDRA_PROJECT_DIR%" cs1 -import "%~dp0ed8.exe" -analysisTimeoutPerFile 3600
endlocal

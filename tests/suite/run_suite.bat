@echo off
rem Runs tests\suite\run_suite.py with whichever Python 3.8+ is installed. Tried in order:
rem   %NESSYS_PYTHON% (set it to a python.exe if yours is somewhere unusual),
rem   py -3 (the python.org launcher), python (python.org or Scoop on PATH).
rem Arguments go straight to run_suite.py, for example:
rem   tests\suite\run_suite.bat --roms nes-test-roms=C:\src\nes-test-roms --roms AccuracyCoin=C:\src\AccuracyCoin
setlocal
set "SUITE=%~dp0run_suite.py"
set "CHECK=import sys; sys.exit(sys.version_info < (3, 8))"

if not defined NESSYS_PYTHON goto try_py
"%NESSYS_PYTHON%" -c "%CHECK%" >nul 2>nul
if errorlevel 1 goto bad_env
"%NESSYS_PYTHON%" "%SUITE%" %*
exit /b %errorlevel%

:try_py
py -3 -c "%CHECK%" >nul 2>nul
if errorlevel 1 goto try_python
py -3 "%SUITE%" %*
exit /b %errorlevel%

:try_python
python -c "%CHECK%" >nul 2>nul
if errorlevel 1 goto none
python "%SUITE%" %*
exit /b %errorlevel%

:bad_env
echo run_suite: NESSYS_PYTHON is set to "%NESSYS_PYTHON%", which is not Python 3.8 or newer.
exit /b 9

:none
echo run_suite: no Python 3.8 or newer found ("py -3" and "python" were tried).
echo Install Python from python.org or with Scoop, or set NESSYS_PYTHON to your python.exe.
exit /b 9

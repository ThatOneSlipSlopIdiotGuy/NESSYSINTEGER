@echo off
rem NESSYSINTEGER build script: uses MSVC (cl) if available, otherwise MinGW-w64 gcc.
setlocal
cd /d "%~dp0"
set CORE=src\cpu.c src\ppu.c src\apu.c src\cart.c src\nes.c src\boards.c src\boards_konami.c src\boards_misc.c src\boards_mmc5.c src\ppuview.c
where cl >nul 2>nul
if %errorlevel%==0 goto msvc
where gcc >nul 2>nul
if %errorlevel%==0 goto mingw
echo.
echo No C compiler found. Options:
echo   - Visual Studio / Build Tools: open "x64 Native Tools Command Prompt for VS" and run build.bat there
echo   - w64devkit or MSYS2 (MinGW-w64): run build.bat or make from that shell
exit /b 1
:msvc
echo Building with MSVC...
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS %CORE% win\main.c /Fe:NESSYSINTEGER.exe /link /SUBSYSTEM:WINDOWS || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS %CORE% tools\nessys_cli.c /Fe:nessys_cli.exe || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS %CORE% tests\dma_trace.c /Fe:dma_trace.exe || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS %CORE% tools\accuracycoin_run.c /Fe:accuracycoin_run.exe || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS %CORE% tests\mapper_test.c /Fe:mapper_test.exe || exit /b 1
del /q *.obj >nul 2>nul
goto done
:mingw
echo Building with gcc...
gcc -std=c99 -O2 -Wall -mwindows -o NESSYSINTEGER.exe %CORE% win\main.c -lgdi32 -luser32 -lwinmm -lcomdlg32 -lshell32 || exit /b 1
gcc -std=c99 -O2 -Wall -o nessys_cli.exe %CORE% tools\nessys_cli.c || exit /b 1
gcc -std=c99 -O2 -Wall -o dma_trace.exe %CORE% tests\dma_trace.c || exit /b 1
gcc -std=c99 -O2 -Wall -o accuracycoin_run.exe %CORE% tools\accuracycoin_run.c || exit /b 1
gcc -std=c99 -O2 -Wall -o mapper_test.exe %CORE% tests\mapper_test.c || exit /b 1
:done
echo.
echo Done: NESSYSINTEGER.exe, nessys_cli.exe, dma_trace.exe, accuracycoin_run.exe and mapper_test.exe
echo Try: NESSYSINTEGER.exe tests\roms\selftest.nes

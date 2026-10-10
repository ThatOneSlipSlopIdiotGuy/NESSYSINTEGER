# NESSYSINTEGER - MinGW-w64 (w64devkit / MSYS2) or Linux/macOS (CLI only)
CC     = gcc
CFLAGS = -std=c99 -O2 -Wall
CORE   = src/cpu.c src/ppu.c src/apu.c src/cart.c src/nes.c src/boards.c src/boards_konami.c src/boards_misc.c src/boards_mmc5.c src/ppuview.c
HDR    = src/nes.h src/board.h src/chips.h

ifeq ($(OS),Windows_NT)
all: NESSYSINTEGER.exe nessys_cli.exe dma_trace.exe accuracycoin_run.exe mapper_test.exe
NESSYSINTEGER.exe: $(CORE) win/main.c $(HDR)
	$(CC) $(CFLAGS) -mwindows -o $@ $(CORE) win/main.c -lgdi32 -luser32 -lwinmm -lcomdlg32 -lshell32
nessys_cli.exe: $(CORE) tools/nessys_cli.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tools/nessys_cli.c
dma_trace.exe: $(CORE) tests/dma_trace.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tests/dma_trace.c
accuracycoin_run.exe: $(CORE) tools/accuracycoin_run.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tools/accuracycoin_run.c
mapper_test.exe: $(CORE) tests/mapper_test.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tests/mapper_test.c
CLI = ./nessys_cli.exe
TRACE = ./dma_trace.exe
MAPT = ./mapper_test.exe
else
all: nessys_cli dma_trace accuracycoin_run mapper_test
nessys_cli: $(CORE) tools/nessys_cli.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tools/nessys_cli.c -lm
dma_trace: $(CORE) tests/dma_trace.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tests/dma_trace.c -lm
accuracycoin_run: $(CORE) tools/accuracycoin_run.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tools/accuracycoin_run.c -lm
mapper_test: $(CORE) tests/mapper_test.c $(HDR)
	$(CC) $(CFLAGS) -o $@ $(CORE) tests/mapper_test.c -lm
CLI = ./nessys_cli
TRACE = ./dma_trace
MAPT = ./mapper_test
endif

test: all
	$(CLI) tests/roms/selftest.nes -q
	$(CLI) tests/roms/mmc3test.nes -q
	$(CLI) tests/roms/mmc1test.nes -q
	$(TRACE)
	$(MAPT)

roms:
	python3 tests/make_test_roms.py tests/roms

clean:
	rm -f NESSYSINTEGER.exe nessys_cli.exe nessys_cli dma_trace.exe dma_trace accuracycoin_run.exe accuracycoin_run mapper_test.exe mapper_test *.o *.obj

# Build the recompiled game. Run `python -m recomp.lift ...` first to produce ../generated/.
CC ?= gcc
CFLAGS ?= -O1 -g0 -fno-strict-aliasing -fwrapv -w -Iruntime
GEN := $(wildcard ../generated/*.c)
OBJ := $(patsubst ../generated/%.c,../build/%.o,$(GEN)) ../build/runtime.o ../build/hle.o ../build/ios.o ../build/threads.o ../build/dsp.o ../build/gx.o

../build/ttpc: $(OBJ)
	$(CC) -o $@ $^ -lm

../build/%.o: ../generated/%.c runtime/cpu.h
	@mkdir -p ../build
	$(CC) $(CFLAGS) -c $< -o $@

../build/runtime.o: runtime/runtime.c runtime/cpu.h
	@mkdir -p ../build
	$(CC) -O2 -g -Wall -Wno-unused -c $< -o $@

../build/hle.o: runtime/hle.c runtime/cpu.h
	@mkdir -p ../build
	$(CC) -O2 -g -Wall -Wno-unused -c $< -o $@

../build/ios.o: runtime/ios.c runtime/cpu.h
	@mkdir -p ../build
	$(CC) -O2 -g -Wall -Wno-unused -c $< -o $@

../build/threads.o: runtime/threads.c runtime/cpu.h
	@mkdir -p ../build
	$(CC) -O2 -g -Wall -Wno-unused -c $< -o $@

../build/dsp.o: runtime/dsp.c runtime/cpu.h
	@mkdir -p ../build
	$(CC) -O2 -g -Wall -Wno-unused -c $< -o $@

../build/gx.o: runtime/gx.c runtime/cpu.h
	@mkdir -p ../build
	$(CC) -O2 -g -Wall -Wno-unused -c $< -o $@

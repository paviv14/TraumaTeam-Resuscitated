# Build the recompiled game. Run `python -m recomp.lift ...` first to produce ../generated/.
#   make -f build.mk -j                      Linux, SDL2 window
#   make -f build.mk -j SDL=0                headless
#   make -f build.mk -j TARGET=windows       Windows .exe (mingw-w64 cross compiler, see docs/RECOMP.md)
TARGET ?= linux
SDL ?= 1

ifeq ($(TARGET),windows)
CC := x86_64-w64-mingw32-gcc
WINDRES ?= x86_64-w64-mingw32-windres
BUILD ?= ../build-win
EXE ?= $(BUILD)/Resuscitated.exe
SDL_DIR ?= ../third_party/SDL2-win
SDL_CFLAGS ?= -I$(SDL_DIR)/include -DSDL_MAIN_HANDLED
SDL_LIBS ?= -L$(SDL_DIR)/lib -lSDL2
LDFLAGS += -static-libgcc -Wl,--stack,16777216 -mwindows   # a windowed program: no console (logs to ttpc.log)
WINRES := $(BUILD)/resuscitated_res.o
VR ?= 1   # VR=0: no VR mode and no OpenXR (the release build Setup makes)
ifeq ($(VR),1)
XR_DIR ?= ../third_party/openxr
RTFLAGS += -I$(XR_DIR)
LIBS += $(XR_DIR)/libopenxr_loader.dll.a
else
RTFLAGS += -DTTPC_NO_VR
endif
LIBS += -lopengl32 -lhid -lsetupapi -lwinmm
else
CC := gcc
BUILD ?= ../build
EXE ?= $(BUILD)/ttpc
SDL_CFLAGS ?= $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS ?= $(shell pkg-config --libs sdl2 2>/dev/null)
endif

OMP ?= -fopenmp
RTFLAGS += $(OMP)
LDFLAGS += $(OMP)

ifeq ($(SDL),1)
RTFLAGS += -DTTPC_SDL $(SDL_CFLAGS)
LIBS += $(SDL_LIBS)
endif

CFLAGS ?= -O1 -g0 -fno-strict-aliasing -fwrapv -w -Iruntime
RTCFLAGS := -O2 -g -Wall -Wno-unused -Wno-misleading-indentation -fno-strict-aliasing -fwrapv -Iruntime $(RTFLAGS) $(EXTRA)
RUNTIME := port runtime hle ios threads dsp ax gx disc video gl texpack platform wiimote menu vr forensics motion wiimote_hid speaker tuttext
GEN := $(wildcard ../generated/*.c)
OBJ := $(patsubst ../generated/%.c,$(BUILD)/%.o,$(GEN)) $(patsubst %,$(BUILD)/rt_%.o,$(RUNTIME))

$(EXE): $(OBJ) $(WINRES)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS) -lm

$(BUILD)/resuscitated_res.o: res/resuscitated.rc res/resuscitated.ico
	@mkdir -p $(BUILD)
	$(WINDRES) -I res res/resuscitated.rc -O coff -o $@

$(BUILD)/%.o: ../generated/%.c runtime/cpu.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(EXTRA) -c $< -o $@

$(BUILD)/rt_%.o: runtime/%.c runtime/cpu.h runtime/platform.h runtime/port.h runtime/gx_state.h
	@mkdir -p $(BUILD)
	$(CC) $(RTCFLAGS) -c $< -o $@

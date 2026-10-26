# jwemu: the emulator plus the Win32 GUI layer, native build.
#   sh tools/build.sh            (low priority, parallel)
CXX ?= g++
CC ?= gcc
OPT ?= -O2
CXXFLAGS = -std=c++17 $(OPT) -Wall -Wextra -Wno-unused-parameter -Isrc -Isrc/emu -Isrc/win32 -Isrc/third_party -Isrc/third_party/libtera -MMD -MP
CFLAGS = $(OPT) -Isrc/third_party/libtera -MMD -MP
OBJ = build/obj

EMU_SRC = $(filter-out src/emu/main.cpp,$(wildcard src/emu/*.cpp))
GUI_SRC = $(wildcard src/win32/*.cpp)
HOST_SRC = src/host/main_gui.cpp
C_SRC = src/third_party/libtera/kanji.c

OBJS = $(patsubst src/%.cpp,$(OBJ)/%.o,$(EMU_SRC) $(GUI_SRC) $(HOST_SRC)) $(patsubst src/%.c,$(OBJ)/%.o,$(C_SRC))

all: waemu.exe

waemu.exe: $(OBJS)
	$(CXX) $(OPT) -o $@ $^ -lgdi32 -luser32 -lshell32

$(OBJ)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

clean:
	rm -rf build waemu.exe

-include $(OBJS:.o=.d)

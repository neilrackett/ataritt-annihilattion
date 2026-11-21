# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
# AnnihilaTTion: the Total Annihilation demo for the Atari TT.
#
# Build with the gcc 13 mintelf cross compiler, here through
# atarist-toolkit-docker:  STCMD_NO_TTY=1 stcmd make
# The program is dist/ANNIHILA.TOS, beside the data tools/convert.py writes
# there (build.sh): dist holds only what the TT needs, and the objects go in
# build.

CC := m68k-atari-mintelf-gcc
CFLAGS := -m68020-60 -mtune=68030 -O2 -fomit-frame-pointer -Wall -Wextra -Wno-unused-parameter -std=gnu99
SRC := src/main.c src/game.c src/sim.c src/gfx.c src/data.c src/tt.c src/snd.c
HDR := $(wildcard src/*.h)
OBJ := $(SRC:src/%.c=build/%.o)

dist/ANNIHILA.TOS: $(OBJ)
	mkdir -p dist
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c $(HDR) Makefile
	mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build dist/ANNIHILA.TOS

# In Hatari, run on the host rather than through stcmd: each builds first
# with build.sh, which compiles through stcmd itself and brings dist up to date.
# MACHINE=falcon runs either on a Falcon, as tools/hatari_tt.sh says.
#   make play                        the game in a Hatari window
#   make test TEST=tests/win2.txt    a test script headless; NAME names the run
TEST ?= tests/smoke1.txt

play:
	./build.sh
	tools/play_hatari.sh

test:
	./build.sh
	tools/test_hatari.sh $(or $(NAME),$(basename $(notdir $(TEST)))) $(TEST)

.PHONY: clean play test

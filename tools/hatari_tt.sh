# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later

# hatari_tt.sh: sourced by test_hatari.sh and play_hatari.sh.
#
# Sets HERE and REPO (the project's folder), GAME (the build build.sh
# makes), TOS, HATARI and HATARI_FLAGS, Hatari's options for the
# target TT: TOS 3.06, 2 MB ST-RAM, 4 MB TT-RAM, VGA monitor, the 68030's
# caches. With MACHINE=falcon they are a 14 MB Falcon's on a VGA monitor
# instead, running EmuTOS: the Falcon is an experiment (see the README).
#
# Environment: MACHINE (tt, the default, or falcon), TOS (default: the TOS
# 3.06 image of the ataritt-doom checkout beside this one; for a Falcon,
# EmuTOS 512K from the atarist-stdl checkout), HATARI (default: hatari on
# the PATH, else the Mac app).
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPO=$HERE
GAME=$REPO/local/ANNIHILA
HATARI=${HATARI:-$(command -v hatari || echo /Applications/Hatari.app/Contents/MacOS/hatari)}
if [ "${MACHINE:-tt}" = falcon ]; then
  TOS=${TOS:-$REPO/../atarist-stdl/tmp/tos/etos512uk.img}
  MACHINE_FLAGS=(--machine falcon --fpu none --memsize 14)
else
  TOS=${TOS:-$REPO/../ataritt-doom/tmp/tos/tos306uk.img}
  MACHINE_FLAGS=(--machine tt --memsize 2 --ttram 4 --addr24 off)
fi
HATARI_FLAGS=(--tos "$TOS" "${MACHINE_FLAGS[@]}" --mmu off --monitor vga --cpu-exact on
  --data-cache on --fast-boot on --confirm-quit false)

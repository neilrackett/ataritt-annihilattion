// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The download: the converted files in an ANNIHILA folder's DATA folder,
// and ANNIHILA.TOS beside it when the page could fetch it.

import { zipSync, type Zippable } from 'fflate';
import type { OutFile } from './convert';
import { PROGRAM } from './program';

export const FOLDER = 'ANNIHILA';

/** The folder beside ANNIHILA.TOS that the program reads its data from. */
export const DATA = 'DATA';

/**
 * Every entry is stored, not deflated, as RExtract's are: a strict reader
 * such as Windows' own refuses some of what fflate deflates, and most of
 * this data would not shrink much anyway.
 */
export function buildZip(files: OutFile[], program: Uint8Array | null = null): Uint8Array {
	const entries: Zippable = {};
	if (program) {
		entries[`${FOLDER}/${PROGRAM}`] = program;
	}
	for (const file of files) {
		entries[`${FOLDER}/${DATA}/${file.name}`] = file.bytes;
	}
	return zipSync(entries, { level: 0 });
}

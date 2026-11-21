// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The download: the converted files in an ANNIHILA folder, where
// ANNIHILA.TOS goes too.

import { zipSync, type Zippable } from 'fflate';
import type { OutFile } from './convert';

export const FOLDER = 'ANNIHILA';

/**
 * Every entry is stored, not deflated, as RExtract's are: a strict reader
 * such as Windows' own refuses some of what fflate deflates, and most of
 * this data would not shrink much anyway.
 */
export function buildZip(files: OutFile[]): Uint8Array {
	const entries: Zippable = {};
	for (const file of files) {
		entries[`${FOLDER}/${file.name}`] = file.bytes;
	}
	return zipSync(entries, { level: 0 });
}

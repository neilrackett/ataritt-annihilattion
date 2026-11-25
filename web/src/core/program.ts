// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ANNIHILA.TOS for the zip: the latest build, through the address that
// counts its downloads, which lets this page's origin fetch it.

export const PROGRAM = 'ANNIHILA.TOS';
export const PROGRAM_URL = 'https://downloads.neilrackett.com/ataritt-annihilattion/ANNIHILA.TOS';

/**
 * The program, or null when it can't be had: offline, refused (the server
 * lets only labs.neilrackett.com fetch it, so a page served anywhere else
 * gets nothing), or something that isn't a TOS program, whose first word
 * is always 0x601A.
 */
export async function fetchProgram(url = PROGRAM_URL): Promise<Uint8Array | null> {
	try {
		const response = await fetch(url, { cache: 'no-store' });
		if (!response.ok) {
			return null;
		}
		const bytes = new Uint8Array(await response.arrayBuffer());
		return bytes.length > 28 && bytes[0] === 0x60 && bytes[1] === 0x1a ? bytes : null;
	} catch {
		return null;
	}
}

// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The program in the zip: fetched only when it is one, and beside DATA.

import { unzipSync } from 'fflate';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { PROGRAM, PROGRAM_URL, fetchProgram } from '../src/core/program';
import { buildZip } from '../src/core/zip';

const TOS = new Uint8Array(64);
TOS.set([0x60, 0x1a]);

describe('fetchProgram', () => {
	afterEach(() => vi.unstubAllGlobals());

	it('returns a TOS program', async () => {
		const fetch = vi.fn(async () => new Response(TOS));
		vi.stubGlobal('fetch', fetch);
		expect(await fetchProgram()).toEqual(TOS);
		expect(fetch).toHaveBeenCalledWith(PROGRAM_URL, { cache: 'no-store' });
	});

	it('returns null for anything else', async () => {
		vi.stubGlobal('fetch', async () => new Response('<html>'));
		expect(await fetchProgram()).toBeNull();
		vi.stubGlobal('fetch', async () => new Response(TOS, { status: 404 }));
		expect(await fetchProgram()).toBeNull();
	});

	it('returns null when the fetch is refused', async () => {
		vi.stubGlobal('fetch', async () => {
			throw new TypeError('Failed to fetch');
		});
		expect(await fetchProgram()).toBeNull();
	});
});

describe('buildZip', () => {
	const files = [{ name: 'GAME.DAT', bytes: new Uint8Array([1, 2, 3]) }];

	it('puts the program beside DATA', () => {
		const names = Object.keys(unzipSync(buildZip(files, TOS)));
		expect(names.sort()).toEqual([`ANNIHILA/${PROGRAM}`, 'ANNIHILA/DATA/GAME.DAT']);
	});

	it('leaves it out when there is none', () => {
		expect(Object.keys(unzipSync(buildZip(files)))).toEqual(['ANNIHILA/DATA/GAME.DAT']);
	});
});

// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The web converter against build.sh: run in Node, through the same Pyodide
// the page uses, its files must be byte for byte the ones in ../dist. It
// needs the demo's installer in ../tmp/ta-demo and a dist built from it by
// build.sh, and skips itself when either is missing, which on GitHub's
// runners they always are: no game data goes near this repository.

import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { loadPyodide } from 'pyodide';
import { describe, expect, it } from 'vitest';
import { PYODIDE_VERSION, SCRIPTS, checkInstaller, convert, type Stage } from '../src/core/convert';

const REPO = join(import.meta.dirname, '..', '..');
const INSTALLER = join(REPO, 'tmp', 'ta-demo', 'Total Annihilation.exe');
const DIST = join(REPO, 'dist');
const have = existsSync(INSTALLER) && existsSync(join(DIST, 'MISS3.DAT'));

describe('checkInstaller', () => {
	it('refuses a file of the wrong size', async () => {
		expect(await checkInstaller(new Uint8Array(100))).toMatch(/100 bytes/);
	});
});

describe.skipIf(!have)('convert', () => {
	it('makes the files build.sh makes', { timeout: 15 * 60 * 1000 }, async () => {
		const installer = new Uint8Array(readFileSync(INSTALLER));
		expect(await checkInstaller(installer)).toBeNull();

		const py = await loadPyodide();
		expect(py.version).toBe(PYODIDE_VERSION);
		await py.loadPackage(['numpy', 'pillow'], { messageCallback: () => {} });
		const scripts = Object.fromEntries(SCRIPTS.map((n) => [n, readFileSync(join(REPO, 'tools', n), 'utf8')]));

		const stages: Stage[] = [];
		const t0 = Date.now();
		const files = await convert(py, scripts, installer, (s) => stages.push(s), () => {});
		console.log(`converted in ${((Date.now() - t0) / 1000).toFixed(1)} s`);

		expect(stages).toEqual(['unpacking', 'converting']);
		// every file build.sh makes but the program, and no others
		const expected = readdirSync(DIST).filter((n) => n !== 'ANNIHILA.TOS').sort();
		expect(files.map((f) => f.name)).toEqual(expected);
		for (const file of files) {
			const want = readFileSync(join(DIST, file.name));
			expect(Buffer.compare(Buffer.from(file.bytes), want), file.name).toBe(0);
		}
	});
});

// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conversion, run by the project's own Python (tools/*.py) in Pyodide:
// the same steps as build.sh, so the files come out byte for byte the same
// as a build on a Mac or Linux. Nothing here knows about the page; the
// worker drives it in a browser and test/convert.test.ts in Node.

import type { PyodideAPI } from 'pyodide';

/** Pinned, and the same as package.json's: the CDN serves its packages. */
export const PYODIDE_VERSION = '314.0.7';
export const PYODIDE_URL = `https://cdn.jsdelivr.net/pyodide/v${PYODIDE_VERSION}/full/`;

/** The demo's installer, "Total Annihilation.exe", as build.sh checks it. */
export const INSTALLER_SIZE = 21540864;
export const INSTALLER_SHA256 = '5e41cf05226c274b4ac9e4398f74f6b321506bd7a4317ee1744ff7aceba34c49';

/** The scripts the conversion runs, by file name. */
export const SCRIPTS = ['pe_resource.py', 'hpi_extract.py', 'convert.py', 'ta.py', 'quant.py', 'render3do.py'];

export interface OutFile {
	name: string;
	bytes: Uint8Array;
}

export type Stage = 'unpacking' | 'converting';

/** Why these bytes are not the demo's installer, or null when they are. */
export async function checkInstaller(bytes: Uint8Array): Promise<string | null> {
	if (bytes.length !== INSTALLER_SIZE) {
		return `it is ${bytes.length.toLocaleString()} bytes, and the demo's installer is ${INSTALLER_SIZE.toLocaleString()}.`;
	}
	const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes as BufferSource));
	const hex = Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
	if (hex !== INSTALLER_SHA256) {
		return "it is the right size, but its contents are not the demo's installer.";
	}
	return null;
}

/**
 * Runs build.sh's two data steps on the installer: the archive out of it
 * (pe_resource.py), unpacked (hpi_extract.py), then converted for the TT
 * (convert.py). numpy and Pillow must already be loaded. Each line the
 * scripts print goes to log; the files the TT reads come back in name
 * order.
 */
export async function convert(
	py: PyodideAPI,
	scripts: Record<string, string>,
	installer: Uint8Array,
	stage: (s: Stage) => void,
	log: (line: string) => void,
): Promise<OutFile[]> {
	const fs = py.FS;
	fs.mkdirTree('/tools');
	for (const name of SCRIPTS) {
		fs.writeFile(`/tools/${name}`, scripts[name]);
	}
	fs.mkdirTree('/work');
	fs.writeFile('/work/installer.exe', installer);
	py.setStdout({ batched: log });
	py.setStderr({ batched: log });

	const run = (script: string, ...args: string[]) => {
		py.globals.set('argv', py.toPy([`/tools/${script}`, ...args]));
		py.runPython(`import runpy, sys\nsys.argv = list(argv)\nrunpy.run_path(sys.argv[0], run_name='__main__')`);
	};

	stage('unpacking');
	run('pe_resource.py', '/work/installer.exe', 'ADD', '130', '1033', '/work/TADemo.hpi');
	fs.unlink('/work/installer.exe'); // 21MB the rest has no use for
	run('hpi_extract.py', '/work/TADemo.hpi', '/work/data');
	fs.unlink('/work/TADemo.hpi');

	stage('converting');
	// The previews are for checking the art by eye; nobody sees them here.
	run('convert.py', '/work/data', '/work/out', '/work/previews');

	return (fs.readdir('/work/out') as string[])
		.filter((name) => name !== '.' && name !== '..')
		.sort()
		.map((name) => ({ name, bytes: fs.readFile(`/work/out/${name}`) as Uint8Array }));
}

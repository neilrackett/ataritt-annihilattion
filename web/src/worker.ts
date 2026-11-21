// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The conversion takes minutes of Python, so it runs here rather than on
// the thread that draws the page.

/// <reference lib="webworker" />

import type { loadPyodide as LoadPyodide } from 'pyodide';
import { PYODIDE_URL, SCRIPTS, convert, type OutFile, type Stage } from './core/convert';

declare const self: DedicatedWorkerGlobalScope;

export interface ConvertRequest {
	installer: ArrayBuffer;
}

export type ConvertResponse =
	| { type: 'stage'; stage: 'loading' | Stage }
	| { type: 'log'; line: string }
	| { type: 'done'; files: OutFile[] }
	| { type: 'error'; message: string };

// The project's own scripts, bundled with the page: one converter for the
// web and for build.sh.
const sources = import.meta.glob<string>('../../tools/*.py', { query: '?raw', import: 'default', eager: true });
const scripts = Object.fromEntries(
	Object.entries(sources).map(([path, text]) => [path.slice(path.lastIndexOf('/') + 1), text]),
);

const post = (response: ConvertResponse, transfer: Transferable[] = []) => self.postMessage(response, transfer);

self.onmessage = async (event: MessageEvent<ConvertRequest>) => {
	try {
		for (const name of SCRIPTS) {
			if (!(name in scripts)) {
				throw new Error(`${name} is missing from this page`);
			}
		}
		post({ type: 'stage', stage: 'loading' });
		// From the CDN that serves its packages, rather than bundled: Pyodide
		// finds numpy and Pillow beside itself.
		const { loadPyodide } = (await import(/* @vite-ignore */ `${PYODIDE_URL}pyodide.mjs`)) as {
			loadPyodide: typeof LoadPyodide;
		};
		const py = await loadPyodide({ indexURL: PYODIDE_URL });
		await py.loadPackage(['numpy', 'pillow'], { messageCallback: () => {} });

		const files = await convert(
			py,
			scripts,
			new Uint8Array(event.data.installer),
			(stage) => post({ type: 'stage', stage }),
			(line) => post({ type: 'log', line }),
		);
		post({ type: 'done', files }, files.map((f) => f.bytes.buffer as ArrayBuffer));
	} catch (e) {
		post({ type: 'error', message: (e as Error).message });
	}
};

// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later

import './main.css';
import { checkInstaller, type OutFile } from './core/convert';
import { FOLDER, buildZip } from './core/zip';
import type { ConvertRequest, ConvertResponse } from './worker';

type StepState = 'waiting' | 'working' | 'done' | 'failed';
type StepId = 'check' | 'loading' | 'unpacking' | 'converting';

const STEPS: { id: StepId; label: string }[] = [
	{ id: 'check', label: "The demo's installer" },
	{ id: 'loading', label: 'Python, numpy and Pillow' },
	{ id: 'unpacking', label: "Unpacking the demo's data" },
	{ id: 'converting', label: 'Converting it for the TT' },
];

let installer: { name: string; data: ArrayBuffer } | null = null;
let state: Record<StepId, StepState> = blank();
let detail: Partial<Record<StepId, string>> = {};
let files: OutFile[] | null = null;
let worker: Worker | null = null;
let started = 0;
let ticker = 0;

const el = <T extends HTMLElement>(id: string): T => document.getElementById(id) as T;
const dropZone = el<HTMLElement>('drop');
const picker = el<HTMLInputElement>('picker');
const stepList = el<HTMLUListElement>('steps');
const messages = el<HTMLElement>('messages');
const convertButton = el<HTMLButtonElement>('convert');
const saveButton = el<HTMLButtonElement>('save');
const resetButton = el<HTMLButtonElement>('reset');
const themeButton = el<HTMLButtonElement>('theme');

function blank(): Record<StepId, StepState> {
	return { check: 'waiting', loading: 'waiting', unpacking: 'waiting', converting: 'waiting' };
}

// --- colour theme ----------------------------------------------------

type Theme = 'system' | 'light' | 'dark';

const THEME_KEY = 'annihilattion-theme';
const THEME_CYCLE: Theme[] = ['system', 'light', 'dark'];

/**
 * The page follows the machine unless a reader says otherwise, and the
 * choice is remembered in this browser only. Every localStorage call is
 * wrapped: with site data blocked the accessor throws, and a colour
 * preference is not worth a page that will not start.
 */
function storedTheme(): Theme {
	try {
		const value = localStorage.getItem(THEME_KEY);
		return value === 'light' || value === 'dark' ? value : 'system';
	} catch {
		return 'system';
	}
}

let theme: Theme = storedTheme();

function applyTheme(next: Theme): void {
	theme = next;
	if (next === 'system') {
		delete document.documentElement.dataset.theme;
	} else {
		document.documentElement.dataset.theme = next;
	}
	themeButton.textContent = next;
	themeButton.setAttribute('aria-label', `Colour theme: ${next}. Press to change it.`);
	try {
		if (next === 'system') {
			localStorage.removeItem(THEME_KEY);
		} else {
			localStorage.setItem(THEME_KEY, next);
		}
	} catch {
		// Nothing to do: the page still looks right for this visit.
	}
}

// --- the installer ---------------------------------------------------

async function accept(list: FileList | null): Promise<void> {
	const file = list?.[0];
	if (!file || worker) {
		return;
	}
	reset();
	state.check = 'working';
	detail.check = escapeHtml(file.name);
	render();
	const data = await file.arrayBuffer();
	const wrong = await checkInstaller(new Uint8Array(data));
	if (wrong) {
		state.check = 'failed';
		detail.check = `${escapeHtml(file.name)} &middot; not used: ${escapeHtml(wrong)}`;
	} else {
		state.check = 'done';
		detail.check = `${escapeHtml(file.name)} &middot; ${(data.byteLength / 1048576).toFixed(1)}MB &middot; checked`;
		installer = { name: file.name, data };
	}
	render();
}

// --- converting ------------------------------------------------------

function convert(): void {
	if (!installer || worker) {
		return;
	}
	const data = installer.data.slice(0); // the worker takes its own copy
	worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' });
	let current: StepId = 'loading';
	worker.onmessage = (event: MessageEvent<ConvertResponse>) => {
		const response = event.data;
		if (response.type === 'stage') {
			if (current !== response.stage) {
				state[current] = 'done';
			}
			current = response.stage;
			state[current] = 'working';
			detail[current] = '';
		} else if (response.type === 'log') {
			detail[current] = escapeHtml(response.line.trim());
		} else if (response.type === 'done') {
			state[current] = 'done';
			files = response.files;
			detail[current] = `${files.length} files for the TT`;
			finish();
		} else {
			state[current] = 'failed';
			detail[current] = escapeHtml(response.message);
			finish();
		}
		render();
	};
	worker.onerror = (event) => {
		state[current] = 'failed';
		detail[current] = escapeHtml(event.message || 'the converter stopped');
		finish();
		render();
	};
	started = Date.now();
	ticker = window.setInterval(render, 1000);
	const request: ConvertRequest = { installer: data };
	worker.postMessage(request, [data]);
	render();
}

function finish(): void {
	worker?.terminate();
	worker = null;
	window.clearInterval(ticker);
	if (files) {
		download();
	}
}

function download(): void {
	if (!files) {
		return;
	}
	const zip = buildZip(files);
	const url = URL.createObjectURL(new Blob([zip as BlobPart], { type: 'application/zip' }));
	const a = document.createElement('a');
	a.href = url;
	a.download = 'ANNIHILA-DATA.zip';
	a.click();
	URL.revokeObjectURL(url);
}

/**
 * Chrome and Edge can write the files straight into a folder, which saves
 * unzipping them and moving them by hand. Everywhere else the zip is the
 * answer, so this button only appears where it works.
 */
async function saveToFolder(): Promise<void> {
	if (!files) {
		return;
	}
	try {
		const picked = await (window as unknown as {
			showDirectoryPicker(options?: { mode?: string }): Promise<FileSystemDirectoryHandle>;
		}).showDirectoryPicker({ mode: 'readwrite' });
		for (const file of files) {
			const handle = await picked.getFileHandle(file.name, { create: true });
			const stream = await handle.createWritable();
			await stream.write(file.bytes as BufferSource);
			await stream.close();
		}
		messages.innerHTML =
			`<p><strong>${files.length} files written to the folder you chose.</strong></p>` +
			'<p>Put <code>ANNIHILA.TOS</code> in the same folder, copy it to your TT\'s hard disk and run it.</p>';
	} catch (e) {
		const err = e as Error;
		if (err.name !== 'AbortError') {
			messages.innerHTML = `<p class="bad">Could not write the folder: ${escapeHtml(err.message)}</p>`;
		}
	}
}

function reset(): void {
	installer = null;
	state = blank();
	detail = {};
	files = null;
	picker.value = '';
	render();
}

// --- rendering -------------------------------------------------------

const LABEL: Record<StepState, string> = { waiting: 'Waiting', working: 'Working', done: 'Done', failed: 'Failed' };
const CLASS: Record<StepState, string> = { waiting: '', working: 'busy', done: 'ok', failed: 'bad' };

function escapeHtml(s: string): string {
	return s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c] as string);
}

function render(): void {
	stepList.innerHTML = STEPS.map(({ id, label }, i) => {
		// a step at work shows the time since the conversion began
		let shown = LABEL[state[id]];
		if (state[id] === 'working' && id !== 'check') {
			const s = Math.floor((Date.now() - started) / 1000);
			shown = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
		}
		const more = detail[id];
		return `<li class="slot ${CLASS[state[id]]}">
			<span class="no">${i + 1}</span>
			<span class="name">${label}${more ? `<span class="detail">${more}</span>` : ''}</span>
			<span class="state">${shown}</span>
		</li>`;
	}).join('');

	const busy = worker !== null;
	convertButton.disabled = !installer || busy || files !== null;
	saveButton.hidden = files === null || !('showDirectoryPicker' in window);
	resetButton.hidden = busy || (state.check === 'waiting' && !files);
	dropZone.classList.toggle('disabled', busy);

	const failed = STEPS.some(({ id }) => state[id] === 'failed');
	if (files) {
		const kb = Math.round(files.reduce((n, f) => n + f.bytes.length, 0) / 1024);
		messages.innerHTML =
			`<p><strong>${files.length} files, ${kb.toLocaleString()}KB, in ${FOLDER}-DATA.zip.</strong></p>` +
			`<p>Unzip it, put <code>ANNIHILA.TOS</code> in the <code>${FOLDER}</code> folder with them, copy ` +
			'the folder to your TT\'s hard disk and run <code>ANNIHILA.TOS</code>.</p>' +
			'<p><button type="button" id="again" class="link">Download the zip again</button></p>';
		el<HTMLButtonElement>('again').addEventListener('click', download);
	} else if (busy) {
		messages.innerHTML =
			'<p>Keep this tab open while it works: the conversion takes a few minutes, and the zip ' +
			'downloads when it is done.</p>';
	} else if (failed && state.check !== 'failed') {
		messages.innerHTML =
			'<p class="bad">The conversion stopped. Starting again may help if the network dropped while ' +
			'Python was loading.</p>';
	} else {
		messages.innerHTML = '';
	}
}

// --- wiring ----------------------------------------------------------

dropZone.addEventListener('click', () => picker.click());
dropZone.addEventListener('keydown', (e) => {
	if (e.key === 'Enter' || e.key === ' ') {
		e.preventDefault();
		picker.click();
	}
});
picker.addEventListener('change', () => void accept(picker.files));

for (const type of ['dragenter', 'dragover'] as const) {
	dropZone.addEventListener(type, (e) => {
		e.preventDefault();
		dropZone.classList.add('over');
	});
}
for (const type of ['dragleave', 'drop'] as const) {
	dropZone.addEventListener(type, () => dropZone.classList.remove('over'));
}
dropZone.addEventListener('drop', (e) => {
	e.preventDefault();
	void accept(e.dataTransfer?.files ?? null);
});
// Anywhere on the page, so a near miss still works.
window.addEventListener('dragover', (e) => e.preventDefault());
window.addEventListener('drop', (e) => e.preventDefault());

el<HTMLButtonElement>('choose').addEventListener('click', (e) => {
	e.stopPropagation();
	picker.click();
});
convertButton.addEventListener('click', convert);
saveButton.addEventListener('click', () => void saveToFolder());
resetButton.addEventListener('click', reset);
themeButton.addEventListener('click', () => {
	applyTheme(THEME_CYCLE[(THEME_CYCLE.indexOf(theme) + 1) % THEME_CYCLE.length]);
});

applyTheme(theme);
render();

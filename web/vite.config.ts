// Copyright (C) 2026 Neil Rackett
// SPDX-License-Identifier: GPL-3.0-or-later

import { defineConfig } from 'vite';

// GitHub Pages serves a project site from /<repo>/, so the built asset URLs
// need that prefix. A user site or a custom domain serves from the root
// instead, which is what BASE is for:
//
//   BASE=/ pnpm run build
export default defineConfig({
	base: process.env.BASE ?? '/ataritt-annihilattion/',
	build: {
		target: 'es2022',
	},
	worker: {
		format: 'es',
	},
	server: {
		// the converter's Python is in ../tools, outside this folder
		fs: { allow: ['..'] },
	},
});

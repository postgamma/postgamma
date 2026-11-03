/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {writeFile} from 'node:fs/promises';
import {resolve} from 'node:path';
import {copyAssets, isolationHeaders} from '@postgamma/wasm/tooling';
await copyAssets('dist/postgamma');
await writeFile('dist/index.html', '<!doctype html><meta charset="utf-8"><title>PostGamma</title><link rel="icon" href="data:,"><pre>Opening database…</pre><script defer src="./app.js"></script>');
export default {
  mode: 'production', target: ['web', 'es2022'], entry: './app.js',
  output: {path: resolve('dist'), filename: 'app.js', clean: false},
  devServer: {static: {directory: resolve('dist')}, headers: isolationHeaders},
};

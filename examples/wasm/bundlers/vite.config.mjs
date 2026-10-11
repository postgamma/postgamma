/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {copyAssets, isolationHeaders} from '@postgamma/wasm/tooling';
await copyAssets('public/postgamma');
export default {
  base: './',
  server: {headers: isolationHeaders},
  preview: {headers: isolationHeaders},
  optimizeDeps: {exclude: ['@postgamma/wasm']},
  build: {target: 'es2022'},
};

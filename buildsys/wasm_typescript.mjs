/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {stripTypeScriptTypes} from 'node:module';
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {dirname} from 'node:path';
const [source, destination] = process.argv.slice(2);
if (!source || !destination) throw new Error('source and destination are required');
mkdirSync(dirname(destination), {recursive: true});
writeFileSync(destination, stripTypeScriptTypes(readFileSync(source, 'utf8'), {mode: 'strip'}));

/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
import {Terminal} from '../vendor/xterm/xterm.mjs';
import {FitAddon} from '../vendor/xterm/addon-fit.mjs';

// psql owns parsing, prompts and results. This is the browser's line discipline:
// echo/edit keystrokes locally, then deliver a complete line to native stdin.
export class PsqlScreen {
  constructor(element, handlers) {
    this.handlers = handlers;
    this.term = new Terminal({convertEol: true, cursorBlink: true, scrollback: 2000,
      fontSize: 13, lineHeight: 1.45, fontFamily: 'Menlo, Consolas, monospace',
      allowProposedApi: true,
      theme: {background: '#090d10', foreground: '#c4dedc', cursor: '#48e6ce',
        selectionBackground: '#365954', black: '#090d10', green: '#85e3ac'}});
    this.fit = new FitAddon(); this.term.loadAddon(this.fit);
    element.replaceChildren(); this.term.open(element);
    // Expose the actual renderer for inspection, without a duplicate transcript.
    element.terminal = this.term;
    this.term.textarea.setAttribute('aria-label', 'psql terminal input');
    this.term.textarea.setAttribute('autocapitalize', 'off');
    this.term.textarea.setAttribute('spellcheck', 'false');
    this.history = []; this.position = 0; this.draft = '';
    this.line = []; this.cursor = 0; this.pending = ''; this.active = false;
    this.work = Promise.resolve();
    this.encoder = new TextEncoder();
    this.segmenter = new Intl.Segmenter(undefined, {granularity: 'grapheme'});
    this.term.attachCustomKeyEventHandler(event => {
      // Keep the browser's copy/paste and focus traversal shortcuts.
      if (event.key === 'Tab' && event.shiftKey) return false;
      if (event.key === 'Escape' && element.closest('dialog[open]')) return false;
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'c' && this.term.hasSelection()) return false;
      return true;
    });
    this.term.onData(data => {
      if (data === '\x03') {this.pending = ''; this.handlers.interrupt(); return;}
      if (data === '\x04' && this.active && !this.line.length) {this.handlers.eof(); return;}
      if (this.handlers.busy?.()) {this.term.write('\x07'); return;}
      if (this.handlers.closed()) {if (data === '\r') this.handlers.restart(); return;}
      data = data.replace(/\r\n|\n/g, '\r');
      if (this.encoder.encode(this.pending + data).length > 65536) {this.term.write('\x07'); return;}
      this.pending += data;
      void this.enqueue(() => this.drain());
    });
    this.observer = new ResizeObserver(() => {void this.resize();});
    this.observer.observe(element);
    this.fit.fit();
  }
  enqueue(action) {
    const result = this.work.then(action).finally(() => this.handlers.changed?.());
    this.work = result.catch(error => this.handlers.error(error));
    return result;
  }
  write(text) {return new Promise(resolve => this.term.write(text, resolve));}
  focus() {this.term.textarea.focus({preventScroll: true});}
  async insert(text, submit = false) {
    if (/[\r\n\x00-\x1f\x7f]/.test(text) || this.encoder.encode(text + '\n').length > 16384)
      throw new Error('Example must fit on one complete terminal input line.');
    const inserted = await this.enqueue(async () => {
      if (!this.active || this.line.length || this.pending) return false;
      this.pending = text + (submit ? '\r' : '');
      await this.drain();
      return true;
    });
    if (!inserted) throw new Error('Finish or clear the current input first.');
  }
  split(text) {return Array.from(this.segmenter.segment(text), part => part.segment);}
  forget() {
    this.active = false; this.line = []; this.cursor = 0;
    this.anchor?.dispose(); this.anchor = undefined;
  }
  clear() {
    this.pending = '';
    return this.enqueue(() => {this.forget(); this.term.reset();});
  }
  message(text) {return this.enqueue(() => this.write(text));}
  output(text, input) {
    return this.enqueue(async () => {
      if (text) await this.write(text);
      if (input() && !this.active) {
        // Read the real prompt, including custom prompts and wrapped lines.
        const buffer = this.term.buffer.active;
        let row = buffer.baseY + buffer.cursorY;
        this.prefix = buffer.getLine(row).translateToString(false, 0, buffer.cursorX);
        while (buffer.getLine(row)?.isWrapped && row > 0) {
          this.prefix = buffer.getLine(--row).translateToString(false) + this.prefix;
        }
        this.anchor = this.term.registerMarker(row - buffer.baseY - buffer.cursorY);
        this.active = true;
      }
      await this.drain();
    });
  }
  eraseSequence() {
    const buffer = this.term.buffer.active;
    const row = Math.max(buffer.baseY, this.anchor?.line ?? buffer.baseY + buffer.cursorY);
    const up = buffer.baseY + buffer.cursorY - row;
    this.anchor?.dispose(); this.anchor = this.term.registerMarker(-up);
    return (up > 0 ? '\x1b[' + up + 'A' : '') + '\r\x1b[J';
  }
  erase() {return this.write(this.eraseSequence());}
  async render() {
    const erase = this.eraseSequence();
    // Keep an editable window around the cursor for lines taller than the
    // viewport. A terminal cannot address old scrollback with cursor escapes.
    // Reserve two cells per code point (eight for tabs); full input is echoed
    // on submission and delivered unchanged to psql.
    const cost = value => value === '\t' ? 8 : Array.from(value).length * 2;
    let remaining = Math.max(this.term.cols, this.term.cols * (this.term.rows - 4) - this.prefix.length * 2);
    let left = this.cursor, right = this.cursor;
    while (remaining > 8 && (left || right < this.line.length)) {
      if (left) remaining -= cost(this.line[--left]);
      if (right < this.line.length) remaining -= cost(this.line[right++]);
    }
    // Parse the erase, replacement and cursor placement in one write so no
    // intermediate empty prompt reaches the renderer. xterm's saved cursor
    // tracks scrollback when the text after the cursor wraps at the bottom.
    await this.write(erase + this.prefix + (left ? '…' : '') + this.line.slice(left, this.cursor).join('') +
      '\x1b7' + this.line.slice(this.cursor, right).join('') + (right < this.line.length ? '…' : '') + '\x1b8');
    this.term.scrollToBottom();
  }
  resize() {
    return this.enqueue(async () => {
      const size = this.fit.proposeDimensions();
      if (!size || !Number.isFinite(size.cols) || !Number.isFinite(size.rows) ||
          (size.cols === this.term.cols && size.rows === this.term.rows)) return;
      if (this.active) await this.erase();
      this.fit.fit();
      if (this.active) {
        this.anchor?.dispose(); this.anchor = this.term.registerMarker(0);
        await this.render();
      }
    });
  }
  cancel() {
    this.pending = '';
    return this.enqueue(async () => {
      if (this.active) {this.cursor = this.line.length; await this.render();}
      this.forget(); await this.write('^C\r\n');
    });
  }
  async drain() {
    while (this.active && this.pending) {
      const match = this.pending.match(/^\x1b(?:\[[0-9;]*[A-Za-z~]|O[HF]|.)|^[^\x00-\x1f\x7f]+/u);
      const key = match ? match[0] : this.pending[0]; this.pending = this.pending.slice(key.length);
      if (key === '\r') {
        const text = this.line.join('');
        await this.write(this.eraseSequence() + this.prefix + text + '\r\n');
        if (text && this.history.at(-1) !== text) this.history.push(text);
        if (this.history.length > 100) this.history.shift();
        this.position = this.history.length; this.draft = '';
        this.forget(); await this.handlers.line(text + '\n'); return;
      }
      if (key === '\x1b[A' || key === '\x1b[B' || key === '\x10' || key === '\x0e') {
        if (this.position === this.history.length) this.draft = this.line.join('');
        this.position = Math.max(0, Math.min(this.history.length, this.position + (key === '\x1b[A' || key === '\x10' ? -1 : 1)));
        this.line = this.split(this.position === this.history.length ? this.draft : this.history[this.position]); this.cursor = this.line.length;
      } else if (key === '\x1b[D' || key === '\x02') this.cursor = Math.max(0, this.cursor - 1);
      else if (key === '\x1b[C' || key === '\x06') this.cursor = Math.min(this.line.length, this.cursor + 1);
      else if (['\x01', '\x1b[H', '\x1bOH', '\x1b[1~', '\x1b[7~'].includes(key)) this.cursor = 0;
      else if (['\x05', '\x1b[F', '\x1bOF', '\x1b[4~', '\x1b[8~'].includes(key)) this.cursor = this.line.length;
      else if (key === '\x7f' || key === '\b') {if (this.cursor) this.line.splice(--this.cursor, 1);}
      else if (key === '\x1b[3~' || key === '\x04') this.line.splice(this.cursor, 1);
      else if (key === '\x15') {this.line.splice(0, this.cursor); this.cursor = 0;}
      else if (key === '\x0b') this.line.splice(this.cursor);
      else if (key === '\x17') {
        let start = this.cursor;
        while (start && /\s/u.test(this.line[start - 1])) start--;
        while (start && !/\s/u.test(this.line[start - 1])) start--;
        this.line.splice(start, this.cursor - start); this.cursor = start;
      } else if (key === '\x0c') {
        await this.erase(); await this.write('\x1b[2J\x1b[H');
        this.anchor?.dispose(); this.anchor = this.term.registerMarker(0);
      } else if (key === '\t' || key.charCodeAt(0) >= 32) {
        const before = this.line.slice(0, this.cursor).join('') + key, after = this.line.slice(this.cursor).join('');
        if (this.encoder.encode(before + after + '\n').length > 16384) {
          // Reject the whole pending paste, never execute a silently truncated SQL line.
          this.pending = ''; await this.write('\x07'); return;
        }
        this.line = this.split(before + after); this.cursor = this.split(before).length;
        // Report accepted text from typing, paste or committed IME input. Keep
        // navigation/control keys and native output out of this interaction.
        if (key !== '\t') this.handlers.textInput?.();
      } else continue;
      await this.render();
    }
  }
}

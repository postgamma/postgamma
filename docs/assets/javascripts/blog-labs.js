/* Copyright 2026 Shujie Zhang; SPDX-License-Identifier: Apache-2.0 */
// Small explanatory models. They never import the database or run a benchmark.
for (const lab of document.querySelectorAll('[data-blog-lab]')) {
  const output = lab.querySelector('output');
  if (lab.dataset.blogLab === 'ownership') {
    let step = 0;
    const descriptions = [
      'Both logical sessions begin in UTC. The shared database is the same; their settings have separate owners.',
      'Session A: SET TIME ZONE \'Asia/Tokyo\'. Only A changes; B still observes UTC.',
      'Session B: SET TIME ZONE \'America/New_York\'. B changes without overwriting A.',
      'A resumes on a carrier: its active context must still identify A, so SHOW TIME ZONE yields Asia/Tokyo.',
      'B resumes: SHOW TIME ZONE yields America/New_York. Scheduling is separate from state ownership.',
    ];
    const render = () => {
      lab.dataset.step = String(step);
      lab.querySelector('[data-zone="a"]').textContent = step >= 1 ? 'Asia/Tokyo' : 'UTC';
      lab.querySelector('[data-zone="b"]').textContent = step >= 2 ? 'America/New_York' : 'UTC';
      output.textContent = descriptions[step];
      lab.querySelector('[data-next]').disabled = step === 4;
    };
    lab.querySelector('[data-next]').addEventListener('click', () => {step = Math.min(4, step + 1); render();});
    lab.querySelector('[data-reset]').addEventListener('click', () => {step = 0; render();});
    render();
  }
  if (lab.dataset.blogLab === 'weekly') {
    const stages = [...lab.querySelectorAll('[data-stage]')];
    for (const button of lab.querySelectorAll('[data-failure]')) button.addEventListener('click', () => {
      const failed = button.dataset.failure;
      for (const peer of lab.querySelectorAll('[data-failure]')) peer.setAttribute('aria-pressed', String(peer === button));
      const states = new Map();
      for (const [index, stage] of stages.entries()) {
        const name = stage.dataset.stage;
        const unavailable = (stage.dataset.depends || '').split(' ').filter(Boolean).some(dep => states.get(dep) !== 'passed');
        const state = unavailable ? 'not-run' : name === failed ? 'failed' : 'passed';
        states.set(name, state); stage.dataset.state = state;
        stage.style.setProperty('--stage-order', String(index));
        stage.querySelector('span').textContent = state;
      }
      const passed = [...states.values()].filter(x => x === 'passed').length;
      const blocked = [...states.values()].filter(x => x === 'not-run').length;
      output.textContent = `${failed === 'none' ? 'PASS' : 'FAIL'}: ${passed} passed, ${failed === 'none' ? 0 : 1} failed, ${blocked} not-run. Independent work continues in this keep-going model; no elapsed time is measured.`;
    });
  }
  if (lab.dataset.blogLab === 'commit') {
    const messages = {
      before: 'Before COMMIT: this transaction has not committed. Recovery must not expose its uncommitted writes as a committed note.',
      lost: 'Storage barrier completed, reply missing: the client has no successful COMMIT acknowledgement. Treat the outcome as unknown; reconcile by a stable application key before retrying.',
      received: 'Successful COMMIT reply received: the implementation has completed its required browser storage barriers. Site-data deletion and hardware power-loss qualification are separate questions.',
    };
    for (const button of lab.querySelectorAll('[data-cut]')) button.addEventListener('click', () => {
      lab.dataset.cut = button.dataset.cut;
      for (const peer of lab.querySelectorAll('[data-cut]')) peer.setAttribute('aria-pressed', String(peer === button));
      output.textContent = messages[button.dataset.cut];
    });
  }
  for (const button of lab.querySelectorAll('button[data-enable]')) button.disabled = false;
}

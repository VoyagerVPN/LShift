import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {runLiveCases} from './live-cases.mjs';

// Согласованный контракт: точный текст/курсор/раскладка, отсутствие программного выделения,
// рабочий Undo и проверки устаревшего контекста. Быстрое удаление и повторный набор допустимы.
// Промежуточные rAF-кадры остаются отдельной диагностикой, не обещанием атомарной отрисовки.
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cache = path.join(root, '.cache');
const profile = path.join(cache, 'edge-test-profile');
const temporary = path.join(cache, 'automation-temp');
const fixtureUrl = pathToFileURL(path.join(root, 'tests/automation-fixture.html')).href;
const args = process.argv.slice(2);
const option = (name, fallback) => { const index = args.indexOf(name); return index >= 0 ? args[index + 1] : fallback; };
const probePath = path.resolve(option('--probe', path.join(cache, 'build/automation_probe.exe')));
const edgePath = option('--edge', 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe');
const reportPath = path.resolve(option('--report', path.join(cache, 'automation-results.json')));
const repeat = Number(option('--repeat', '1'));
if (!Number.isInteger(repeat) || repeat < 1 || repeat > 3) throw new Error('--repeat must be 1..3');
const deadline = Number(option('--deadline-ms', '1500'));
if (!Number.isInteger(deadline) || deadline < 500 || deadline > 8000) throw new Error('--deadline-ms must be 500..8000');
const suite = option('--suite', 'first');
if (!['first', 'selection', 'recent', 'full'].includes(suite)) throw new Error('--suite must be first, selection, recent or full');
const selectedCase = option('--case', null);
const legacyProbe = args.includes('--legacy-probe');
function typedEvents(tokens) {
  const events = [];
  const stroke = (vk, code, key, text = '', modifiers = 0) => {
    events.push({vk, code, key, text, modifiers, down: true}, {vk, code, key, text: '', modifiers, down: false});
  };
  for (const token of tokens) {
    if (token === '<Backspace>') stroke(8, 'Backspace', 'Backspace');
    else if (token === '<Delete>') stroke(46, 'Delete', 'Delete');
    else if (token === '<Ctrl+A>') {
      events.push({vk: 162, code: 'ControlLeft', key: 'Control', text: '', modifiers: 2, down: true});
      stroke(65, 'KeyA', 'a', '', 2);
      events.push({vk: 162, code: 'ControlLeft', key: 'Control', text: '', modifiers: 0, down: false});
    }
    else if (token === '<ArrowLeft>') stroke(37, 'ArrowLeft', 'ArrowLeft');
    else if (token === '<Enter>') stroke(13, 'Enter', 'Enter', '\r');
    else if (token === '<Escape>') stroke(27, 'Escape', 'Escape');
    else if (/^<Num[0-9]>$/.test(token)) {
      const digit = token[4];
      stroke(96 + Number(digit), `Numpad${digit}`, digit, digit);
    }
    else if (/^[0-9]$/.test(token)) stroke(token.charCodeAt(0), `Digit${token}`, token, token);
    else if (token === ' ') stroke(32, 'Space', ' ', ' ');
    else if (token === '(' || token === ')') {
      events.push({vk: 160, code: 'ShiftLeft', key: 'Shift', text: '', modifiers: 8, down: true});
      stroke(token === '(' ? 57 : 48, token === '(' ? 'Digit9' : 'Digit0', token, token, 8);
      events.push({vk: 160, code: 'ShiftLeft', key: 'Shift', text: '', modifiers: 0, down: false});
    }
    else if (/^[a-zA-Z]$/.test(token)) {
      const shifted = token === token.toUpperCase();
      if (shifted) events.push({vk: 160, code: 'ShiftLeft', key: 'Shift', text: '', modifiers: 8, down: true});
      stroke(token.toUpperCase().charCodeAt(0), `Key${token.toUpperCase()}`, token, token, shifted ? 8 : 0);
      if (shifted) events.push({vk: 160, code: 'ShiftLeft', key: 'Shift', text: '', modifiers: 0, down: false});
    } else throw new Error('Unsupported fixture key token');
  }
  return events;
}
const wordTokens = [...'ghbdtn'];
const shortLatin = 'ghbdtn '.repeat(144).trim();
const shortRussian = 'привет '.repeat(144).trim();
const longLatin = 'ghbdtn '.repeat(585) + 'g';
const longRussian = 'привет '.repeat(585) + 'п';
// Ожидаемые строки заданы независимо от реализации Convert; текст остаётся внутри собственной fixture.
const selectionCase = (name, kind, original, converted) => ({name, kind, text: `KEEP ${original} TAIL`,
  start: 5, end: 5 + original.length, expected: `KEEP ${converted} TAIL`, caret: 5 + converted.length});
const cases = [
  {name: 'input-combobox-word', kind: 'input', inputRole: 'combobox', text: 'ghbdtn', typed: true,
    tokens: wordTokens, expected: 'привет', caret: 6, roundTrip: true},
  {...selectionCase('input-combobox-selection', 'input', 'ghbdtn', 'привет'), inputRole: 'combobox'},
  {name: 'input-combobox-readonly', kind: 'input', inputRole: 'combobox', readOnly: true,
    text: 'ghbdtn', start: 0, end: 6, expected: 'ghbdtn', expectedStart: 0, caret: 6, status: 'Unsupported'},
  {name: 'input-combobox-password', kind: 'input', inputRole: 'combobox', password: true,
    text: 'ghbdtn', start: 0, end: 6, expected: 'ghbdtn', expectedStart: 0, caret: 6, status: 'Unsupported'},
  {name: 'input-lastword', kind: 'input', text: 'ghbdtn', typed: true, tokens: wordTokens, expected: 'привет', caret: 6},
  {name: 'textarea-trailing-spaces', kind: 'textarea', text: 'ghbdtn   ', typed: true, tokens: [...'ghbdtn   '], expected: 'привет   ', caret: 9},
  {name: 'contenteditable-lastword', kind: 'contenteditable', text: 'ghbdtn', typed: true, tokens: wordTokens, expected: 'привет', caret: 6},
  {name: 'contenteditable-roundtrip', kind: 'contenteditable', text: 'ghbdtn', typed: true, tokens: wordTokens, expected: 'привет', caret: 6, roundTrip: true},
  ...['input', 'textarea', 'contenteditable'].flatMap(kind => [
    {name: `${kind}-select-all-backspace-retype`, kind, text: 'ghbdtn', typed: true,
      tokens: [...wordTokens, '<Ctrl+A>', '<Backspace>', ...wordTokens], expected: 'привет', caret: 6, roundTrip: true},
    {name: `${kind}-select-all-delete-retype`, kind, text: 'ghbdtn', typed: true,
      tokens: [...wordTokens, '<Ctrl+A>', '<Delete>', ...wordTokens], expected: 'привет', caret: 6},
    {name: `${kind}-select-all-replace`, kind, text: 'ghbdtn', typed: true,
      tokens: [...'old', '<Ctrl+A>', ...wordTokens], expected: 'привет', caret: 6},
  ]),
  {name: 'contenteditable-select-all-denied-guard', kind: 'contenteditable', text: 'olghbdtn', typed: true,
    tokens: [...'old', '<Ctrl+A>', '<Backspace>', ...wordTokens], preventSelectAll: true,
    expected: 'olghbdtn', caret: 8, status: 'Stale'},
  ...['input', 'textarea', 'contenteditable'].map(kind => ({name: `${kind}-word-then-space`, kind,
    text: 'ghbdtn ', typed: true, tokens: [...'ghbdtn '], expected: 'привет ', caret: 7})),
  {name: 'contenteditable-space-after-pause', kind: 'contenteditable', text: 'ghbdtn ', typed: true,
    tokens: [...'ghbdtn '], expected: 'привет ', caret: 7, waitBeforeGestureMs: 3000},
  {name: 'contenteditable-surrounding-document', kind: 'contenteditable', text: 'ghbdtn ', typed: true,
    tokens: [...'ghbdtn '], expected: 'привет ', caret: 7, surroundingText: true},
  ...['input', 'textarea', 'contenteditable'].map(kind => ({name: `${kind}-multiword-roundtrips`, kind,
    text: 'hello ghbdtn  ', typed: true, tokens: [...'hello ghbdtn  '], expected: 'hello привет  ', caret: 14,
    wordUnits: 8, roundTrip: true, roundTrips: 11})),
  {name: 'contenteditable-shift-word', kind: 'contenteditable', text: 'Ghbdtn', typed: true, tokens: [...'Ghbdtn'], expected: 'Привет', caret: 6},
  ...['input', 'textarea', 'contenteditable'].flatMap(kind => [
    {name: `${kind}-backspace-empty-word`, kind, text: 'ghbdtn', typed: true,
      tokens: ['<Backspace>', ...wordTokens], expected: 'привет', caret: 6},
    {name: `${kind}-erase-word-overrun`, kind, text: 'ghbdtn', typed: true,
      tokens: [...'old', ...Array(4).fill('<Backspace>'), ...wordTokens], expected: 'привет', caret: 6},
    {name: `${kind}-uncertain-partial-word`, kind, seed: 'old', text: 'olghbdtn', typed: true,
      tokens: ['<Backspace>', ...wordTokens], expected: 'olghbdtn', caret: 8, status: 'Stale'},
    {name: `${kind}-numpad-word`, kind, text: 'ghbdtn1', typed: true,
      tokens: [...wordTokens, '<Num1>'], expected: 'привет1', caret: 7},
    {name: `${kind}-digit-word`, kind, text: 'ghbdtn1', typed: true,
      tokens: [...'ghbdtn1'], expected: 'привет1', caret: 7},
  ]),
  {name: 'input-backspace-word', kind: 'input', text: 'ghbdtn', typed: true, tokens: [...'ghbdtx', '<Backspace>', 'n'], expected: 'привет', caret: 6},
  ...['input', 'textarea', 'contenteditable'].map(kind => ({name: `${kind}-paste-denied-word`, kind, text: 'ghbdtn',
    typed: true, tokens: wordTokens, preventPaste: true, expected: 'привет', caret: 6})),
  {name: 'input-auto-pair-guard', kind: 'input', text: 'ghbdtn()', typed: true, tokens: [...wordTokens, '('],
    autoPair: true, start: 7, expected: 'ghbdtn()', caret: 7, status: 'Unsupported'},
  {name: 'contenteditable-repeated-word', kind: 'contenteditable', seed: 'ghbdtn ', text: 'ghbdtn ghbdtn', typed: true, tokens: wordTokens,
    expected: 'ghbdtn привет', caret: 13},
  {name: 'input-no-history', kind: 'input', text: 'ghbdtn', typed: true, tokens: wordTokens, journal: false,
    expected: 'ghbdtn', caret: 6, status: 'Switched'},
  {name: 'input-stale-journal', kind: 'input', text: 'ghbdtn', typed: true, tokens: wordTokens, journalTokens: [...'abcdef'],
    expected: 'ghbdtn', caret: 6, status: 'Stale'},
  {name: 'input-navigation-invalidates', kind: 'input', text: 'ghbdtn', typed: true, tokens: [...wordTokens, '<ArrowLeft>'],
    start: 5, expected: 'ghbdtn', caret: 5, status: 'Switched'},
  selectionCase('textarea-selection-1007', 'textarea', shortLatin, shortRussian),
  selectionCase('contenteditable-selection-1007', 'contenteditable', shortLatin, shortRussian),
  selectionCase('textarea-selection-4096', 'textarea', longLatin, longRussian),
  selectionCase('contenteditable-selection-4096', 'contenteditable', longLatin, longRussian),
  ...['input', 'textarea', 'contenteditable'].flatMap(kind => [
    selectionCase(`${kind}-selection-bilingual`, kind, 'привет ghbdtn', 'ghbdtn привет'),
    selectionCase(`${kind}-selection-mixed-word`, kind, 'привет приvet ghbdtn', 'ghbdtn приvet привет'),
    {...selectionCase(`${kind}-selection-last-english`, kind, 'hello мир приvet', 'руддщ vbh приvet'),
      targetLayout: 0x04090409, sourceLayout: 0x04190419},
    selectionCase(`${kind}-selection-physical-punctuation`, kind, ';bdjq [jhjij', 'живой хорошо'),
    {...selectionCase(`${kind}-selection-inverse`, kind, 'ghbdtn приvet привет', 'привет приvet ghbdtn'),
      targetLayout: 0x04090409, sourceLayout: 0x04190419},
    {...selectionCase(`${kind}-selection-only-mixed`, kind, 'приvet', 'приvet'),
      expectedStart: 5, status: 'Switched'},
  ]),
  {name: 'textarea-middle-caret', kind: 'textarea', seed: ' tail', seedStart: 0, text: 'ghbdtn tail', typed: true,
    tokens: wordTokens, start: 6, expected: 'привет tail', caret: 6},
  {name: 'contenteditable-repeated-selection', kind: 'contenteditable', text: 'ghbdtn ghbdtn', start: 0, end: 6,
    expected: 'привет ghbdtn', caret: 6},
  {name: 'textarea-newline-boundary', kind: 'textarea', text: 'ghbdtn\n', typed: true, tokens: [...wordTokens, '<Enter>'],
    expected: 'ghbdtn\n', caret: 7, status: 'Switched'},
  {name: 'input-password', kind: 'input', text: 'ghbdtn', start: 0, end: 6, expected: 'ghbdtn',
    expectedStart: 0, caret: 6, password: true, status: 'Unsupported'},
  {name: 'input-readonly', kind: 'input', text: 'ghbdtn', start: 0, end: 6, expected: 'ghbdtn',
    expectedStart: 0, caret: 6, readOnly: true, status: 'Unsupported'},
];
const tests = cases.filter(test => selectedCase ? test.name === selectedCase : suite === 'full' ||
  (suite === 'first' ? test.name === 'input-lastword' : suite === 'recent' ? test.typed : test.name.includes('-selection-')));
if (tests.length === 0) throw new Error('Unknown --case');
const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
for (const directory of [profile, temporary]) fs.mkdirSync(directory, {recursive: true});
if (!fs.existsSync(edgePath) || !fs.existsSync(probePath)) throw new Error('Installed Edge and compiled automation_probe are required');
if (typeof WebSocket !== 'function') throw new Error('This Node runtime does not provide global WebSocket');

class Cdp {
  constructor(url) {
    this.pending = new Map(); this.counter = 0; this.ws = new WebSocket(url);
    this.ready = new Promise((resolve, reject) => {
      this.ws.addEventListener('open', resolve, {once: true});
      this.ws.addEventListener('error', () => reject(new Error('CDP WebSocket failed')), {once: true});
    });
    this.ws.addEventListener('message', event => {
      const message = JSON.parse(String(event.data));
      if (!message.id) return;
      const task = this.pending.get(message.id); if (!task) return;
      this.pending.delete(message.id); clearTimeout(task.timer);
      if (message.error) task.reject(new Error(message.error.message)); else task.resolve(message.result);
    });
    this.ws.addEventListener('close', () => {
      for (const task of this.pending.values()) { clearTimeout(task.timer); task.reject(new Error('CDP closed')); }
      this.pending.clear();
    });
  }
  async call(method, params = {}) {
    await this.ready;
    const id = ++this.counter;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error(`CDP timeout: ${method}`)); }, 5000);
      this.pending.set(id, {resolve, reject, timer});
      this.ws.send(JSON.stringify({id, method, params}));
    });
  }
  close() { this.ws.close(); }
}

function probe(env, pid, extra = [], executable = probePath) {
  return new Promise((resolve, reject) => {
    const child = spawn(executable, ['--title', 'LShift Automation Fixture', '--deadline-ms', String(deadline), '--fixture-pid', String(pid), ...extra],
      {cwd: root, env, shell: false, windowsHide: true, stdio: ['ignore', 'pipe', 'pipe']});
    let stdout = ''; let stderr = '';
    child.stdout.on('data', data => { stdout += data.toString('utf8'); });
    child.stderr.on('data', data => { stderr += data.toString('utf8'); });
    child.on('error', reject);
    child.on('close', code => {
      if (code !== 0) { reject(new Error(`probe failed (${code}): ${stderr.trim() || stdout.trim()}`)); return; }
      try { resolve(JSON.parse(stdout.trim())); } catch { reject(new Error('probe returned invalid JSON')); }
    });
  });
}

let browser = null;
let page = null;
let child = null;
const results = [];
const lockPath = path.join(cache, 'automation-test.lock');
const lock = fs.openSync(lockPath, 'wx');
try {
  const activePortPath = path.join(profile, 'DevToolsActivePort');
  if (fs.existsSync(activePortPath)) {
    const oldPort = fs.readFileSync(activePortPath, 'utf8').split(/\r?\n/)[0];
    if (/^\d+$/.test(oldPort)) {
      let running = false;
      try { const response = await fetch(`http://127.0.0.1:${oldPort}/json/version`, {signal: AbortSignal.timeout(300)}); running = response.ok; } catch {}
      if (running) throw new Error('Dedicated Edge test profile is already active; it was not closed or reused');
    }
  }
  const previousPort = fs.existsSync(activePortPath) ? fs.readFileSync(activePortPath, 'utf8') : null;
  const env = {...process.env, TEMP: temporary, TMP: temporary};
  child = spawn(edgePath, [`--user-data-dir=${profile}`, '--remote-debugging-port=0', '--no-first-run',
    '--no-default-browser-check', '--disable-background-networking', '--disable-extensions', '--disable-sync',
    '--disable-default-apps', `--disk-cache-dir=${path.join(profile, 'Cache')}`, `--app=${fixtureUrl}`],
    {cwd: root, env, shell: false, windowsHide: true, stdio: 'ignore'});
  let launchError = null;
  child.on('error', error => { launchError = error; });
  let endpoint = null;
  const until = Date.now() + 15000;
  while (Date.now() < until) {
    if (launchError) throw launchError;
    if (fs.existsSync(activePortPath)) {
      const contents = fs.readFileSync(activePortPath, 'utf8');
      const [port, suffix] = contents.trim().split(/\r?\n/);
      if (contents !== previousPort && /^\d+$/.test(port) && suffix?.startsWith('/devtools/browser/')) {
        endpoint = {port, url: `ws://127.0.0.1:${port}${suffix}`}; break;
      }
    }
    await pause(100);
  }
  if (!endpoint) throw new Error('New dedicated Edge DevTools endpoint did not become ready');
  browser = new Cdp(endpoint.url); await browser.ready;
  const version = await browser.call('Browser.getVersion');
  const processes = await browser.call('SystemInfo.getProcessInfo');
  const browserPid = processes.processInfo.find(item => item.type === 'browser')?.id;
  if (!Number.isInteger(browserPid) || browserPid <= 0) throw new Error('Dedicated browser PID is unavailable');
  let target = null;
  while (Date.now() < until) {
    const response = await fetch(`http://127.0.0.1:${endpoint.port}/json/list`, {signal: AbortSignal.timeout(1000)});
    if (!response.ok) throw new Error(`Own Edge target listing failed: ${response.status}`);
    target = (await response.json()).find(item => item.type === 'page' && item.url === fixtureUrl);
    if (target) break;
    await pause(100);
  }
  if (!target) throw new Error('Own exact fixture page was not found');
  page = new Cdp(target.webSocketDebuggerUrl); await page.ready;
  await page.call('Page.bringToFront');
  async function evaluate(expression) {
    const step = expression.includes('.reset(') ? 'reset' : expression.includes('.oracle(') ? 'oracle' : 'geometry';
    let result;
    try { result = await page.call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}); }
    catch (error) { throw new Error(`${step}: ${error.message}`); }
    if (result.exceptionDetails) throw new Error('Own fixture JavaScript oracle failed');
    return result.result.value;
  }
  if (args.includes('--health-only')) {
    const healthy = await evaluate('Boolean(window.lshiftFixture && document.readyState === "complete")');
    if (!healthy) throw new Error('Own fixture DOM is not ready');
    console.log(JSON.stringify({browser: version.product, cdp: true, ownFixture: true}));
    fs.writeFileSync(reportPath, JSON.stringify({browser: version.product, cdp: true, ownFixture: true}) + '\n');
  } else {
  const geometry = await evaluate(`(() => { const r = document.querySelector('#plain').getBoundingClientRect(); return {width:innerWidth,height:innerHeight,x:r.x+r.width/2,y:r.y+r.height/2}; })()`);
  await probe(env, browserPid, ['--activate-own-fixture', '--viewport-width', String(geometry.width), '--viewport-height', String(geometry.height), '--point-x', String(geometry.x), '--point-y', String(geometry.y)]);
  if(args.includes('--live-app')) {
    results.push(...await runLiveCases({evaluate,probe,env,browserPid,pause,repeat,filter:option('--live-filter',null)}));
  } else {
  for (const test of tests) {
  for (let attempt = 1; attempt <= repeat; ++attempt) {
  const seed = test.seed ?? '';
  const initial = test.typed ? {...test, text: seed, start: test.seedStart ?? seed.length, end: test.seedStart ?? seed.length} : test;
  const focused = await evaluate(`window.lshiftFixture.reset(${JSON.stringify(initial)})`);
  if (!focused.focused) throw new Error('Own input did not receive DOM focus');
  const sourceLayout = test.sourceLayout ?? test.recentLayout ?? 0x04090409;
  const preparationProbe = legacyProbe ? path.join(cache, 'build/automation_probe.exe') : probePath;
  const prepared = await probe(env, browserPid, ['--set-layout', String(sourceLayout)], preparationProbe);
  if (prepared.status !== 'Prepared' || prepared.layout_after !== sourceLayout) throw new Error('Own fixture source layout was not prepared');
  let recentArguments = [];
  if (test.typed) {
    for (const event of typedEvents(test.tokens)) {
      await page.call('Input.dispatchKeyEvent', {type: event.down ? 'keyDown' : 'keyUp', key: event.key, code: event.code,
        windowsVirtualKeyCode: event.vk, modifiers: event.modifiers, ...(event.text ? {text: event.text, unmodifiedText: event.text} : {})});
    }
    const typed = await evaluate('window.lshiftFixture.oracle()');
    if (typed.value !== test.text || typed.start !== (test.start ?? test.text.length) || typed.start !== typed.end) {
      throw new Error(`Actual fixture typing did not match specification: ${test.name}`);
    }
    if (!legacyProbe && test.journal !== false) {
      const stream = typedEvents(test.journalTokens ?? test.tokens).map(event => `${event.vk}:${event.down ? 1 : 0}`).join(',');
      recentArguments = ['--recent-events', stream, '--recent-layout', String(sourceLayout)];
    }
    await evaluate(`window.lshiftFixture.beginObservation(${JSON.stringify({allowedValues: [test.text, test.expected]})})`);
  }
  await pause(test.waitBeforeGestureMs ?? 150);
  const outcome = await probe(env, browserPid, recentArguments);
  await pause(250);
  const oracle = await evaluate('window.lshiftFixture.oracle()');
  const expectedStatus = test.status ?? 'Submitted';
  const converted = expectedStatus === 'Submitted';
  const record = {name: test.name, attempt, ...outcome, valueMatches: oracle.value === test.expected,
    kind: test.kind, selectionLength: (test.end ?? test.start ?? test.text.length) - (test.start ?? test.text.length),
    actuallyTyped: Boolean(test.typed), journalPassed: recentArguments.length > 0,
    coldAccessibility: results.length === 0,
    observedStart: oracle.start, observedEnd: oracle.end,
    commandKeys: oracle.commandKeys,
    nonemptySelectionSamples: oracle.observation.nonempty,
    selectionSampleSources: oracle.observation.sources,
    firstNonemptySelection: oracle.observation.first,
    ctrlShiftLeftEvents: oracle.observation.ctrlShiftLeft,
    intermediateTextFrames: oracle.observation.intermediateTextFrames,
    caretMatches: oracle.start === (test.expectedStart ?? test.caret) && oracle.end === test.caret,
    inputEvents: oracle.inputEvents.length, eventTypes: oracle.inputEvents.map(event => event.type),
    preventedPastes: oracle.preventedPastes,
    allInputTrusted: oracle.inputEvents.every(event => event.trusted), withinDeadline: outcome.elapsed_ms <= deadline};
  record.layoutMatches = converted ? outcome.layout_after === (test.targetLayout ?? 0x04190419)
    : expectedStatus === 'Switched' ? outcome.layout_after !== outcome.layout_before
    : outcome.layout_after === outcome.layout_before;
  const wordUnits = test.wordUnits ?? test.caret - (test.seedStart ?? test.seed?.length ?? 0);
  record.deleteEvents = record.eventTypes.filter(type => type === 'deleteContentBackward').length;
  record.insertTextEvents = record.eventTypes.filter(type => type === 'insertText').length;
  record.pasteEvents = record.eventTypes.filter(type => type === 'insertFromPaste').length;
  record.eventMatches = !converted ? record.inputEvents === 0 : oracle.observation.baselineSelected
    ? record.inputEvents === 1 && record.pasteEvents === 1 && record.allInputTrusted
    : record.deleteEvents === wordUnits && record.insertTextEvents === wordUnits && record.pasteEvents === 0 &&
      record.inputEvents === 2 * wordUnits && record.allInputTrusted && record.preventedPastes === 0;
  record.noProgramSelection = (oracle.observation.baselineSelected || record.nonemptySelectionSamples === 0) && record.ctrlShiftLeftEvents === 0;
  record.noIntermediateFrames = oracle.observation.baselineSelected || record.intermediateTextFrames === 0;
  record.undoMatches = null;
  record.undoSteps = 0;
  record.roundTripMatches = !test.roundTrip;
  if (converted && outcome.status === 'Submitted' && record.valueMatches) {
    let expectedUndo = test.text;
    if (test.roundTrip) {
      // Повторный жест использует те же физические VK, но уже текущую HKL целевого окна.
      record.roundTripMatches = true;
      for (let trip = 0; trip < (test.roundTrips ?? 1); ++trip) {
      const reverse = await probe(env, browserPid, recentArguments.slice(0, 2));
      await pause(250);
      const reversed = await evaluate('window.lshiftFixture.oracle()');
      record.roundTripMatches &&= reverse.status === 'Submitted' && reverse.layout_after === (trip % 2 === 0 ? 0x04090409 : 0x04190419) &&
        reversed.value === (trip % 2 === 0 ? test.text : test.expected) && reversed.start === test.caret && reversed.end === test.caret &&
        reversed.inputEvents.filter(event => event.type === 'insertText').length === (trip + 2) * wordUnits &&
        reversed.inputEvents.filter(event => event.type === 'deleteContentBackward').length === (trip + 2) * wordUnits &&
        reversed.inputEvents.length === 2 * (trip + 2) * wordUnits &&
        reversed.inputEvents.every(event => event.trusted && ['insertText', 'deleteContentBackward'].includes(event.type));
      record.nonemptySelectionSamples = reversed.observation.nonempty;
      record.selectionSampleSources = reversed.observation.sources;
      record.firstNonemptySelection = reversed.observation.first;
      record.ctrlShiftLeftEvents = reversed.observation.ctrlShiftLeft;
      record.intermediateTextFrames = reversed.observation.intermediateTextFrames;
      record.noProgramSelection = reversed.observation.nonempty === 0 && reversed.observation.ctrlShiftLeft === 0;
      record.noIntermediateFrames = record.intermediateTextFrames === 0;
      expectedUndo = trip % 2 === 0 ? test.expected : test.text;
      }
    }
    // CDP посылает Undo только своему редактору; глобальные клавиши и чужие HWND не используются.
    if (!oracle.focused) throw new Error('Own editor lost focus before Undo');
    await evaluate('window.lshiftFixture.stopObservation()');
    for (let step = 0; step < 12; ++step) {
      await page.call('Input.dispatchKeyEvent', {type: 'rawKeyDown', key: 'z', code: 'KeyZ', windowsVirtualKeyCode: 90, modifiers: 2});
      await page.call('Input.dispatchKeyEvent', {type: 'keyUp', key: 'z', code: 'KeyZ', windowsVirtualKeyCode: 90, modifiers: 2});
      ++record.undoSteps;
      await pause(75);
      const undone = await evaluate('window.lshiftFixture.oracle()');
      if (undone.value === expectedUndo) {
        record.undoMatches = undone.inputEvents.at(-1)?.type === 'historyUndo' && undone.inputEvents.at(-1).trusted;
        break;
      }
    }
  }
  record.passed = record.status === expectedStatus && record.valueMatches && record.caretMatches &&
    record.eventMatches && record.withinDeadline && record.layoutMatches && record.roundTripMatches &&
    record.noProgramSelection && (!converted || record.undoMatches === true);
  results.push(record);
  }
  }
  }
  const report = {browser: version.product, forcedAccessibility: false, probe: probePath, deadlineMs: deadline, suite,
    mode: args.includes('--live-app') ? 'live-app' : 'text-service',
    strictFrameCheckPassed: args.includes('--live-app') ? null : results.every(record => record.noIntermediateFrames), results};
  fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report, null, 2));
  if (results.some(record => !record.passed)) process.exitCode = 1;
  }
} catch (error) {
  fs.writeFileSync(reportPath, JSON.stringify({results, error: error.message}, null, 2) + '\n');
  console.error(`FAIL: ${error.message}`);
  process.exitCode = 1;
} finally {
  page?.close();
  if (browser) {
    try { await browser.call('Browser.close'); } catch {}
    browser.close();
  } else if (child && child.exitCode === null) {
    // Только непосредственно запущенный собственный процесс, никогда другие Edge PID.
    child.kill();
  }
  fs.closeSync(lock); fs.unlinkSync(lockPath);
}

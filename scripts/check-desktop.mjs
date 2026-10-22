import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

// Запускать при свободном вводе: проверки последовательно активируют только свои окна.
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const temporary = path.join(root, '.cache/temp');
fs.mkdirSync(temporary, {recursive: true});
const env = {...process.env, TEMP: temporary, TMP: temporary};
const version = fs.readFileSync(path.join(root, 'CMakeLists.txt'), 'utf8').match(/project\(LShift VERSION (\d+\.\d+\.\d+)/)?.[1];
if(!version) throw new Error('Project version is missing');
const report = path.join(root, `.cache/desktop-${version}.json`);
const checks = [
  {name: 'browser', exe: process.execPath, args: ['scripts/automation-test.mjs', '--suite', 'full', '--report', report]},
  {name: `native-window-${version}`, exe: path.join(root, '.cache/build/native_tests.exe'), args: ['--own-window']},
  {name: `tray-${version}`, exe: path.join(root, '.cache/build/tray_tests.exe'), args: []},
];
for (const check of checks) {
  const result = spawnSync(check.exe, check.args, {
    cwd: root, env, shell: false, windowsHide: true, encoding: 'utf8', timeout: 60000,
  });
  fs.writeFileSync(path.join(root, `.cache/${check.name}-test.log`), (result.stdout ?? '') + (result.stderr ?? ''));
  console.log(JSON.stringify({check: check.name, exit: result.status, error: result.error?.message}));
  if (check.name === 'browser' && fs.existsSync(report)) {
    const observed = JSON.parse(fs.readFileSync(report, 'utf8'));
    console.log(JSON.stringify({error: observed.error, results: observed.results?.map(item => ({
      name: item.name, passed: item.passed, status: item.status, elapsed_ms: item.elapsed_ms,
      nonemptySelectionSamples: item.nonemptySelectionSamples, intermediateTextFrames: item.intermediateTextFrames,
      undoSteps: item.undoSteps,
    }))}));
  }
  if (result.status !== 0) process.exit(result.status ?? 1);
}

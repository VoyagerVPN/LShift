import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const cache = path.join(root, '.cache');
const temp = path.join(cache, 'temp');
fs.mkdirSync(temp, {recursive: true});
const candidates = [
  'C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools',
  'C:/Program Files/Microsoft Visual Studio/18/Community',
  'C:/Program Files/Microsoft Visual Studio/2022/Community',
  'C:/Program Files/Microsoft Visual Studio/2022/Enterprise',
  'C:/Program Files/Microsoft Visual Studio/2022/Professional',
  'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools',
];
const vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe';
let detected = '';
if (fs.existsSync(vswhere)) {
  const discovery = spawnSync(vswhere, ['-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', 'Microsoft.VisualStudio.Component.VC.CMake.Project',
    '-property', 'installationPath'],
    {cwd: root, env: {...process.env, TEMP: temp, TMP: temp}, shell: false, windowsHide: true, encoding: 'utf8'});
  if (discovery.error) throw discovery.error;
  if (discovery.status !== 0) throw new Error('Visual Studio discovery failed');
  detected = discovery.stdout.trim();
}
const completeInstall = p => fs.existsSync(path.join(p, 'VC/Tools/MSVC')) &&
  fs.existsSync(path.join(p, 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')) &&
  fs.existsSync(path.join(p, 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'));
const vs = (detected && completeInstall(detected) ? detected : '') || candidates.find(completeInstall);
if (!vs) throw new Error('MSVC C++ Build Tools not found');
const vc = path.join(vs, 'VC/Tools/MSVC', fs.readdirSync(path.join(vs, 'VC/Tools/MSVC')).sort().at(-1));
const kits = 'C:/Program Files (x86)/Windows Kits/10';
const sdkVersion = fs.readdirSync(path.join(kits, 'Include')).filter(v => fs.existsSync(path.join(kits, 'Lib', v, 'um/x64/kernel32.lib'))).sort().at(-1);
const cmakeRoot = path.join(vs, 'Common7/IDE/CommonExtensions/Microsoft/CMake');
const cmake = path.join(cmakeRoot, 'CMake/bin/cmake.exe');
const ninja = path.join(cmakeRoot, 'Ninja/ninja.exe');
const env = {...process.env,
  TEMP: temp, TMP: temp,
  PATH: [path.join(vc, 'bin/Hostx64/x64'), path.join(kits, 'bin', sdkVersion, 'x64'), path.dirname(cmake), path.dirname(ninja), process.env.PATH].join(';'),
  INCLUDE: [path.join(vc, 'include'), ...['ucrt','shared','um','winrt','cppwinrt'].map(p => path.join(kits, 'Include', sdkVersion, p))].join(';'),
  LIB: [path.join(vc, 'lib/x64'), ...['ucrt','um'].map(p => path.join(kits, 'Lib', sdkVersion, p, 'x64'))].join(';'),
};
function run(exe, args) {
  const result = spawnSync(exe, args, {cwd: root, env, shell: false, windowsHide: true, encoding: 'utf8'});
  process.stdout.write(result.stdout ?? ''); process.stderr.write(result.stderr ?? '');
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`Command failed with exit code ${result.status ?? 1}: ${exe}`);
}
const coreOnly = process.argv.includes('--core-only');
const diagnostics = process.argv.includes('--diagnostics');
const target = process.argv.find(arg => arg.startsWith('--target='))?.slice('--target='.length);
const build = path.join(cache, coreOnly ? 'core-build' : diagnostics ? 'diagnostic-build' : 'build');
const lockPath = `${build}.lock`;
let lock;
try {
  lock = fs.openSync(lockPath, 'wx');
} catch (error) {
  if (error.code === 'EEXIST') throw new Error(`Another build owns ${lockPath}`);
  throw error;
}
try {
  fs.writeSync(lock, String(process.pid));
  if (!coreOnly) run(process.execPath, [path.join(root, 'scripts/make-icon.mjs')]);
  run(cmake, ['-S', root, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', `-DCMAKE_MAKE_PROGRAM=${ninja}`,
    `-DLSHIFT_BUILD_APP=${coreOnly ? 'OFF' : 'ON'}`, `-DLSHIFT_DIAGNOSTICS=${diagnostics ? 'ON' : 'OFF'}`,
    `-DLSHIFT_APP_OUTPUT_DIRECTORY=${diagnostics ? path.join(cache, 'diagnostic-dist') : path.join(root, 'dist')}`]);
  run(cmake, ['--build', build, ...(target ? ['--target', target] : [])]);
  if (!target) run(path.join(path.dirname(cmake), 'ctest.exe'), ['--test-dir', build, '--output-on-failure']);
} finally {
  fs.closeSync(lock);
  fs.unlinkSync(lockPath);
}

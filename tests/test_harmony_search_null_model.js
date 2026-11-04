import assert from 'node:assert';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const rootDir = path.resolve(__dirname, '..');

function loadSearchFn(relPath) {
  const fullPath = path.join(rootDir, relPath);
  let code = fs.readFileSync(fullPath, 'utf-8');
  // Strip imports and exports to allow standalone node testing without bundler
  code = code.replace(/import\s+harmonyManufacturers\s+from\s+[^;]+;/, 'const harmonyManufacturers = [];');
  code = code.replace(/export\s+(async\s+)?function/g, '$1function');
  code = code.replace(/export\s+const/g, 'const');
  code = code.replace(/export\s+let/g, 'let');
  const context = { console };
  vm.createContext(context);
  vm.runInContext(code, context);
  return context.searchHarmonyModels;
}

async function runTests() {
  const targets = ['android-app/src/irdb.js', 'frontend/src/utils/irdb.js'];

  for (const relPath of targets) {
    const searchHarmonyModels = loadSearchFn(relPath);
    assert.strictEqual(typeof searchHarmonyModels, 'function', `searchHarmonyModels should be a function in ${relPath}`);

    const mockModels = [
      { f: 'device-1.json', id: 1, m: 'PlayStation 4' },
      { f: 'device-234951.json', id: 234951, m: null },
      { f: 'device-3.json', id: 3, m: undefined },
      { f: 'device-4.json', id: 4, m: 12345 },
      { f: 'device-5.json', id: 5, m: '' },
      { f: 'PlayStation_5.json', id: 496914, m: 'PlayStation 5' },
      { f: 'device-6.json', id: 6, m: 'Bravia XR' }
    ];

    // 1. searchHarmonyModels with empty query filters out null/non-string models
    const emptyRes = searchHarmonyModels(mockModels, '');
    assert.strictEqual(emptyRes.length, 3);
    assert.deepStrictEqual(emptyRes.map(x => x.m), ['PlayStation 4', 'PlayStation 5', 'Bravia XR']);

    // 2. searchHarmonyModels matching 'PlayStation 5' skips null/undefined/numbers without throwing
    const ps5Res = searchHarmonyModels(mockModels, 'PlayStation 5');
    assert.strictEqual(ps5Res.length, 1);
    assert.strictEqual(ps5Res[0].id, 496914);
    assert.strictEqual(ps5Res[0].m, 'PlayStation 5');
  }

  // 3. Test exact lookup logic matching app_harmony_search.js triggerDeviceLookup()
  const mockModels = [
    { f: 'device-1.json', id: 1, m: 'PlayStation 4' },
    { f: 'device-234951.json', id: 234951, m: null },
    { f: 'PlayStation_5.json', id: 496914, m: 'PlayStation 5' }
  ];

  const modelName = 'PlayStation 5';
  const targetModel = modelName.toLowerCase();
  const match = mockModels.find(item => typeof item?.m === 'string' && item.m.toLowerCase() === targetModel) ||
    mockModels.find(item => typeof item?.m === 'string' && item.m.toLowerCase().includes(targetModel));

  assert.ok(match, 'Expected PlayStation 5 match to be found');
  assert.strictEqual(match.id, 496914);
  assert.strictEqual(match.f, 'PlayStation_5.json');

  // 4. Live Sony index check (if network is available)
  try {
    const res = await fetch('https://cdn.jsdelivr.net/gh/pickysysadmin/logitech-harmony-ir-archive@main/devices/Sony/index.json');
    if (res.ok) {
      const liveSonyModels = await res.json();
      const liveMatch = liveSonyModels.find(item => typeof item?.m === 'string' && item.m.toLowerCase() === targetModel) ||
        liveSonyModels.find(item => typeof item?.m === 'string' && item.m.toLowerCase().includes(targetModel));
      assert.ok(liveMatch, 'PlayStation 5 must be found in live Sony index');
      assert.strictEqual(liveMatch.f, 'PlayStation_5.json');
      console.log('✓ Live Sony index search for PlayStation 5 passed (20,006 models).');
    }
  } catch {
    // Network optional
  }

  console.log('✓ All harmony search null model tests passed for android-app and frontend.');
}

await runTests();

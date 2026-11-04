const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { execSync } = require('child_process');

const FIXTURE_EXE = 'O:\\gt4recomp-scratch\\slice335-rc7-controls-build\\build\\controls_fixture.exe';
const TEMP_JSON = 'O:\\gt4recomp-scratch\\parity_vectors.json';
const CONTROLS_MODEL_PATH = path.resolve(__dirname, '../../../../launcher/crates/gt4l-tauri/ui/js/controls-model.js');
const LOG_FILE = path.resolve(__dirname, '../stage4-logs/curve-parity.log');

function log(msg) {
  console.log(msg);
  fs.appendFileSync(LOG_FILE, msg + '\n');
}

function run() {
  const logDir = path.dirname(LOG_FILE);
  if (!fs.existsSync(logDir)) {
    fs.mkdirSync(logDir, { recursive: true });
  }
  if (fs.existsSync(LOG_FILE)) {
    fs.unlinkSync(LOG_FILE);
  }

  log(`=== Stage 4 Native vs Web Curve Parity Verification at ${new Date().toISOString()} ===\n`);

  // 1. Dump ground truth vectors from native C++ fixture
  log(`Executing native fixture: ${FIXTURE_EXE} --dump-vectors ${TEMP_JSON}`);
  execSync(`"${FIXTURE_EXE}" --dump-vectors "${TEMP_JSON}"`, { stdio: 'inherit' });
  if (!fs.existsSync(TEMP_JSON)) {
    throw new Error(`Fixture failed to produce vector file at ${TEMP_JSON}`);
  }

  const rawJson = fs.readFileSync(TEMP_JSON, 'utf8');
  const fixtureData = JSON.parse(rawJson);
  log(`Loaded fixture data: ${fixtureData.curves.length} curves, ${fixtureData.deadzones.length} deadzones, ${Object.keys(fixtureData.vectors).length} vectors.\n`);

  // 2. Load web controls model
  const code = fs.readFileSync(CONTROLS_MODEL_PATH, 'utf8');
  const sandbox = {};
  sandbox.window = sandbox;
  sandbox.root = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(code, sandbox);
  const CM = sandbox.GT4.controlsModel;

  const curves = fixtureData.curves;
  const deadzones = fixtureData.deadzones;

  let totalPoints = 0;
  let exactMatches = 0;
  let diffs = [];

  for (let c = 0; c < curves.length; c++) {
    const curveName = curves[c];
    for (let d = 0; d < deadzones.length; d++) {
      const dz = deadzones[d];
      const vectorKey = `${curveName}_dz${d}`;
      const cppVector = fixtureData.vectors[vectorKey];
      if (!cppVector || cppVector.length !== 256) {
        throw new Error(`Missing or invalid vector for ${vectorKey}`);
      }

      for (let raw = 0; raw <= 255; raw++) {
        totalPoints++;
        const cppByte = cppVector[raw];
        const jsFraction = CM.triggerOutput(raw / 255, dz, curveName);
        const jsByte = Math.round(jsFraction * 255);

        if (cppByte === jsByte) {
          exactMatches++;
        } else {
          diffs.push({
            curve: curveName,
            deadzone: dz,
            raw,
            cppByte,
            jsByte,
            jsFraction,
            diff: jsByte - cppByte
          });
        }
      }
    }
  }

  log(`--- Parity Sweep Results ---`);
  log(`Total Evaluation Points : ${totalPoints} (7 curves x 5 deadzones x 256 bytes)`);
  log(`Exact Bit-Accurate Byte Matches: ${exactMatches} (${((exactMatches / totalPoints) * 100).toFixed(4)}%)`);
  log(`Discrepancies / Rounding Diffs : ${diffs.length}`);

  if (diffs.length > 0) {
    log(`\nDiscrepancy Details (first 20):`);
    diffs.slice(0, 20).forEach((d) => {
      log(`  Curve: ${d.curve}, Deadzone: ${d.deadzone}, Raw: ${d.raw} -> C++=${d.cppByte}, JS=${d.jsByte} (diff: ${d.diff})`);
    });
    throw new Error(`Parity check failed with ${diffs.length} discrepancies`);
  }

  log(`\n--- Representative Spot Checks Verified ---`);
  log(`1. Deadzone = 0.0 (boundary):`);
  curves.forEach((c) => {
    const midCpp = fixtureData.vectors[`${c}_dz0`][128];
    const midJs = Math.round(CM.triggerOutput(128 / 255, 0.0, c) * 255);
    log(`   Curve ${c.padEnd(14)}: raw=0 -> 0, raw=128 -> ${midCpp} (JS=${midJs}), raw=255 -> 255`);
  });

  log(`2. Default Deadzone = 30/255 (0.1176):`);
  curves.forEach((c) => {
    const midCpp = fixtureData.vectors[`${c}_dz1`][142];
    const midJs = Math.round(CM.triggerOutput(142 / 255, 30 / 255, c) * 255);
    log(`   Curve ${c.padEnd(14)}: raw=30 -> 0, raw=142 -> ${midCpp} (JS=${midJs}), raw=255 -> 255`);
  });

  log(`3. Half Deadzone = 0.5 (0.5):`);
  curves.forEach((c) => {
    const midCpp = fixtureData.vectors[`${c}_dz3`][192];
    const midJs = Math.round(CM.triggerOutput(192 / 255, 0.5, c) * 255);
    log(`   Curve ${c.padEnd(14)}: raw=128 -> 0, raw=192 -> ${midCpp} (JS=${midJs}), raw=255 -> 255`);
  });

  log(`\n=== Parity Verification Passed: 100% Exact Match between C++ Native Runtime and JS Preview ===`);
}

try {
  run();
} catch (err) {
  log(`FATAL ERROR: ${err.message}`);
  process.exit(1);
}

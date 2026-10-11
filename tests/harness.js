// The page tests' shared part (tests/test_*.js; tests/run.sh runs them): src/web's files run in a sandbox
// the way the page loads them, and the checks with their totals.

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const web = path.join(__dirname, '..', 'src', 'web');
let checks = 0, failures = 0;

// Runs src/web's `files`, in that order, in one sandbox holding `globals` (what each file needs from the
// browser: TextEncoder, fetch, document...). Returns its window, where each file leaves its internals.
function load(files, globals = {}) {
  const window = {};
  const context = vm.createContext({ window, ...globals });
  for (const f of [].concat(files)) vm.runInContext(fs.readFileSync(path.join(web, f), 'utf8'), context);
  return window;
}

function check(cond, what) {
  checks++;
  if (!cond) {
    failures++;
    console.log('  FAIL ' + what);
  }
}

// The totals, and the exit code.
function done(name) {
  console.log(failures ? `${name}: ${failures} of ${checks} checks failed` : `${name}: ${checks} checks ok`);
  process.exit(failures ? 1 : 0);
}

module.exports = { web, load, check, done };

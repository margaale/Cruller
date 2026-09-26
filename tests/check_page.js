// Syntax check of the web page's JavaScript, which lives in src/http.c as C string literals: a stray
// C escape there (a "\n" meant as "\\n", say) breaks the whole page. Run by tests/run.sh when node
// is available.

const fs = require('fs');
const path = require('path');

const src = fs.readFileSync(path.join(__dirname, '..', 'src', 'http.c'), 'utf8');
const start = src.indexOf('static const char PAGE[] =');
if (start < 0) throw new Error('PAGE not found in src/http.c');
const end = src.indexOf('";', start);
const region = src.slice(start, end + 1)
    .split('\n')
    .filter((line) => !line.trim().startsWith('//'))   // C comments between the literals
    .join('\n');

// Concatenate the C string literals, undoing C escapes.
let page = '';
for (const m of region.matchAll(/"((?:[^"\\\n]|\\.)*)"/g)) {
    page += m[1].replace(/\\(.)/g, (_, c) => ({ n: '\n', r: '\r', t: '\t', '"': '"', '\\': '\\', "'": "'" }[c] ?? c));
}

const scripts = [...page.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
if (!scripts.length) throw new Error('no <script> in the page');
let ok = true;
scripts.forEach((code, i) => {
    try {
        new Function(code); // parses without running
    } catch (e) {
        ok = false;
        console.log(`page script ${i}: ${e.message}`);
    }
});
console.log(ok ? `page: ${scripts.length} script(s), ${page.length} bytes, syntax ok` : 'page: syntax errors');
process.exit(ok ? 0 : 1);

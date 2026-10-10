// Checks of the web page (src/web/index.html + app.js): app.js parses, and every element it looks up
// by id ($('...') or text('...')) exists in index.html, so a renamed element can't silently break a
// panel. Run by tests/run.sh when node is available.

const fs = require('fs');
const path = require('path');

const web = path.join(__dirname, '..', 'src', 'web');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const app = fs.readFileSync(path.join(web, 'app.js'), 'utf8');

let ok = true;
try {
    new Function(app); // parses without running
} catch (e) {
    ok = false;
    console.log(`app.js: ${e.message}`);
}

const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]));
ids.add('fwh'); // made by fw.js inside #fw
const used = new Set([...app.matchAll(/(?<![\w.])(?:\$|text|rows)\((['"])([\w-]+)\1[,)]/g)].map((m) => m[2])); // (either quotes: minified too)
for (const id of used) {
    if (!ids.has(id)) {
        ok = false;
        console.log(`app.js uses #${id}, which index.html doesn't have`);
    }
}
console.log(ok ? `page: app.js syntax ok, ${used.size} ids all in index.html` : 'page: errors');
process.exit(ok ? 0 : 1);

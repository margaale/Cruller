// Checks of the routes (src/core/http.c's handle()): every route docs/API.md documents is there with
// its method, the old names it lists too, every path the page fetches exists, and the doc's version is
// HTTP_API_VERSION. A route renamed or dropped can't silently break a client. Run by tests/run.sh when
// node is available.

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = (f) => fs.readFileSync(path.join(root, f), 'utf8');
const http = read('src/core/http.c');
const doc = read('docs/API.md');

let ok = true;
const fail = (msg) => {
    ok = false;
    console.log(msg);
};

// The routes: "else if (post && (!strcmp(r->path, "/a") || !strcmp(r->path, "/b"))) ..." -> POST /a, POST /b.
const body = http.slice(http.indexOf('static void handle(request_t *r) {'));
const table = body.slice(0, body.indexOf('\n}\n'));
const routes = new Map(); // path -> Set of methods
for (const line of table.split('\n')) {
    const paths = [...line.matchAll(/strcmp\(r->path, "([^"]+)"\)/g)].map((m) => m[1]);
    if (!paths.length) continue;
    const methods = [];
    if (/\(\s*get\s*\|\|\s*post\s*\)|\bget &&/.test(line)) methods.push('GET');
    if (/\(\s*get\s*\|\|\s*post\s*\)|\bpost &&/.test(line)) methods.push('POST');
    for (const p of paths) {
        if (!routes.has(p)) routes.set(p, new Set());
        methods.forEach((m) => routes.get(p).add(m));
    }
}
if (routes.size < 10) fail(`http.c: found only ${routes.size} routes in handle(); has its shape changed?`);

// docs/API.md: "GET /api/v1/info" with its method; a bare `/api/...` (the old names) with any, except
// a version's root (`/api/v1`).
const documented = new Set([...doc.matchAll(/\b(GET|POST) (\/api\/[\w/]+)/g)].map((m) => `${m[1]} ${m[2]}`));
for (const route of documented) {
    const [method, p] = route.split(' ');
    if (!routes.has(p)) fail(`docs/API.md documents ${route}, which http.c doesn't route`);
    else if (!routes.get(p).has(method)) fail(`docs/API.md documents ${route}; http.c routes it only as ${[...routes.get(p)].join(', ')}`);
}
for (const p of new Set([...doc.matchAll(/`(\/api\/[\w/]+)`/g)].map((m) => m[1]))) {
    if (!routes.has(p) && !/^\/api\/v\d+$/.test(p)) fail(`docs/API.md names ${p}, which http.c doesn't route`);
}
if (!documented.size) fail('docs/API.md: no routes found');

// The version: HTTP_API_VERSION, the doc's /api/v1/info example, and the paths.
const version = (read('src/core/http.h').match(/#define HTTP_API_VERSION "(\d+)"/) || [])[1];
const docVersion = (doc.match(/"api_version": (\d+)/) || [])[1];
if (!version) fail('http.h: no HTTP_API_VERSION');
else if (docVersion !== version) fail(`docs/API.md's api_version is ${docVersion}, HTTP_API_VERSION is ${version}`);
else if (![...routes.keys()].some((p) => p.startsWith(`/api/v${version}/`))) fail(`http.c routes nothing under /api/v${version}/`);

// The page: every path it fetches (fetch('/x...'), xhr.open('POST', '/x...')) is a route.
let fetched = 0;
for (const f of ['app.js', 'fw.js', 'sd.js', 'profiles.js', 'mapper.js']) {
    const js = read(path.join('src', 'web', f));
    for (const [, p] of js.matchAll(/(?:fetch\(|\.open\('\w+',\s*)'(\/[^'?]*)/g)) {
        fetched++;
        if (!routes.has(p)) fail(`${f} fetches ${p}, which http.c doesn't route`);
    }
}

console.log(ok ? `api: ${documented.size} documented routes and ${fetched} page fetches all in http.c (v${version})` : 'api: errors');
process.exit(ok ? 0 : 1);

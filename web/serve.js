// serve.js — zero-dependency static server for the Ribosome shell.
// Usage: node serve.js [port]   (serves the web/ directory)
const http = require('http');
const fs = require('fs');
const path = require('path');

const root = __dirname;
const port = Number(process.argv[2] || 8086);
const mime = {
    '.html': 'text/html; charset=utf-8',
    '.wasm': 'application/wasm',
    '.js': 'text/javascript',
    '.rna': 'text/plain; charset=utf-8',
};

http.createServer((req, res) => {
    const url = decodeURIComponent(req.url.split('?')[0]);
    let file = path.join(root, url === '/' ? 'index.html' : url);
    if (!file.startsWith(root)) { res.writeHead(403); res.end(); return; }
    fs.readFile(file, (err, data) => {
        if (err) { res.writeHead(404); res.end('404'); return; }
        res.writeHead(200, { 'Content-Type': mime[path.extname(file)] || 'application/octet-stream' });
        res.end(data);
    });
}).listen(port, '127.0.0.1', () => console.log(`serving ${root} on http://127.0.0.1:${port}`));

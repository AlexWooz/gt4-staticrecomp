const { chromium } = require('playwright');
const http = require('http');
const fs = require('fs');
const path = require('path');

const UI_DIR = path.resolve(__dirname, '../../../../launcher/crates/gt4l-tauri/ui');
const LOG_FILE = path.resolve(__dirname, '../stage3-logs/ui-checks.log');
const PORT = 8769;

function log(msg) {
  console.log(msg);
  fs.appendFileSync(LOG_FILE, msg + '\n');
}

function startServer() {
  const mimeTypes = {
    '.html': 'text/html',
    '.js': 'text/javascript',
    '.css': 'text/css',
    '.json': 'application/json',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.svg': 'image/svg+xml',
    '.ttf': 'font/ttf',
    '.woff2': 'font/woff2'
  };

  const server = http.createServer((req, res) => {
    let reqPath = req.url.split('?')[0].split('#')[0];
    if (reqPath === '/favicon.ico') {
      res.writeHead(204);
      res.end();
      return;
    }
    if (reqPath === '/' || reqPath === '') reqPath = '/index.html';
    const filePath = path.join(UI_DIR, reqPath);

    fs.readFile(filePath, (err, data) => {
      if (err) {
        log(`[HTTP 404] ${req.url} -> ${filePath}`);
        res.writeHead(404, { 'Content-Type': 'text/plain' });
        res.end('Not Found');
        return;
      }
      const ext = path.extname(filePath).toLowerCase();
      const contentType = mimeTypes[ext] || 'application/octet-stream';
      res.writeHead(200, { 'Content-Type': contentType });
      res.end(data);
    });
  });

  return new Promise((resolve) => {
    server.listen(PORT, '127.0.0.1', () => {
      resolve(server);
    });
  });
}

async function run() {
  if (fs.existsSync(LOG_FILE)) {
    fs.unlinkSync(LOG_FILE);
  }
  const server = await startServer();
  let browser;
  try {
    browser = await chromium.launch({
      channel: 'msedge',
      headless: true
    });
    const context = await browser.newContext({
      viewport: { width: 1280, height: 720 },
      deviceScaleFactor: 1
    });

    log(`=== UI Check Execution at ${new Date().toISOString()} ===\n`);

    // 1. Focus check en-US
    {
      log('--- Focus Check (lang=en-US) ---');
      const page = await context.newPage();
      const jsErrors = [];
      page.on('pageerror', (err) => jsErrors.push(err.message));
      page.on('console', (msg) => {
        log(`[BROWSER CONSOLE ${msg.type()}] ${msg.text()}`);
      });
      page.on('requestfailed', (req) => {
        log(`[REQUEST FAILED] ${req.url()} - ${req.failure().errorText}`);
      });

      await page.goto(`http://127.0.0.1:${PORT}/dev/focus-check.html?lang=en-US`);
      await page.waitForSelector('pre#focus-check-results', { state: 'attached', timeout: 90000 });
      const status = await page.locator('body').getAttribute('data-focus-check');
      const resultsText = await page.locator('pre#focus-check-results').textContent();
      const summaryLine = resultsText.split('\n')[0];
      log(`Status: ${status}`);
      log(`Summary: ${summaryLine}`);
      log(`JS Errors (${jsErrors.length}): ${jsErrors.join('; ') || 'none'}`);
      if (status !== 'pass') {
        const failures = resultsText.split('\n').filter(l => l.startsWith('FAIL'));
        log(`Failures:\n${failures.join('\n')}`);
      }
      await page.close();
      log('');
    }

    // 2. Focus check pt-BR
    {
      log('--- Focus Check (lang=pt-BR) ---');
      const page = await context.newPage();
      const jsErrors = [];
      page.on('pageerror', (err) => jsErrors.push(err.message));
      page.on('console', (msg) => {
        log(`[BROWSER CONSOLE ${msg.type()}] ${msg.text()}`);
      });
      page.on('requestfailed', (req) => {
        log(`[REQUEST FAILED] ${req.url()} - ${req.failure().errorText}`);
      });

      await page.goto(`http://127.0.0.1:${PORT}/dev/focus-check.html?lang=pt-BR`);
      await page.waitForSelector('pre#focus-check-results', { state: 'attached', timeout: 90000 });
      const status = await page.locator('body').getAttribute('data-focus-check');
      const resultsText = await page.locator('pre#focus-check-results').textContent();
      const summaryLine = resultsText.split('\n')[0];
      log(`Status: ${status}`);
      log(`Summary: ${summaryLine}`);
      log(`JS Errors (${jsErrors.length}): ${jsErrors.join('; ') || 'none'}`);
      if (status !== 'pass') {
        const failures = resultsText.split('\n').filter(l => l.startsWith('FAIL'));
        log(`Failures:\n${failures.join('\n')}`);
      }
      await page.close();
      log('');
    }

    // 3. Navigation test
    {
      log('--- Spatial Navigation Test (nav-test.html) ---');
      const page = await context.newPage();
      const jsErrors = [];
      page.on('pageerror', (err) => jsErrors.push(err.message));
      page.on('console', (msg) => {
        log(`[BROWSER CONSOLE ${msg.type()}] ${msg.text()}`);
      });
      page.on('requestfailed', (req) => {
        log(`[REQUEST FAILED] ${req.url()} - ${req.failure().errorText}`);
      });

      await page.goto(`http://127.0.0.1:${PORT}/dev/nav-test.html`);
      await page.waitForSelector('#summary:not(:empty)', { state: 'attached', timeout: 10000 });
      const summaryText = await page.locator('#summary').textContent();
      const steps = await page.locator('#results li').allTextContents();
      const failures = steps.filter(s => !s.includes('PASS'));
      log(`Summary: ${summaryText}`);
      log(`Total steps: ${steps.length}, Failures: ${failures.length}`);
      log(`JS Errors (${jsErrors.length}): ${jsErrors.join('; ') || 'none'}`);
      if (failures.length > 0) {
        log(`Failing steps:\n${failures.join('\n')}`);
      }
      await page.close();
      log('');
    }

    log('=== UI Checks Finished Successfully ===');
  } finally {
    if (browser) await browser.close();
    server.close();
  }
}

run().catch((err) => {
  console.error('Fatal error in UI checks:', err);
  process.exit(1);
});

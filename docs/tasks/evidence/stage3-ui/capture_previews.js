const { chromium } = require('playwright');
const http = require('http');
const fs = require('fs');
const path = require('path');

const UI_DIR = path.resolve(__dirname, '../../../../launcher/crates/gt4l-tauri/ui');
const OUT_DIR = __dirname;
const PORT = 8768;

if (!fs.existsSync(OUT_DIR)) {
  fs.mkdirSync(OUT_DIR, { recursive: true });
}

// Simple static HTTP server
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
        console.log(`404: ${req.url} -> ${filePath}`);
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
      console.log(`Test server running at http://127.0.0.1:${PORT}`);
      resolve(server);
    });
  });
}

const CASES = [
  {
    name: 'gt4-signed-in-en',
    url: `http://127.0.0.1:${PORT}/dev/index.html?lang=en-US&ra=signedin#/settings/achievements`,
    outFiles: ['achievements-en-us-gt4-signed-in.png', 'achievements-en-us-gt4.png'],
    verify: async (page) => {
      const text = await page.locator('.ra-page').textContent();
      if (!text.includes('DemoDriver')) throw new Error('Expected DemoDriver in page text');
      const rowCount = await page.locator('.ra-ach-row').count();
      if (rowCount !== 3) throw new Error(`Expected 3 achievement rows, got ${rowCount}`);
      const bannerCount = await page.locator('.ra-unsupported-banner').count();
      if (bannerCount !== 0) throw new Error('Unsupported banner should not be present');
    }
  },
  {
    name: 'gt4-signed-in-pt',
    url: `http://127.0.0.1:${PORT}/dev/index.html?lang=pt-BR&ra=signedin#/settings/achievements`,
    outFiles: ['achievements-pt-br-gt4-signed-in.png', 'achievements-pt-br-gt4.png'],
    verify: async (page) => {
      const text = await page.locator('.ra-page').textContent();
      if (!text.includes('DemoDriver')) throw new Error('Expected DemoDriver in page text');
      if (!text.includes('Conectado como')) throw new Error('Expected Portuguese text Conectado como');
      const rowCount = await page.locator('.ra-ach-row').count();
      if (rowCount !== 3) throw new Error(`Expected 3 achievement rows, got ${rowCount}`);
    }
  },
  {
    name: 'gt4-signed-out-en',
    url: `http://127.0.0.1:${PORT}/dev/index.html?lang=en-US#/settings/achievements`,
    outFiles: ['achievements-en-us-gt4-signed-out.png'],
    verify: async (page) => {
      const text = await page.locator('.ra-page').textContent();
      if (!text.includes('Not signed in')) throw new Error('Expected Not signed in text');
      const signInBtn = await page.locator('button.primary').count();
      if (signInBtn === 0) throw new Error('Expected Sign in button');
    }
  },
  {
    name: 'gt4-signed-out-pt',
    url: `http://127.0.0.1:${PORT}/dev/index.html?lang=pt-BR#/settings/achievements`,
    outFiles: ['achievements-pt-br-gt4-signed-out.png'],
    verify: async (page) => {
      const text = await page.locator('.ra-page').textContent();
      if (!text.includes('Não conectado')) throw new Error('Expected Não conectado text');
      const signInBtn = await page.locator('button.primary').count();
      if (signInBtn === 0) throw new Error('Expected Sign in button');
    }
  },
  {
    name: 'spec2-unsupported-en',
    url: `http://127.0.0.1:${PORT}/dev/index.html?lang=en-US&selected=spec2#/settings/achievements`,
    outFiles: ['achievements-en-us-spec2.png'],
    verify: async (page) => {
      const banner = await page.locator('.ra-unsupported-banner');
      if (await banner.count() === 0) throw new Error('Expected .ra-unsupported-banner on Spec II');
      const text = await banner.textContent();
      if (!text.includes('RetroAchievements is not available for Gran Turismo 4 Spec II')) {
        throw new Error(`Unexpected banner text: ${text}`);
      }
      const rowCount = await page.locator('.ra-ach-row').count();
      if (rowCount !== 0) throw new Error(`Spec II must have 0 achievement rows, got ${rowCount}`);
    }
  },
  {
    name: 'spec2-unsupported-pt',
    url: `http://127.0.0.1:${PORT}/dev/index.html?lang=pt-BR&selected=spec2#/settings/achievements`,
    outFiles: ['achievements-pt-br-spec2.png'],
    verify: async (page) => {
      const banner = await page.locator('.ra-unsupported-banner');
      if (await banner.count() === 0) throw new Error('Expected .ra-unsupported-banner on Spec II');
      const text = await banner.textContent();
      if (!text.includes('RetroAchievements') || !text.includes('Spec II')) {
        throw new Error(`Unexpected banner text: ${text}`);
      }
      const rowCount = await page.locator('.ra-ach-row').count();
      if (rowCount !== 0) throw new Error(`Spec II must have 0 achievement rows, got ${rowCount}`);
    }
  }
];

async function main() {
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

    for (const testCase of CASES) {
      console.log(`Processing case: ${testCase.name}...`);
      const page = await context.newPage();
      const jsErrors = [];
      page.on('pageerror', (err) => {
        jsErrors.push(err.message);
      });
      page.on('console', (msg) => {
        if (msg.type() === 'error' && !msg.text().includes('favicon.ico')) {
          jsErrors.push(msg.text());
        }
      });

      await page.goto(testCase.url, { waitUntil: 'networkidle' });
      await page.waitForSelector('.ra-page', { state: 'visible', timeout: 5000 });
      await page.evaluate(() => document.fonts.ready);
      await page.waitForTimeout(400); // allow transitions and DOM paint to settle

      if (jsErrors.length > 0) {
        throw new Error(`JavaScript errors encountered on ${testCase.name}: ${jsErrors.join(', ')}`);
      }

      await testCase.verify(page);
      console.log(`  Verification passed for ${testCase.name}`);

      for (const outFile of testCase.outFiles) {
        const dest = path.join(OUT_DIR, outFile);
        await page.screenshot({ path: dest, fullPage: false });
        const size = fs.statSync(dest).size;
        console.log(`  Saved screenshot: ${dest} (${size} bytes)`);
      }

      await page.close();
    }
    console.log('All screenshots captured and verified successfully!');
  } finally {
    if (browser) await browser.close();
    server.close();
  }
}

main().catch((err) => {
  console.error('Fatal error during capture:', err);
  process.exit(1);
});

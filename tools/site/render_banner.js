// Renders tools/site/banner.html to site/assets/banner.jpg (1200 x 630).
//   npm install playwright-core && node tools/site/render_banner.js [path to Chrome]
const path = require('path');
const { chromium } = require('playwright-core');
(async () => {
  const chrome = process.argv[2] || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
  const browser = await chromium.launch({ executablePath: chrome });
  const page = await browser.newPage({ viewport: { width: 1200, height: 630 } });
  await page.goto('file://' + path.join(__dirname, 'banner.html'));
  await page.evaluate(() => document.fonts.ready);
  await page.waitForTimeout(500);
  await page.screenshot({ path: path.join(__dirname, '..', '..', 'site', 'assets', 'banner.jpg'), type: 'jpeg', quality: 90 });
  await browser.close();
})();

// 从 HTML 原版（mage-game/index.html）的 drawMob 程序化绘制函数 1:1 抓取
// 每一种魔物的立绘，存成透明 PNG：mage-game/assets/sp_<type>.png。
//
// 这是「图鉴画像不对」「灵狐像兔子」两个根因的唯一正解：
//  固件之前用的是 prep_sprites.py 抓的旧 sp_* 素材（狐兔不分、猫损坏），
//  而 HTML 根本没有这些 PNG —— 所有魔物都是 drawMob 程序化手绘。
//  直接把 drawMob 的输出截下来当素材，形状 100% 贴合 HTML。
//
// 抓取方式：起一个 http server（同源，避免 canvas 污染）加载 index.html，
// 对每个 ENEMY 类型清屏后调 drawMob(type, 120, footY, t)，再裁出非透明包围盒。
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const ASSETS = path.join(ROOT, 'mage-game', 'assets');

(async () => {
  const browser = await chromium.launch({
    executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe',
    args: ['--headless=new', '--no-sandbox', '--disable-gpu']
  });
  const page = await browser.newPage({ viewport: { width: 240, height: 320 } });
  const url = 'http://127.0.0.1:8123/index.html';
  await page.goto(url);
  await page.waitForFunction(() => window.ENEMY && window.drawMob && window.MOB_SHAPE_SPEC, { timeout: 15000 })
    .catch(() => {});
  await page.waitForTimeout(500);

  const types = await page.evaluate(() => Object.keys(ENEMY));
  console.log('ENEMY 种类数：', types.length);

  const footY = 250;   // 脚底贴到画布靠下，给底部阴影留空间
  const t = 0.8;       // 固定相位，姿势稳定一致
  let ok = 0, skip = 0;

  for (const type of types) {
    try {
      const data = await page.evaluate(({ type, footY, t }) => {
        const cv = document.getElementById('game');
        const ctx = cv.getContext('2d');
        ctx.clearRect(0, 0, cv.width, cv.height);
        drawMob(type, 120, footY, t);
        const img = ctx.getImageData(0, 0, cv.width, cv.height).data;
        let minX = 999, minY = 999, maxX = -1, maxY = -1;
        for (let y = 0; y < cv.height; y++) {
          for (let x = 0; x < cv.width; x++) {
            const a = img[(y * cv.width + x) * 4 + 3];
            if (a > 8) {
              if (x < minX) minX = x; if (x > maxX) maxX = x;
              if (y < minY) minY = y; if (y > maxY) maxY = y;
            }
          }
        }
        if (maxX < 0) return null;
        const pad = 3;
        minX = Math.max(0, minX - pad); minY = Math.max(0, minY - pad);
        maxX = Math.min(cv.width - 1, maxX + pad); maxY = Math.min(cv.height - 1, maxY + pad);
        const w = maxX - minX + 1, h = maxY - minY + 1;
        const off = document.createElement('canvas'); off.width = w; off.height = h;
        const octx = off.getContext('2d');
        octx.drawImage(cv, minX, minY, w, h, 0, 0, w, h);
        return off.toDataURL('image/png');
      }, { type, footY, t });
      if (!data) { console.log('SKIP', type, '(无像素)'); skip++; continue; }
      const b64 = data.replace(/^data:image\/png;base64,/, '');
      fs.writeFileSync(path.join(ASSETS, `sp_${type}.png`), Buffer.from(b64, 'base64'));
      console.log('OK  ', type);
      ok++;
    } catch (e) {
      console.log('FAIL', type, e.message);
      skip++;
    }
  }
  console.log(`\n完成：成功 ${ok} 张，跳过 ${skip} 张 -> ${ASSETS}`);
  await browser.close();
})();

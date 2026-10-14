// 渲染真实 HTML（mage-game/index.html）各阶段，作为 1:1 对比的权威基准。
// 通过页面内 SCENES[scene].draw() 直接绘制目标帧，canvas.toDataURL 抓取。
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

(async () => {
  const browser = await chromium.launch({
    executablePath: 'C:/Program Files/Google/Chrome/Application/chrome.exe',
    args: ['--headless=new', '--no-sandbox', '--disable-gpu']
  });
  const page = await browser.newPage({ viewport: { width: 240, height: 320 } });
  const url = 'http://127.0.0.1:8123/index.html';
  await page.goto(url);
  // 等所有战斗/立绘素材加载完（同源 HTTP，不会污染 canvas）
  await page.waitForFunction(() => {
    return window.Assets && window.Assets.battleBg && window.Assets.spHero && window.SCENES && window.SCENES.BATTLE;
  }, { timeout: 15000 }).catch(() => {});
  await page.waitForTimeout(800);

  const scenarios = {
    title: { scene: 'TITLE' },
    town:  { scene: 'CITY', setup: "state.cityMode='map';" },
    battle: {
      scene: 'BATTLE',
      bt: { phase: 'menu', px: 70, cam: 0, t: 0.1, wave: 1, turn: 1, shake: 0,
            enemies: [
              { type: 'slime', x: 148, hp: 10, maxHp: 10, lvl: 2, boss: false, elite: false, hurtT: 0, bob: 0, shieldMax: 0, shield: 0, rarity: 0 },
              { type: 'goblin', x: 164, hp: 12, maxHp: 12, lvl: 3, boss: false, elite: false, hurtT: 0, bob: 1, shieldMax: 0, shield: 0, rarity: 0 }
            ], fx: [], nums: [], catLunge: null, lungeT: 0, walkF: 0, hurtT: 0 }
    },
    battle_boss: {
      scene: 'BATTLE',
      bt: { phase: 'menu', px: 70, cam: 0, t: 0.1, wave: 10, turn: 3, shake: 0,
            enemies: [
              { type: 'goblin', x: 220, hp: 200, maxHp: 200, lvl: 10, boss: true, elite: false, hurtT: 0, bob: 0, shieldMax: 0, shield: 0, name: '哥布林王', rarity: 4 }
            ], fx: [], nums: [], catLunge: null, lungeT: 0, walkF: 0, hurtT: 0 }
    },
    result: {
      scene: 'BATTLE',
      bt: { phase: 'result', done: false, wave: 3, goldGain: 120, expGain: 80,
            herbGain: 2, matGain: 1, potGain: 1, killCnt: 0, bossEquip: '',
            bossCrystal: 0, bossOrb: 0, bossSeal: 0, cam: 0, px: 70, t: 0, enemies: [], fx: [], nums: [] }
    },
    expo: {
      scene: 'BATTLE',
      mapChapter: 0,
      bt: { phase: 'result', done: true, wave: 10, goldGain: 640, expGain: 420,
            herbGain: 3, matGain: 2, potGain: 1, killCnt: 8, bossEquip: '炎之心片',
            bossCrystal: 2, bossOrb: 2, bossSeal: 1, cam: 0, px: 70, t: 0, enemies: [], fx: [], nums: [] }
    },
    loot_chest: {
      scene: 'BATTLE',
      bt: { phase: 'walk', cam: 0, px: 70, t: 0, wave: 3, enemies: [], fx: [], nums: [],
            lootShow: { title: '✦ 宝箱战利品 ✦',
              lines: ['金币 +50', '药品·经验药水', '素材·月光草', '中级魔石 ×2', '捕捉球 ×2'], t: 3, t0: 3 } }
    },
    loot_herb: {
      scene: 'BATTLE',
      bt: { phase: 'walk', cam: 0, px: 70, t: 0, wave: 3, enemies: [], fx: [], nums: [],
            lootShow: { title: '✦ 采集收获 ✦',
              lines: ['月光草 ×5', '魔晶 ×1', '素材·银矿石'], t: 3, t0: 3 } }
    },
    event_chest: {
      scene: 'BATTLE',
      bt: { phase: 'event', event: { k: 'chest' }, cam: 0, px: 70, t: 0, wave: 3, enemies: [], fx: [], nums: [] }
    },
    town_acad: {
      scene: 'ACAD'
    },
    town_wild: {
      scene: 'WILD'
    },
    town_guild: {
      scene: 'GUILD'
    },
    func_status: {
      scene: 'STATUS'
    },
    func_bag: {
      scene: 'BAG'
    },
    battle_intro: {
      scene: 'BATTLE',
      bt: { phase: 'walk', px: 70, cam: 0, t: 0, wave: 1, turn: 1, shake: 0,
            enemies: [
              { type: 'slime', x: 150, hp: 10, maxHp: 10, lvl: 2, boss: false, elite: false, hurtT: 0, bob: 0, shieldMax: 0, shield: 0, rarity: 0 },
              { type: 'goblin', x: 170, hp: 12, maxHp: 12, lvl: 3, boss: false, elite: false, hurtT: 0, bob: 1, shieldMax: 0, shield: 0, rarity: 0 }
            ], fx: [], nums: [], catLunge: null, lungeT: 0, walkF: 0, hurtT: 0 }
    }
  };

  for (const [name, sc] of Object.entries(scenarios)) {
    try {
      const data = await page.evaluate((sc) => {
        state = newState();
        state.scene = sc.scene;
        if (sc.mapChapter !== undefined) state.map = { chapter: sc.mapChapter };
        if (sc.setup) eval(sc.setup);
        if (sc.bt) bt = sc.bt;
        if (sc.scene === 'BATTLE') { state.partners = [{ name: '咪咪' }]; state.activePartner = 0; }
        SCENES[state.scene].draw();
        if (window.drawOverlay) drawOverlay(); // 战利品面板等 overlay 也属于权威画面
        return document.getElementById('game').toDataURL('image/png');
      }, sc);
      const b64 = data.replace(/^data:image\/png;base64,/, '');
      fs.writeFileSync(path.join('assets', 'preview', 'html_' + name + '_240x320.png'), Buffer.from(b64, 'base64'));
      console.log('OK  ', name);
    } catch (e) {
      console.log('FAIL', name, e.message);
    }
  }
  await browser.close();
})();

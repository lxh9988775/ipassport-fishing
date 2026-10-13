import fs from 'fs';
import vm from 'vm';

const html = fs.readFileSync('./index.html', 'utf8');
const src = html.match(/<script>([\s\S]*?)<\/script>/)[1];

// ---- stub canvas 2d context ----
function makeCtx() {
  return new Proxy({}, {
    get(t, p) {
      if (p === 'canvas') return { width: 240, height: 320 };
      if (p === 'fillStyle' || p === 'strokeStyle' || p === 'font' || p === 'textAlign' || p === 'textBaseline' || p === 'lineWidth' || p === 'globalAlpha') return t[p];
      if (p === 'measureText') return () => ({ width: 10 });
      if (p === 'createLinearGradient' || p === 'createRadialGradient') return () => ({ addColorStop() {} });
      return () => {};
    },
    set(t, p, v) { t[p] = v; return true; }
  });
}
const realRandom = Math.random;
const els = {
  game: { getContext: () => makeCtx(), width: 240, height: 320, addEventListener() {} },
  btnA: { addEventListener() {} }, btnB: { addEventListener() {} }, btnC: { addEventListener() {} }
};
let rafCb = null;
const sandbox = {
  console, Math, JSON, Date, isNaN, parseInt, parseFloat,
  setTimeout: (fn) => { try { fn(); } catch (e) {} return 0; },
  clearTimeout() {}, setInterval: () => 0, clearInterval() {},
  performance: { now: () => 0 },
  requestAnimationFrame: cb => { sandbox.__raf = cb; return 1; },
  confirm: () => false,
  Image: class { set src(v) {} },
  localStorage: { _d: {}, getItem(k) { return this._d[k] || null; }, setItem(k, v) { this._d[k] = v; }, removeItem(k) { delete this._d[k]; } },
  URL: { createObjectURL: () => 'blob:x', revokeObjectURL() {} },
  Blob: class {},
  document: {
    getElementById: id => els[id] || { addEventListener() {}, click() {}, value: '' },
    addEventListener() {},
    createElement: () => ({ click() {}, href: '', download: '', files: [], set onchange(v) {}, set onload(v) {}, set src(v) {}, readAsText() {} })
  },
  AudioContext: class { constructor() { this.currentTime = 0; this.destination = {}; } createOscillator() { return { type: '', frequency: { setValueAtTime() {}, exponentialRampToValueAtTime() {} }, connect: () => ({ connect() {} }), start() {}, stop() {} }; } createGain() { return { gain: { setValueAtTime() {}, exponentialRampToValueAtTime() {} }, connect: () => ({ connect() {} }) }; } }
};
sandbox.window = sandbox;
sandbox.window.AudioContext = sandbox.AudioContext;
sandbox.window.webkitAudioContext = sandbox.AudioContext;
vm.createContext(sandbox);

const hook = `;globalThis.__gt=0;globalThis.__t={
  press, scene:()=>state.scene, state:()=>state, bt:()=>bt,
  setBtEnemy(l){ if(bt) bt.enemies=l; },
  setPhase(p){ if(bt) bt.phase=p; },
  city:()=>({mode:cityMode,loc:locSel,func:funcSel}),
  setCity:(m,l)=>{cityMode=m; if(l!=null)locSel=l;},
  openFunc:(i)=>{state.scene='CITY';cityMode='func';funcSel=i;},
  resetBook:()=>{bookIdx=0;},
  startBattle:()=>{ startBattle(false); },
  spawnGroup:()=>{ spawnGroup(); },
  nextWave:()=>{ nextWave(); },
  rollEvent, applyEvent, killEnemy, magicCast,
  enemyBase:()=>ENEMY,
  scenes:()=>Object.keys(SCENES),
  zoneMobs:()=>ZONE_MOBS,
  dexSeen:()=>state.dex.seen, dexCaught:()=>state.dex.caught,
  markAllSeen:()=>{ DEXORDER.forEach(tp=>{ state.dex.seen[tp]=(state.dex.seen[tp]||0)+1; }); },
  doEvolveSel:()=>{ const p=state.partners[state.activePartner]; if(p) doEvolve(p); },
  evolveAvail:()=>{ const p=state.partners[state.activePartner]; if(!p)return false; const ev=ENEMY[p.type].evo; return !!ev&&!p.evolved&&p.level>=ev.lvl; },
  releaseSel:()=>{ if(state.partners.length){ state.partners.splice(state.activePartner,1); state.cats=Math.max(0,state.cats-1); } },
  canCapAt:(type,ratio,seals,orbs)=>{ const e={type,hp:Math.round(ratio*100),maxHp:100,capturable:ENEMY[type].capturable}; state.seals=seals; state.orbs=orbs; return canCapture(e); },
  frame:(n,step)=>{ for(let i=0;i<n;i++){ globalThis.__gt+=(step||16); globalThis.__raf(globalThis.__gt); } }
};`;
vm.runInContext(src + hook, sandbox, { filename: 'game.js' });
const t = sandbox.__t;
const P = b => t.press(b);
const S = () => t.scene();
const errs = [];
function step(name, fn) {
  try { fn(); console.log('  ok  ' + name); }
  catch (e) { errs.push(name + ' :: ' + e.message); console.log('  FAIL ' + name + ' :: ' + e.message + '\n' + String(e.stack||'').split('\n').slice(1,4).join('\n')); }
}
function chk(name, cond) { if (!cond) errs.push('assert ' + name); else console.log('  ok  assert ' + name); }

console.log('=== 魔法帝之路 V5 冒烟测试 ===');
step('标题页 A 继续(无存档→新游戏)', () => { P('A'); if (S() !== 'NAME') throw new Error('scene=' + S()); });
step('命名页 B 换名 A 确定', () => { P('B'); P('A'); if (S() !== 'CITY') throw new Error('scene=' + S()); });
step('城镇：地图模式 B 切地点 A 进入野外试炼', () => { t.setCity('map', 1); P('A'); if (S() !== 'WILD') throw new Error('scene=' + S()); P('A'); if (S() !== 'BATTLE') throw new Error('scene=' + S()); });
step('战斗：走几秒自动遇敌进入菜单', () => { t.frame(400, 16); const b = t.bt(); if (!['menu', 'enemy', 'result', 'meet', 'walk'].includes(b.phase)) throw new Error('phase=' + b.phase); chk('战斗阶段在正常推进', true); });
step('卡死回归：远距刷怪→敌人走近→A普攻必须推进回合', () => {
  const b = t.bt(); t.setPhase('menu'); b.auto = false; b.turn = 1; b.lootDone = false;
  // 确定性刷一只远端怪（170px 外，模拟最恶劣出生位），避免上一步挂机清场导致空场偶发
  t.setBtEnemy([{ type: 'slime', x: b.px + 170, hp: 20, maxHp: 20, atk: 2, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 0, ivs: [10, 10, 10], bob: 0, mercy: false, elite: false }]);
  t.frame(260, 16); // 4.16秒：146px 以 ~50px/s 走近需约3秒
  const d = Math.min(...b.enemies.map(e => e.x - b.px));
  chk('敌人已进入远程交战距离(78~95)', d >= 76 && d <= 95);
  P('A'); // 普攻
  chk('回合已推进(enemy/result)', ['enemy', 'result'].includes(b.phase));
});
step('手动 A 普攻', () => { const b = t.bt(); t.setPhase('menu'); b.auto = false; t.state().mp = t.state().maxMp; P('A'); });
step('B 打开魔法书列表选魔法', () => { const b = t.bt(); t.setPhase('menu'); b.auto = false; P('B'); if (b.phase !== 'magic') throw new Error('phase=' + b.phase); P('A'); });
step('C 打开道具袋', () => { const b = t.bt(); t.setPhase('menu'); b.auto = false; P('C'); if (b.phase !== 'item') throw new Error('phase=' + b.phase); P('C'); });
step('怪物等级：等级随章节增长且属性按等级缩放', () => {
  const b = t.bt(); t.startBattle(); t.setPhase('menu'); b.auto = false;
  t.state().map.chapter = 1; b.wave = 1; b.enemies = []; t.spawnGroup();
  chk('第1章第1波等级<=2', b.enemies.every(e => e.lvl >= 1 && e.lvl <= 2));
  chk('非精英属性=基础x等级倍率', b.enemies.every(e => { if (e.elite) return true; const base = t.enemyBase()[e.type]; const m = 1 + (e.lvl - 1) * 0.14; return e.maxHp === Math.round(base.hp * m) && e.atk === Math.round(base.atk + m * 0.6); }));
  b.enemies = []; t.state().map.chapter = 4; b.wave = 3; t.spawnGroup();
  chk('第5章(下标4)第3波等级>=10', b.enemies.every(e => e.lvl >= 10));
});
step('锁血规则：无道具能打死 / 有道具才锁 8% 残血', () => {
  const b = t.bt(); const st = t.state(); b.auto = false; st.autoCapture = true; st.partners = []; st.activePartner = 0;
  st.orbs = 0; st.seals = 0;
  t.setPhase('menu');
  t.setBtEnemy([{ type: 'slime', x: b.px + 80, hp: 5, maxHp: 100, atk: 2, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 0, ivs: [10, 10, 10], bob: 0, mercy: false, elite: false, lvl: 1 }]);
  P('A'); /* 普攻 >=9 伤害：无道具不锁血 → 直接打死清场 */
  chk('无道具：怪被正常打死', b.enemies.length === 0);
  st.orbs = 3;
  t.setPhase('menu');
  t.setBtEnemy([{ type: 'slime', x: b.px + 80, hp: 5, maxHp: 100, atk: 2, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 0, ivs: [10, 10, 10], bob: 0, mercy: false, elite: false, lvl: 1 }]);
  P('A'); /* 有球：锁血兜住，怪活下来且 mercy 标记 */
  const e2 = b.enemies[0];
  chk('有道具：怪被锁在 8% 残血且标记 mercy', !!e2 && e2.mercy === true && e2.hp === Math.max(1, Math.round(100 * 0.08)));
});
step('敌人冲锋攻击：冲上来→中点结算伤害→退回', () => {
  const b = t.bt(); const st = t.state(); t.setPhase('menu'); b.auto = false;
  st.partners = []; st.hp = st.maxHp;
  t.setBtEnemy([{ type: 'goblin', x: b.px + 80, hp: 500, maxHp: 500, atk: 4, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 0, ivs: [10, 10, 10], bob: 0, mercy: false, elite: false, lvl: 1 }]);
  P('A'); /* 玩家行动 → enemy 阶段，0.55s 后怪起冲锋 */
  t.frame(40, 16); /* 0.64s：冲锋刚开始 ~0.09s */
  const e1 = b.enemies[0];
  chk('冲锋动画进行中且伤害未结算', !!e1.lunge && !e1.lunge.hit && st.hp === st.maxHp);
  t.frame(30, 16); /* 越过中点 0.275s */
  chk('中点结算：英雄受伤', st.hp < st.maxHp);
  t.frame(24, 16);
  chk('动画结束：lunge 已清除', !b.enemies[0].lunge);
});
step('强制猫灵虚弱→捕捉流程', () => {
  const b = t.bt(); t.setPhase('menu'); b.auto = false;
  t.state().orbs = 5; t.state().partners = []; t.state().activePartner = 0;
  t.setBtEnemy([{ type: 'cat', x: b.px + 30, hp: 3, maxHp: 20, atk: 3, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 4, ivs: [12, 10, 14], bob: 0, mercy: true, elite: false }]);
  t.frame(3, 16);
  P('C'); P('B'); // 道具袋 → 切到捕捉球
  sandbox.Math.random = () => 0.01; // 强制捕捉必然成功
  P('A'); // 投球
  sandbox.Math.random = realRandom;
  chk('猫灵入队', t.state().partners.length === 1);
});
step('封印卡捕捉（封印卡专属怪，必成功并带标记）', () => {
  const b = t.bt(); t.setPhase('menu'); b.auto = false; b.itemIdx = 0;
  const st = t.state(); st.seals = 2; st.orbs = 0; st.potions = 0; st.partners = [];
  t.setBtEnemy([{ type: 'frost', x: b.px + 30, hp: 2, maxHp: 20, atk: 3, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 4, ivs: [12, 10, 14], bob: 0, mercy: true, elite: false, fly: true }]);
  t.frame(3, 16);
  P('C'); // 道具袋（球为 0，仅封印卡）
  sandbox.Math.random = () => 0.01; // 必成功
  P('A');
  sandbox.Math.random = realRandom;
  chk('封印卡-1', st.seals === 1);
  chk('封印冰晶灵入队且带标记', st.partners.length === 1 && st.partners[0].seal === true && st.partners[0].type === 'frost');
});
step('自动捕捉开关：关→虚弱猫灵逃走不空转', () => {
  const s = t.state(); s.autoCapture = false; s.orbs = 5; s.seals = 2;
  const b = t.bt(); t.setPhase('menu'); b.auto = true; b.autoT = 0;
  t.setBtEnemy([{ type: 'cat', x: b.px + 30, hp: 1, maxHp: 20, atk: 3, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 2, ivs: [10, 10, 10], bob: 0, mercy: true, elite: false }]);
  t.frame(10, 16); // 160ms：autoT 触发 autoAct → 猫灵应逃走 → 清场进 result
  chk('猫灵逃走后清场', b.enemies.length === 0 || ['result', 'walk'].includes(b.phase));
});
step('自动捕捉开关：开→虚弱自动扔球', () => {
  const s = t.state(); s.autoCapture = true; s.orbs = 3; s.seals = 1; s.partners = [];
  const b = t.bt(); t.setPhase('menu'); b.auto = true; b.autoT = 0;
  t.setBtEnemy([{ type: 'cat', x: b.px + 30, hp: 2, maxHp: 20, atk: 3, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 0, ivs: [10, 10, 10], bob: 0, mercy: true, elite: false }]);
  const o0 = s.orbs, se0 = s.seals;
  sandbox.Math.random = () => 0.99; // 必失败 → 道具必然消耗，验证"确实扔了"
  t.frame(10, 16);
  sandbox.Math.random = realRandom;
  chk('挂机自动消耗了捕捉道具', s.orbs + s.seals < o0 + se0 || s.partners.length === 1);
});
step('结算→继续前进 自动挂机跑', () => {
  t.startBattle(); const b = t.bt(); t.setPhase('result'); b.auto = true; b.autoT2 = 0; b.wave = 1; b.done = false; b.bossFight = false;
  t.frame(5, 16); chk('回到行走阶段', b && (b.phase === 'walk' || b.phase === 'event'));
});
step('力竭分支回城', () => {
  P('B');
  if (S() !== 'CITY') { P('B'); }
});
step('城镇→角色面板', () => { t.openFunc(0); P('A'); if (S() !== 'STATUS') throw new Error('scene=' + S()); P('C'); });
step('城镇→伙伴面板', () => { t.openFunc(1); P('A'); if (S() !== 'PARTNERS') throw new Error('scene=' + S()); P('C'); });
step('城镇→魔法书节点图', () => { t.openFunc(2); P('A'); if (S() !== 'BOOK') throw new Error('scene=' + S()); const n=Object.keys(t.state().discovered).length; for(let j=0;j<n;j++)P('B'); /* 切到 + 自创格 */ P('A'); if (S() !== 'CRAFT') throw new Error('scene=' + S()); P('C'); P('C'); });
step('城镇→炼金', () => { t.openFunc(3); P('A'); if (S() !== 'ALCHEMY') throw new Error('scene=' + S()); P('A'); P('C'); });
step('城镇→背包', () => { t.openFunc(4); P('A'); if (S() !== 'BAG') throw new Error('scene=' + S()); P('A'); P('C'); });
step('城镇→装备(EQUIP)', () => {
  const s = t.state();
  s.ebag = [{ slot: 'w', name: '试炼剑', rar: 2, atk: 5 }];
  t.openFunc(5); P('A');
  if (S() !== 'EQUIP') throw new Error('scene=' + S());
  P('A'); // 装备选中项（试炼剑→武器栏）
  chk('装备已穿戴到武器栏且移出背包', s.equip.w && s.equip.w.name === '试炼剑' && s.ebag.length === 0);
  P('C'); if (S() !== 'CITY') throw new Error('scene=' + S());
});
step('城镇→系统菜单', () => { t.openFunc(6); P('A'); if (S() !== 'MENU') throw new Error('scene=' + S()); P('A'); P('C'); });
step('远征轮结构：第5波必出精英 / 第10波必出BOSS', () => {
  t.startBattle(); const b = t.bt(); const st = t.state();
  st.map.chapter = 1; b.dungeon = false;
  b.wave = 5; b.enemies = []; t.spawnGroup();
  chk('第5波出现精英怪', b.enemies.some(e => e.elite === true));
  b.wave = 10; b.enemies = []; t.spawnGroup();
  chk('第10波刷出BOSS(单只巨怪)', b.enemies.length === 1 && b.enemies[0].boss === true);
  chk('BOSS 带 5 层破防盾', b.enemies[0].shieldMax === 5);
});
step('元素克制：雷魔法命中史莱姆弱点→削盾破防', () => {
  t.startBattle(); const b = t.bt(); const st = t.state();
  st.level = 10; st.mp = st.maxMp; st.equipped = '0-2-0'; // 弹(0)+雷(2)+无修饰(0)
  b.enemies = [{ type: 'slime', x: b.px + 50, hp: 300, maxHp: 300, atk: 2, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 0, ivs: [10, 10, 10], bob: 0, elite: false, mercy: false, shield: 1, shieldMax: 1, brokenT: 0, boss: false }];
  const hp0 = b.enemies[0].hp;
  t.magicCast();
  const e = b.enemies[0];
  chk('弱点命中造成伤害', e ? e.hp < hp0 : true);
  chk('雷是史莱姆弱点→护盾削空并破防硬直', e && e.shield === 0 && e.brokenT > 0 && e.weakKnown === true);
});
step('BOSS 击杀必掉高品质装备 + 主线门禁推进', () => {
  t.startBattle(); const b = t.bt(); const st = t.state();
  st.map.chapter = 1; st.ebag = []; st.bossKilled = {}; st.gold = 0; st.crystals = 0; st.orbs = 0; st.seals = 0;
  b.enemies = [{ type: 'goblin', x: b.px + 50, hp: 5, maxHp: 400, atk: 2, atkT: 99, hurtT: 0, capCd: 0, skipped: false, rarity: 0, ivs: [10, 10, 10], bob: 0, elite: true, mercy: false, shield: 0, shieldMax: 3, brokenT: 0, boss: true, n: '哥布林王' }];
  t.killEnemy(b.enemies[0]);
  chk('BOSS 死亡发放高品质装备到背包', st.ebag.length >= 1 && st.ebag[0].rar >= 1);
  chk('BOSS 击杀标记该章主线进度', st.bossKilled[1] === true);
  chk('BOSS 战利品含魔晶/捕捉球/封印卡', st.crystals >= 2 && st.orbs >= 2 && st.seals >= 1);
});
step('探索事件：四类事件触发均不崩溃并回到行走', () => {
  t.startBattle(); const b = t.bt(); const st = t.state();
  st.hp = 10; st.maxHp = 100; st.mp = 10; st.maxMp = 100; st.gold = 0; st.herbs = 0; st.crystals = 0; st.orbs = 0; st.seals = 0;
  const kinds = ['chest', 'trap', 'spring', 'herb'];
  for (const k of kinds) { b.event = { k }; b.phase = 'event'; t.applyEvent(); if (b.phase !== 'walk') throw new Error('事件后未回到 walk: ' + k); }
  let ok = true; for (let i = 0; i < 30; i++) { if (!kinds.includes(t.rollEvent().k)) ok = false; }
  chk('rollEvent 始终返回合法事件类型', ok);
});
step('战斗间可触发探索事件(event 阶段)', () => {
  t.startBattle(); const b = t.bt(); const st = t.state();
  st.map.chapter = 1; b.dungeon = false; b.wave = 2;
  sandbox.Math.random = () => 0.0; // 强制触发（Math.random<0.3）
  t.nextWave();
  sandbox.Math.random = realRandom;
  chk('nextWave 进入 event 阶段', b.phase === 'event' && !!b.event);
});
step('强制晋升', () => { const s = t.state(); s.rank = 0; s.kills = 999; s.scene = 'PROMOTE'; if (S() !== 'PROMOTE') throw new Error('scene=' + S()); P('A'); chk('晋升后回城', S() === 'CITY'); });
step('学院：魔力修行 MP上限+5', () => { t.state().scene = 'ACAD'; t.state().gold = 100; const mp0 = t.state().maxMp; P('A'); chk('maxMp+5', t.state().maxMp === mp0 + 5); P('C'); });
step('学院：未达标拦截晋升', () => { t.state().scene = 'ACAD'; t.state().kills = 0; P('B'); P('B'); P('A'); if (S() !== 'ACAD') throw new Error('scene=' + S()); P('C'); });
step('公会：接取委托', () => { t.state().scene = 'GUILD'; P('A'); chk('accepted=0', t.state().accepted === 0); P('C'); });
step('住宅→魔法研究（必成功升1级）', () => { const s = t.state(); s.scene = 'RESEARCH'; s.herbs = 10; s.mats = 10; s.spellLv = {}; sandbox.Math.random = () => 0.01; P('A'); sandbox.Math.random = realRandom; chk('魔法Lv1', Object.values(s.spellLv)[0] === 1); P('C'); if (S() !== 'HOME') throw new Error('scene=' + S()); P('C'); });
step('锻造房：武器锻造 攻击+2', () => { t.state().scene = 'FORGE'; t.state().gold = 500; t.state().mats = 20; const a0 = t.state().atkBonus; P('A'); chk('atkBonus+2', t.state().atkBonus === a0 + 2); P('C'); });
step('黑市：买月光草', () => { t.state().scene = 'MARKET'; t.state().gold = 100; const h0 = t.state().herbs; P('A'); chk('herbs+1', t.state().herbs === h0 + 1); P('C'); });
step('野外：遗迹秘境 3层通关豪华奖励', () => {
  const s = t.state(); s.rank = 1; s.scene = 'WILD';
  for (let i = 0; i < 5; i++) P('B'); // 切到遗迹秘境
  const c0 = s.crystals, o0 = s.orbs;
  P('A'); if (S() !== 'BATTLE') throw new Error('scene=' + S());
  const b = t.bt(); if (!b.dungeon) throw new Error('非地下城模式');
  t.setPhase('result'); b.wave = 3; P('A');
  if (S() !== 'CITY') throw new Error('scene=' + S());
  chk('通关+魔晶', s.crystals === c0 + 1); chk('通关+捕捉球', s.orbs === o0 + 1);
});
step('随机长跑不炸(60秒乱按)', () => {
  t.setCity('map', 1); P('A'); P('A');
  const keys = ['A', 'B', 'C'];
  for (let i = 0; i < 400; i++) { t.frame(10, 16); P(keys[Math.floor(realRandom() * 3)]); }
  chk('仍在合法场景', t.scenes().includes(S()));
});

step('每张地图刷专属怪（分区刷怪表）', () => {
  const zm = t.zoneMobs();
  const s = t.state();
  const seen = {}; // chapter -> Set(types)
  for (let ch = 0; ch < zm.length; ch++) {
    s.map.chapter = ch;
    t.startBattle();
    const b = t.bt();
    seen[ch] = new Set();
    for (let w = 0; w < 40; w++) {
      b.enemies = []; t.spawnGroup();
      for (const e of b.enemies) seen[ch].add(e.type);
    }
    for (const tp of seen[ch]) {
      if (tp !== 'cat' && !zm[ch].includes(tp)) throw new Error('章'+ch+' 刷出非本图怪 ' + tp);
    }
    if (!seen[ch].has('cat')) throw new Error('章'+ch+' 40波没有猫灵');
  }
  chk('5 张图怪物表互不相同（除猫灵）', zm.every((z,i)=>zm.every((z2,j)=>i===j||z.filter(t=>t!=='cat').some(t=>!z2.includes(t)))));
});

step('出战编队：自动循环多个魔法', () => {
  t.startBattle(); // 直接进入战斗（绕过导航）
  if (S() !== 'BATTLE') throw new Error('scene='+S());
  const b = t.bt(); const s = t.state();
  s.discovered['0-0-0']='火弹'; s.discovered['0-1-0']='水弹'; s.discovered['0-2-0']='雷弹';
  s.loadout = ['0-0-0','0-1-0','0-2-0']; s.mp = 1e9; s.equipped = '0-0-0';
  s.hp = 1e9; s.maxHp = 1e9;
  t.setBtEnemy([{ type:'goblin', x: b.px+30, hp:1e9, maxHp:1e9, atk:0, atkT:99, hurtT:0, capCd:0, skipped:false, rarity:0, ivs:[10,10,10], bob:0, mercy:false, elite:false }]);
  t.setPhase('menu'); b.auto=true; b.autoT=0.1;
  const seen = new Set();
  for (let i=0;i<30;i++){ t.frame(1, 120); if(S()!=='BATTLE')break; seen.add(t.state().equipped); }
  chk('编队里出现至少2种不同魔法', seen.size >= 2);
  chk('当前魔法仍在编队内', s.loadout.indexOf(t.state().equipped) >= 0);
});

step('出战编队：BOOK 加入/移出 + 上限4', () => {
  const s = t.state();
  s.discovered = {'0-0-0':'火弹','0-1-0':'水弹','0-2-0':'雷弹','1-3-1':'强力·风·光束','2-4-2':'广域·冰·爆发'};
  s.loadout = ['0-0-0']; s.equipped='0-0-0';
  t.openFunc(2); t.resetBook(); P('A'); // 进入 BOOK，bookIdx 归零
  if (S() !== 'BOOK') throw new Error('scene='+S());
  const keys = Object.keys(s.discovered);
  for (let j=0;j<keys.length;j++){ P('B'); if(S()!=='BOOK'){P('C');P('C');continue;} P('A'); }
  chk('上限4：loadout<=4', s.loadout.length <= 4);
  chk('至少加入了多个', s.loadout.length >= 2);
  P('C');
});

/* ---- 怪物生态新系统：进化 / 放生 / 区域捕捉门槛 / 图鉴 ---- */
step('进化系统：达到等级可进化，稀有度+1 且加后缀', () => {
  const s = t.state(); s.partners = [{type:'slime',name:'布丁',rarity:0,ivs:[10,10,10],level:20,exp:0,seal:false,skill:'claw',trait:'tough',evolved:false}]; s.activePartner = 0;
  chk('进化前置可用', t.evolveAvail() === true);
  const r0 = s.partners[0].rarity; const iv0 = s.partners[0].ivs.reduce((a,b)=>a+b,0);
  t.doEvolveSel();
  chk('稀有度提升', s.partners[0].rarity === r0 + 1);
  chk('名字加进化后缀', s.partners[0].name.indexOf('·王') >= 0);
  chk('已标记进化', s.partners[0].evolved === true);
  chk('资质小幅提升', s.partners[0].ivs.reduce((a,b)=>a+b,0) >= iv0);
});
step('进化门槛：未达等级不可进化', () => {
  const s = t.state(); s.partners = [{type:'cat',name:'绒绒',rarity:0,ivs:[10,10,10],level:5,exp:0,seal:false,skill:'swift',trait:'lucky',evolved:false}]; s.activePartner = 0;
  chk('Lv5 猫灵未达 Lv16 进化门槛', t.evolveAvail() === false);
});
step('放生：移除伙伴并减少计数', () => {
  const s = t.state(); s.cats = 3; s.partners = [{type:'cat',name:'a',rarity:0,ivs:[10,10,10],level:5,exp:0,seal:false,skill:'swift',trait:'lucky',evolved:false},{type:'slime',name:'b',rarity:0,ivs:[10,10,10],level:5,exp:0,seal:false,skill:'claw',trait:'tough',evolved:false}]; s.activePartner = 0;
  t.releaseSel();
  chk('伙伴数-1', s.partners.length === 1);
  chk('cats-1', s.cats === 2);
});
step('区域捕捉门槛：封印卡专属怪无卡不可捕、有卡可捕', () => {
  chk('冰晶灵无封印卡→不可捕', t.canCapAt('frost', 0.1, 0, 5) === false);
  chk('冰晶灵有封印卡→可捕', t.canCapAt('frost', 0.1, 1, 5) === true);
});
step('区域捕捉门槛：雪狼需压到血<40%', () => {
  chk('雪狼血50%→不可捕', t.canCapAt('wolf', 0.5, 0, 5) === false);
  chk('雪狼血30%→可捕', t.canCapAt('wolf', 0.3, 0, 5) === true);
});
step('普通球怪：有球即可捕（无血量门槛）', () => {
  chk('史莱姆满血有球→可捕', t.canCapAt('slime', 1.0, 0, 5) === true);
  chk('史莱姆无球→不可捕', t.canCapAt('slime', 1.0, 0, 0) === false);
});
step('图鉴：刷怪累计"已见"，收服累计"已收服"', () => {
  const b = t.bt() || {};
  // 多次刷怪写入 dex.seen
  for (let i=0;i<10;i++) t.spawnGroup();
  const seen = t.dexSeen();
  const anySeen = Object.keys(seen).length > 0;
  chk('刷怪后图鉴已见>0', anySeen);
  // 直接收服一只 frost 写入 dex.caught
  const s = t.state(); s.seals = 3; s.orbs = 0; s.potions = 0; s.partners = [];
  if (t.bt()) { t.setPhase('menu'); t.bt().auto = false; t.setBtEnemy([{ type:'frost', x: t.bt().px+30, hp:2, maxHp:20, atk:3, atkT:99, hurtT:0, capCd:0, skipped:false, rarity:2, ivs:[10,10,10], bob:0, mercy:true, elite:false, fly:true }]); t.frame(3,16); P('C'); sandbox.Math.random=()=>0.01; P('A'); sandbox.Math.random=realRandom; }
  chk('收服后图鉴已收服 frost', (t.dexCaught().frost||0) >= 1);
  t.markAllSeen();
  chk('markAllSeen 后全图鉴可见', Object.keys(t.dexSeen()).length >= 11);
});

/* 绘制安全：新场景/菜单绘制路径不能抛异常（vm 假 canvas 会吞绘制错误，这里靠不抛错验证逻辑） */
step('图鉴每页绘制不报错 + 伙伴菜单/放生确认绘制不报错', () => {
  const s = t.state();
  s.partners = [{type:'cat',name:'绒绒',rarity:3,ivs:[14,12,13],level:10,exp:0,seal:true,skill:'swift',trait:'lucky',evolved:false}];
  s.scene = 'PARTNERS'; t.frame(2,16);
  P('C'); t.frame(1,16);             // 打开操作菜单
  P('B'); t.frame(1,16);             // 切到“放生”
  P('A'); t.frame(1,16);             // 放生二次确认覆盖层
  P('C'); t.frame(1,16);             // 取消确认
  P('C'); t.frame(1,16);             // 关闭菜单
  s.scene = 'DEX'; t.markAllSeen();
  for (let i=0;i<12;i++){ P('B'); t.frame(1,16); }  // 遍历全部 11 种怪的图鉴绘制
  chk('绘制后仍合法场景', ['DEX','PARTNERS','CITY'].includes(t.scene()));
});

console.log('\n=== 结果 ===');
if (errs.length) { console.log('失败 ' + errs.length + ' 项:'); errs.forEach(e => console.log('  - ' + e)); process.exit(1); }
else console.log('全部通过 ✅');

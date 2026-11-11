// Single source of truth for every colour value that appears in the design
// docs, the Q&A page and the prototype. Generated from ember_color_engine.js,
// so a doc can never quietly drift away from what the screen actually draws.
const fs = require("fs");
const path = require("path");
const E = require("./ember_color_engine.js");

function snap(tot) {
  const c = E.colorFor(tot);
  return {
    tot: tot, hex: c.hex,
    h: +c.h.toFixed(1), s: +c.s.toFixed(2), l: +c.l.toFixed(2),
    progress: +c.progress.toFixed(2),
    main: c.main, second: c.second,
    tipHex: c.tipHex, tipWeight: +c.tipWeight.toFixed(2),
    name: E.colorName(c.h, c.s, c.l)
  };
}

const base = snap([0, 0, 0, 0]);
const dims = E.DIMS.map((d, i) => {
  const t = [0, 0, 0, 0]; t[i] = 15;
  const c = snap(t);
  return {
    label: d.label, hue: d.hue, designHex: d.hex,
    h: d.h, s: d.s, l: d.l,
    engineHex: c.hex, engineName: c.name
  };
});

const steps = [[0,0,0,0],[0,0,1,0],[0,0,2,0],[0,0,3,0],[1,0,3,0],[2,0,3,0]].map(snap);
const nights = [[2,0,3,0],[4,0,6,0],[6,0,9,0],[8,0,12,0]].map(snap);

const facts = {
  source: "tools/ember_color_engine.js",
  base: { h: E.BASE.h, s: E.BASE.s, l: E.BASE.l, hex: base.hex, name: base.name },
  neutral: { h: E.NEUTRAL.h, s: E.NEUTRAL.s, l: E.NEUTRAL.l,
             hex: snap([2,2,2,2]).hex, name: snap([2,2,2,2]).name },
  stops: E.STOPS,
  dims: dims,
  q3to2: {
    steps: steps.map(c => c.hex),
    answer: nights[0].hex,
    nights: nights.map(c => ({ hex: c.hex, h: c.h, s: c.s, l: c.l, progress: c.progress })),
    stable: nights[2].hex,
    oldVectorMean: "#C7275F"
  },
  borrowed: {
    designHex: E.BORROWED.hex,
    engineHex: E.colorFor([2, 0, 3, 0], { borrowed: true }).hex,
    h: E.BORROWED.h, s: E.BORROWED.s, l: E.BORROWED.l,
    note: "借明天的柴 → 整团火直接沉到黑蓝，不参与主轴混色，次夜释放"
  },
  rules: {
    model: "主轴优先 + 本色底火按累计段数退场 + 次色只落外焰 + 避绿路径",
    glowModel: "发光体色阶：暗外焰 → 中焰 → 亮内焰 → 白热芯；脸属白热芯，与焰尖同色",
    screen: "静态夜森林底图 + 精灵独立透明层；森林不参与混色",
    orderInvariant: "添柴顺序只改过程，不改当夜最终色",
    tipMax: 0.20
  }
};

const p = path.join(__dirname, "ember_facts.json");
fs.writeFileSync(p, JSON.stringify(facts, null, 2), "utf8");
console.log("wrote", p);
console.log("base", facts.base.hex, "| answer", facts.q3to2.answer,
            "| stable", facts.q3to2.stable);
console.log("nights", facts.q3to2.nights.map(n => n.hex + "/" + (n.progress * 100) + "%").join(" → "));

/* Real-scenario test for the ember colour engine.
   Run: node tools/test_ember_color.js
   Loads the exact same engine file the playable prototype loads, then walks
   actual log-by-log sequences and asserts the design rules.
*/
"use strict";
const fs = require("fs");
const path = require("path");
const E = require("./ember_color_engine.js");

const ROOT = path.resolve(__dirname, "..");
const PROTO = path.join(ROOT, "余烬_这游戏玩什么_可玩原型.html");
let PASS = 0, FAIL = 0;
const W = 26;
const pad = (s, n) => String(s) + " ".repeat(Math.max(0, n - String(s).length));

function check(name, ok, detail) {
  (ok ? PASS++ : FAIL++);
  console.log((ok ? "  PASS  " : "  FAIL  ") + pad(name, W) + (detail || ""));
}

function seq(plan) {
  // plan: array of dimension indices, one entry per log added
  const tot = [0, 0, 0, 0], out = [];
  for (const i of plan) {
    tot[i]++;
    const c = E.colorFor(tot);
    out.push({ hex: c.hex, h: c.h, s: c.s, l: c.l, main: c.main, p: c.progress });
  }
  return { tot, out };
}

function hexDelta(a, b) {
  const p = h => [1, 3, 5].map(i => parseInt(h.slice(i, i + 2), 16));
  const x = p(a), y = p(b);
  return Math.max(...x.map((v, i) => Math.abs(v - y[i])));
}

console.log("余烬 · 火色引擎真实场景测试");
console.log("引擎: tools/ember_color_engine.js   （与可玩原型同一份源码）");
console.log("=".repeat(78));

// ---------- 1. the question the user actually asked ----------
const love3work2 = E.colorFor([2, 0, 3, 0]);
console.log("\n【1】热爱 3 段 + 工作 2 段（首夜，本色起步）");
console.log("     主轴 = " + E.DIMS[love3work2.main].label + " " + E.DIMS[love3work2.main].hue +
  "   次色 = " + E.DIMS[love3work2.second].label + " " + E.DIMS[love3work2.second].hue);
console.log("     火色 = " + love3work2.hex + "   H " + love3work2.h.toFixed(1) +
  "°  S " + (love3work2.s * 100).toFixed(0) + "%  L " + (love3work2.l * 100).toFixed(0) + "%" +
  "   底火退场 " + (love3work2.progress * 100).toFixed(0) + "%");
console.log("     外焰冷光 = " + love3work2.tipHex + " 权重 " + love3work2.tipWeight.toFixed(2));
check("复现已确认答案 #DF7C4F", love3work2.hex === "#DF7C4F", love3work2.hex);
check("主轴是热爱（占比最高）", love3work2.main === 2, "main=" + love3work2.main);
check("不落进紫色区 270~345°", !(love3work2.h >= 270 && love3work2.h <= 345), love3work2.h.toFixed(1) + "°");
check("不等于旧机制玫红 #C7275F", love3work2.hex !== "#C7275F", "旧机制 #C7275F 已废弃");

// ---------- 2. old mechanism, for the record ----------
const vec = (function () {
  let X = 0, Y = 0;
  const w = [2 / 5, 0, 3 / 5, 0];
  E.DIMS.forEach((d, i) => {
    const a = d.h * Math.PI / 180;
    X += Math.cos(a) * w[i]; Y += Math.sin(a) * w[i];
  });
  let h = Math.atan2(Y, X) * 180 / Math.PI; if (h < 0) h += 360;
  return h;
})();
console.log("\n【2】旧机制对照（四维色圆周向量平均，已判定不成立）");
console.log("     合力色相 = " + vec.toFixed(1) + "° → 玫红 #C7275F；新机制 = " +
  love3work2.h.toFixed(1) + "° → " + love3work2.hex);
check("新旧机制结果不同", Math.abs(vec - love3work2.h) > 60, vec.toFixed(1) + "° vs " + love3work2.h.toFixed(1) + "°");

// ---------- 3. order independence ----------
const a = seq([2, 2, 2, 0, 0]);   // passion first
const b = seq([0, 0, 2, 2, 2]);   // work first
console.log("\n【3】添柴顺序：先热爱 vs 先工作");
console.log("     先热爱逐段: " + a.out.map(v => v.hex).join(" → "));
console.log("     先工作逐段: " + b.out.map(v => v.hex).join(" → "));
check("最终色与顺序无关", a.out[4].hex === b.out[4].hex, a.out[4].hex);
check("过程确实不同（顺序只影响过程）", a.out[1].hex !== b.out[1].hex, a.out[1].hex + " vs " + b.out[1].hex);
const hs = a.out.map(v => v.h);
check("逐段色相单调向热爱靠近", hs.every((v, i) => i === 0 || v <= hs[i - 1] + .01),
  hs.map(v => v.toFixed(1)).join("° → ") + "°");
check("逐段不冒绿火 75~155°", a.out.every(v => !(v.h >= 75 && v.h <= 155)),
  "全程 " + hs[0].toFixed(1) + "° → " + hs[4].toFixed(1) + "°");
const purple = b.out.filter(v => v.h >= 290 && v.h <= 350);
check("冷色过渡态已失色、不闪艳紫", purple.every(v => v.s < .5),
  purple.length ? "艳紫区最大 S=" + Math.max(...purple.map(v => v.s)).toFixed(2) : "未进入艳紫区");

// ---------- 4. single-dimension nights ----------
console.log("\n【4】整夜只给一个板块（首夜 5 段 / 稳定 15 段）");
const single = [
  { i: 0, label: "工作", name: "墨蓝" },
  { i: 1, label: "关系", name: "蜜金" },
  { i: 2, label: "热爱", name: "朱绯" },
  { i: 3, label: "身体", name: "苔青" }
];
for (const d of single) {
  const t5 = [0, 0, 0, 0], t15 = [0, 0, 0, 0];
  t5[d.i] = 5; t15[d.i] = 15;
  const c5 = E.colorFor(t5), c15 = E.colorFor(t15);
  console.log("     " + pad(d.label + " · " + d.name, 12) +
    "首夜 " + c5.hex + " (h " + c5.h.toFixed(1) + "° / " + E.colorName(c5.h, c5.s, c5.l) + ")" +
    "   稳定 " + c15.hex + " (h " + c15.h.toFixed(1) + "°)");
  check(d.label + "稳定色到达主轴色相", Math.abs(E.arc(c15.h, E.DIMS[d.i].h)) < 1.0,
    "误差 " + Math.abs(E.arc(c15.h, E.DIMS[d.i].h)).toFixed(2) + "°");
}
const work5 = E.colorFor([5, 0, 0, 0]);
const body5 = E.colorFor([0, 0, 0, 5]);
check("全给工作首夜不玫紫", !(work5.h >= 310 && work5.h <= 350),
  work5.h.toFixed(1) + "° " + work5.hex);
check("全给身体首夜已进苔青带", body5.h >= 135, body5.h.toFixed(1) + "° " + body5.hex);

// ---------- 5. evenly shared ----------
const even = E.colorFor([2, 2, 2, 2]);
console.log("\n【5】四盏各 2 段 · 分得匀");
console.log("     火色 = " + even.hex + "  S " + (even.s * 100).toFixed(0) + "%  主轴 = " +
  (even.main < 0 ? "无（不偏色）" : even.main));
check("判定为均衡态", even.balanced === true, "balanced=" + even.balanced);
check("低饱和、不撞任一板块色", even.s < .5, "S=" + even.s.toFixed(2));
check("均衡态不挂次色冷光", even.tipWeight === 0 && even.tipHex === null, "tip=" + even.tipWeight);

// ---------- 6. base ember withdrawal across nights ----------
console.log("\n【6】本色底火按累计段数退场（热爱 3 : 工作 2 连着几夜）");
const nights = [[2, 0, 3, 0], [4, 0, 6, 0], [6, 0, 9, 0], [10, 0, 15, 0]];
let prevH = null, mono = true; const arcs = [];
nights.forEach((t) => {
  const c = E.colorFor(t);
  console.log("     累计 " + pad(c.W + " 段", 7) + " 退场 " + (c.progress * 100).toFixed(0) + "%   " +
    c.hex + "   h " + c.h.toFixed(1) + "°   " + E.colorName(c.h, c.s, c.l));
  if (prevH !== null) {
    const d = E.arc(prevH, c.h);       // signed arc, handles the 0deg wrap
    arcs.push(d.toFixed(1));
    if (d > .01) mono = false;
  }
  prevH = c.h;
});
check("跨夜色相单调向朱绯靠近", mono, "逐夜 " + arcs.join("° / ") + "°（跨 0° 用有符号弧判定）");
check("底火最终完全退场", E.colorFor(nights[3]).progress === 1, "progress=1.00");
check("稳定色接近朱绯 #E83420", hexDelta(E.colorFor(nights[3]).hex, "#E83420") < 30,
  E.colorFor(nights[3]).hex);

// ---------- 7. determinism / uniqueness ----------
const r1 = E.colorFor([2, 0, 3, 0]), r2 = E.colorFor([2, 0, 3, 0]);
check("同一累计结果确定性", r1.hex === r2.hex && r1.h === r2.h, r1.hex);
check("引擎无隐藏随机", !/Math\.random/.test(fs.readFileSync(path.join(__dirname, "ember_color_engine.js"), "utf8")),
  "唯一色号由显示层 seed 决定，不进混色规则");

// ---------- 7b. borrowed wood: ink-blue state, released next night ----------
console.log("\n【7b】借柴 · 黑蓝冷焰状态");
const borrowC = E.colorFor([2, 0, 3, 0], { borrowed: true });
check("借柴压到黑蓝 #173063", hexDelta(borrowC.hex, "#173063") < 6, borrowC.hex);
check("黑蓝时不带次色冷光", borrowC.tipHex === null && borrowC.tipWeight === 0);
check("黑蓝与主轴色明确不同", hexDelta(borrowC.hex, E.colorFor([2, 0, 3, 0]).hex) > 80,
  borrowC.hex + " vs " + E.colorFor([2, 0, 3, 0]).hex);
check("不借柴时恢复主轴绯橙", E.colorFor([2, 0, 3, 0]).hex === "#DF7C4F");

// ---------- 8. the prototype really ships this engine ----------
const html = fs.readFileSync(PROTO, "utf8");
console.log("\n【8】交付一致性");
check("原型引用同一份引擎", /<script src="tools\/ember_color_engine\.js"><\/script>/.test(html),
  "script src=tools/ember_color_engine.js");
check("原型已删除圆周向量平均", !/Math\.atan2\(Y,X\)/.test(html), "旧 targetColor 已移除");
check("原型不再写「脸部不染」", !/脸部不染/.test(html), "与发光体模型一致");
check("原型不再写「圆周向量混色」", !/圆周向量混色/.test(html), "机制描述已同步");
check("借柴时立即重算火色", /borrowed=true; wood\+=2; stepColor\(\);/.test(html));
check("过夜会重算火色、释放黑蓝状态", /function nextDay\(\)[\s\S]*?stepColor\(\)/.test(html));
check("跳周与快捷演示先释放借柴状态",
  /function jumpWeek\(\)[\s\S]*?borrowed=false/.test(html) &&
  /function demo\([\s\S]*?borrowed=false/.test(html));

console.log("\n" + "=".repeat(78));
console.log("结果: " + PASS + " PASS / " + FAIL + " FAIL");
console.log("状态: 未改固件 / 未 push / 未烧录（仅电脑端原型与测试）");
process.exit(FAIL ? 1 : 0);

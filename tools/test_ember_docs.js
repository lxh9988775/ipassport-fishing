// Doc consistency gate: every colour value and every rule statement in the
// deliverables must agree with ember_facts.json (generated from the engine).
// This is the check that stops "docs say one thing, screen draws another".
const fs = require("fs");
const path = require("path");
const E = require("./ember_color_engine.js");

const ROOT = path.join(__dirname, "..");
const facts = JSON.parse(fs.readFileSync(path.join(__dirname, "ember_facts.json"), "utf8"));
const read = f => fs.readFileSync(path.join(ROOT, f), "utf8");

const DOCS = {
  proto: "余烬_这游戏玩什么_可玩原型.html",
  design: "余烬_设计案V1.html",
  spec: "余烬_界面设计规范V1.html",
  qa: "余烬_火色问答_热爱3工作2.html"
};
const S = Object.fromEntries(Object.entries(DOCS).map(([k, f]) => [k, read(f)]));

let pass = 0, fail = 0;
function check(name, ok, detail) {
  if (ok) { pass++; console.log("  PASS  " + name + (detail ? "  " + detail : "")); }
  else { fail++; console.log("  FAIL  " + name + (detail ? "  " + detail : "")); }
}

console.log("【1】事实源与引擎一致");
const q = facts.q3to2;
check("本色 #DBA86B 与引擎相符", facts.base.hex === E.colorFor([0,0,0,0]).hex, facts.base.hex);
check("3:2 答案 #DF7C4F 与引擎相符", q.answer === E.colorFor([2,0,3,0]).hex, q.answer);
check("稳定色 #E92028 与引擎相符", q.stable === E.colorFor([6,0,9,0]).hex, q.stable);
check("三个退场档 40/70/100", [0.4,0.7,1].every((v,i) => q.nights[i].progress === v),
  q.nights.slice(0,3).map(n => (n.progress*100)+"%").join(" / "));
check("四主轴设计色与引擎色号齐全", facts.dims.length === 4 &&
  facts.dims.every(d => /^#[0-9A-F]{6}$/.test(d.designHex) && /^#[0-9A-F]{6}$/.test(d.engineHex)));
check("借柴黑蓝状态已入事实源", /^#[0-9A-F]{6}$/.test(facts.borrowed.engineHex),
  facts.borrowed.engineHex);

console.log("\n【2】旧口径零残留");
const stale = [
  ["#E8A24E", "旧初始色号"],
  ["85% 后稳定", "旧的 85% 退场口径"],
  ["#E6352C", "旧稳定色"],
  ["#E3573C", "旧第二夜色"],
  ["圆润暖橙火苗", "早期萌物角色设定"],
  ["脸永远是那张暖白", "被否定的暖白脸结论"],
  ["287° 暮紫", "向量平均混色结论"],
  ["约 36%", "旧的逐段逼近速率"],
  ["几乎纯程序化绘制", "旧技术方案"],
  ["V5 用「四维色圆周向量平均」", "旧版本自述"]
];
for (const [needle, label] of stale) {
  const hits = Object.entries(S).filter(([, s]) => s.includes(needle)).map(([k]) => k);
  check(label + " 已清除", hits.length === 0, hits.length ? "残留于 " + hits.join(",") : "0 处");
}

console.log("\n【3】新口径落进每份文档");
["#DBA86B", "#DF7C4F", "#E92028"].forEach(hex => {
  check("问答页含 " + hex, S.qa.includes(hex));
});
check("问答页写明 40% / 70% / 100%",
  S.qa.includes("40%") && S.qa.includes("70%") && S.qa.includes("100%"));
["#285780", "#EDC55A", "#E93420", "#4AB58E", "#C7A67A", "#173163"].forEach(hex => {
  check("设计案 + 界面规范均含 " + hex,
    S.design.includes(hex) && S.spec.includes(hex));
});
check("设计案写明主轴优先与发光体色阶",
  S.design.includes("主轴优先") && S.design.includes("发光体色阶"));
check("界面规范写明分层与背景不参与混色",
  S.spec.includes("ember_bg_clean.png") && S.spec.includes("ember_spirit_skeleton.png") &&
  S.spec.includes("背景不参与混色"));
check("原型写明主轴优先与底火退场",
  S.proto.includes("主轴优先") && S.proto.includes("退场 40%"));
check("原型不再把暖白脸当设定", !S.proto.includes("暖白的脸") && !S.proto.includes("暖白留"));

console.log("\n【4】界面规范：森林滤镜只改明暗");
check("8 屏内联滤镜不含 saturate / sepia",
  !/style="filter:[^"]*(saturate|sepia|hue-rotate)/.test(S.spec));
check("火相四态不含 saturate 数值", !/brightness [\d.]+ \/ saturate/.test(S.spec));
check("8 屏统一指向分层合成图",
  (S.spec.match(/ember_screen_default\.png/g) || []).length >= 8,
  (S.spec.match(/ember_screen_default\.png/g) || []).length + " 处");

console.log("\n【5】结构与资源");
for (const [k, f] of Object.entries(DOCS)) {
  const s = S[k];
  const d = (s.match(/<div\b/g) || []).length, dc = (s.match(/<\/div>/g) || []).length;
  check(k + " div 标签配平", d === dc, d + "/" + dc);
  check(k + " 无未替换占位符", (s.match(/__[A-Z0-9]+__/g) || []).length === 0);
}
const assets = [
  "generated-images/ember_bg_clean.png",
  "generated-images/ember_spirit_skeleton.png",
  "generated-images/ember_screen_default.png",
  "generated-images/ember_screen_fix_compare.png",
  "generated-images/ember_relight_family.png",
  "generated-images/ember_3to2_evolution.png",
  "tools/ember_color_engine.js",
  "tools/ember_spirit_skeleton.js",
  "tools/ember_facts.json"
];
assets.forEach(a => check("资源存在 " + a, fs.existsSync(path.join(ROOT, a))));

console.log("\n" + "=".repeat(78));
console.log("结果: " + pass + " PASS / " + fail + " FAIL");
console.log("状态: 未改固件 / 未 push / 未烧录（仅电脑端文档、原型与测试）");
process.exitCode = fail ? 1 : 0;

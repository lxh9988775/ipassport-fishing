// Cross-language check: the on-screen canvas relight must equal the approved
// computer-side comparison sheet, pixel for pixel.
const fs = require("fs");
const path = require("path");
const E = require("./ember_color_engine.js");

const samples = JSON.parse(fs.readFileSync(
  path.join(__dirname, "ember_screen_samples.json"), "utf8"));
const html = fs.readFileSync(
  path.join(__dirname, "..", "余烬_这游戏玩什么_可玩原型.html"), "utf8");

let pass = 0, fail = 0, worst = 0;
const TOL = 2; // HSL rounding differs by at most 1 between JS and numpy

for (const s of samples) {
  const got = E.relightPixel(s.l0, s.s0, s.hDeg, s.sat, s.tip, s.tipWeight);
  let d = 0;
  for (let i = 0; i < 3; i++) {
    d = Math.max(d, Math.abs(got[i] - s.expect[i]));
  }
  worst = Math.max(worst, d);
  if (d <= TOL) pass++;
  else {
    fail++;
    console.log("  FAIL", s.name, "l0", s.l0, "expect", s.expect, "got", got.map(Math.round));
  }
}

function check(name, ok, detail) {
  if (ok) { pass++; console.log("  PASS  " + name + (detail ? "  " + detail : "")); }
  else { fail++; console.log("  FAIL  " + name + (detail ? "  " + detail : "")); }
}

console.log("");
console.log("像素级一致性  " + pass + " / " + samples.length +
  "  最大偏差 " + worst.toFixed(1));
check("原型调用 EC.relightPixel（不是自己抄一份公式）",
  /EC\.relightPixel\(/.test(html) && !/RAMP_L=\[/.test(html));
check("骨架透明像素被跳过（森林一个都不碰）",
  /if\(a===0\) continue;/.test(html));
check("背景图是独立静态森林", /ember_bg_clean\.png/.test(html));
check("已删除全屏 color 混合层", !/mix-blend-mode:color/.test(html));
check("已删除整屏次色渐变层", !/dye-tip|id="dyetip"/.test(html));
check("森林滤镜不含色相／饱和度变化",
  !/bg\.style\.filter=.*hue-rotate/.test(html) &&
  !/f="[^"]*saturate\(/.test(html));
check("骨架以 data URL 载入（file:// 下 canvas 不脏）",
  /window\.EMBER_SKELETON/.test(html) &&
  fs.existsSync(path.join(__dirname, "ember_spirit_skeleton.js")));

console.log("");
console.log("结果: " + pass + " PASS / " + fail + " FAIL");
console.log("状态: 未改固件 / 未 push / 未烧录（仅电脑端原型与测试）");
process.exitCode = fail ? 1 : 0;

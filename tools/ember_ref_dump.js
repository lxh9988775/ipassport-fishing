// ember_ref_dump.js — 用真实 JS 引擎（ember_color_engine.js）导出对照基准，供 Python 镜像对拍。
const EC = require("./ember_color_engine.js");
const fs = require("fs");

const tots = [
  [0,0,0,0],[2,0,3,0],[5,0,0,0],[0,0,0,5],[2,2,2,2],
  [10,0,15,0],[3,1,1,0],[8,8,8,8],[6,0,0,0],[1,5,1,0],
  [0,0,1,6],[4,0,0,0],[1,0,1,0],[0,0,0,0],[7,0,0,0],
  [0,7,0,0],[0,0,7,0],[0,0,0,7],[2,2,1,0],[1,1,1,1],
  [3,0,2,0],[0,3,0,3],[9,0,0,0],[0,9,0,0]
];

const color = [];
for (const t of tots) {
  for (const bor of [false, true]) {
    const c = EC.colorFor(t, { borrowed: bor });
    color.push({ tot: t, borrowed: bor, h: c.h, s: c.s, l: c.l, hex: c.hex,
                 balanced: c.balanced, main: c.main, second: c.second, tipWeight: c.tipWeight });
  }
}

// relight 电池：l0/s0 网格 × 目标火色 × 有无次色冷光
const relight = [];
const hues = [6, 44, 158, 208, 220];
const tipRgb = EC.hslToRgb(44, 0.80, 0.64); // 关系蜜金
for (const l0 of [0, 0.12, 0.30, 0.50, 0.70, 0.88, 1.0]) {
  for (const s0 of [0, 0.5, 1.0]) {
    for (const h of hues) {
      for (const sat of [0.5, 0.82]) {
        const a = EC.relightPixel(l0, s0, h, sat, null, 0);
        const b = EC.relightPixel(l0, s0, h, sat, tipRgb, l0 < 0.38 ? 0.15 : 0);
        relight.push({ l0, s0, h, sat, base: a, tip: b });
      }
    }
  }
}

fs.writeFileSync("tools/ember_ref.json",
  JSON.stringify({ color, relight }, null, 0));
console.log("dumped", color.length, "color +", relight.length, "relight cases");

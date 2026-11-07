/* Ember colour engine — single source of truth.
   The playable prototype loads this file with a plain <script> tag; the Node
   self-test requires the very same file, so what is tested is what ships.

   Model: the spirit is a self-luminous flame. Its hue comes from the
   dominant dimension (not a vector average of all four), the base ember
   withdraws as cumulative logs accumulate, a secondary dimension only pulls
   the hue a few degrees and leaves a trace on the OUTER flame, and every hue
   travel detours around the green band. No DOM, no LVGL, no side effects.
*/
(function (root, factory) {
  var api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.EmberColor = api;
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  var DIMS = [
    { h: 208, s: .52, l: .33, hex: "#28577F", hue: "墨蓝", label: "工作" },
    { h: 44,  s: .80, l: .64, hex: "#ECC559", hue: "蜜金", label: "关系" },
    { h: 6,   s: .82, l: .52, hex: "#E83420", hue: "朱绯", label: "热爱" },
    { h: 158, s: .42, l: .50, hex: "#49B58D", hue: "苔青", label: "身体" }
  ];
  // Measured mean of the master sprite's flame, not a guessed value.
  var BASE = { h: 32.7, s: .61, l: .64 };
  // "Logs shared evenly" is a real state, not a failed mix.
  var NEUTRAL = { h: 36, s: .26, l: .62 };
  // Cumulative logs -> how far the base ember has withdrawn.
  // 5 / 10 / 15 logs roughly equal night 1 / 2 / 3.
  var STOPS = [[0, 0], [5, .40], [10, .70], [15, 1.0]];
  var GREEN_LO = 75, GREEN_HI = 155;
  var COLD_LO = 185, COLD_HI = 330;
  // Borrowing tomorrow's wood is a STATE, not a mix: the flame falls to the
  // coldest ink-blue and stays there for the rest of the night. It overrides
  // the dominant-dimension colour, then releases on the next night.
  var BORROWED = { h: 220, s: .62, l: .24, hex: "#173063" };

  function norm(h) { return ((h % 360) + 360) % 360; }
  function arc(a, b) { return ((b - a + 540) % 360) - 180; }

  function greenHits(from, d) {
    var c = 0;
    for (var k = 1; k <= 12; k++) {
      var hh = norm(from + d * k / 12);
      if (hh >= GREEN_LO && hh <= GREEN_HI) c++;
    }
    return c;
  }

  // Only cold destinations need the detour. Heading to a green-ish target is
  // allowed to pass through yellow-green, because that IS the destination.
  function travelArc(from, to) {
    var d1 = arc(from, to), t = norm(to);
    if (!(t >= COLD_LO && t <= COLD_HI)) return d1;
    var d2 = d1 >= 0 ? d1 - 360 : d1 + 360;
    return greenHits(from, d1) <= greenHits(from, d2) ? d1 : d2;
  }

  function progressFor(W) {
    if (W <= 0) return 0;
    if (W >= STOPS[STOPS.length - 1][0]) return STOPS[STOPS.length - 1][1];
    for (var i = 0; i < STOPS.length - 1; i++) {
      var a = STOPS[i], b = STOPS[i + 1];
      if (W <= b[0]) return a[1] + (b[1] - a[1]) * (W - a[0]) / (b[0] - a[0]);
    }
    return 1;
  }

  function analyze(tot) {
    var W = 0, active = 0, i;
    for (i = 0; i < 4; i++) { W += tot[i]; if (tot[i] > 0) active++; }
    var order = tot.map(function (v, idx) { return { i: idx, v: v }; })
      .sort(function (a, b) { return b.v - a.v || a.i - b.i; });
    var main = W > 0 ? order[0].i : -1;
    var second = (W > 0 && order[1].v > 0) ? order[1].i : -1;
    var wMain = main >= 0 ? tot[main] / W : 0;
    var wSecond = second >= 0 ? tot[second] / W : 0;
    var balanced = active >= 3 && wMain < .38;
    return {
      W: W, active: active, main: balanced ? -1 : main, second: second,
      wMain: wMain, wSecond: wSecond, balanced: balanced,
      // An evenly shared night has no "runner-up" tint on the outer flame.
      tipWeight: (!balanced && second >= 0) ? Math.min(.20, .30 * wSecond / wMain) : 0,
      tipHex: (!balanced && second >= 0) ? DIMS[second].hex : null
    };
  }

  function hslToRgb(h, s, l) {
    h = norm(h) / 360;
    s = Math.max(0, Math.min(1, s)); l = Math.max(0, Math.min(1, l));
    if (s === 0) { var v = Math.round(l * 255); return [v, v, v]; }
    var q = l < .5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    var f = function (t) {
      t = (t + 1) % 1;
      if (t < 1 / 6) return p + (q - p) * 6 * t;
      if (t < 1 / 2) return q;
      if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
      return p;
    };
    return [Math.round(f(h + 1 / 3) * 255), Math.round(f(h) * 255), Math.round(f(h - 1 / 3) * 255)];
  }

  // ---- screen relight: ONE colour ramp for the whole spirit ----
  // Mirrors tools/ember_relight.py (RAMP_X / RAMP_L / RAMP_S). The browser
  // canvas and the computer-side comparison sheet call this SAME function,
  // so the sprite on screen cannot drift away from the approved artwork.
  var RAMP_X = [0, .15, .32, .50, .70, .88, 1],
      RAMP_L = [.05, .14, .31, .50, .68, .80, .86],
      RAMP_S = [1, 1, .96, .90, .80, .62, .48];

  function rampVal(arr, x) {
    for (var i = 0; i < RAMP_X.length - 1; i++) {
      if (x <= RAMP_X[i + 1]) {
        var t = (x - RAMP_X[i]) / (RAMP_X[i + 1] - RAMP_X[i]);
        return arr[i] + (arr[i + 1] - arr[i]) * t;
      }
    }
    return arr[arr.length - 1];
  }

  // l0: ramp position (skeleton R), s0: brush density (skeleton G),
  // hDeg/sat: current fire colour, tipRgb/tipWeight: outer-flame cool light.
  function relightPixel(l0, s0, hDeg, sat, tipRgb, tipWeight) {
    var L = Math.max(0, Math.min(1, rampVal(RAMP_L, l0) * .86 + l0 * .14));
    var S = Math.max(0, Math.min(.95, rampVal(RAMP_S, l0) * sat * (.82 + .28 * s0)));
    var rgb = hslToRgb(hDeg, S, L);
    if (tipRgb && tipWeight > 0 && l0 < .38) {
      var w = tipWeight * .5 * (1 - l0 / .38);
      rgb = [rgb[0] * (1 - w) + tipRgb[0] * w,
             rgb[1] * (1 - w) + tipRgb[1] * w,
             rgb[2] * (1 - w) + tipRgb[2] * w];
    }
    return rgb;
  }

  function rgbToHex(r, g, b) {
    return "#" + [r, g, b].map(function (v) {
      return Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, "0");
    }).join("").toUpperCase();
  }

  function colorFor(tot, opts) {
    var a = analyze(tot), p = progressFor(a.W), target;
    if (opts && opts.borrowed) {
      var rgbB = hslToRgb(BORROWED.h, BORROWED.s, BORROWED.l);
      return {
        h: BORROWED.h, s: BORROWED.s, l: BORROWED.l, rgb: rgbB,
        hex: rgbToHex(rgbB[0], rgbB[1], rgbB[2]),
        progress: p, main: a.main, second: a.second, balanced: a.balanced,
        tipHex: null, tipWeight: 0, W: a.W,
        wMain: a.wMain, wSecond: a.wSecond, borrowed: true
      };
    }
    if (a.W === 0) target = { h: BASE.h, s: BASE.s, l: BASE.l };
    else if (a.balanced) target = NEUTRAL;
    else {
      var D = DIMS[a.main], h = D.h;
      if (a.second >= 0) {
        var pull = .08 * a.wSecond / a.wMain;
        h = norm(D.h + travelArc(D.h, DIMS[a.second].h) * pull);
      }
      target = { h: h, s: D.s, l: D.l };
    }
    // Hue travel that has to detour around green (or pass straight through it)
    // is a transition the player should not be parked on. Rush the HUE out of
    // that zone, while saturation / lightness still move at the calm 40%-a-night
    // pace. Safe short arcs inside the warm band keep the slow drift, which is
    // what makes "passion 3 / work 2" land on a gently reddened ember.
    var dh = travelArc(BASE.h, target.h);
    // A hue swing over 90 degrees is a real change of flame, not a drift.
    // Such a transition must not be parked on: rush the hue and desaturate
    // its middle, otherwise a half-mixed magenta flashes mid-night.
    var risky = Math.abs(dh) > 90 || greenHits(BASE.h, dh) > 0;
    var pHue = risky ? Math.min(1, p * 2.2) : p;
    var hh = norm(BASE.h + dh * pHue);
    // A flame cooling down loses colour before it reaches the new hue: on a
    // risky arc the mid-transition desaturates, so no saturated magenta or
    // yellow-green ever flashes on screen. Both endpoints keep full chroma.
    var dip = risky ? .34 * Math.sin(Math.PI * Math.min(1, pHue)) : 0;
    var ss = Math.max(.05, BASE.s + (target.s - BASE.s) * p - dip);
    var ll = BASE.l + (target.l - BASE.l) * p;
    var rgb = hslToRgb(hh, ss, ll);
    return {
      h: hh, s: ss, l: ll, rgb: rgb, hex: rgbToHex(rgb[0], rgb[1], rgb[2]),
      progress: p, main: a.main, second: a.second, balanced: a.balanced,
      tipHex: a.tipHex, tipWeight: a.tipWeight, W: a.W,
      wMain: a.wMain, wSecond: a.wSecond
    };
  }

  var BANDS = [[0, "朱绯"], [10, "绯橙"], [24, "暮橙"], [34, "暖金"], [52, "蜜金"], [70, "秋禾"],
    [100, "苔金"], [140, "苔青"], [182, "烟青"], [205, "墨蓝"], [238, "夜蓝"],
    [268, "暮紫"], [310, "绯紫"], [342, "绛红"], [361, "朱绯"]];

  function colorName(h, s, l) {
    h = norm(h);
    var base = "朱绯";
    for (var i = 0; i < BANDS.length - 1; i++) {
      if (h >= BANDS[i][0] && h < BANDS[i + 1][0]) { base = BANDS[i][1]; break; }
    }
    var pre = l < .34 ? "玄" : (s < .28 ? "烟" : s < .42 ? "雾" : (l < .5 ? "夜" : ""));
    return pre + base;
  }

  return {
    DIMS: DIMS, BASE: BASE, NEUTRAL: NEUTRAL, STOPS: STOPS, BORROWED: BORROWED,
    norm: norm, arc: arc, travelArc: travelArc, greenHits: greenHits,
    progressFor: progressFor, analyze: analyze,
    hslToRgb: hslToRgb, rgbToHex: rgbToHex,
    RAMP_X: RAMP_X, RAMP_L: RAMP_L, RAMP_S: RAMP_S,
    rampVal: rampVal, relightPixel: relightPixel,
    colorFor: colorFor, colorName: colorName
  };
});

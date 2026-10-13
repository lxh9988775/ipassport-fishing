/* 全字段 null/脏值扫描：逐字段破坏 state，跑全部场景 draw/update，抓剩余裸读取崩溃 */
import fs from 'fs';
import vm from 'vm';
import { fileURLToPath } from 'url';
import path from 'path';
const dir=path.dirname(fileURLToPath(import.meta.url));

function makeCtx(){
  const noop=()=>{};
  const m=[1,0,0,1,0,0];
  return {
    canvas:{width:240,height:320},
    save:noop,restore:noop,beginPath:noop,closePath:noop,arc:noop,fill:noop,stroke:noop,
    moveTo:noop,lineTo:noop,rect:noop,clip:noop,ellipse:noop,
    translate(x,y){m[4]+=m[0]*x+m[2]*y;m[5]+=m[1]*x+m[3]*y;},
    scale(x,y){m[0]*=x;m[1]*=x;m[2]*=y;m[3]*=y;},
    rotate(){},setTransform(){},resetTransform(){},clearRect:noop,fillRect:noop,strokeRect:noop,
    drawImage:noop,createLinearGradient:()=>({addColorStop:noop}),createRadialGradient:()=>({addColorStop:noop}),
    fillText:noop,strokeText:noop,measureText:()=>({width:10}),
    getImageData:()=>({data:new Uint8ClampedArray(4)}),putImageData:noop,
    set fillStyle(v){},get fillStyle(){return '#000';},
    set strokeStyle(v){},get strokeStyle(){return '#000';},
    set globalAlpha(v){},get globalAlpha(){return 1;},
    set font(v){},get font(){return '';},
    set textAlign(v){},get textAlign(){return 'left';},
    set textBaseline(v){},get textBaseline(){return 'top';},
    set lineWidth(v){},get lineWidth(){return 1;},
    set imageSmoothingEnabled(v){},get imageSmoothingEnabled(){return false;},
    set globalCompositeOperation(v){},get globalCompositeOperation(){return 'source-over';},
    set shadowColor(v){},set shadowBlur(v){},
    _m:m
  };
}
const sandbox={
  console,Math,JSON,Object,Array,String,Number,Boolean,Date,RegExp,parseInt,parseFloat,isNaN,isFinite,
  Uint8Array,Float32Array,Set,Map,Promise,Symbol,Error,
  performance:{now:()=>Date.now()},
  requestAnimationFrame:()=>0,setTimeout:()=>0,clearTimeout:()=>{},setInterval:()=>0,clearInterval:()=>{},
  localStorage:{getItem:()=>null,setItem:()=>{},removeItem:()=>{}},
  document:{ getElementById:(id)=>{
      const el={getContext:()=>makeCtx(),width:240,height:320,style:{},addEventListener:()=>{},appendChild:()=>{},classList:{add:()=>{},remove:()=>{},toggle:()=>{}},textContent:'',innerHTML:'',value:'',disabled:false};
      return el;},
    createElement:()=>({getContext:()=>makeCtx(),style:{},onload:null,onerror:null,src:'',appendChild:()=>{},classList:{add:()=>{},remove:()=>{}}}),
    addEventListener:()=>{},body:{appendChild:()=>{}}},
  Image:function(){return {set src(v){},get src(){return '';},onload:null,onerror:null};},
  confirm:()=>true,alert:()=>{},navigator:{userAgent:'x'},
  addEventListener:()=>{},
};
sandbox.window=sandbox;
sandbox.globalThis=sandbox;
sandbox.self=sandbox;
vm.createContext(sandbox);
const html=fs.readFileSync(path.join(dir,'index.html'),'utf8');
const code=html.match(/<script>([\s\S]*)<\/script>/)[1];
vm.runInContext(code,sandbox,{filename:'game.js'});

/* 暴露内部引用 */
const G=vm.runInContext(`({get state(){return state;},set state(v){state=v;},SCENES,save,normalizeState,
  startBattle,endBattle,ensureDungeonOffers,startDungeon,
  getBt(){return typeof bt!=='undefined'?bt:null;}})`,sandbox);

const scenes=Object.keys(G.SCENES);
let pass=0,fail=0;
const fails=[];
function T(name,fn){try{fn();pass++;console.log('  ✓ '+name);}catch(e){fail++;fails.push(name+' :: '+e.message);console.log('  ✗ '+name+' :: '+e.message);}}

/* 场景驱动：每个场景 draw + update + 随机按键 */
function driveScene(sc,steps=6){
  const S=G.SCENES[sc];if(!S)return;
  G.state.scene=sc;
  for(let i=0;i<steps;i++){
    if(S.update)S.update(0.016);
    if(G.state.scene!==sc)break; /* 场景跳走就停 */
    if(S.draw)S.draw();
    const keys=['A','B','C'];
    S.onBtn&&S.onBtn(keys[Math.floor(Math.random()*3)]);
    if(G.state.scene!==sc)break;
    if(S.draw)S.draw();
  }
  G.state.scene='CITY';
}

/* 深度枚举所有字段路径 */
function allPaths(obj,prefix='',depth=0,out=[]){
  if(obj===null||typeof obj!=='object'||depth>4)return out;
  for(const k of Object.keys(obj)){
    const p=prefix?prefix+'.'+k:k;
    out.push(p);
    allPaths(obj[k],p,depth+1,out);
  }
  return out;
}
function setPath(obj,p,val){
  const ks=p.split('.');let o=obj;
  for(let i=0;i<ks.length-1;i++){if(o==null||typeof o!=='object')return;o=o[ks[i]];}
  if(o!=null&&typeof o==='object')o[ks[ks.length-1]]=val;
}
function getPath(obj,p){return p.split('.').reduce((o,k)=>(o==null?undefined:o[k]),obj);}
function clone(o){return JSON.parse(JSON.stringify(o));}

/* 基准存档：中期进度，含伙伴/装备/副本/图鉴/魔法 */
function baseSave(){
  G.state.scene='CITY';
  const s=G.state;
  s.gold=800;s.level=9;s.rank=2;s.hp=s.maxHp=90;s.mp=s.maxMp=60;
  s.potions=3;s.orbs=4;s.seals=2;s.herbs=6;s.mats=8;s.crystals=5;
  s.stones=[0,2,1,1,1];
  s.spellLv={'0-0-0':2,'1-0-1':1,'2-1-2':1};
  s.discovered={'0-0-0':'火弹','1-0-1':'中级火弹','2-1-2':'广域水箭'};
  s.loadout=['0-0-0','1-0-1','2-1-2'];
  s.equipped='0-0-0';
  s.dungeonOffers=[3,7];
  s.map={chapter:2};
  s.weaponLv=2;s.atkBonus=4;
  s.cats=2;s.kills=30;
  s.ebag=[{slot:'w',name:'橡木杖',rar:1,atk:3,pow:1.05},{slot:'a',name:'布袍',rar:0,def:1}];
  s.equip={w:{slot:'w',name:'铁杖',rar:1,atk:5,pow:1.1},a:{slot:'a',name:'皮甲',rar:1,def:2},t:null};
  s.partners=[{type:'cat',name:'猫灵',rarity:2,level:8,ivs:[10,11,9],skill:0,trait:0,evolved:false},
              {type:'fox',name:'灵狐',rarity:3,level:6,ivs:[12,8,10],skill:1,trait:1,evolved:false}];
  s.activePartner=0;
  s.seen={'cat':1,'fox':1,'slime':1};
  s.autoCapture=true;
  s.pbag={};s.mbag={};
  return s;
}

console.log('=== 基线（干净存档）全场景驱动 ===');
T('baseline all scenes',()=>{
  baseSave();
  for(const sc of scenes){driveScene(sc,4);G.state.scene='CITY';}
});

console.log('=== 逐字段置 null 扫描（'+scenes.length+' 场景 × 每字段） ===');
const base=clone(baseSave());
const paths=allPaths(base).filter(p=>p!=='scene');
console.log('字段总数:',paths.length);
let scanFail=0;
for(const p of paths){
  const orig=getPath(base,p);
  for(const bad of [null,undefined,NaN]){
    const s=clone(base);
    setPath(s,p,bad);
    try{
      G.normalizeState(s);
      G.state=s;
      for(const sc of scenes){driveScene(sc,2);G.state.scene='CITY';}
    }catch(e){
      scanFail++;
      if(scanFail<=15)console.log('  ✗ 脏字段 '+p+'=' + String(bad)+' → '+e.message);
    }
  }
}
if(scanFail===0)console.log('  ✓ 全部字段 × 3 种脏值 × 全场景：0 崩溃');
else{fail++;console.log('  ✗ 共 '+scanFail+' 例崩溃');}

/* 嵌套结构整体替换为 null 元素 */
console.log('=== 结构级脏档（数组含 null 元素等） ===');
const structCases=[
  ['partners=[null,{...}]',s=>{s.partners=[null,{type:'cat',name:'x',rarity:2,level:3,ivs:[5,5,5],skill:0,trait:0}];}],
  ['partners 元素缺 ivs',s=>{s.partners=[{type:'cat',name:'x',rarity:2,level:3,skill:0,trait:0}];}],
  ['partners 未知 type',s=>{s.partners=[{type:'zzz',name:'x',rarity:2,level:3,ivs:[5,5,5],skill:0,trait:0}];}],
  ['ebag=[null]',s=>{s.ebag=[null];}],
  ['equip 全 null',s=>{s.equip={w:null,a:null,t:null};}],
  ['equip 未定义槽',s=>{s.equip={};}],
  ['ebag 装备缺 rar/slot',s=>{s.ebag=[{name:'x'}];}],
  ['dungeonOffers=[null,null]',s=>{s.dungeonOffers=[null,null];}],
  ['dungeonOffers 越界 id',s=>{s.dungeonOffers=[999,-5];}],
  ['dungeonOffers=null',s=>{s.dungeonOffers=null;}],
  ['discovered 值为 null',s=>{s.discovered={'0-0-0':null,'9-9-9':'x'};}],
  ['loadout 含未知 key',s=>{s.loadout=['9-9-9','0-0-0'];}],
  ['loadout=null',s=>{s.loadout=null;}],
  ['spellLv 值为 null/NaN',s=>{s.spellLv={'0-0-0':null,'1-0-1':NaN};}],
  ['spellLv=null',s=>{s.spellLv=null;}],
  ['stones=[null,null,null,null,null]',s=>{s.stones=[null,null,null,null,null];}],
  ['stones=null',s=>{s.stones=null;}],
  ['stones=[0,1,2]（旧3格）',s=>{s.stones=[0,1,2];}],
  ['pbag/mbag=null',s=>{s.pbag=null;s.mbag=null;}],
  ['seen=null',s=>{s.seen=null;}],
  ['map=null',s=>{s.map=null;}],
  ['map.chapter=null',s=>{s.map={chapter:null};}],
  ['skills/trait 越界',s=>{s.partners[0].skill=99;s.partners[0].trait=99;}],
  ['rarity 越界',s=>{s.partners[0].rarity=99;}],
];
for(const [name,mut] of structCases){
  T(name,()=>{
    const s=clone(base);mut(s);G.normalizeState&&G.normalizeState(s);G.state=s;
    for(const sc of scenes){driveScene(sc,3);G.state.scene='CITY';}
  });
}

/* 战斗内脏档（bt 相关） */
console.log('=== 战斗内场景 ===');
T('battle with dirty save',()=>{
  const s=clone(base);s.partners=[null];s.ebag=[null];G.state=s;
  G.startBattle(false);
  const bt=G.getBt();
  if(bt){bt.phase='menu';G.SCENES.BATTLE.draw();/* 战斗菜单绘制冒烟（新美化卡片） */
    for(let i=0;i<30;i++){G.SCENES.BATTLE.update(0.05);if(!G.getBt())break;
    const b=G.getBt();if(b.phase==='menu')G.SCENES.BATTLE.onBtn('A');else if(b.phase==='event')G.SCENES.BATTLE.onBtn('A');
    else if(b.phase==='item'){G.SCENES.BATTLE.onBtn('B');G.SCENES.BATTLE.onBtn('A');G.SCENES.BATTLE.onBtn('C');}
    else if(b.phase==='magic'){G.SCENES.BATTLE.onBtn('A');G.SCENES.BATTLE.onBtn('C');}}}
  G.state.scene='CITY';
});

console.log('\n=== 结果: '+pass+' 通过 / '+fail+' 失败 ===');
if(fails.length){console.log('失败明细:');fails.slice(0,20).forEach(f=>console.log(' - '+f));}
process.exit(fail?1:0);

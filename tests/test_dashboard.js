const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(require('path').join(__dirname,'../dashboard_source.html'),'utf8');
const blocks=[...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
const elements=new Map();
function element(id){if(!elements.has(id))elements.set(id,{id,dataset:{},style:{},disabled:false,textContent:'',className:'',classList:{toggle(){},add(){},remove(){}}});return elements.get(id);}
for(const m of html.matchAll(/id="([^"]+)"/g))element(m[1]);
const profiles=['MODEL_YL','MODEL_Y_JUNIPER','MODEL_Y_LEGACY','MODEL_3_HIGHLAND','MODEL_3_LEGACY'].map((vehicle,i)=>Object.assign(element('vehicle'+i),{dataset:{vehicle}}));
const turns=['STALK','STALKLESS'].map(turn=>Object.assign(element(turn),{dataset:{turn}}));
const document={getElementById:id=>elements.get(id)||null,querySelectorAll:q=>q==='.vehicleChoice'?profiles:q==='.turnChoice'?turns:[],body:element('body')};
let profileScript=blocks.find(x=>x.includes('const PROFILES='));
profileScript=profileScript.replace('  boot();','  profileStatusReady=true; profileStatus={nvsError:false};');
const context={document,window:{},console,setTimeout(){},clearTimeout(){}};
vm.createContext(context);vm.runInContext(profileScript,context);
for(let i=0;i<profiles.length;i++){
 profiles[i].onclick();
 if(i===3){assert(element('saveProfileBtn').disabled);turns[1].onclick();}
 assert(!element('saveProfileBtn').disabled,'Profile must be selectable with all 3 buses');
 element('saveProfileBtn').onclick();
 assert(element('profileConfirmSummary').textContent.includes('CAN 1 BODY / CAN 2 CHASSIS / CAN 3 PARTY'));
}
// Execute the actual home renderer with physical bus data and strict DOM lookup.
let main=blocks.find(x=>x.includes('function renderHomeFast'));
let render=main.slice(main.indexOf('function renderHomeFast'),main.indexOf('function renderHomeSlow'));
Object.assign(context,{$:id=>elements.get(id)||null,nagCfg:{method:0},fmtMs:n=>n+' ms',renderApPresentation(){},renderHomeBlinkerState(){},renderR79Summary(){}});
vm.runInContext(render,context);
const t={};for(let n=1;n<=3;n++){t['can'+n+'State']=1;t['can'+n+'Ready']=true;t['can'+n+'TrafficSeen']=true;t['can'+n+'TrafficOnline']=n!==2;t['can'+n+'TrafficAgeMs']=n===2?5000:10;}
context.renderHomeFast({cantraffic:t,nag:{torque:1,apActive:true},blink:{},summon:{},r79:{}});
assert.equal(element('can1Traffic').textContent,'LIVE');assert.equal(element('can2Traffic').textContent,'NO DATA');assert.equal(element('can3Traffic').textContent,'LIVE');
console.log('PASS: actual dashboard profile selection for all five models, Highland choice, three home traffic states');
// Execute the capture panel's actual route formatter, including the third bus.
const busFormatter=html.match(/const busText=v=>\{[^\n]+?\};/)[0];
for(const [mapping,names] of [[[1,2,3],['BODY','CHASSIS','PARTY']],[[3,1,2],['PARTY','BODY','CHASSIS']]]){
 const s={canAPhysical:mapping[0],canBPhysical:mapping[1],canExtraPhysical:mapping[2],canAName:names[0],canBName:names[1],canExtraName:names[2]};
 const c={s};vm.createContext(c);vm.runInContext(busFormatter+'this.formatBus=busText;',c);
 for(let route=0;route<3;route++)assert.equal(c.formatBus(route),'CAN '+mapping[route]+' · '+names[route]);
 assert.equal(c.formatBus(-1),'—');assert.equal(c.formatBus(3),'—');
}
console.log('PASS: capture panel maps all three physical buses for standard and YL profiles');
// Exercise the real diagnostics renderer with independent physical counters.
const diagnostics=main.slice(main.indexOf('function renderCanDiagnostics'),main.indexOf('function fmtMs'));
context.fmtUptime=n=>n+' s';vm.runInContext(diagnostics,context);
const stats={fwVersion:'test-net2',networkState:'READY',hostedHostVersion:'2.6.1',hostedC6Version:'2.6.0'};
for(let n=1;n<=3;n++)Object.assign(stats,{
 ['can'+n+'State']:n===2?2:1,['can'+n+'Ready']:n!==2,['can'+n+'StatusSeen']:true,['can'+n+'StatusAgeMs']:25,
 ['can'+n+'RxCount']:100*n,['can'+n+'RxPolls']:1000*n,['can'+n+'RxResult']:'ESP_ERR_TIMEOUT',
 ['can'+n+'InitResult']:'ESP_OK',['can'+n+'Mode']:'NORMAL',['can'+n+'Controller']:n-1,
 ['can'+n+'TxPin']:[4,2,26][n-1],['can'+n+'RxPin']:[5,3,27][n-1],['can'+n+'Tec']:n,['can'+n+'Rec']:n+10
});
context.fetchSys(stats).then(()=>{
 for(let n=1;n<=3;n++){
  assert(element('diagCan'+n+'Rx').textContent.startsWith(String(100*n)+' /'));
  assert(element('diagCan'+n+'Polls').textContent.startsWith(String(1000*n)+' /'));
  assert.equal(element('diagCan'+n+'Errors').textContent,n+' / '+(n+10));
 }
 assert(element('diagCan2State').textContent.startsWith('BUS OFF'));
 assert.equal(element('diagHostedVersion').textContent,'2.6.1 / 2.6.0');
 console.log('PASS: full system diagnostics render three independent physical buses and Hosted versions');
});

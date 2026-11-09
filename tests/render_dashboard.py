from pathlib import Path
from playwright.sync_api import sync_playwright
import json
root=Path(__file__).resolve().parents[1]
html=(root/'dashboard_source.html').read_text(encoding='utf8')
profile={'ok':True,'setupMode':False,'nvsError':False,'profile':3,'topology':2,'turn':1,'canA':'BODY','canB':'CHASSIS','canBusCount':3,'nagSupported':True,'nagTorqueSupported':True,'nagTsl9Supported':True,'advancedEapSupported':True,'euUnlockSupported':True,'pedalMapSupported':True}
traffic={}
for n in range(1,4):
 traffic.update({f'can{n}State':2 if n==2 else 1,f'can{n}Ready':n!=2,f'can{n}TrafficSeen':True,f'can{n}TrafficOnline':n!=2,f'can{n}TrafficAgeMs':5000 if n==2 else 12*n})
snapshot={'nag':{'torque':0,'apActive':False},'blink':{'dasStateValid':True,'dasState':0},'summon':{},'cantraffic':traffic,'r79':{},'s3xy':{'bluetoothEnabled':False},'lab3f8':{}}
diag={}
with sync_playwright() as pw:
 browser=pw.chromium.launch(executable_path=r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',headless=True)
 page=browser.new_page(viewport={'width':430,'height':1000},device_scale_factor=1)
 errors=[];page.on('pageerror',lambda e: errors.append(str(e)))
 def route(req):
  url=req.request.url
  if '/api/' in url:
   obj=profile if '/api/profile/status' in url else diag if '/api/system/stats' in url else snapshot if '/api/snapshot' in url else {}
   req.fulfill(status=200,content_type='application/json',body=json.dumps(obj))
  elif url.rstrip('/')=='http://p4can.test':req.fulfill(status=200,content_type='text/html',body=html)
  else:req.fulfill(status=204,body='')
 page.route('**/*',route)
 page.goto('http://p4can.test/',wait_until='load')
 page.wait_for_timeout(1000)
 # Drive the production renderer with explicit simulated traffic; no hardware claim.
 page.evaluate('''data=>{document.body.classList.remove('setup-active');universalBootReady=false;nagCfg={method:0,enabled:false};renderHomeFast(data);}''',snapshot)
 for n,role in [(1,'BODY'),(2,'CHASSIS'),(3,'PARTY')]:
  assert page.locator(f'#homeCan{n}Name').is_visible()
  assert role in page.locator(f'#homeCan{n}Name').inner_text()
 assert page.locator('#can2Badge').inner_text()=='BUS OFF'
 assert page.locator('#can1Traffic').inner_text()=='LIVE'
 assert page.locator('#can2Traffic').inner_text()=='NO DATA'
 assert page.locator('#can3Traffic').inner_text()=='LIVE'
 boxes=[page.locator(f'.homeCan{n}Card').bounding_box() for n in range(1,4)]
 assert max(b['y'] for b in boxes)-min(b['y'] for b in boxes)<2,boxes
 assert boxes[0]['x']+boxes[0]['width']<=boxes[1]['x'] and boxes[1]['x']+boxes[1]['width']<=boxes[2]['x'],boxes
 for width in [360,430,1024]:
  page.set_viewport_size({'width':width,'height':1000})
  assert page.evaluate('document.documentElement.scrollWidth <= innerWidth'),f'Horizontal overflow at {width}'
  for n in range(1,4):assert page.locator(f'.homeCan{n}Card').is_visible()
 page.set_viewport_size({'width':430,'height':1000})
 page.screenshot(path=str(root/'artifacts/dashboard-three-buses-simulated.png'),full_page=True)
 diag=dict(traffic,fwVersion='test-net2',networkState='READY',hostedHostVersion='2.6.1',hostedC6Version='2.6.0')
 for n in range(1,4):
  diag.update({f'can{n}RxCount':100*n,f'can{n}RxPolls':2000*n,f'can{n}RxResult':'ESP_ERR_TIMEOUT',f'can{n}InitResult':'ESP_OK',f'can{n}Mode':'NORMAL',f'can{n}StatusSeen':True,f'can{n}StatusAgeMs':20,f'can{n}Controller':n-1,f'can{n}TxPin':[4,2,26][n-1],f'can{n}RxPin':[5,3,27][n-1]})
 page.evaluate('async data=>{await fetchSys(data);openPanel("panelDiag");}',diag)
 page.wait_for_timeout(400)
 for width in [360,430,1024]:
  page.set_viewport_size({'width':width,'height':1000})
  assert page.locator('#panelDiag').evaluate('(p)=>p.scrollWidth<=p.clientWidth'),f'Diagnostics overflow at {width}'
  for n in range(1,4):
   page.locator(f'#diagCan{n}Card').scroll_into_view_if_needed()
   assert page.locator(f'#diagCan{n}Rx').inner_text().startswith(f'{100*n} /')
   assert page.locator(f'#diagCan{n}Pins').inner_text().startswith(f'TWAI {n-1}')
 assert 'CAN A' not in page.locator('#panelDiag').inner_text()
 assert 'CAN B' not in page.locator('#panelDiag').inner_text()
 page.set_viewport_size({'width':430,'height':3100})
 page.locator('#panelDiag').evaluate('(p)=>p.scrollTop=0')
 page.screenshot(path=str(root/'artifacts/diagnostics-three-buses-simulated.png'))
 browser.close()
 print('PASS: actual Edge rendering at 360/430/1024 px; three aligned bus cards; simulated BODY/PARTY live and CHASSIS bus-off; no horizontal overflow')
 print('Page errors:',errors)
 print('PASS: Diagnostics / Recovery has three physical bus cards with independent RX values, correct pins and no overflow')
 if errors:raise SystemExit(1)

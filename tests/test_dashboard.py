from pathlib import Path
import re, subprocess, tempfile, gzip
from html.parser import HTMLParser
root=Path(__file__).resolve().parents[1]
html=(root/'dashboard_source.html').read_text(encoding='utf8')
class DOM(HTMLParser):
 def __init__(self): super().__init__(); self.ids=[]
 def handle_starttag(self,tag,attrs):
  for key,value in attrs:
   if key=='id':self.ids.append(value)
d=DOM();d.feed(html)
assert len(d.ids)==len(set(d.ids)), 'Duplicate DOM IDs'
for n in range(1,4):
 for key in [f'homeCan{n}Name',f'can{n}Badge',f'can{n}Traffic',f'can{n}Age']:assert key in d.ids,key
assert 'data-topology=' not in html
assert "pendingTopology=pendingProfile===1?1:2" in html
assert "canABadge" not in html and "canBBadge" not in html
scripts=re.findall(r'<script[^>]*>(.*?)</script>',html,re.S)
with tempfile.TemporaryDirectory() as temp:
 for n,script in enumerate(scripts):
  path=Path(temp)/f'script{n}.js';path.write_text(script,encoding='utf8')
  subprocess.run(['node','--check',str(path)],check=True)
embedded=(root/'index_html.h').read_text(encoding='utf8')
assert gzip.decompress(bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',embedded)))==(root/'dashboard_source.html').read_bytes()
# README legitimately links to the upstream project name; the legacy-transport
# scan applies to code, not to prose documentation.
for p in list(root.glob('*.h'))+list(root.glob('*.ino'))+[x for x in root.glob('*.md') if x.name != 'README.md']:
 text=p.read_text(encoding='utf-8-sig')
 assert not re.search(r'\bMCP2515\b|\bSPI\b',text,re.I),p.name
 assert not re.search(r't-?2-?can',text,re.I),p.name
 assert not re.search(r'\btwai_(?:transmit|receive|driver_install|driver_uninstall|start|stop)\(',text),p.name
print(f'PASS: {len(scripts)} JavaScript scripts parsed, physical DOM IDs, fixed profile selection, gzip/source identity, native API and removed transport scan')

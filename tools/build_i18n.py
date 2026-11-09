"""Build the Chinese UI layer for dashboard_source.html.

Reads tools/i18n_source.json and injects a self-contained runtime language
switcher before </body>. Idempotent.

Translation model
-----------------
1. ``strings`` — one authoritative Chinese entry per visible dashboard string.
   Exact match only. This is the primary path and covers the whole interface.
2. ``tokens`` — a short list of unambiguous UPPER-CASE state words that may be
   substituted inside strings the firmware composes at runtime (for example
   ``"CAN 2 · CHASSIS · BUS OFF"``). Substitution is case-sensitive and
   word-bounded, so ``Off-Highway`` never turns into ``关闭-Highway``.

There is deliberately no word-by-word translation: the previous version could
emit mixed text such as "AP 激活 only" or "已封禁 Car". Anything not covered by
an exact entry or a state token stays in English instead of being half
translated.

Run before tools/build_dashboard.py:
    python tools/build_i18n.py
    python tools/build_dashboard.py
"""
from __future__ import annotations

import html as html_mod
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HTML = ROOT / "dashboard_source.html"
SOURCE = ROOT / "tools" / "i18n_source.json"

START = "<!-- P4CAN I18N START -->"
END = "<!-- P4CAN I18N END -->"

# The upstream host test rejects these tokens anywhere in the HTML file.
FORBIDDEN = re.compile(r"\bMCP2515\b|\bSPI\b|t-?2-?can|twai_(?:transmit|receive|driver_install|driver_uninstall|start|stop)\(", re.I)

# Strings that are intentionally identical in both languages (product names,
# bus labels, units). They are declared in i18n_source.json -> identical.


def candidate_strings(html: str) -> set[str]:
    """Visible strings + attribute strings + literal UI strings in scripts."""
    out: set[str] = set()
    # Only real markup: script/style bodies contain comparison operators that
    # otherwise leak JS fragments into the dictionary.
    markup = re.sub(r"<script\b.*?</script>", "", html, flags=re.S | re.I)
    markup = re.sub(r"<style\b.*?</style>", "", markup, flags=re.S | re.I)
    for raw in re.findall(r">([^<>]+)<", markup):
        t = re.sub(r"\s+", " ", html_mod.unescape(raw)).strip()
        if t and re.search(r"[A-Za-z]{2}", t) and len(t) < 200:
            out.add(t)
    for attr in ("placeholder", "title", "aria-label"):
        for v in re.findall(attr + r'="([^"]+)"', markup):
            v = html_mod.unescape(v).strip()
            if v and re.search(r"[A-Za-z]{2}", v):
                out.add(v)
    # Literal UI strings inside scripts: no code punctuation allowed.
    for script in re.findall(r"<script\b.*?</script>", html, flags=re.S | re.I):
        for v in re.findall(r"['\"]([A-Za-z][A-Za-z0-9 /+&.,%\-\u00b7\u2026<>]{3,90})['\"]", script):
            v = html_mod.unescape(v).strip()
            if (len(v) > 3 and " " in v and not v.startswith("http")
                    and re.search(r"[A-Za-z]{3}", v)
                    and not re.search(r"\b(function|return|const|let|var|null|true|false|undefined)\b", v)):
                out.add(v)
    return out


def tidy(s: str) -> str:
    """Drop redundant spaces between CJK characters, keep spaces near Latin/digits."""
    prev = None
    while prev != s:
        prev = s
        s = re.sub(r"(?<=[\u4e00-\u9fff])[ \t]+(?=[\u4e00-\u9fff])", "", s)
    return s


def validate(cfg: dict) -> None:
    """Fail the build on empty, duplicated or obviously untranslated entries."""
    strings = cfg["strings"]
    keep = set(cfg.get("identical", []))
    problems = []
    for en, zh in strings.items():
        if not en.strip():
            problems.append("empty source key")
        if not str(zh).strip():
            problems.append(f"empty translation: {en!r}")
        if en == zh and en not in keep:
            problems.append(f"untranslated entry: {en!r}")
        if FORBIDDEN.search(en) or FORBIDDEN.search(str(zh)):
            problems.append(f"forbidden token: {en!r}")
    if problems:
        raise SystemExit("i18n source is invalid:\n  " + "\n  ".join(problems))


def coverage(cands: set[str], cfg: dict) -> tuple[list[str], list[str]]:
    """Split untranslated candidates into state-token resolvable and real gaps."""
    keep = set(cfg.get("identical", []))
    token_re = re.compile(
        r"(?<![A-Za-z0-9])(?:" + "|".join(map(re.escape, cfg["tokens"])) + r")(?![A-Za-z0-9])"
        if cfg["tokens"] else r"(?!x)x"
    )
    resolved, gaps = [], []
    for text in sorted(cands):
        if text in cfg["strings"]:
            continue
        if text in keep:
            resolved.append(text)
        elif token_re.search(text) and len(text) <= 120:
            resolved.append(text)
        else:
            gaps.append(text)
    return resolved, gaps


def js_literal(obj) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")


RUNTIME = r"""
(function(){
var D=%DICT%,T=%TOKENS%;
var KEY='p4can_lang',lang='zh';
try{lang=localStorage.getItem(KEY)||'zh';}catch(e){}
var ATTR=['placeholder','title','aria-label'];
var textMemo=new WeakMap(),attrMemo=new WeakMap();
function esc(s){return s.replace(/[.*+?^${}()|[\]\\]/g,'\\$&');}
function tidy(s){return s.replace(/([\u4e00-\u9fff])[ \t]+(?=[\u4e00-\u9fff])/g,'$1');}
var TK=Object.keys(T).sort(function(a,b){return b.length-a.length;});
var pats=TK.map(function(k){
  return {k:k,re:new RegExp('(^|[^A-Za-z0-9])'+esc(k)+'(?![A-Za-z0-9])','g')};
});
/* State tokens only: case-sensitive, word-bounded, never reorders the text. */
function tokens(s){
  var out=s,hit=false;
  for(var i=0;i<pats.length;i++){
    var p=pats[i];
    p.re.lastIndex=0;
    if(!p.re.test(out))continue;
    p.re.lastIndex=0;
    out=out.replace(p.re,function(m,a){return a+T[p.k];});
    hit=true;
  }
  return hit?tidy(out):null;
}
function tr(s){
  if(!s||typeof s!=='string')return s;
  var t=s.replace(/\s+/g,' ').trim();
  if(!t||!/[A-Za-z]/.test(t))return s;
  var v;
  if(Object.prototype.hasOwnProperty.call(D,t))v=D[t];
  else if(t.length<=120)v=tokens(t);
  if(!v||v===t)return s;
  var lead=(s.match(/^\s*/)||[''])[0],tail=(s.match(/\s*$/)||[''])[0];
  return lead+v+tail;
}
function applyNode(n){
  if(!n||n.nodeType!==3)return;
  var p=n.parentNode;
  if(!p||p.nodeName==='SCRIPT'||p.nodeName==='STYLE'||p.nodeName==='TEXTAREA')return;
  if(p.id==='p4canLangBtn')return;
  var cur=n.nodeValue,orig=textMemo.has(n)?textMemo.get(n):cur;
  if(textMemo.has(n)&&cur!==orig&&(lang==='en'||!/[\u4e00-\u9fff]/.test(cur)))orig=cur;
  textMemo.set(n,orig);
  var want=lang==='zh'?tr(orig):orig;
  if(cur!==want)n.nodeValue=want;
}
function restore(root){
  var tw=document.createTreeWalker(root,NodeFilter.SHOW_TEXT,null,false),n;
  while((n=tw.nextNode())){
    if(textMemo.has(n)&&n.nodeValue!==textMemo.get(n))n.nodeValue=textMemo.get(n);
  }
  var els=root.querySelectorAll('*');
  for(var i=0;i<els.length;i++){
    var el=els[i];
    if(!attrMemo.has(el))continue;
    var memo=attrMemo.get(el);
    for(var a in memo)if(el.getAttribute(a)!==memo[a])el.setAttribute(a,memo[a]);
  }
}
function applyAttrs(el){
  if(!el||el.nodeType!==1)return;
  for(var i=0;i<ATTR.length;i++){
    var a=ATTR[i],v=el.getAttribute(a);
    if(v==null)continue;
    var memo=attrMemo.has(el)?attrMemo.get(el):{};
    attrMemo.set(el,memo);
    if(!(a in memo))memo[a]=v;
    var want=lang==='zh'?tr(memo[a]):memo[a];
    if(v!==want)el.setAttribute(a,want);
  }
}
function apply(root){
  if(!root)return;
  if(root.nodeType===3){applyNode(root);return;}
  if(root.nodeType!==1)return;
  applyAttrs(root);
  var tw=document.createTreeWalker(root,NodeFilter.SHOW_TEXT,null,false),n;
  while((n=tw.nextNode()))applyNode(n);
  var els=root.querySelectorAll('*');
  for(var i=0;i<els.length;i++)applyAttrs(els[i]);
}
function mount(){
  var host=document.querySelector('.globaltop');
  if(!host||document.getElementById('p4canLangBtn'))return;
  var b=document.createElement('button');
  b.id='p4canLangBtn';b.type='button';b.setAttribute('aria-label','Switch language');
  b.textContent=lang==='zh'?'EN':'中';
  b.style.cssText='flex:0 0 auto;margin-left:8px;padding:3px 9px;font-size:11px;line-height:1.4;border-radius:999px;border:1px solid rgba(128,128,128,.45);background:transparent;color:inherit;cursor:pointer;';
  b.onclick=function(){setLang(lang==='zh'?'en':'zh');};
  host.appendChild(b);
}
function setLang(v){
  mo.disconnect();
  restore(document.documentElement);
  lang=v;
  try{localStorage.setItem(KEY,v);}catch(e){}
  document.documentElement.lang=v==='zh'?'zh-CN':'en';
  var b=document.getElementById('p4canLangBtn');
  if(b)b.textContent=v==='zh'?'EN':'中';
  apply(document.documentElement);
  mount();
  mo.observe(document.documentElement,OPTS);
}
var OPTS={childList:true,subtree:true,characterData:true,attributes:true,attributeFilter:ATTR};
var mo=new MutationObserver(function(list){
  mo.disconnect();
  for(var i=0;i<list.length;i++){
    var m=list[i];
    if(m.type==='characterData')applyNode(m.target);
    else if(m.type==='attributes')applyAttrs(m.target);
    else if(m.addedNodes&&m.addedNodes.length){
      for(var j=0;j<m.addedNodes.length;j++){
        var n=m.addedNodes[j];
        if(n.nodeType===3)applyNode(n);
        else if(n.nodeType===1)apply(n);
      }
    }
  }
  mount();
  mo.observe(document.documentElement,OPTS);
});
document.documentElement.lang=lang==='zh'?'zh-CN':'en';
apply(document.documentElement);
mount();
mo.observe(document.documentElement,OPTS);
})();
"""


def main() -> None:
    cfg = json.loads(SOURCE.read_text(encoding="utf8"))
    validate(cfg)

    html = HTML.read_text(encoding="utf8")
    if START in html and END in html:
        a, b = html.index(START), html.index(END) + len(END)
        html = html[:a] + html[b:]
        html = html.replace("\n\n\n</body>", "\n</body>")
    for old, new in cfg["static_fixes"].items():
        html = html.replace(old, new)

    dictionary = {en: tidy(zh) for en, zh in cfg["strings"].items()}
    resolved, gaps = coverage(candidate_strings(html), cfg)

    script = (
        RUNTIME.replace("%DICT%", js_literal(dictionary))
        .replace("%TOKENS%", js_literal(cfg["tokens"]))
        .strip()
    )
    if FORBIDDEN.search(script):
        raise SystemExit("i18n payload contains a token rejected by tests/test_dashboard.py")
    block = START + "\n<script>" + script + "</script>\n" + END + "\n"
    html = html.replace("</body>", block + "</body>")
    HTML.write_text(html, encoding="utf8")

    print(f"i18n: {len(dictionary)} exact entries, {len(cfg['tokens'])} state tokens, "
          f"{len(resolved)} covered strings, {len(gaps)} gaps, html={len(html.encode('utf8'))} bytes")
    if gaps:
        print("i18n gaps (kept in English on purpose):")
        for g in gaps:
            print("  -", g)


if __name__ == "__main__":
    main()

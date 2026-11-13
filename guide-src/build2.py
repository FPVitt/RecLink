import re, json, sys, os
from PIL import Image
from bs4 import BeautifulSoup, Comment
from svgs import SVG
ANN=json.load(open("shots/annot.json"))
PARTS={"guide":["part1.html","part2.html","part3.html","part4.html"],"qs":["qs.html"]}
FOOT={("guide","en"):"RecLink User Guide · firmware 0.1.0",("guide","it"):"RecLink Guida utente · firmware 0.1.0",
      ("qs","en"):"RecLink Quick Start · firmware 0.1.0",("qs","it"):"RecLink Guida rapida · firmware 0.1.0"}
NAMES={("guide","en","port"):"RecLink-User-Guide",("guide","en","land"):"RecLink-User-Guide-Landscape",
       ("qs","en","port"):"RecLink-Quick-Start",("qs","en","land"):"RecLink-Quick-Start-Landscape",
       ("guide","it","port"):"RecLink-Guida-Utente",("guide","it","land"):"RecLink-Guida-Utente-Orizzontale",
       ("qs","it","port"):"RecLink-Guida-Rapida",("qs","it","land"):"RecLink-Guida-Rapida-Orizzontale"}
UNIT_TAGS={"p","li","td","th","h1","h2","h3","text","div","figcaption"}
BLOCK={"p","ul","ol","li","div","table","tr","td","th","svg","figure","section","h1","h2","h3"}
ATTR=re.compile(r'(\w+)="([^"]*)"')
def letters(s): return len(re.sub(r'[^A-Za-zÀ-ÿ]','',re.sub(r'<[^>]+>','',s)))

def protect(src):
    shots=[]
    def f(m): shots.append(m.group(1)); return f"@@SHOT{len(shots)-1}@@"
    src=re.sub(r'\[\[shot (.*?)\]\]',f,src,flags=re.S)
    src=re.sub(r'\[\[svg:(\w+)\]\]',lambda m:SVG[m.group(1)](),src)
    return src,shots

def units_of(soup):
    out=[]
    for el in soup.find_all(True):
        if el.name in UNIT_TAGS or (el.name=="span" and "tag" in (el.get("class") or [])):
            if el.find(BLOCK): continue
            inner="".join(str(c) for c in el.contents).strip()
            if letters(inner)>=3: out.append((el,inner))
    return out

def collect(doc):
    src="".join(open(f).read() for f in PARTS[doc])
    src,shots=protect(src)
    soup=BeautifulSoup(src,"html.parser")
    seen={}; order=[]
    def add(s):
        if s not in seen: seen[s]=f"u{len(seen)+1}"; order.append(s)
    for el,inner in units_of(soup): add(inner)
    for a in shots:
        d=dict(ATTR.findall(a))
        if "cap" in d: add(d["cap"])
        for it in d.get("legend","").split(";"):
            if "|" in it: add(it.split("|",1)[1])
    return src,shots,soup,seen,order

def unified():
    if os.path.exists("units.json"):
        return {v:k for k,v in json.load(open("units.json")).items()}
    seen_all={}
    for doc in ("guide","qs"):
        _,_,_,_s,order=collect(doc)
        for s in order:
            if s not in seen_all: seen_all[s]=f"u{len(seen_all)+1}"
    return seen_all
PL=[(r'\b(le|Le|delle|Delle|dalle|alle|quattro|Quattro|due|Max|altre|più)\s+camera\b',r'\1 camere'),(r'camera (abbinate|trovate)',r'camere \1')]
def load_it():
    tr={}
    for f in ("it_a.txt","it_b.txt","it_c.txt"):
        for line in open(f):
            line=line.rstrip("\n")
            if "|||" in line:
                k,v=line.split("|||",1)
                for pat,rep in PL: v=re.sub(pat,rep,v)
                tr[k]=v
    extra={}
    for line in open("it_x.txt"):
        line=line.rstrip("\n")
        if "|||" in line:
            k,v=line.split("|||",1)
            for pat,rep in PL: v=re.sub(pat,rep,v)
            extra[k]=v
    return tr,extra
def build(doc,lang,orient):
    src,shots,soup,_s,_o=collect(doc)
    uni=unified()
    tr,extra=load_it() if lang=="it" else ({},{})
    miss=[]
    def T(s):
        if lang!="it": return s
        if s in extra: return extra[s]
        k=uni.get(s)
        if k in tr: return tr[k]
        miss.append(s); return s
    if lang=="it":
        for el,inner in units_of(soup):
            new=T(inner)
            if new!=inner:
                el.clear(); el.append(BeautifulSoup(new,"html.parser"))
    body=str(soup)
    if lang=="it":
        seen_m=set()
        for s in miss:
            t=re.sub(r'<[^>]+>','',s)
            if s not in seen_m and not s.startswith("@@SHOT") and re.search(r'[a-z]{3,}',t): seen_m.add(s); print("UNTRANSLATED:",s[:90])
    def shot(m):
        a=dict(ATTR.findall(shots[int(m.group(1))])); name=a["name"]; an=ANN.get(name)
        img=f"shots/{name}.png"; W=H=None; marks=[]; y0=0; y1=None
        if an:
            W,H=an["w"],an["h"]; marks=an["marks"]; anch={k["n"]:k["box"] for k in marks if isinstance(k["n"],str)}
            if "crop" in a:
                s,e=a["crop"].split(",")
                y0=0 if s=="-" else max(0,anch[s][1]-4)
                y1=H if e=="-" else (max(0,anch[e[:-1]][1]-3) if e.endswith("^") else min(H,anch[e][1]+anch[e][3]+6))
                im=Image.open(img); sc=im.width/W
                out=f"shots/_{name}_{abs(hash(a['crop']))%10000}.png"
                im.crop((0,int(y0*sc),im.width,int(y1*sc))).save(out); img=out
            else: y1=H
        ch=(y1-y0) if an else None
        badges=""
        if an:
            for k in marks:
                if not isinstance(k["n"],int): continue
                x,y,w,h=k["box"]
                if y<y0 or y>y1: continue
                bx=min(max(x-9,9),W-9)/W*100; by=(min(max(y-y0,10),ch-10))/ch*100
                badges+=f'<i class="bd" style="left:{bx:.2f}%;top:{by:.2f}%">{k["n"]}</i>'
        badges=""
        leg=""
        if a.get("legend"):
            items="".join(f'<li><i class="lgd"></i><span>{T(p.split("|",1)[1])}</span></li>' for p in a["legend"].split(";"))
            leg=f'<ol class="legend">{items}</ol>'
        cap=f'<figcaption>{T(a["cap"])}</figcaption>' if "cap" in a else ""
        style=f'width:{a["w"]}' if "w" in a else ""
        return f'<figure class="shot" style="{style}"><div class="imgwrap"><img src="{img}" alt="">{badges}</div>{cap}{leg}</figure>'
    body=re.sub(r'@@SHOT(\d+)@@',shot,body)
    def tbl(m):
        return m.group(0).replace('<div class="tbl">','<div class="tbl long">',1) if m.group(0).count('<tr')>9 else m.group(0)
    body=re.sub(r'<div class="tbl">.*?</table></div>',tbl,body,flags=re.S)
    css=open("guide.css").read()
    css=css.replace("RecLink User Guide · firmware 0.1.0",FOOT[(doc,lang)])
    if orient=="land": css+=open("land.css").read()
    if doc=="qs": css=re.sub(r'@page:first\{.*?\}\}','',css,flags=re.S); css=css.replace("@page:first{margin:0}","")
    css+=open("qs.css").read() if doc=="qs" else ""
    if doc=="qs" and orient=="land": css+=open("qsland.css").read()
    name=NAMES[(doc,lang,orient)]
    title={"en":"RecLink "+("Quick Start" if doc=="qs" else "User Guide"),"it":"RecLink "+("Guida rapida" if doc=="qs" else "Guida utente")}[lang]
    open(f"{name}.html","w").write(f'<!doctype html><html lang="{lang}"><head><meta charset="utf-8"><title>{title}</title><style>{css}</style></head><body>{body}</body></html>')
    return name

if __name__=="__main__":
    cmd=sys.argv[1]
    if cmd=="units":
        uni=unified()
        json.dump({v:k for k,v in uni.items()},open("units.json","w"),ensure_ascii=False,indent=0)
        print(len(uni),"units")
        with open("units.txt","w") as f:
            for k,v in uni.items(): f.write(f"{v}\t{k}\n")

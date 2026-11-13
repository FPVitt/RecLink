#!/usr/bin/env python3
"""Generate the GitHub Pages guide site into <repo>/docs from the guide sources.

    python build_web.py            # rebuild every page + copy assets

Needs: beautifulsoup4, pillow (see README.md). Reuses build2.py (the same
pipeline that produces the PDFs) so the web text, translations and screenshots
are always identical to the PDFs.
"""
import os, re, sys, shutil, hashlib, html

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
sys.path.insert(0, HERE)

from bs4 import BeautifulSoup, NavigableString, Tag      # noqa: E402
from PIL import Image                                       # noqa: E402
import build2                                               # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, ".."))
DOCS = os.path.join(ROOT, "docs")
ASSETS = os.path.join(DOCS, "assets")
IMG = os.path.join(ASSETS, "img")
PDFDIR = os.path.join(DOCS, "pdf")
REPO_URL = "https://github.com/FPVitt/RecLink"
FW = "0.2.0"

PAGES = {  # (lang, doc) -> file name
    ("en", "index"): "index.html", ("en", "qs"): "quick-start.html", ("en", "guide"): "user-guide.html",
    ("it", "index"): "index.html", ("it", "qs"): "guida-rapida.html", ("it", "guide"): "guida-utente.html",
}

S = {
    "en": dict(
        lang_name="English", site="RecLink guides", qs="Quick start", guide="User guide",
        toc="On this page", pdf="PDF", portrait="Portrait", landscape="Landscape",
        theme="Switch light / dark theme", menu="Contents", top="Back to top", skip="Skip to content",
        github="Source on GitHub", fwline=f"Firmware {FW} (pre-release) · Betaflight 2025.12+",
        pre_tag="Pre-release", pre=f"Firmware {FW} has been bench-tested only and is <b>not flight tested</b>. Do every step with the <b>propellers removed</b>.",
        home_sub="Set up and use the RecLink camera bridge for your Betaflight quad: wiring, pairing, recording control, OSD and the web configurator.",
        qs_t="Quick start", qs_d="From the box to your first recording in about 15 minutes. Seven short steps, in plain wording.",
        gd_t="User guide", gd_d="Every feature, setting and indicator, explained with diagrams and screenshots of the web configurator. 14 chapters and a reference.",
        read="Read online", dl="Download PDF", meta=["Betaflight 2025.12+", "GoPro HERO 9–13", "DJI Osmo (experimental)"],
        lang_label="Language", on_page="On this page", pdf_title="Download this guide as a PDF",
        which_h="Which one do I need?", which=["New to RecLink? Start with the <b>Quick start</b>.", "Want every detail, or something is not working? Open the <b>User guide</b>."],
        desc_qs="RecLink quick start: wire the board, set up Betaflight, pair your GoPro or DJI camera, record from arm or a radio switch.",
        desc_gd="RecLink user guide: wiring, Betaflight setup, the web configurator, recording control, zones, OSD, clock sync, troubleshooting.",
    ),
    "it": dict(
        lang_name="Italiano", site="Guide RecLink", qs="Guida rapida", guide="Guida utente",
        toc="In questa pagina", pdf="PDF", portrait="Verticale", landscape="Orizzontale",
        theme="Passa al tema chiaro / scuro", menu="Indice", top="Torna su", skip="Vai al contenuto",
        github="Codice su GitHub", fwline=f"Firmware {FW} (pre-release) · Betaflight 2025.12+",
        pre_tag="Pre-release", pre=f"Il firmware {FW} è stato provato solo a banco e <b>non è stato provato in volo</b>. Esegui ogni passaggio con le <b>eliche rimosse</b>.",
        home_sub="Configura e usa il bridge per action cam RecLink sul tuo quad Betaflight: cablaggio, abbinamento, controllo della registrazione, OSD e configuratore web.",
        qs_t="Guida rapida", qs_d="Dalla scatola alla prima registrazione in circa 15 minuti. Sette passaggi brevi, in parole semplici.",
        gd_t="Guida utente", gd_d="Ogni funzione, impostazione e indicatore, spiegati con diagrammi e schermate del configuratore web. 14 capitoli e un riferimento.",
        read="Leggi online", dl="Scarica il PDF", meta=["Betaflight 2025.12+", "GoPro HERO 9–13", "DJI Osmo (experimental)"],
        lang_label="Lingua", on_page="In questa pagina", pdf_title="Scarica questa guida in PDF",
        which_h="Quale mi serve?", which=["Sei nuovo con RecLink? Parti dalla <b>Guida rapida</b>.", "Vuoi ogni dettaglio, oppure qualcosa non funziona? Apri la <b>Guida utente</b>."],
        desc_qs="Guida rapida RecLink: cabla la scheda, configura Betaflight, abbina la GoPro o la DJI, registra con l'arm o con un interruttore della radio.",
        desc_gd="Guida utente RecLink: cablaggio, configurazione di Betaflight, configuratore web, controllo della registrazione, zone, OSD, sincronizzazione orologio, risoluzione dei problemi.",
    ),
}

MARK = ('<svg class="mark" viewBox="0 0 168 80" aria-hidden="true"><path fill="currentColor" d="M6 6h30v12H18v44h18v12H6zM132 6h30v68h-30V62h18V18h-18z"/>'
        '<circle fill="currentColor" cx="84" cy="40" r="21"/></svg>')

# --------------------------------------------------------------------- images
_img_cache = {}


def publish_image(src):
    """Copy shots/<x>.png into docs/assets/img as a lossless webp with a content hash in the name."""
    if src in _img_cache:
        return _img_cache[src]
    data = open(src, "rb").read()
    stem = os.path.splitext(os.path.basename(src))[0]
    stem = re.sub(r"^_", "", stem)
    stem = re.sub(r"_\d+$", "", stem)
    name = f"{stem}-{hashlib.md5(data).hexdigest()[:8]}.webp"
    out = os.path.join(IMG, name)
    im = Image.open(src).convert("RGBA" if Image.open(src).mode in ("RGBA", "LA", "P") else "RGB")
    if not os.path.exists(out):
        im.save(out, "WEBP", lossless=True, quality=100, method=6)
    _img_cache[src] = (name, im.width, im.height)
    return _img_cache[src]


# ------------------------------------------------------------------ extraction
def clean_text(el):
    return re.sub(r"\s+", " ", el.get_text(" ", strip=True)).strip()


def render_body(doc, lang):
    name = build2.build(doc, lang, "port")
    path = name + ".html"
    soup = BeautifulSoup(open(path, encoding="utf-8").read(), "html.parser")
    os.remove(path)
    return soup.body


XREF = {
    "en": re.compile(r"\b(chapters?)\s+(\d+)", re.I),
    "it": re.compile(r"\b(capitol[oi])\s+(\d+)", re.I),
}
STEPREF = {"en": re.compile(r"\b(step)\s+(\d)\b"), "it": re.compile(r"\b(passo|passaggio)\s+(\d)\b", re.I)}
SKIP_PARENTS = {"a", "code", "svg", "h1", "h2", "h3", "script", "style", "kbd"}


def link_refs(root, lang, doc, ids):
    pats = [(XREF[lang], "ch")] if doc == "guide" else [(STEPREF[lang], "step")]
    for node in list(root.find_all(string=True)):
        if not isinstance(node, NavigableString):
            continue
        par = node.parent
        if par is None or any(p.name in SKIP_PARENTS or "chno" in (p.get("class") or []) for p in [par, *par.parents] if isinstance(p, Tag)):
            continue
        text = str(node)
        for pat, pre in pats:
            def rep(m):
                tid = f"{pre}{m.group(2)}"
                return f'<a class="xref" href="#{tid}">{html.escape(m.group(0))}</a>' if tid in ids else html.escape(m.group(0))
            if pat.search(text):
                parts, last = [], 0
                for m in pat.finditer(text):
                    parts.append(html.escape(text[last:m.start()]))
                    parts.append(rep(m))
                    last = m.end()
                parts.append(html.escape(text[last:]))
                node.replace_with(BeautifulSoup("".join(parts), "html.parser"))
                break


def structure(body, doc, lang):
    """Split the PDF body into hero / intro / chapter groups and build the TOC."""
    hero, intro = {}, []
    groups = []
    for el in [c for c in body.children if isinstance(c, Tag)]:
        cls = el.get("class") or []
        if "cover" in cls:
            hero = dict(kicker=clean_text(el.select_one(".kick")), sub=clean_text(el.select_one("h2")),
                        meta=[clean_text(s) for s in el.select(".meta span")])
            continue
        if "qshead" in cls:
            lead = el.select_one("p.lead")
            hero = dict(kicker=clean_text(el.select_one(".brandrow .chno")), sub=clean_text(lead) if lead else "", meta=[])
            for t in (el.select_one(".brandrow"), lead):
                if t:
                    t.decompose()
            intro.append(el)
            continue
        chno = el.find("p", class_="chno", recursive=False)
        if chno and doc == "guide" and not groups and not (el.get("class") and "chapter" in cls):
            # the "Contents" section: keep its callouts, drop the printed table of contents
            for t in el.select(".toc"):
                t.decompose()
            chno.decompose()
            h = el.find("h1", class_="ch", recursive=False)
            if h:
                h.decompose()
            intro.append(el)
            continue
        if "chapter" in cls or chno:
            groups.append([el])
        elif groups:
            groups[-1].append(el)
        else:
            intro.append(el)

    out = []
    for secs in groups:
        first = secs[0]
        chno = first.find("p", class_="chno", recursive=False)
        label = clean_text(chno) if chno else ""
        m = re.search(r"(\d+|[A-Z])$", label)
        if doc == "qs":
            gid = f"step{m.group(1)}" if m else "ref"
        else:
            tok = m.group(1) if m else ""
            gid = f"ch{tok}" if tok.isdigit() else (f"app{tok}" if tok else "ref")
        h1 = first.find("h1", class_="ch", recursive=False)
        out.append(dict(id=gid, num=(m.group(1) if m else ""), label=label, title=clean_text(h1) if h1 else None, secs=secs, h2=[]))
    return hero, intro, out


def assemble(groups, lang, doc):
    """Flatten sections into <section id> blocks, give headings ids + anchors, return html and toc."""
    ids = {g["id"] for g in groups}
    toc = []
    parts = []
    for g in groups:
        sec = BeautifulSoup(f'<section class="chapter" id="{g["id"]}"></section>', "html.parser").section
        for s in g["secs"]:
            for ch in list(s.children):
                sec.append(ch.extract() if hasattr(ch, "extract") else ch)
        h1 = sec.find("h1", class_="ch")
        if h1:
            a = BeautifulSoup(f'<a class="anchor" href="#{g["id"]}" aria-label="link">#</a>', "html.parser").a
            h1.append(a)
        subs = []
        for j, h in enumerate(sec.find_all("h2", recursive=True), 1):
            if h.find_parent("svg") or h.find_parent(class_="callout"):
                continue
            hid = f'{g["id"]}-{j}'
            h["id"] = hid
            h.append(BeautifulSoup(f'<a class="anchor" href="#{hid}" aria-label="link">#</a>', "html.parser").a)
            subs.append((hid, clean_text(h).rstrip("#").strip()))
        if g["title"]:
            toc.append(dict(id=g["id"], num=g["num"], title=g["title"], subs=subs))
        else:  # reference block without an h1: its h2s are the entries
            for hid, t in subs:
                toc.append(dict(id=hid, num="", title=t, subs=[]))
        parts.append(sec)
    return parts, toc, ids


def process_images(root, rel):
    for fig in root.select("figure.shot"):
        img = fig.find("img")
        if not img:
            continue
        name, w, h = publish_image(img["src"])
        cap = fig.find("figcaption")
        img["src"] = f"{rel}assets/img/{name}"
        img["width"], img["height"] = str(w), str(h)
        img["alt"] = clean_text(cap) if cap else ""
        img["loading"], img["decoding"] = "lazy", "async"
        wrap = img.find_parent(class_="imgwrap")
        a = BeautifulSoup(f'<a class="zoom" href="{img["src"]}" data-caption="{html.escape(img["alt"], quote=True)}"></a>', "html.parser").a
        img.replace_with(a)
        a.append(img)
        if wrap is None:
            pass


# ------------------------------------------------------------------- templating
def pdf_links(lang, doc, rel):
    items = []
    for orient, key in (("port", "portrait"), ("land", "landscape")):
        fn = build2.NAMES[(doc, lang, orient)] + ".pdf"
        p = os.path.join(PDFDIR, fn)
        size = f" · {os.path.getsize(p) / 1048576:.1f} MB" if os.path.exists(p) else ""
        items.append((S[lang][key], f"{rel}pdf/{fn}", size))
    return items


def header(lang, doc, rel, alt_href):
    s = S[lang]
    other = "it" if lang == "en" else "en"
    tabs = ""
    for key in ("qs", "guide"):
        cur = ' aria-current="page"' if key == doc else ""
        tabs += f'<a href="{PAGES[(lang, key)]}"{cur}>{s[key]}</a>'
    langs = ""
    for code in ("en", "it"):
        href = alt_href if code == other else "#"
        cur = ' aria-current="true"' if code == lang else ""
        langs += f'<a href="{href}" hreflang="{code}" lang="{code}" data-lang="{code}"{cur}>{code.upper()}</a>'
    pdf = ""
    if doc in ("qs", "guide"):
        links = "".join(f'<a href="{h}" download>{t}<small>{sz}</small></a>' for t, h, sz in pdf_links(lang, doc, rel))
        pdf = f'<details class="menu pdf"><summary title="{s["pdf_title"]}">{s["pdf"]}</summary><div class="menu-body">{links}</div></details>'
    toc_btn = f'<button class="icon toc-btn" type="button" aria-controls="toc" aria-expanded="false" title="{s["menu"]}"><span>{s["menu"]}</span></button>' if doc != "index" else ""
    return f'''<header class="topbar"><div class="bar">
<a class="brand" href="index.html" aria-label="RecLink">{MARK}<span>RecLink</span></a>
<nav class="tabs" aria-label="Guides">{tabs}</nav>
<div class="tools">{pdf}<div class="seg" role="group" aria-label="{s["lang_label"]}">{langs}</div>
<button class="icon theme-btn" type="button" title="{s["theme"]}" aria-label="{s["theme"]}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3a9 9 0 1 0 9 9 7 7 0 0 1-9-9z" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"/></svg></button>{toc_btn}</div>
</div><div id="progress" aria-hidden="true"></div></header>'''


def toc_html(toc, lang):
    s = S[lang]
    li = ""
    for t in toc:
        num = f"<b>{html.escape(t['num'])}</b>" if t["num"] else ""
        subs = "".join(f'<li><a href="#{i}">{html.escape(x)}</a></li>' for i, x in t["subs"])
        sub = f'<ol class="sub">{subs}</ol>' if subs else ""
        li += f'<li data-id="{t["id"]}"><a href="#{t["id"]}">{num}<span>{html.escape(t["title"])}</span></a>{sub}</li>'
    return f'<aside class="sidebar" id="toc" aria-label="{s["toc"]}"><div class="side-in"><p class="toc-title">{s["toc"]}</p><ol>{li}</ol></div></aside>'


def page(lang, doc, title, desc, main_html, rel="../", toc="", alt_href="#"):
    s = S[lang]
    other = "it" if lang == "en" else "en"
    alts = (f'<link rel="alternate" hreflang="{lang}" href="{PAGES[(lang, doc)]}">'
            f'<link rel="alternate" hreflang="{other}" href="{alt_href}">')
    layout = "layout has-toc" if toc else "layout"
    return f'''<!doctype html>
<html lang="{lang}" data-theme="auto">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{html.escape(title)} · RecLink</title>
<meta name="description" content="{html.escape(desc, quote=True)}">
<meta name="theme-color" content="#0f1216">
<meta property="og:title" content="{html.escape(title, quote=True)} · RecLink">
<meta property="og:description" content="{html.escape(desc, quote=True)}">
<meta property="og:type" content="website">
<link rel="icon" type="image/svg+xml" href="{rel}assets/favicon.svg">
{alts}
<script>try{{var t=localStorage.getItem('rl-theme');if(t)document.documentElement.dataset.theme=t}}catch(e){{}}</script>
<link rel="stylesheet" href="{rel}assets/site.css">
</head>
<body data-doc="{doc}">
<a class="skip" href="#main">{s["skip"]}</a>
{header(lang, doc, rel, alt_href)}
<div class="{layout}">
{toc}
<main id="main" class="content">
{main_html}
</main>
</div>
<footer class="site-foot"><div><span>{s["fwline"]}</span><a href="{REPO_URL}">{s["github"]}</a><a href="#top" class="to-top">{s["top"]}</a></div></footer>
<script src="{rel}assets/site.js" defer></script>
</body>
</html>
'''


def build_doc(doc, lang):
    s = S[lang]
    body = render_body(doc, lang)
    hero, intro, groups = structure(body, doc, lang)
    parts, toc, ids = assemble(groups, lang, doc)
    intro_html = "".join(str(el) for el in intro)
    chapters = "".join(str(p) for p in parts)
    soup = BeautifulSoup(f'<div id="wrap"><div class="intro">{intro_html}</div>{chapters}</div>', "html.parser")
    link_refs(soup, lang, doc, ids)
    process_images(soup, "../")
    meta = "".join(f"<span>{html.escape(m)}</span>" for m in (hero.get("meta") or S[lang]["meta"]))
    hero_html = (f'<div class="hero"><p class="eyebrow">{html.escape(hero["kicker"])}</p><h1>RecLink</h1>'
                 f'<p class="sub">{html.escape(hero["sub"])}</p><div class="meta">{meta}</div></div>')
    main_html = hero_html + soup.find(id="wrap").decode_contents()
    other = "it" if lang == "en" else "en"
    alt_href = f"../{other}/{PAGES[(other, doc)]}"
    title = s["qs"] if doc == "qs" else s["guide"]
    desc = s["desc_qs"] if doc == "qs" else s["desc_gd"]
    return page(lang, doc, title, desc, main_html, toc=toc_html(toc, lang), alt_href=alt_href)


def build_index(lang):
    s = S[lang]
    other = "it" if lang == "en" else "en"
    cards = ""
    for doc, t, d in (("qs", s["qs_t"], s["qs_d"]), ("guide", s["gd_t"], s["gd_d"])):
        pdfs = "".join(f'<a class="btn ghost" href="../pdf/{build2.NAMES[(doc, lang, o)]}.pdf" download>{s["dl"]} · {s[k]}</a>'
                       for o, k in (("port", "portrait"), ("land", "landscape")))
        cards += (f'<article class="card"><h2>{t}</h2><p>{d}</p><div class="actions"><a class="btn" href="{PAGES[(lang, doc)]}">{s["read"]}</a></div>'
                  f'<div class="actions sub">{pdfs}</div></article>')
    fw_t, fw_d, fw_b, fw_n = (("Firmware download", "The latest RecLink firmware (OTA image). Upload it from the portal's System tab; the other slot keeps the previous build. Pre-release: bench-tested only.", "Download firmware", "All releases")
                              if lang == "en" else
                              ("Scarica il firmware", "L'ultimo firmware RecLink (immagine OTA). Caricalo dalla scheda Sistema del portale; l'altro slot conserva la versione precedente. Pre-release: provato solo a banco.", "Scarica il firmware", "Tutte le release"))
    rel = REPO_URL + "/releases"
    cards += (f'<article class="card" id="fw"><h2>{fw_t}</h2><p>{fw_d}</p><div class="actions">'
              f'<a class="btn" id="fw-c3" href="{rel}" data-asset="fc_cam_bridge-esp32c3.bin">{fw_b} · ESP32-C3</a>'
              f'<a class="btn ghost" id="fw-s3" href="{rel}" data-asset="fc_cam_bridge-esp32s3.bin">{fw_b} · ESP32-S3</a></div>'
              f'<div class="actions sub"><a class="btn ghost" href="{rel}">{fw_n}</a><span class="muted" id="fw-ver"></span></div></article>'
              '<script>(function(){fetch("https://api.github.com/repos/FPVitt/RecLink/releases?per_page=1").then(function(r){return r.json();})'
              '.then(function(a){var r=a&&a[0];if(!r)return;document.getElementById("fw-ver").textContent=r.tag_name+(r.prerelease?" (pre-release)":"");'
              '(r.assets||[]).forEach(function(x){var e=document.querySelector(\'[data-asset="\'+x.name+\'"]\');if(e)e.href=x.browser_download_url;});})'
              '.catch(function(){});})();</script>')
    which = "".join(f"<li>{w}</li>" for w in s["which"])
    meta = "".join(f"<span>{html.escape(m)}</span>" for m in s["meta"])
    main = (f'<div class="hero"><p class="eyebrow">{s["site"]}</p><h1>RecLink</h1><p class="sub">{s["home_sub"]}</p><div class="meta">{meta}</div></div>'
            f'<div class="callout danger"><span class="tag">{s["pre_tag"]}</span><p>{s["pre"]}</p></div>'
            f'<div class="cards">{cards}</div><h2 class="plain">{s["which_h"]}</h2><ul class="which">{which}</ul>')
    alt_href = f"../{other}/index.html"
    return page(lang, "index", s["site"], s["home_sub"], main, alt_href=alt_href)


ROOT_INDEX = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RecLink guides</title>
<meta name="theme-color" content="#0f1216">
<link rel="icon" type="image/svg+xml" href="assets/favicon.svg">
<link rel="alternate" hreflang="en" href="en/">
<link rel="alternate" hreflang="it" href="it/">
<script>
(function () {
  var l = null;
  try { l = localStorage.getItem('rl-lang'); } catch (e) {}
  if (l !== 'en' && l !== 'it') l = /^it\\b/i.test(navigator.language || '') ? 'it' : 'en';
  location.replace(l + '/' + location.hash);
})();
</script>
<style>
body{margin:0;min-height:100vh;display:grid;place-items:center;background:#0f1216;color:#f2f4f8;font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;text-align:center}
a{color:#2ea043;font-weight:650;font-size:1.2rem;margin:0 .8rem}
h1{font-size:2.4rem;margin:0 0 1rem}
</style>
</head>
<body><main><h1>RecLink</h1><p><a href="en/">English</a> <a href="it/">Italiano</a></p></main></body>
</html>
'''


def main():
    os.makedirs(IMG, exist_ok=True)
    for sub in ("en", "it"):
        os.makedirs(os.path.join(DOCS, sub), exist_ok=True)
    for f in ("site.css", "site.js", "favicon.svg"):
        shutil.copy(os.path.join(HERE, "web", f), os.path.join(ASSETS, f))
    # drop images from a previous build so renamed/unused ones do not pile up
    used = set()
    for lang in ("en", "it"):
        open(os.path.join(DOCS, lang, "index.html"), "w", encoding="utf-8").write(build_index(lang))
        for doc in ("qs", "guide"):
            out = build_doc(doc, lang)
            open(os.path.join(DOCS, lang, PAGES[(lang, doc)]), "w", encoding="utf-8").write(out)
            print("built", lang, doc, len(out) // 1024, "KB")
    used = {v[0] for v in _img_cache.values()}
    for f in os.listdir(IMG):
        if f not in used:
            os.remove(os.path.join(IMG, f))
    open(os.path.join(DOCS, "index.html"), "w", encoding="utf-8").write(ROOT_INDEX)
    open(os.path.join(DOCS, ".nojekyll"), "w").close()
    print("images:", len(used))


if __name__ == "__main__":
    main()

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
SITE = "https://fpvitt.github.io/RecLink/"
OG_IMG = SITE + "assets/og.png"


PAGES = {  # (lang, doc) -> file name
    ("en", "index"): "index.html", ("en", "qs"): "quick-start.html", ("en", "guide"): "user-guide.html", ("en", "board"): "the-board.html",
    ("it", "index"): "index.html", ("it", "qs"): "guida-rapida.html", ("it", "guide"): "guida-utente.html", ("it", "board"): "la-scheda.html",
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
        lang_label="Language", on_page="On this page", pdf_title="Download this guide as a PDF", gh="View on GitHub", gh_t="Source code", gh_d="Browse the firmware, read the spec, report an issue or download releases.",
        which_h="Which one do I need?", which=["New to RecLink? Start with the <b>Quick start</b>.", "Want every detail, or something is not working? Open the <b>User guide</b>.", "Curious about the hardware? See <b>The board</b> (in development)."],
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
        lang_label="Lingua", on_page="In questa pagina", pdf_title="Scarica questa guida in PDF", gh="Vedi su GitHub", gh_t="Codice sorgente", gh_d="Sfoglia il firmware, leggi le specifiche, segnala un problema o scarica le release.",
        which_h="Quale mi serve?", which=["Sei nuovo con RecLink? Parti dalla <b>Guida rapida</b>.", "Vuoi ogni dettaglio, oppure qualcosa non funziona? Apri la <b>Guida utente</b>.", "Curioso dell'hardware? Guarda <b>La scheda</b> (in sviluppo)."],
        desc_qs="Guida rapida RecLink: cabla la scheda, configura Betaflight, abbina la GoPro o la DJI, registra con l'arm o con un interruttore della radio.",
        desc_gd="Guida utente RecLink: cablaggio, configurazione di Betaflight, configuratore web, controllo della registrazione, zone, OSD, sincronizzazione orologio, risoluzione dei problemi.",
    ),
}

GH_URL = "https://github.com/FPVitt/RecLink"
GH_SVG = '<svg viewBox="0 0 16 16" aria-hidden="true" fill="currentColor"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27s1.36.09 2 .27c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8z"/></svg>'
ICONS = {
    "qs": '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M13 2 4 14h7l-1 8 9-12h-7z" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/></svg>',
    "guide": '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2zM4 19a2 2 0 0 0 2 2h13" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linejoin="round"/><path d="M9 8h6" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"/></svg>',
    "fw": '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3v12m0 0-4-4m4 4 4-4M5 19h14" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>',
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
    tabs += f'<a href="{PAGES[(lang, "board")]}"{' aria-current="page"' if doc == "board" else ""}>{"The board" if lang == "en" else "La scheda"}</a>'
    tabs += f'<a href="../flash/">{"Install firmware" if lang == "en" else "Installa firmware"}</a>'
    langs = ""
    for code in ("en", "it"):
        href = alt_href if code == other else "#"
        cur = ' aria-current="true"' if code == lang else ""
        langs += f'<a href="{href}" hreflang="{code}" lang="{code}" data-lang="{code}"{cur}>{code.upper()}</a>'
    pdf = ""
    if doc in ("qs", "guide"):
        links = "".join(f'<a href="{h}" download>{t}<small>{sz}</small></a>' for t, h, sz in pdf_links(lang, doc, rel))
        pdf = f'<details class="menu pdf"><summary title="{s["pdf_title"]}">{s["pdf"]}</summary><div class="menu-body">{links}</div></details>'
    toc_btn = f'<button class="icon toc-btn" type="button" aria-controls="toc" aria-expanded="false" title="{s["menu"]}"><span>{s["menu"]}</span></button>' if doc not in ("index", "board") else ""
    return f'''<header class="topbar"><div class="bar">
<a class="brand" href="index.html" aria-label="RecLink">{MARK}<span>RecLink</span></a>
<nav class="tabs" aria-label="Guides">{tabs}</nav>
<div class="tools">{pdf}<a class="icon gh-btn" href="{GH_URL}" target="_blank" rel="noopener" title="{s["gh"]}" aria-label="{s["gh"]}">{GH_SVG}</a><div class="seg" role="group" aria-label="{s["lang_label"]}">{langs}</div>
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


def page_url(lang, doc):
    return SITE + lang + "/" + ("" if doc == "index" else PAGES[(lang, doc)])


def seo_head(lang, doc, title, desc):
    import json
    other = "it" if lang == "en" else "en"
    url, other_url = page_url(lang, doc), page_url(other, doc)
    t = html.escape(title + " · RecLink", quote=True)
    d = html.escape(desc, quote=True)
    loc, oloc = ("en_US", "it_IT") if lang == "en" else ("it_IT", "en_US")
    if doc == "index":
        ld = {"@context": "https://schema.org", "@type": "SoftwareApplication", "name": "RecLink",
              "applicationCategory": "DeveloperApplication", "operatingSystem": "ESP32 (ESP-IDF)",
              "softwareVersion": FW, "description": desc, "url": url, "image": OG_IMG,
              "downloadUrl": REPO_URL + "/releases", "codeRepository": REPO_URL,
              "license": REPO_URL, "offers": {"@type": "Offer", "price": "0", "priceCurrency": "EUR"}}
    else:
        ld = {"@context": "https://schema.org", "@type": "TechArticle", "headline": title + " · RecLink",
              "description": desc, "inLanguage": lang, "url": url, "image": OG_IMG,
              "about": "RecLink, an ESP32 bridge between a Betaflight flight controller and a GoPro / DJI action camera",
              "isPartOf": {"@type": "WebSite", "name": "RecLink", "url": SITE}}
    return (f'<link rel="canonical" href="{url}">\n'
            f'<link rel="alternate" hreflang="{lang}" href="{url}">\n'
            f'<link rel="alternate" hreflang="{other}" href="{other_url}">\n'
            f'<link rel="alternate" hreflang="x-default" href="{page_url("en", doc)}">\n'
            f'<meta property="og:url" content="{url}">\n<meta property="og:site_name" content="RecLink">\n'
            f'<meta property="og:locale" content="{loc}"><meta property="og:locale:alternate" content="{oloc}">\n'
            f'<meta property="og:image" content="{OG_IMG}"><meta property="og:image:width" content="1200"><meta property="og:image:height" content="630">\n'
            f'<meta name="twitter:card" content="summary_large_image"><meta name="twitter:title" content="{t}">'
            f'<meta name="twitter:description" content="{d}"><meta name="twitter:image" content="{OG_IMG}">\n'
            f'<script type="application/ld+json">{json.dumps(ld, ensure_ascii=False)}</script>')


def make_og():
    from PIL import ImageDraw, ImageFont
    fp = "/System/Library/Fonts/Helvetica.ttc"
    out = os.path.join(ASSETS, "og.png")
    if not os.path.exists(fp):
        return
    im = Image.new("RGB", (1200, 630), "#0f1216")
    d = ImageDraw.Draw(im)
    g = "#2ea043"
    d.rectangle([0, 0, 1200, 10], fill=g)
    ox, oy, k = 80, 120, 2.4
    for x, y, w, h in ((6, 6, 30, 12), (6, 6, 12, 68), (6, 62, 30, 12), (132, 6, 30, 12), (150, 6, 12, 68), (132, 62, 30, 12)):
        d.rectangle([ox + x * k, oy + y * k, ox + (x + w) * k, oy + (y + h) * k], fill=g)
    cx, cy, r = ox + 84 * k, oy + 40 * k, 21 * k
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=g)
    big = ImageFont.truetype(fp, 120, index=1)
    mid = ImageFont.truetype(fp, 42, index=0)
    small = ImageFont.truetype(fp, 30, index=0)
    d.text((80, 345), "RecLink", font=big, fill="#f2f4f8")
    d.text((80, 485), "Betaflight to GoPro / DJI camera bridge", font=mid, fill="#a7b0be")
    d.text((80, 545), "Wiring, pairing, recording control, OSD and web configurator", font=small, fill="#a7b0be")
    im.save(out, optimize=True)


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
{seo_head(lang, doc, title, desc)}
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


BOARD = {
    "en": dict(
        title="The board", eyebrow="RecLink hardware",
        sub="The custom RecLink PCB: an ESP32-C3 with its own antenna, about the size of a fingernail, with four solder pads to the flight controller.",
        desc="The RecLink board: a custom 12 x 13 mm four-layer PCB with an ESP32-C3, chip antenna and four castellated solder pads (GND, 5V, TX, RX). Currently in development.",
        tag="In development",
        warn="The RecLink board is <b>currently in development</b>. What you see here is the work-in-progress design: dimensions, pad layout and components may still change, and it is not a finished product yet. Until then, any ESP32-C3 or ESP32-S3 dev board works (see the Quick start).",
        h3d="Explore the board", hint="Scroll to take the board apart. Drag to look around.", alt3d="The RecLink board",
        steps=[("The assembled board", "12 × 13 mm, one ESP32-C3 and a handful of tiny parts. Keep scrolling to take it apart."),
               ("Components", "The ESP32-C3 chip, ceramic antenna, crystal, status LED and about twenty tiny resistors and capacitors."),
               ("Top layer", "Solder mask, exposed pads, silkscreen and the first copper layer."),
               ("Inner layers", "Two copper layers hidden inside the board give the routing room it needs on such a small board."),
               ("Bottom layer", "The bottom side carries the four solder pads and the test points."),
               ("Four wires", "GND, 5V, TX and RX are all that connects the board to the flight controller.")],
        hphotos="Top and bottom", cap_top="Top side: ESP32-C3, chip antenna (top left), status LED and the supporting parts.",
        cap_bot="Bottom side: the four solder pads are labelled GND, 5V, TX and RX.",
        hfacts="At a glance",
        facts=[("Size", "about 12 × 13.3 mm"), ("Thickness", "1.0 mm"), ("Layers", "4 (top, two inner, bottom)"),
               ("Processor", "ESP32-C3 (RISC-V, Bluetooth LE 5, Wi-Fi), bare QFN-32 chip"),
               ("Antenna", "3.2 × 1.6 mm ceramic chip antenna"),
               ("Indicator", "one status LED (GPIO10, active-high)"),
               ("Test points", "three, for programming and debug"),
               ("Connection", "four castellated solder pads along one edge"),
               ("Firmware", "default board profile of the RecLink firmware")],
        hpads="The four pads", pads_intro="Solder four wires from the flight controller to the four pads along the board's edge, in the order printed on its bottom side (left to right when you look at that side):",
        padrows=[("GND", "FC ground", "Common ground."), ("5V", "FC 5V", "Power. The board regulates it down on its own."),
                 ("TX", "FC UART RX", "Board transmit (GPIO21, through a 470 Ω series resistor)."), ("RX", "FC UART TX", "Board receive (GPIO20).")],
        padhead=("Pad", "Connect to", "Notes"),
        pads_note="TX and RX are named from the board's side, so TX goes to the flight controller's RX. If you cross them by mistake, you can fix it in the web configurator (MSP tab, <i>Swap MSP TX/RX pins</i>) without re-soldering.",
        hnext="Next steps", next=['Wiring step by step: <a href="quick-start.html">Quick start</a>.', 'Put the firmware on the board, or update it over Wi-Fi: <a href="../flash/">Install firmware</a>.'],
        foot="Dimensions and part list are taken from the current design files and may change while the board is in development."),
    "it": dict(
        title="La scheda", eyebrow="Hardware RecLink",
        sub="Il PCB personalizzato RecLink: un ESP32-C3 con la sua antenna, grande quanto un'unghia, con quattro pad da saldare al flight controller.",
        desc="La scheda RecLink: un PCB personalizzato a quattro strati di 12 x 13 mm con ESP32-C3, antenna ceramica e quattro pad castellati (GND, 5V, TX, RX). Attualmente in sviluppo.",
        tag="In sviluppo",
        warn="La scheda RecLink è <b>attualmente in sviluppo</b>. Quello che vedi qui è il progetto in corso d'opera: dimensioni, disposizione dei pad e componenti possono ancora cambiare, e non è ancora un prodotto finito. Nel frattempo va bene qualsiasi scheda di sviluppo ESP32-C3 o ESP32-S3 (vedi la Guida rapida).",
        h3d="Esplora la scheda", hint="Scorri per smontare la scheda. Trascina per guardarla da altre angolazioni.", alt3d="La scheda RecLink",
        steps=[("La scheda assemblata", "12 × 13 mm, un ESP32-C3 e pochi componenti minuscoli. Continua a scorrere per smontarla."),
               ("Componenti", "Il chip ESP32-C3, l'antenna ceramica, il quarzo, il LED di stato e una ventina di resistenze e condensatori minuscoli."),
               ("Strato superiore", "Solder mask, pad scoperti, serigrafia e il primo strato di rame."),
               ("Strati interni", "Due strati di rame nascosti dentro la scheda danno alle piste lo spazio che serve su una scheda così piccola."),
               ("Strato inferiore", "Il lato inferiore porta i quattro pad da saldare e i punti di test."),
               ("Quattro fili", "GND, 5V, TX e RX sono tutto ciò che collega la scheda al flight controller.")],
        hphotos="Sopra e sotto", cap_top="Lato superiore: ESP32-C3, antenna ceramica (in alto a sinistra), LED di stato e i componenti di contorno.",
        cap_bot="Lato inferiore: i quattro pad da saldare sono marcati GND, 5V, TX e RX.",
        hfacts="In breve",
        facts=[("Dimensioni", "circa 12 × 13,3 mm"), ("Spessore", "1,0 mm"), ("Strati", "4 (sopra, due interni, sotto)"),
               ("Processore", "ESP32-C3 (RISC-V, Bluetooth LE 5, Wi-Fi), chip QFN-32 nudo"),
               ("Antenna", "antenna ceramica 3,2 × 1,6 mm"),
               ("Indicatore", "un LED di stato (GPIO10, attivo alto)"),
               ("Punti di test", "tre, per programmazione e debug"),
               ("Collegamento", "quattro pad castellati da saldare lungo un bordo"),
               ("Firmware", "profilo scheda predefinito del firmware RecLink")],
        hpads="I quattro pad", pads_intro="Salda quattro fili dal flight controller ai quattro pad lungo il bordo della scheda, nell'ordine stampato sul lato inferiore (da sinistra a destra guardando quel lato):",
        padrows=[("GND", "Massa FC", "Massa comune."), ("5V", "5V FC", "Alimentazione. La scheda la regola da sola."),
                 ("TX", "RX UART FC", "Trasmissione della scheda (GPIO21, con resistenza in serie da 470 Ω)."), ("RX", "TX UART FC", "Ricezione della scheda (GPIO20).")],
        padhead=("Pad", "Collega a", "Note"),
        pads_note="TX e RX sono indicati dal punto di vista della scheda, quindi TX va all'RX del flight controller. Se li incroci per errore, puoi correggere dal configuratore web (scheda MSP, <i>Inverti pin TX/RX MSP</i>) senza risaldare.",
        hnext="Prossimi passi", next=['Il cablaggio passo per passo: <a href="guida-rapida.html">Guida rapida</a>.', 'Installa il firmware sulla scheda, o aggiornalo via Wi-Fi: <a href="../flash/">Installa firmware</a>.'],
        foot="Dimensioni ed elenco componenti derivano dai file di progetto attuali e possono cambiare mentre la scheda è in sviluppo."),
}


def build_board(lang):
    b = BOARD[lang]
    other = "it" if lang == "en" else "en"
    A = "../assets/board/"
    facts = "".join(f"<tr><td>{html.escape(k)}</td><td>{html.escape(v)}</td></tr>" for k, v in b["facts"])
    pads = "".join(f'<tr><td><b>{a}</b></td><td>{html.escape(c)}</td><td class="wrap">{html.escape(n)}</td></tr>' for a, c, n in b["padrows"])
    th = "".join(f"<th>{h}</th>" for h in b["padhead"])
    nxt = "".join(f"<li>{n}</li>" for n in b["next"])
    n_steps = len(b["steps"])
    steps = "".join(f'<li class="exp-step" data-from="{[0, .12, .30, .50, .70, .88][k]}" data-to="{[.12, .30, .50, .70, .88, 1.01][k]}"><b>{html.escape(t)}</b><span>{html.escape(d)}</span></li>'
                    for k, (t, d) in enumerate(b["steps"]))
    main = f"""<div class="hero"><p class="eyebrow">{b["eyebrow"]}</p><h1>{b["title"]}</h1><p class="sub">{b["sub"]}</p></div>
<div class="callout warn"><span class="tag">{b["tag"]}</span><p>{b["warn"]}</p></div>
<section class="exp" id="exp" aria-label="{b["h3d"]}"><div class="exp-sticky"><canvas id="bc" role="img" aria-label="{b["alt3d"]}"></canvas><img class="exp-poster" src="{A}board-iso.webp" alt="{b["alt3d"]}">
<p class="exp-hint">{b["hint"]}</p>
<ol class="exp-steps">{steps}</ol><div class="exp-bar" aria-hidden="true"><i></i></div></div></section>
<h2 class="plain">{b["hphotos"]}</h2>
<div class="pair"><figure class="shot"><img src="{A}board-top.webp" alt="{html.escape(b["cap_top"], quote=True)}" width="800" height="870" loading="lazy"><figcaption>{b["cap_top"]}</figcaption></figure>
<figure class="shot"><img src="{A}board-bottom.webp" alt="{html.escape(b["cap_bot"], quote=True)}" width="800" height="870" loading="lazy"><figcaption>{b["cap_bot"]}</figcaption></figure></div>
<h2 class="plain">{b["hfacts"]}</h2><div class="tbl"><table><tbody>{facts}</tbody></table></div>
<h2 class="plain">{b["hpads"]}</h2><p>{b["pads_intro"]}</p><div class="tbl"><table><thead><tr>{th}</tr></thead><tbody>{pads}</tbody></table></div>
<p>{b["pads_note"]}</p>
<h2 class="plain">{b["hnext"]}</h2><ul class="which">{nxt}</ul>
<p class="fine">{b["foot"]}</p>
<script type="module" src="{A}board.js"></script>
"""
    return page(lang, "board", b["title"], b["desc"], main, alt_href=f"../{other}/{PAGES[(other, 'board')]}")


def build_index(lang):
    s = S[lang]
    other = "it" if lang == "en" else "en"
    cards = ""
    for doc, t, d in (("qs", s["qs_t"], s["qs_d"]), ("guide", s["gd_t"], s["gd_d"])):
        cards += (f'<article class="card"><span class="ico">{ICONS[doc]}</span><h2>{t}</h2><p>{d}</p><div class="actions"><a class="btn" href="{PAGES[(lang, doc)]}">{s["read"]}</a></div>'
                  '</article>')
    fw_t, fw_d, fw_b = (("Install the firmware", "Pick your board, choose a version, then flash it by USB or update it over Wi-Fi.", "Open the installer")
                        if lang == "en" else
                        ("Installa il firmware", "Scegli la scheda e la versione, poi caricala via USB o aggiornala via Wi-Fi.", "Apri l'installer"))
    cards += (f'<article class="card" id="fw"><span class="ico">{ICONS["fw"]}</span><h2>{fw_t}</h2><p>{fw_d}</p>'
              f'<div class="actions"><a class="btn" href="../flash/">{fw_b}</a></div></article>')
    which = "".join(f"<li>{w}</li>" for w in s["which"])
    meta = "".join(f"<span>{html.escape(m)}</span>" for m in s["meta"])
    main = (f'<div class="hero"><p class="eyebrow">{s["site"]}</p><h1>RecLink</h1><p class="sub">{s["home_sub"]}</p><div class="meta">{meta}</div><p class="gh-row"><a class="btn ghost" href="{GH_URL}" target="_blank" rel="noopener">{GH_SVG}{s["gh"]}</a></p></div>'
            f'<div class="callout danger"><span class="tag">{s["pre_tag"]}</span><p>{s["pre"]}</p></div>'
            f'<div class="cards">{cards}</div><h2 class="plain">{s["which_h"]}</h2><ul class="which">{which}</ul>')
    alt_href = f"../{other}/index.html"
    return page(lang, "index", s["site"], s["home_sub"], main, alt_href=alt_href)


ROOT_INDEX = '''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RecLink · Betaflight to GoPro / DJI camera bridge (ESP32)</title>
<meta name="description" content="RecLink is an ESP32 bridge between a Betaflight flight controller and a GoPro or DJI action camera: OSD status, record on arm, switch-controlled presets, GPS clock sync. Guides and firmware installer.">
<link rel="canonical" href="https://fpvitt.github.io/RecLink/">
<meta property="og:title" content="RecLink · Betaflight to GoPro / DJI camera bridge">
<meta property="og:description" content="ESP32 bridge between a Betaflight FC and a GoPro / DJI action camera. Guides and firmware installer.">
<meta property="og:type" content="website"><meta property="og:url" content="https://fpvitt.github.io/RecLink/">
<meta property="og:image" content="https://fpvitt.github.io/RecLink/assets/og.png">
<meta name="twitter:card" content="summary_large_image">
<meta name="theme-color" content="#0f1216">
<link rel="icon" type="image/svg+xml" href="assets/favicon.svg">
<link rel="alternate" hreflang="en" href="https://fpvitt.github.io/RecLink/en/">
<link rel="alternate" hreflang="it" href="https://fpvitt.github.io/RecLink/it/">
<link rel="alternate" hreflang="x-default" href="https://fpvitt.github.io/RecLink/en/">
<script>
(function () {
  var l = null;
  try { l = localStorage.getItem('rl-lang'); } catch (e) {}
  if (l !== 'en' && l !== 'it') l = (function(){var a=navigator.languages&&navigator.languages.length?navigator.languages:[navigator.language||''];for(var i=0;i<a.length;i++){if(/^it\\b/i.test(a[i]))return 'it';if(/^en\\b/i.test(a[i]))return 'en';}return 'en';})();
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
    shutil.rmtree(os.path.join(ASSETS, "board"), ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "web", "board"), os.path.join(ASSETS, "board"), dirs_exist_ok=True)
    # drop images from a previous build so renamed/unused ones do not pile up
    used = set()
    for lang in ("en", "it"):
        open(os.path.join(DOCS, lang, "index.html"), "w", encoding="utf-8").write(build_index(lang))
        open(os.path.join(DOCS, lang, PAGES[(lang, "board")]), "w", encoding="utf-8").write(build_board(lang))
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
    make_og()
    urls = [SITE] + [page_url(l, d) for d in ("index", "qs", "guide", "board") for l in ("en", "it")] + [SITE + "flash/"]
    def alt(u):
        for d in ("index", "qs", "guide", "board"):
            for l in ("en", "it"):
                if u == page_url(l, d):
                    return "".join(f'<xhtml:link rel="alternate" hreflang="{x}" href="{page_url(x, d)}"/>' for x in ("en", "it"))
        return ""
    sm = ('<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9" '
          'xmlns:xhtml="http://www.w3.org/1999/xhtml">\n' +
          "".join(f"<url><loc>{u}</loc>{alt(u)}</url>\n" for u in urls) + "</urlset>\n")
    open(os.path.join(DOCS, "sitemap.xml"), "w", encoding="utf-8").write(sm)
    open(os.path.join(DOCS, "robots.txt"), "w").write(f"User-agent: *\nAllow: /\n\nSitemap: {SITE}sitemap.xml\n")
    print("images:", len(used))


if __name__ == "__main__":
    main()

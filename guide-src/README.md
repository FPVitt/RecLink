# Guide sources

Everything needed to regenerate the RecLink guides:

| Output | Where | Made by |
|---|---|---|
| Website (GitHub Pages) | `../docs/` (`index.html`, `en/`, `it/`, `assets/`) | `build_web.py` |
| PDFs (portrait + landscape, EN + IT) | `../docs/pdf/` | `build2.py` + `render.py` |

The website is generated from the same HTML sources as the PDFs, so text, translations and
screenshots always match.

## Setup (once)

```bash
python3 -m venv .venv
.venv/bin/pip install beautifulsoup4 pillow playwright pypdfium2
```

The PDF render and the screenshot scripts drive the system Chrome (`channel="chrome"`), so no
browser download is needed.

## Rebuild the website

```bash
.venv/bin/python build_web.py
```

Writes `../docs/{index.html,en/,it/,assets/}` (screenshots are converted to lossless WebP).
Open `docs/index.html` through any static server to preview, for example
`python3 -m http.server -d ../docs 8123`.

## Rebuild the PDFs

```bash
.venv/bin/python - <<'EOF'
import build2
for doc in ("guide", "qs"):
    for lang in ("en", "it"):
        for orient in ("port", "land"):
            build2.build(doc, lang, orient)
EOF
.venv/bin/python render.py $(cat names.txt)
cp RecLink-*.pdf ../docs/pdf/
```

## Where things are

* `part1.html … part4.html` – the user guide (English). `qs.html` – the quick start (English).
  `[[shot name="…" cap="…" legend="…"]]` inserts a screenshot from `shots/` (see `build2.py`).
* `it_a.txt`, `it_b.txt`, `it_c.txt` – Italian translation, one `uN|||text` line per text unit.
  `units.json` freezes the English-text → `uN` mapping. If you change an English sentence, change it in
  `units.json` as well, or add the new sentence to `it_x.txt` (`English|||Italiano`, matched by exact text).
* `guide.css`, `land.css`, `qs.css`, `qsland.css` – PDF styling. `web/site.css`, `web/site.js` – website.
* `shots/*.png` + `shots/annot.json` – screenshots. Portal screenshots: `mock_server.py` serves the real
  `components/portal/www` with fake data, `shoot.py` / `shoot2.py` capture it. Betaflight Configurator
  screenshots: `bf_shoot.py` drives app.betaflight.com against a simulated flight controller.
* Screenshot text in the Italian guides stays English on purpose (the configurator UI is English).

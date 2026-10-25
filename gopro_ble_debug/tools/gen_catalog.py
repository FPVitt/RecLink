#!/usr/bin/env python3
"""gen_catalog.py -- build www/catalog.js (the web UI's Open GoPro decode tables).

Inputs, all from gopro/OpenGoPro:
  --proto DIR     protobuf/*.proto                        (main branch)
  --statuses F    open_gopro/models/constants/statuses.py (Python SDK)
  --settings-db F settings database embedded in the gh-pages BLE spec site, as
                  extracted JSON ({"W0": {setting_id: {...}}, ...}). Falls back
                  to the SDK settings.py enums when omitted.
  --settings F    open_gopro/models/constants/settings.py (fallback / names)

Output: one JS file defining window.OGP = {CAMS, STATUS, SETTING, PROTO, ENUM}.
Run from anywhere; regenerate after bumping the OpenGoPro revision.
"""
import argparse
import json
import re
from pathlib import Path


# ---- protobuf (proto2, no oneof/map; nested messages are flattened) ---------
def parse_protos(d):
    msgs, enums = {}, {}
    for f in sorted(Path(d).glob("*.proto")):
        src = re.sub(r"/\*.*?\*/", "", f.read_text(), flags=re.S)
        src = re.sub(r"//[^\n]*", "", src)
        stack = []  # (kind, name)
        for tok in re.finditer(r"(message|enum)\s+(\w+)\s*\{|\}|([^{};]+);", src):
            if tok.group(1):
                stack.append((tok.group(1), tok.group(2)))
                if tok.group(1) == "message":
                    msgs.setdefault(tok.group(2), [])
                else:
                    enums.setdefault(tok.group(2), {})
            elif tok.group(0) == "}":
                if stack:
                    stack.pop()
            else:
                stmt = " ".join(tok.group(3).split())
                if not stack:
                    continue
                kind, name = stack[-1]
                if kind == "enum":
                    m = re.match(r"(\w+)\s*=\s*(-?(?:0x[0-9a-fA-F]+|\d+))", stmt)
                    if m:
                        enums[name][int(m.group(2), 0)] = m.group(1)
                else:
                    m = re.match(r"(required|optional|repeated)\s+([\w.]+)\s+(\w+)\s*=\s*(\d+)", stmt)
                    if m:
                        label = {"required": "q", "optional": "o", "repeated": "r"}[m.group(1)]
                        msgs[name].append([int(m.group(4)), m.group(3), m.group(2).split(".")[-1], label])
    return msgs, enums


# ---- SDK GoProIntEnum classes ------------------------------------------------
def parse_py_enums(path):
    src = Path(path).read_text()
    out = {}
    for m in re.finditer(r"class (\w+)\(GoProIntEnum\):(.*?)(?=\nclass |\Z)", src, flags=re.S):
        vals = {}
        for v in re.finditer(r"^\s+([A-Z0-9_]+)\s*=\s*(-?\d+)", m.group(2), flags=re.M):
            vals.setdefault(int(v.group(2)), v.group(1))
        out[m.group(1)] = vals
    return out


def camel(name):
    return "".join(p.capitalize() for p in name.lower().split("_"))


def pretty(name):
    s = name.replace("NUM_", "").replace("_", " ").strip()
    return s[:1] + s[1:].lower() if s.isupper() else s


def id_table(enums, cls):
    tbl = {}
    for idv, nm in enums[cls].items():
        e = None
        for cand in (camel(nm), camel(nm).replace("Wi_fi", "Wi_Fi")):
            if cand in enums:
                e = enums[cand]
        ent = {"n": pretty(nm)}
        if e:
            ent["o"] = {str(k): pretty(v) for k, v in e.items()}
        tbl[str(idv)] = ent
    return tbl


# ---- unofficial names from camera gpControl dumps -------------------------
# The camera's own settings JSON (/gp/gpControl) names every setting it has,
# including the ~40 the Open GoPro spec omits (Protune ISO/shutter/EV/WB,
# streaming, setup). Dumps from KonradIT/goprowifihack (Apache-2.0). Accepts
# raw dumps or the trimmed extract written by --write-gpcontrol-extract.
def load_gpcontrol(paths):
    """-> (settings {id: {n, o{v: name}, src}}, statuses {id: {n, src}}), first file wins."""
    sets, stats = {}, {}
    for p in paths:
        d = json.loads(Path(p).read_text(encoding="utf-8-sig"))
        if isinstance(d.get("settings"), dict):          # already an extract
            for k, v in d["settings"].items():
                sets.setdefault(k, v)
            for k, v in d.get("status", {}).items():
                stats.setdefault(k, v)
            continue
        src = (d.get("info") or {}).get("model_name") or Path(p).stem
        for s in d.get("settings", []):
            sets.setdefault(str(s["id"]), {"n": s["display_name"], "src": src,
                                           "o": {str(o["id"]): o["display_name"] for o in s.get("options", [])}})
        for g in (d.get("status") or {}).get("groups", []):
            for f in g.get("fields", []):
                stats.setdefault(str(f["id"]), {"n": f["name"].replace("_", " ").capitalize(), "src": src})
    return sets, stats


def merge_gpcontrol(SETTING, STATUS, gsets, gstats):
    added_s = added_o = added_st = 0
    for sid, g in gsets.items():
        if sid in SETTING:
            # official setting: only fill option values the spec DB lacks
            opts = SETTING[sid].setdefault("o", {})
            for v, name in g["o"].items():
                if v not in opts:
                    opts[v] = name
                    added_o += 1
            continue
        wide = any(int(v) > 255 for v in g["o"])
        SETTING[sid] = {"n": g["n"], "f": "int32ub" if wide else "int8ub", "o": dict(g["o"]),
                        "u": 1, "src": g.get("src", "")}
        added_s += 1
    for sid, g in gstats.items():
        if sid not in STATUS:
            STATUS[sid] = {"n": g["n"], "u": 1, "src": g.get("src", "")}
            added_st += 1
    return added_s, added_o, added_st


# ---- Settings tab sections + clearer names for per-mode duplicates ---------
# Cameras keep separate Protune ISO/shutter per capture mode under different
# IDs, all called "ISO Max"/"Shutter" in the camera's own UI. Mode for video
# and the shutters is evident from each ID's option list (video shutter =
# fractions of the frame rate, night = seconds); photo vs multishot ISO pairs
# have identical options, so that split follows legacy ID numbering ("nt").
GROUPS = {
    "Video": [2, 3, 6, 13, 96, 102, 106, 108, 121, 135, 145, 148, 150, 156, 162, 167, 173, 176, 180, 182, 183, 184, 186,
              232, 234],
    "Protune (active preset)": [79, 114, 115, 116, 117, 118, 124, 139, 149, 160, 174],
    "Photo": [19, 24, 75, 105, 122, 125, 126, 133, 146, 147, 151, 171, 177, 181, 191, 227],
    "Time lapse & multishot": [5, 30, 31, 32, 37, 76, 111, 123, 128, 155, 157, 172, 179, 187, 192, 233],
    "Streaming & webcam": [41, 42, 43, 44, 45, 47, 48, 60, 61, 62, 64, 65, 66, 67],
}
GROUP_ORDER = list(GROUPS) + ["Setup & other"]
INFERRED = "mode inferred from legacy ID numbering (photo and multishot pairs have identical options)"
RENAME = {   # unofficial settings only: id -> (name, note)
    13: ("ISO Max (video)", ""), 102: ("ISO Min (video)", ""), 145: ("Shutter (video)", ""),
    24: ("ISO Max (photo)", INFERRED), 75: ("ISO Min (photo)", INFERRED),
    37: ("ISO Max (multishot / time lapse)", INFERRED), 76: ("ISO Min (multishot / time lapse)", INFERRED),
    146: ("Shutter (photo)", ""), 19: ("Shutter (night photo)", ""), 31: ("Shutter (night lapse)", ""),
    6: ("Looping Interval", ""), 126: ("Output (photo)", ""), 160: ("Bit Rate (alt, 160)", "second bit-rate setting; mode unknown"),
    41: ("Resolution (stream)", ""), 47: ("Resolution (stream, alt)", ""), 42: ("Frame Rate (stream)", ""),
    165: ("Horizon Lock (165)", "two Horizon Lock IDs exist; which mode each controls is unknown"),
    166: ("Horizon Lock (166)", "two Horizon Lock IDs exist; which mode each controls is unknown"),
}


def apply_groups(SETTING):
    where = {str(i): g for g, ids in GROUPS.items() for i in ids}
    for sid, d in SETTING.items():
        d["g"] = where.get(sid, "Setup & other")
        r = RENAME.get(int(sid))
        if r and d.get("u"):
            d["n"], note = r
            if note:
                d["nt"] = note


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proto", required=True)
    ap.add_argument("--statuses", required=True)
    ap.add_argument("--settings", required=True)
    ap.add_argument("--settings-db")
    ap.add_argument("--extra-status", action="append", default=[],
                    help="ID=Name for statuses documented on the spec site but not yet in the SDK")
    ap.add_argument("--gpcontrol", action="append", default=[],
                    help="camera gpControl JSON dump or extract (repeatable, first wins) -> unofficial names")
    ap.add_argument("--write-gpcontrol-extract", help="write the merged gpControl names to this file")
    ap.add_argument("--rev", default="")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()

    msgs, enums = parse_protos(a.proto)
    st = parse_py_enums(a.statuses)
    STATUS = id_table(st, "StatusId")
    for e in a.extra_status:
        k, v = e.split("=", 1)
        STATUS.setdefault(k, {"n": v})

    se = parse_py_enums(a.settings)
    SETTING = id_table(se, "SettingId")
    cams = []
    if a.settings_db:
        db = json.loads(Path(a.settings_db).read_text())
        if isinstance(db, list):
            db = db[0]
        db = db.get("W0", db)

        def ci(lst):
            out = []
            for c in lst or []:
                if c not in cams:
                    cams.append(c)
                out.append(cams.index(c))
            return out

        for sid, s in db.items():
            ent = {"n": s["name"], "f": s.get("format", ""), "c": ci(s.get("cameras"))}
            opts = {}
            for ov, o in (s.get("options") or {}).items():
                opts[ov] = [o["name"], ci(o.get("cameras"))]
            ent["o"] = opts
            SETTING[sid] = ent

    extra = ""
    if a.gpcontrol:
        gsets, gstats = load_gpcontrol(a.gpcontrol)
        if a.write_gpcontrol_extract:
            Path(a.write_gpcontrol_extract).write_text(json.dumps(
                {"_source": "camera /gp/gpControl dumps via github.com/KonradIT/goprowifihack (Apache-2.0)",
                 "settings": gsets, "status": gstats}, indent=0, sort_keys=True))
        s, o, st = merge_gpcontrol(SETTING, STATUS, gsets, gstats)
        extra = f"; gpControl: +{s} unofficial settings, +{o} option names, +{st} statuses"

    apply_groups(SETTING)

    data = {
        "REV": a.rev,
        "GROUPS": GROUP_ORDER,
        "CAMS": cams,
        "STATUS": STATUS,
        "SETTING": SETTING,
        "PROTO": msgs,
        "ENUM": {k: {str(n): s for n, s in v.items()} for k, v in enums.items()},
    }
    js = ("// GENERATED by tools/gen_catalog.py -- do not edit.\n"
          "// Open GoPro decode tables (statuses, settings, protobuf schema).\n"
          "window.OGP=" + json.dumps(data, separators=(",", ":")) + ";\n")
    Path(a.out).write_text(js)
    print(f"{a.out}: {len(STATUS)} statuses, {len(SETTING)} settings, "
          f"{len(msgs)} messages, {len(enums)} enums, {len(js)} bytes{extra}")


if __name__ == "__main__":
    main()

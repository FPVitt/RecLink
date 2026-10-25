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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proto", required=True)
    ap.add_argument("--statuses", required=True)
    ap.add_argument("--settings", required=True)
    ap.add_argument("--settings-db")
    ap.add_argument("--extra-status", action="append", default=[],
                    help="ID=Name for statuses documented on the spec site but not yet in the SDK")
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

    data = {
        "REV": a.rev,
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
          f"{len(msgs)} messages, {len(enums)} enums, {len(js)} bytes")


if __name__ == "__main__":
    main()

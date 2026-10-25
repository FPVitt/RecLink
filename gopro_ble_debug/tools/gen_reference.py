#!/usr/bin/env python3
"""gen_reference.py -- build docs/OPEN_GOPRO_BLE_REFERENCE.md.

Prose comes from docs/reference_head.md (hand-written). The appendices are
generated from the tables the debug firmware ships, so they can't drift:
www/catalog.js (statuses, settings, protobuf schema) and www/ops.js
(operations; dumped to JSON with any JS shell, see --ops).

    # ops.js -> JSON (macOS JavaScriptCore shown; node works the same)
    cd www && $JSC ../tools/dump_ops.js > /tmp/ops.json
    python3 tools/gen_reference.py --ops /tmp/ops.json
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_catalog():
    s = (ROOT / "www/catalog.js").read_text()
    return json.loads(s[s.index("=") + 1:s.rindex(";")])


def esc(x):
    return str(x).replace("|", "\\|").replace("\n", " ")


def hx(n):
    return f"0x{n:02X}"


def params(op):
    out = []
    for p in op.get("p") or []:
        s = f"`{p['n']}` {p['t']}"
        if p.get("o"):
            s += " {" + ", ".join(f"{k}={v}" for k, v in p["o"].items()) + "}"
        out.append(s)
    if op["k"] == "query":
        out.append(f"{op['ids']} IDs ({op.get('w', 1)}-byte), empty = all")
    if op.get("multi"):
        out.append("⚠️ multi-param encoding (§4.3)")
    return "<br>".join(out) or "—"


def appendix_ops(d, ops):
    L = ["## Appendix B — All BLE operations\n",
         "Kind: `cmd` TLV on GP-0072 · `setk`/`set` GP-0074 · `query` GP-0076 · `pb` protobuf `[feature][action][msg]` · "
         "`read`/`write` plain GATT. ⚠️ = destructive · 🔒 = request schema not public.\n",
         "Parameter wire format (TLV commands): `[cmd id]` then each parameter as `[len][value]`; see §4.3 for "
         "multi-parameter commands. Types: `u8`/`u16`/`u32` big-endian unsigned; `dt7` = `[year u16 BE][month][day]"
         "[hour][min][sec]` (spec example 2023-01-31 03:04:05 → `07 E7 01 1F 03 04 05`); `dt10` = `dt7` + "
         "`[utc_offset_min int16 BE][is_dst u8]` (… utc-02:00, dst on → `… FF 88 01`); `rawstr` = UTF-8 bytes, "
         "no header (plain GATT write).\n",
         "| Group | Operation | Kind | Char | ID | Parameters / body | Request → Response | Notes |",
         "|---|---|---|---|---|---|---|---|"]
    for o in ops["OPS"]:
        if o["k"] == "pb":
            ident = f"{hx(o['f'])}/{hx(o['a'])}"
            body = (f"`{o['req']}`" if o.get("req") else "—") + " → " + (f"`{o['rsp']}`" if o.get("rsp") else "—")
            par = "JSON per schema" if o.get("pub") is not False else "fields unknown"
            ch = o["c"]
        elif o["k"] in ("read", "write"):
            ident, body, par, ch = "—", "GATT " + o["k"], params(o), o["c"]
        else:
            ident = hx(o["id"])
            ch = {"cmd": "0072", "setk": "0074", "query": "0076"}[o["k"]]
            par = params(o)
            body = "TLV → `[id][status]…`" if o["k"] != "query" else "TLV → `[id][status][id len val]…`"
        flags = ("⚠️ " if o.get("danger") else "") + ("🔒 " if o.get("pub") is False else "")
        L.append(f"| {esc(o['g'])} | {flags}{esc(o['n'])} | {o['k']} | GP-{ch} | {ident} | {esc(par) if '<br>' not in par else par} | "
                 f"{body} | {esc(o.get('note', ''))} |")
    return "\n".join(L)


def appendix_pb(ops):
    L = ["## Appendix C — Protobuf messages the camera sends (feature/action → message)\n",
         "Use this table for the §4.1 protobuf-vs-TLV decision.\n",
         "| Feature/Action | Message |", "|---|---|"]
    for k, v in sorted(ops["PB_RX"].items()):
        f, a = k.split(":")
        L.append(f"| 0x{f.upper()}/0x{a.upper()} | `{v}` |")
    return "\n".join(L)


def opt_text(v):
    return v[0] if isinstance(v, list) else v


def appendix_settings(d):
    cams = d["CAMS"]
    L = ["## Appendix D — Settings\n",
         "Grouped like the debug UI's Settings tab. **Source**: `spec` = Open GoPro spec DB, `GPC (model)` = "
         "unofficial, from that camera's gpControl dump. **Width** = value bytes on the wire (§4.4). "
         "Camera support lists come from the spec DB; an option can still be unavailable in the current mode "
         "(query capabilities, §6.4). `?` in a name means its mode is inferred (§6.2).\n"]
    by = {}
    for sid, s in d["SETTING"].items():
        by.setdefault(s.get("g", "Setup & other"), []).append((int(sid), s))
    for g in d.get("GROUPS") or by:
        if g not in by:
            continue
        L += [f"### {g}\n", "| ID | Setting | Source | Width | Options (value = name) |", "|---|---|---|---|---|"]
        for sid, s in sorted(by[g]):
            src = f"GPC ({s.get('src', '')})" if s.get("u") else "spec"
            width = {"int64ub": 8, "int32ub": 4}.get(s.get("f"), 1)
            opts = ", ".join(f"{k}={esc(opt_text(v))}" for k, v in sorted((s.get("o") or {}).items(), key=lambda kv: int(kv[0])))
            name = esc(s["n"]) + (" ?" if s.get("nt") else "")
            extra = ""
            if s.get("c") and len(s["c"]) < len(cams):
                extra = "<br><sub>models: " + ", ".join(cams[i] for i in s["c"]) + "</sub>"
            if s.get("nt"):
                extra += f"<br><sub>{esc(s['nt'])}</sub>"
            L.append(f"| {sid} | {name}{extra} | {src} | {width} | {opts or '—'} |")
        L.append("")
    return "\n".join(L)


def appendix_status(d):
    L = ["## Appendix E — Statuses\n",
         "Values are big-endian; widths vary (§4.6). `-1` arrives as `0xFF`.\n",
         "| ID | Status | Source | Values |", "|---|---|---|---|"]
    for sid, s in sorted(d["STATUS"].items(), key=lambda kv: int(kv[0])):
        src = f"GPC ({s.get('src', '')})" if s.get("u") else "spec"
        vals = ", ".join(f"{k}={esc(v)}" for k, v in (s.get("o") or {}).items())
        L.append(f"| {sid} | {esc(s['n'])} | {src} | {vals or '—'} |")
    return "\n".join(L)


def appendix_proto(d):
    lab = {"q": "required", "o": "optional", "r": "repeated"}
    L = ["## Appendix F — Protobuf schema (public `.proto` files, proto2)\n",
         "Field lines: `number name label type`. Enums are listed below the messages.\n"]
    for m, fields in sorted(d["PROTO"].items()):
        L.append(f"**`{m}`**" + ("  _(empty)_" if not fields else ""))
        if fields:
            L.append("```")
            L += [f"{no:>3}  {name}  {lab[l]}  {t}" for no, name, t, l in fields]
            L.append("```")
    L.append("\n### Enums\n")
    L += ["| Enum | Values |", "|---|---|"]
    for e, vals in sorted(d["ENUM"].items()):
        L.append(f"| `{e}` | " + ", ".join(f"{v}={k}" for k, v in sorted(vals.items(), key=lambda kv: int(kv[0]))) + " |")
    return "\n".join(L)


def appendix_chars(ops):
    L = ["## Appendix A — GoPro characteristic short names\n", "| GP-XXXX | Name | Response on |", "|---|---|---|"]
    for k, v in ops["CH"].items():
        L.append(f"| GP-{k} | {v} | {('GP-' + ops['RSP_OF'][k]) if k in ops['RSP_OF'] else ''} |")
    L.append(f"\nPacketized (Open GoPro headers) on receive: {', '.join('GP-' + c for c in ops['PACKETIZED'])}.")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ops", required=True, help="ops.js dumped as JSON")
    ap.add_argument("-o", "--out", default=str(ROOT / "docs/OPEN_GOPRO_BLE_REFERENCE.md"))
    a = ap.parse_args()
    d = load_catalog()
    ops = json.loads(Path(a.ops).read_text())
    head = (ROOT / "docs/reference_head.md").read_text()
    doc = "\n\n".join([head.rstrip(), "---", appendix_chars(ops), appendix_ops(d, ops), appendix_pb(ops),
                       appendix_settings(d), appendix_status(d), appendix_proto(d)]) + "\n"
    Path(a.out).write_text(doc)
    print(f"{a.out}: {len(doc)} bytes, {len(ops['OPS'])} operations, {len(d['SETTING'])} settings, "
          f"{len(d['STATUS'])} statuses, {len(d['PROTO'])} messages")


if __name__ == "__main__":
    main()

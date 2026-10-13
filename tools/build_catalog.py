#!/usr/bin/env python3
"""Build data/a32nx_catalog.json from the FlyByWire documentation.

Sources (fetched from GitHub unless local paths are given):
  * flybywiresim/docs      docs/aircraft/a32nx/a32nx-api/a32nx-flightdeck-api.md
  * flybywiresim/aircraft  fbw-a32nx/docs/a320-events.md

Usage:
  tools/build_catalog.py [--flightdeck FILE] [--events FILE] [-o data/a32nx_catalog.json]
"""
import argparse
import json
import re
import sys
import urllib.request

FLIGHTDECK_URL = ("https://raw.githubusercontent.com/flybywiresim/docs/primary/"
                  "docs/aircraft/a32nx/a32nx-api/a32nx-flightdeck-api.md")
EVENTS_URL = ("https://raw.githubusercontent.com/flybywiresim/aircraft/master/"
              "fbw-a32nx/docs/a320-events.md")

EVENT_TYPES = {"SIMCONNECT EVENT": "sim_event", "MSFS EVENT": "sim_event",
               "CUSTOM EVENT": "sim_event", "HTML EVENT (AKA H EVENT)": "h_event"}
VAR_TYPES = {"CUSTOM LVAR": "lvar", "SIMCONNECT LVAR": "lvar",
             "MSFS VAR": "avar", "SIMCONNECT VAR": "avar"}


def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read().decode("utf-8")


def clean(cell):
    cell = cell.strip().replace("&#124;", "|").replace("\\|", "|")
    cell = re.sub(r"\{[:. ][^}]*\}", "", cell)      # mkdocs attr lists, keep {index}
    cell = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", cell)  # links
    return cell.strip()


def parse_flightdeck(md):
    entries = []
    panel = section = ""
    function = ""
    for raw in md.splitlines():
        line = raw.rstrip()
        if line.startswith("## "):
            panel, section, function = line[3:].strip(), "", ""
            continue
        if line.startswith("### "):
            section, function = line[4:].strip(), ""
            continue
        if not line.startswith("|"):
            continue
        cells = [clean(c) for c in line.strip().strip("|").split("|")]
        if len(cells) < 5:
            continue
        if cells[0].lower() == "function" or set(cells[0]) <= set(":-"):
            continue
        func, api, values, rw, typ = cells[:5]
        remark = cells[5] if len(cells) > 5 else ""
        if func:
            function = func
        if not api:
            continue
        kind = EVENT_TYPES.get(typ.upper())
        if kind is None:
            vk = VAR_TYPES.get(typ.upper())
            if vk is None or "W" not in rw.upper():
                continue
            kind = vk
        name = api
        value_hint = None
        if kind == "h_event":
            name = name[2:] if name.startswith("H:") else name
        elif kind == "sim_event":
            m = re.fullmatch(r"([A-Za-z0-9_.]+):(\d+)", name)
            if m:
                name, value_hint = m.group(1), int(m.group(2))
        variants = [(name, api)]
        if "{index}" in name:
            variants = [(name.replace("{index}", str(i)), api.replace("{index}", str(i))) for i in (1, 2)]
        for vname, vapi in variants:
            entries.append({
                "panel": panel, "section": section, "function": function,
                "api": vapi, "name": vname, "kind": kind, "values": values,
                "rw": rw, "type": typ, "remark": remark,
                **({"value": value_hint} if value_hint is not None else {}),
            })
    return entries


def parse_events(md):
    """Return {event_name: description} from a320-events.md."""
    out = {}
    current = None
    for raw in md.splitlines():
        m = re.match(r"^- (A32NX\.[A-Za-z0-9_{}]+)\s*$", raw)
        if m:
            current = m.group(1)
            out[current] = []
            continue
        m = re.match(r"^\s{2,}- (.*)$", raw)
        if m and current:
            out[current].append(m.group(1).replace("**", "").strip())
            continue
        if raw.startswith("#"):
            current = None
    expanded = {}
    for name, lines in out.items():
        desc = " ".join(lines)
        if "{index}" in name:
            for i in (1, 2):
                expanded[name.replace("{index}", str(i))] = desc.replace("{index}", str(i))
        else:
            expanded[name] = desc
    return expanded


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--flightdeck")
    ap.add_argument("--events")
    ap.add_argument("-o", "--output", default="data/a32nx_catalog.json")
    args = ap.parse_args()
    fd_md = open(args.flightdeck, encoding="utf-8").read() if args.flightdeck else fetch(FLIGHTDECK_URL)
    ev_md = open(args.events, encoding="utf-8").read() if args.events else fetch(EVENTS_URL)

    entries = parse_flightdeck(fd_md)
    descriptions = parse_events(ev_md)
    known = {e["name"] for e in entries}
    for e in entries:
        d = descriptions.get(e["name"])
        if d:
            e["description"] = d
    for name, desc in descriptions.items():
        if name in known or "{" in name:
            continue
        if "internal" in desc.lower() or "not to be used" in desc.lower():
            continue
        entries.append({"panel": "Custom events", "section": "Other A32NX custom events",
                        "function": name.split(".", 1)[1].replace("_", " ").title(),
                        "api": name, "name": name, "kind": "sim_event", "values": "",
                        "rw": "", "type": "Custom EVENT", "remark": "", "description": desc})

    doc = {"aircraft": "A32NX", "sources": [FLIGHTDECK_URL, EVENTS_URL], "entries": entries}
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)
        f.write("\n")
    kinds = {}
    for e in entries:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    print(f"wrote {len(entries)} entries to {args.output}: {kinds}", file=sys.stderr)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build data/<aircraft>_catalog.json from the FlyByWire documentation.

Sources (fetched from GitHub unless local paths are given):
  * flybywiresim/docs      docs/aircraft/{a32nx,a380x}/<aircraft>-api/<aircraft>-flight(-)deck-api.md
  * flybywiresim/aircraft  fbw-a32nx/docs/a320-events.md  (A32NX custom event descriptions; the A380X
                           shares the A32NX.* FCU/EFIS events, so its entries borrow these descriptions)

Usage:
  tools/build_catalog.py [--aircraft A32NX|A380X] [--flightdeck FILE] [--events FILE] [-o FILE]
  tools/build_catalog.py --all            # regenerates every catalog in data/
"""
import argparse
import json
import re
import sys
import urllib.request

DOCS = "https://raw.githubusercontent.com/flybywiresim/docs/primary/docs/aircraft"
FLIGHTDECK_URL = f"{DOCS}/a32nx/a32nx-api/a32nx-flightdeck-api.md"
AIRCRAFT = {
    "A32NX": {"title": "FlyByWire A320neo (A32NX)", "flightdeck": FLIGHTDECK_URL, "output": "data/a32nx_catalog.json",
              "extra_events": True, "recipes": True},
    "A380X": {"title": "FlyByWire A380-842 (A380X)", "flightdeck": f"{DOCS}/a380x/a380x-api/a380x-flight-deck-api.md",
              "output": "data/a380x_catalog.json", "extra_events": False,
              # The stable A380X build (fs2020-master) drives its baro knob through its own H events
              # (A380X_EFIS_CP_BARO_PUSH/PULL_<n>) and has neither the A32NX.FCU_EFIS_*_BARO_* events nor the
              # A32NX_FCU_EFIS_*_DISPLAY_BARO_MODE variable the development-branch docs list, so the A32NX recipes
              # would silently misbehave there. No recipes until the A380X ones can be verified on the aircraft.
              "recipes": False},
}
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
    cell = re.sub(r"<br\s*/?>", "", cell)                # table-cell line breaks inside names
    cell = cell.replace("{NUM]", "{NUM}")                 # typo in the A380X tables
    cell = re.sub(r"\{[:. ][^}]*\}", "", cell)      # mkdocs attr lists, keep {index}
    cell = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", cell)  # links
    return cell.strip()


SKIPPED = []  # api strings with placeholders we could not expand (reported by main)

# Placeholder conventions of the FlyByWire tables: {SIDE} L/R, {NUM} 1..4 ("first ... fourth"), {index} 1/2.
# Section-specific caps keep numbers the aircraft does not have out of the catalog.
NUM_MAX_BY_SECTION = {"ADIRS Panel": 3}
NUM_MAX_BY_FUNCTION = (("PACK", 3),)


ALT = re.compile(r"\{([^{}|]+(?:\|[^{}|]+)+)\}")  # {1|2} style alternatives


def expand_placeholders(name, api, function, values, remark, section):
    """Yields one dict per concrete combination of the placeholders present in name/api/values."""
    import itertools
    text = name + " " + api + " " + values
    axes = []  # (token, [values])
    for token, vals in (("{NUM}", None), ("{SIDE}", ["L", "R"]), ("{side}", ["L", "R"]), ("{index}", ["1", "2"])):
        if token in text:
            if vals is None:
                top = NUM_MAX_BY_SECTION.get(section, 4)
                for prefix, cap in NUM_MAX_BY_FUNCTION:
                    if function.upper().startswith(prefix):
                        top = min(top, cap)
                vals = [str(i) for i in range(1, top + 1)]
            axes.append((token, vals))
    for m in sorted({m.group(0): m.group(1).split("|") for m in ALT.finditer(text)}.items()):
        axes.append((m[0], m[1]))
    if not axes:
        return [{"name": name, "api": api, "function": function, "values": values, "remark": remark}]
    out = []
    for combo in itertools.product(*[v for _, v in axes]):
        mapping = {token: val for (token, _), val in zip(axes, combo)}

        def sub(t):
            for token, val in mapping.items():
                t = t.replace(token, val)
            return t
        d = {"name": sub(name), "api": sub(api), "function": sub(function), "values": values, "remark": sub(remark)}
        # an event whose Values cell is the {NUM} placeholder takes that number as its parameter
        if values == "{NUM}" and "{NUM}" in mapping:
            d["value"] = int(mapping["{NUM}"])
            d["values"] = "parameter = unit number"
        out.append(d)
    return out


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
        if cells[0].lower() == "function" or (cells[0] and set(cells[0]) <= set(":-")):
            continue  # header / separator rows (an empty Function cell is a continuation row, keep it)
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
        skipped_here = False
        for sub in expand_placeholders(name, api, function, values, remark, section):
            if "{" in sub["api"] or "{" in sub["name"]:
                skipped_here = True  # token we cannot expand (e.g. {KEY}, {DECK}): not a usable entry
                continue
            v = value_hint if value_hint is not None else sub.get("value")
            entries.append({
                "panel": panel, "section": section,
                # a Function cell may keep a {TOKEN} the API cell does not use (doc inconsistency): drop it
                "function": re.sub(r"\s+", " ", re.sub(r"\{[A-Za-z]+\}", "", sub["function"])).strip(),
                "api": sub["api"], "name": sub["name"], "kind": kind, "values": sub["values"],
                "rw": rw, "type": typ, "remark": sub["remark"],
                **({"value": v} if v is not None else {}),
            })
        if skipped_here:
            SKIPPED.append(api)
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
        desc = " ".join(l for l in lines if not l.startswith("side = ") and not l.startswith("index = "))
        if "{index}" in name:
            for i in (1, 2):
                expanded[name.replace("{index}", str(i))] = desc.replace("{index}", str(i))
        elif "{side}" in name:
            for sd in ("L", "R"):
                expanded[name.replace("{side}", sd)] = desc + (" (captain side)" if sd == "L" else " (first officer side)")
        else:
            expanded[name] = desc
    return expanded


def build(aircraft, flightdeck_path=None, events_path=None, output=None):
    cfg = AIRCRAFT[aircraft]
    fd_md = open(flightdeck_path, encoding="utf-8").read() if flightdeck_path else fetch(cfg["flightdeck"])
    ev_md = open(events_path, encoding="utf-8").read() if events_path else fetch(EVENTS_URL)

    del SKIPPED[:]
    entries = parse_flightdeck(fd_md)
    if SKIPPED:
        print(f"{aircraft}: skipped {len(SKIPPED)} row(s) with placeholders that cannot be expanded: {', '.join(sorted(set(SKIPPED))[:6])}", file=sys.stderr)
    descriptions = parse_events(ev_md)
    known = {e["name"] for e in entries}
    for e in entries:
        d = descriptions.get(e["name"])
        if d:
            e["description"] = d
    if cfg["extra_events"]:
        # Custom events documented only in the A32NX events file (no row in the flight deck tables).
        for name, desc in descriptions.items():
            if name in known or "{" in name:
                continue
            if "internal" in desc.lower() or "not to be used" in desc.lower():
                continue
            entries.append({"panel": "Custom events", "section": "Other A32NX custom events",
                            "function": name.split(".", 1)[1].replace("_", " ").title(),
                            "api": name, "name": name, "kind": "sim_event", "values": "",
                            "rw": "", "type": "Custom EVENT", "remark": "", "description": desc})

    def baro_toggle(side):
        # A32NX_FCU_EFIS_<side>_DISPLAY_BARO_MODE: 0 = STD, 1 = QNH, 2 = QFE (same on A32NX and A380X).
        # Airbus convention: push = QNH, pull = STD.
        return (f"(L:A32NX_FCU_EFIS_{side}_DISPLAY_BARO_MODE) 0 == "
                f"if{{ (>K:A32NX.FCU_EFIS_{side}_BARO_PUSH) }} els{{ (>K:A32NX.FCU_EFIS_{side}_BARO_PULL) }}")
    # Each recipe is a list of RPN snippets; several snippets become several actions in the binding so
    # the sim sees one event per frame per side (one combined string occasionally dropped a side).
    recipes = [
        ("Baro STD / QNH toggle (captain)", [baro_toggle("L")], "Switches the captain's baro reference between STD and QNH depending on its current state"),
        ("Baro STD / QNH toggle (first officer)", [baro_toggle("R")], "Switches the first officer's baro reference between STD and QNH"),
        ("Baro STD / QNH toggle (both sides)", [baro_toggle("L"), baro_toggle("R")], "Toggles both baro references, each according to its own state"),
        ("Baro QNH + (both sides)", ["(>K:A32NX.FCU_EFIS_L_BARO_INC)", "(>K:A32NX.FCU_EFIS_R_BARO_INC)"], "One click up on both baro knobs; bind with repeat-while-held"),
        ("Baro QNH - (both sides)", ["(>K:A32NX.FCU_EFIS_L_BARO_DEC)", "(>K:A32NX.FCU_EFIS_R_BARO_DEC)"], "One click down on both baro knobs; bind with repeat-while-held"),
    ]
    for function, codes, desc in (recipes if cfg["recipes"] else []):
        entries.append({"panel": "Recipes", "section": "EFIS baro", "function": function, "api": " + ".join(codes), "name": codes[0],
                        "codes": codes, "kind": "calc_code", "values": "", "rw": "", "type": "RPN (needs MobiFlight)", "remark": "",
                        "description": desc})

    doc = {"aircraft": aircraft, "title": cfg["title"], "sources": [cfg["flightdeck"], EVENTS_URL], "entries": entries}
    out = output or cfg["output"]
    with open(out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1, ensure_ascii=False)
        f.write("\n")
    kinds = {}
    for e in entries:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    print(f"{aircraft}: wrote {len(entries)} entries to {out}: {kinds}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--aircraft", default="A32NX", choices=sorted(AIRCRAFT))
    ap.add_argument("--all", action="store_true", help="regenerate the catalog of every supported aircraft")
    ap.add_argument("--flightdeck")
    ap.add_argument("--events")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()
    if args.all:
        for name in AIRCRAFT:
            build(name)
    else:
        build(args.aircraft, args.flightdeck, args.events, args.output)


if __name__ == "__main__":
    main()

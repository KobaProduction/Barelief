"""Compare observed graph layouts from the same native capture harness.

Usage: python compare_graph_layout.py baseline/manifest.json current/manifest.json report.json
Exit 1 means a measured layout regression; capture errors/mismatched fixtures exit 2.
"""
import json
import sys
from pathlib import Path


def compare(baseline, current):
    for key in ("font", "qtVersion", "platform"):
        if baseline[key] != current[key]:
            raise ValueError(f"Capture environment differs: {key}")
    old = {case["case"]: case for case in baseline["cases"]}
    new = {case["case"]: case for case in current["cases"]}
    if old.keys() != new.keys():
        raise ValueError("Capture case sets differ")
    results = []
    for name, before in old.items():
        after = new[name]
        if before["dpr"] != after["dpr"]:
            raise ValueError(f"DPI differs for {name}")
        if before["logicalDpi"] != after["logicalDpi"] or before["panel"] != after["panel"]:
            raise ValueError(f"Logical DPI or panel size differs for {name}")
        failures = []
        previous_fraction = before["visiblePlotFraction"]
        fraction = after["visiblePlotFraction"]
        # Relative to real Widgets baseline, with 20% layout tolerance.
        if fraction < previous_fraction * 0.8:
            failures.append("visible graph area fell by more than 20%")
        named = lambda case: {c["name"]: c for c in case["controls"] if c["name"]}
        previous_controls, controls = named(before), named(after)
        common_controls = []
        for key in ("numericDisplayMode", "frequencyAxisMode", "timeAxisMode"):
            if key in previous_controls and key not in controls:
                failures.append(f"visible control disappeared: {key}")
            if key not in previous_controls or key not in controls:
                continue
            a, b = previous_controls[key], controls[key]
            common_controls.append({"name": key, "beforeHeight": a["bounds"][3],
                                    "afterHeight": b["bounds"][3],
                                    "beforeFontHeight": a["fontHeight"],
                                    "afterFontHeight": b["fontHeight"],
                                    "beforeTextWidth": a["textWidth"],
                                    "afterTextWidth": b["textWidth"],
                                    "afterWidth": b["bounds"][2]})
            if b["bounds"][3] > a["bounds"][3] * 1.3:
                failures.append(f"{key} height grew by more than 30%")
        if before.get("popupOpen") and not after.get("popupOpen"):
            failures.append("display mode menu did not open")
        if before.get("popup") and after.get("popup"):
            if before["popup"]["rows"] != after["popup"]["rows"]:
                failures.append("display mode menu lost or added choices")
            if before["popup"]["font"] != after["popup"]["font"]:
                raise ValueError(f"Menu font differs for {name}")
            if after["popup"]["bounds"][3] > before["popup"]["bounds"][3] * 1.3:
                failures.append("display mode menu height grew by more than 30%")
            if "selectedBackground" in after["popup"] and after["popup"]["selectedBackground"] != after["popup"]["themeHighlight"]:
                failures.append("selected menu row does not use Theme highlight")
        if after["qmlErrors"]:
            failures.append("QML errors present")
        results.append({"case": name, "dpr": after["dpr"], "mode": after["mode"],
                        "beforePlot": before["visiblePlot"], "afterPlot": after["visiblePlot"],
                        "beforePlotFraction": previous_fraction, "afterPlotFraction": fraction,
                        "allocationScope": before["allocationScope"],
                        "beforePopup": before.get("popup"), "afterPopup": after.get("popup"),
                        "controls": common_controls, "failures": failures})
    return {"cases": results, "total": len(results),
            "regressions": sum(bool(case["failures"]) for case in results),
            "scope": "Measured renderer allocation / actual 3D viewport and named selector heights, existence, popup readiness/row count/height. Other spacing, clipping, menu appearance require screenshot inspection; no desktop capture or whole-application menu claim."}


if __name__ == "__main__":
    try:
        result = compare(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8-sig")),
                         json.loads(Path(sys.argv[2]).read_text(encoding="utf-8-sig")))
        Path(sys.argv[3]).write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
        print(f"Layout comparison: {result['regressions']}/{result['total']} cases regressed")
        sys.exit(1 if result["regressions"] else 0)
    except (ValueError, KeyError, OSError, IndexError) as error:
        print(f"Comparison fixture error: {error}", file=sys.stderr)
        sys.exit(2)

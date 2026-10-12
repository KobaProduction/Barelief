"""Merge owned UI probe evidence; findings are deliberately distinct from coverage."""
import argparse
import json
from pathlib import Path


def load(directory):
    reports = []
    for group in ("workspace", "graphs", "application"):
        report = json.loads((directory / f"{group}.json").read_text(encoding="utf-8-sig"))
        if report["schema"] != 1 or report["group"] != group:
            raise ValueError(f"Unexpected schema/group: {group}")
        transitions = report["transitions"]
        aborted = any(item.get("category") == "harness" for item in report["findings"])
        if (not transitions and not aborted) or not report["coverage"]:
            raise ValueError(f"Missing transitions/coverage: {group}")
        ids = [item["id"] for item in transitions]
        if len(set(ids)) != len(ids):
            raise ValueError(f"Duplicate transition IDs: {group}")
        for item in transitions:
            if item["status"] not in ("pass", "fail", "execution_error"):
                raise ValueError(f"Invalid transition status: {item['id']}")
            if not all(key in item for key in ("intent", "dispatch", "before", "after")):
                raise ValueError(f"Incomplete transition: {item['id']}")
        reports.append(report)
    return reports


def changed_controls(before, after):
    def index(snapshot):
        result = {(kind, item["path"]): item for kind in ("widgets", "quickItems")
                  for item in snapshot.get(kind, [])}
        result.update({("actions", str(index)): item for index, item in enumerate(snapshot.get("actions", []))})
        return result
    old, new = index(before), index(after)
    result = []
    for key in sorted(old.keys() | new.keys()):
        a, b = old.get(key), new.get(key)
        if a != b:
            result.append({"tree": key[0], "path": key[1], "before": a, "after": b})
    return result


def merge(reports):
    result = {"schema": 1, "scope": "Offscreen transition evidence, not native/aesthetic acceptance",
              "groups": [], "findings": [], "coverage": [], "transitions": []}
    for report in reports:
        group = report["group"]
        result["groups"].append({"group": group, "summary": report["summary"], "evidence": report["evidence"]})
        result["findings"].extend({"group": group, **item} for item in report["findings"])
        result["coverage"].extend({"group": group, **item} for item in report["coverage"])
        for item in report["transitions"]:
            changes = changed_controls(item["before"], item["after"])
            result["transitions"].append({"group": group,
                **{key: item[key] for key in ("id", "intent", "status", "dispatch", "elapsed_ms")},
                "violations": item.get("violations", []), "exception": item.get("exception"),
                "beforeImage": item.get("beforeImage"), "afterImage": item.get("afterImage"), "popupImage": item.get("popupImage"), "popup": item.get("popup"),
                "changedControls": changes[:200], "changesTruncated": len(changes) > 200,
                "beforeInventory": {kind: len(item["before"].get(kind, [])) for kind in ("widgets", "quickItems", "actions")},
                "afterInventory": {kind: len(item["after"].get(kind, [])) for kind in ("widgets", "quickItems", "actions")}})
    result["summary"] = {"transitions": len(result["transitions"]),
        "behaviorFailures": sum(item["status"] == "fail" for item in result["transitions"]),
        "executionErrors": sum(item["category"] == "harness" for item in result["findings"]),
        "errorFindings": sum(item["severity"] == "error" for item in result["findings"]),
        "findings": len(result["findings"]),
        "inputTransitions": sum(item["dispatch"].startswith("qt_input") for item in result["transitions"])}
    return result


def render(report):
    payload = json.dumps(report, ensure_ascii=False).replace("<", "\\u003c")
    return """<!doctype html><html lang="ru"><meta charset="utf-8">
<title>Аудит переходов интерфейса</title><style>
body{font:15px system-ui;margin:24px;max-width:1300px;color:#222;background:#fafafa}
button,input{font:inherit;padding:8px;margin:4px}article,details{border:1px solid #ccc;padding:12px;margin:10px 0;background:white}
.error,.fail,.execution_error{border-left:5px solid #a21d26}.warning{border-left:5px solid #8a5b00}.review{border-left:5px solid #3d5794}
pre{white-space:pre-wrap;overflow-wrap:anywhere}small{color:#555}h2{margin-top:28px}summary{cursor:pointer}a{margin-right:15px}
</style><h1>Аудит переходов интерфейса</h1>
<p>Поведение, найденные проблемы и покрытие разделены. PASS сценария не означает, что интерфейс удобен или визуально принят.</p>
<nav><a href="workspace.json">Исходные данные вкладок</a><a href="graphs.json">Графики</a><a href="application.json">Команды и схема</a><a href="summary.json">Сводка JSON</a></nav>
<p id="totals"></p><input id="query" placeholder="Поиск сценария или проблемы" aria-label="Поиск">
<button data-filter="all">Все</button><button data-filter="workspace">Вкладки и окна</button><button data-filter="graphs">Графики</button><button data-filter="application">Команды и схема</button>
<h2>Проблемы и вопросы ревью</h2><section id="findings"></section>
<h2>Покрытие и ограничения</h2><section id="coverage"></section>
<h2>Переходы</h2><section id="transitions"></section><h2>Дополнительные данные сценариев</h2><section id="evidence"></section>
<script>const data=""" + payload + """;
let filter='all';const query=document.getElementById('query');
function text(tag,value,parent){const e=document.createElement(tag);e.textContent=value;parent.append(e);return e}
function matches(item){return (filter==='all'||item.group===filter)&&JSON.stringify(item).toLowerCase().includes(query.value.toLowerCase())}
function captures(item,card){for(const key of ['beforeImage','afterImage','popupImage']){const path=item[key];if(typeof path!=='string'||!/^captures[/][a-zA-Z0-9_.-]+[.]png$/.test(path))continue;const link=document.createElement('a');link.href=path;link.textContent=key;link.target='_blank';card.append(link);const img=document.createElement('img');img.src=path;img.alt=key+' '+item.id;img.loading='lazy';img.style.maxWidth='48%';card.append(img)}}
function show(){
 document.getElementById('totals').textContent=`Переходов: ${data.summary.transitions}; нарушений поведения: ${data.summary.behaviorFailures}; ошибок инструмента: ${data.summary.executionErrors}; замечаний: ${data.summary.findings}`;
 for(const name of ['findings','coverage','transitions','evidence'])document.getElementById(name).replaceChildren();
 for(const item of data.groups.filter(matches)){const card=document.createElement('details');text('summary',item.group,card);for(const [id,value] of Object.entries(item.evidence)){if(value&&value.saved===true&&typeof value.path==='string')captures({id,popupImage:value.path},card)}text('pre',JSON.stringify(item.evidence,null,2),card);document.getElementById('evidence').append(card)}
 for(const item of data.findings.filter(matches)){const card=document.createElement('article');card.className=item.severity;text('b',`${item.group} / ${item.id} · ${item.severity} · ${item.category}`,card);text('p',item.observed,card);text('p','Ожидается: '+item.expected,card);text('pre',JSON.stringify(item.evidence,null,2),card);document.getElementById('findings').append(card)}
 for(const item of data.coverage.filter(matches)){const card=document.createElement('article');text('b',`${item.group} / ${item.area} · ${item.status}`,card);text('p',item.reason,card);document.getElementById('coverage').append(card)}
 for(const item of data.transitions.filter(matches)){const card=document.createElement('details');card.className=item.status;text('summary',`${item.group} / ${item.id} · ${item.status} · ${item.dispatch}`,card);text('p',item.intent,card);captures(item,card);text('pre',JSON.stringify(item,null,2),card);document.getElementById('transitions').append(card)}
}
query.addEventListener('input',show);for(const b of document.querySelectorAll('[data-filter]'))b.addEventListener('click',()=>{filter=b.dataset.filter;show()});show();
</script></html>"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--fail-on-findings", action="store_true", help="Fail on behavior violations or error findings; warning/review flags remain advisory")
    args = parser.parse_args()
    report = merge(load(args.directory))
    (args.directory / "summary.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    (args.directory / "index.html").write_text(render(report), encoding="utf-8")
    print(json.dumps(report["summary"]))
    return int(bool(report["summary"]["executionErrors"] or (args.fail_on_findings and
        (report["summary"]["behaviorFailures"] or report["summary"]["errorFindings"]))))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit(f"Invalid UI audit evidence: {error}")

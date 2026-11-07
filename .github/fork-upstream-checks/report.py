import glob
import json
import os

summary = open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8")


def w(text=""):
    print(text)
    summary.write(text + "\n")


plans = glob.glob("parts/plan/plan.json")
if not plans:
    w("## Upstream checks")
    w("RED: the plan job produced no plan (the branch may not merge cleanly into the base, or the plan failed).")
    raise SystemExit(1)
plan = json.load(open(plans[0], encoding="utf-8"))
results = {}
for path in glob.glob("parts/result-*/result.json"):
    data = json.load(open(path, encoding="utf-8"))
    results[data["unit"]["id"]] = data

red = False
w(f"## Upstream checks: branch `{plan['branch'][:10]}` merged into upstream `{plan['base'][:10]}` ({len(plan['files'])} changed files)")
if not os.environ.get("TITLE"):
    w("PR title and description were not given, so the title and template checks of the conventions job did not run.")
w()
w("| workflow | job | unit | result | seconds |")
w("| - | - | - | - | - |")
failures = []
for unit in plan["units"]:
    r = results.get(unit["id"])
    if r is None:
        red = True
        w(f"| {unit['workflow']} | {unit['job']} | {unit['id']} | no result (job did not finish) | - |")
        continue
    seconds = round(sum(s["seconds"] for s in r["steps"]))
    if r["status"] != "success":
        red = True
        failures.append(r)
    w(f"| {unit['workflow']} | {unit['job']} | {unit['id']} | {r['status'].upper()} | {seconds} |")
w()
w("### Coverage of the upstream workflows")
w("| workflow | job | runner | pull request trigger | status | reason |")
w("| - | - | - | - | - | - |")
for row in plan["inventory"]:
    w(f"| {row['workflow']} | {row['job']} | {row['runner']} | {row['triggers']} | {row['status']} | {row.get('reason', '')} |")
for r in failures:
    w()
    w(f"### Failure: {r['unit']['id']}")
    for step in r["steps"]:
        if step["status"] in ("failure", "unsupported"):
            w(f"Step `{step['name']}` {step['status']}:")
            w("```")
            w(step["excerpt"].replace("::error ", "error: ").replace("::warning ", "warning: ").replace("::", ": :"))
            w("```")
w()
w("## Verdict")
w("RED" if red else "GREEN")
raise SystemExit(1 if red else 0)

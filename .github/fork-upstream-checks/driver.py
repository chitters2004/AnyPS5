import itertools
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import yaml

UPSTREAM_URL = os.environ.get("UPSTREAM", "https://github.com/boykopovar/AnyPS5.git")
UPSTREAM_SLUG = "boykopovar/AnyPS5"
HERE = Path(__file__).resolve().parent
CONFIG = json.loads((HERE / "config.json").read_text(encoding="utf-8"))
OUT = Path(os.environ.get("CHECK_OUT", "out"))
KNOWN_USES = ("actions/checkout", "actions/setup-node", "actions/setup-python", "actions/cache", "actions/upload-artifact", "actions/download-artifact")


class Unsupported(Exception):
    pass


def bash_exe():
    if os.name == "nt":
        for candidate in (r"C:\Program Files\Gitinash.exe", r"C:\Program Files (x86)\Gitinash.exe"):
            if os.path.exists(candidate):
                return candidate
    return "bash"


def git(*args, check=True, cwd=None):
    result = subprocess.run(["git", *args], capture_output=True, text=True, cwd=cwd)
    if check and result.returncode:
        raise SystemExit(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def prepare():
    branch_sha = os.environ["BRANCH_SHA"]
    base = os.environ.get("BASE", "").strip() or "main"
    subprocess.run(["git", "fetch", "-q", "--no-tags", UPSTREAM_URL, base], check=True)
    base_sha = git("rev-parse", "FETCH_HEAD")
    git("checkout", "-q", "--detach", base_sha)
    merged = subprocess.run(["git", "-c", "user.name=fork-upstream-checks", "-c", "user.email=ci@localhost", "merge", "--no-ff", "--no-edit", branch_sha],
                            capture_output=True, text=True)
    if merged.returncode:
        files = git("diff", "--name-only", "--diff-filter=U", check=False)
        git("merge", "--abort", check=False)
        print(f"::error::the branch does not merge cleanly into {base_sha[:10]}; conflicting files: {files.replace(chr(10), ', ')}")
        raise SystemExit(3)
    return base_sha, branch_sha, git("rev-parse", "HEAD")


def glob_regex(pattern):
    out, i = "", 0
    while i < len(pattern):
        c = pattern[i]
        if pattern.startswith("**", i):
            out += ".*"
            i += 2
            if pattern.startswith("/", i):
                out += "/?"
                i += 1
            continue
        if c == "*":
            out += "[^/]*"
        elif c == "?":
            out += "[^/]"
        elif c == "[":
            j = pattern.find("]", i)
            if j == -1:
                out += re.escape(c)
            else:
                out += pattern[i:j + 1]
                i = j
        else:
            out += re.escape(c)
        i += 1
    return re.compile("^" + out + "$")


def path_matches(patterns, path):
    matched = False
    for pattern in patterns:
        negative = pattern.startswith("!")
        if glob_regex(pattern[1:] if negative else pattern).match(path):
            matched = not negative
    return matched


def triggered(config, files, base_ref="main"):
    config = config or {}
    branches = config.get("branches")
    if branches and not path_matches(branches, base_ref):
        return False
    paths, ignore = config.get("paths"), config.get("paths-ignore")
    if paths:
        return any(path_matches(paths, f) for f in files)
    if ignore:
        return any(not path_matches(ignore, f) for f in files)
    return True


class Expr:
    TOKEN = re.compile(r"\s*(?:(?P<str>'(?:[^']|'')*')|(?P<num>-?\d+(?:\.\d+)?)|(?P<op>==|!=|&&|\|\||<=|>=|[!<>(),\[\]])|(?P<id>[A-Za-z_*][\w\-*]*(?:\.[A-Za-z_*][\w\-*]*)*))")

    def __init__(self, text, ctx):
        self.ctx = ctx
        self.tokens, pos = [], 0
        text = text.strip()
        while pos < len(text):
            m = self.TOKEN.match(text, pos)
            if not m or m.end() == pos:
                raise Unsupported(f"cannot parse expression {text!r}")
            self.tokens.append((m.lastgroup, m.group(m.lastgroup)))
            pos = m.end()
            while pos < len(text) and text[pos].isspace():
                pos += 1
        self.i = 0

    def peek(self):
        return self.tokens[self.i] if self.i < len(self.tokens) else (None, None)

    def take(self):
        token = self.peek()
        self.i += 1
        return token

    def parse(self):
        value = self.or_()
        if self.i != len(self.tokens):
            raise Unsupported("trailing tokens in expression")
        return value

    def or_(self):
        left = self.and_()
        while self.peek() == ("op", "||"):
            self.take()
            right = self.and_()
            left = left if truthy(left) else right
        return left

    def and_(self):
        left = self.cmp()
        while self.peek() == ("op", "&&"):
            self.take()
            right = self.cmp()
            left = right if truthy(left) else left
        return left

    def cmp(self):
        left = self.unary()
        while self.peek()[0] == "op" and self.peek()[1] in ("==", "!=", "<", ">", "<=", ">="):
            op = self.take()[1]
            right = self.unary()
            left = compare(op, left, right)
        return left

    def unary(self):
        if self.peek() == ("op", "!"):
            self.take()
            return not truthy(self.unary())
        return self.atom()

    def atom(self):
        kind, value = self.take()
        if kind == "str":
            return value[1:-1].replace("''", "'")
        if kind == "num":
            return float(value) if "." in value else int(value)
        if kind == "op" and value == "(":
            inner = self.or_()
            if self.take() != ("op", ")"):
                raise Unsupported("missing )")
            return inner
        if kind == "id":
            if value in ("true", "false", "null"):
                return {"true": True, "false": False, "null": None}[value]
            if self.peek() == ("op", "("):
                self.take()
                args = []
                while self.peek() != ("op", ")"):
                    args.append(self.or_())
                    if self.peek() == ("op", ","):
                        self.take()
                self.take()
                return call(value, args, self.ctx)
            result = lookup(self.ctx, value.split("."))
            while self.peek() == ("op", "["):
                self.take()
                key = self.or_()
                self.take()
                result = result.get(key) if isinstance(result, dict) else None
            return result
        raise Unsupported(f"unexpected token {value!r}")


def truthy(value):
    if value is None:
        return False
    if isinstance(value, (bool, int, float)):
        return bool(value)
    return value != ""


def compare(op, a, b):
    if isinstance(a, str) and isinstance(b, str):
        a, b = a.lower(), b.lower()
    elif a is None:
        a = "" if isinstance(b, str) else 0 if isinstance(b, (int, float)) else a
    elif b is None:
        b = "" if isinstance(a, str) else 0 if isinstance(a, (int, float)) else b
    if isinstance(a, bool) != isinstance(b, bool):
        a, b = to_number(a), to_number(b)
    return {"==": a == b, "!=": a != b, "<": a < b, ">": a > b, "<=": a <= b, ">=": a >= b}[op]


def to_number(value):
    if isinstance(value, bool):
        return int(value)
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0


def lookup(ctx, parts):
    value = ctx
    for part in parts:
        if isinstance(value, dict):
            value = value.get(part)
        else:
            return None
    return value


def call(name, args, ctx):
    lower = name.lower()
    if lower == "fromjson":
        return json.loads(args[0])
    if lower == "tojson":
        return json.dumps(args[0], indent=2)
    if lower == "contains":
        return args[1] in args[0] if isinstance(args[0], list) else str(args[1]).lower() in str(args[0] or "").lower()
    if lower == "startswith":
        return str(args[0] or "").lower().startswith(str(args[1]).lower())
    if lower == "endswith":
        return str(args[0] or "").lower().endswith(str(args[1]).lower())
    if lower == "format":
        return re.sub(r"\{(\d+)\}", lambda m: stringify(args[int(m[1]) + 1]), args[0])
    if lower == "join":
        return str(args[1] if len(args) > 1 else ",").join(stringify(a) for a in args[0])
    if lower in ("success", "always", "failure", "cancelled"):
        failed = ctx.get("__failed__", False)
        return {"success": not failed, "always": True, "failure": failed, "cancelled": False}[lower]
    raise Unsupported(f"function {name}()")


def stringify(value):
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (dict, list)):
        return json.dumps(value)
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


INTERP = re.compile(r"\$\{\{(.*?)\}\}", re.S)


def interpolate(text, ctx):
    if not isinstance(text, str):
        return text
    return INTERP.sub(lambda m: stringify(Expr(m[1], ctx).parse()), text)


def condition(value, ctx):
    if value is None:
        return not ctx.get("__failed__", False)
    if isinstance(value, bool):
        return value
    text = str(value).strip()
    match = re.fullmatch(r"\$\{\{(.*)\}\}", text, re.S)
    if match:
        text = match[1]
    return truthy(Expr(text, ctx).parse())


def value_of(value, ctx):
    if isinstance(value, str):
        match = re.fullmatch(r"\s*\$\{\{(.*)\}\}\s*", value, re.S)
        if match:
            return Expr(match[1], ctx).parse()
        return interpolate(value, ctx)
    return value


def expand_matrix(strategy, ctx):
    matrix = dict((strategy or {}).get("matrix") or {})
    if isinstance(matrix.get("include"), str):
        matrix["include"] = value_of(matrix["include"], ctx)
    include = matrix.pop("include", None) or []
    exclude = matrix.pop("exclude", None) or []
    if isinstance(exclude, str):
        exclude = value_of(exclude, ctx)
    dims = {k: (value_of(v, ctx) if isinstance(v, str) else v) for k, v in matrix.items()}
    combos = [dict(zip(dims, values)) for values in itertools.product(*dims.values())] if dims else []
    resolved_exclude = []
    for row in exclude:
        resolved_exclude.append({k: value_of(v, ctx) if isinstance(v, str) else v for k, v in row.items()})
    combos = [c for c in combos if not any(all(c.get(k) == v for k, v in row.items()) for row in resolved_exclude)]
    for row in include:
        row = {k: value_of(v, ctx) if isinstance(v, str) else v for k, v in row.items()}
        extended = False
        for combo in combos:
            if dims and all(combo[k] == v for k, v in row.items() if k in dims):
                combo.update(row)
                extended = True
        if not extended:
            combos.append(row)
    return combos or [{}]


def load_workflow(text):
    data = yaml.safe_load(text)
    if True in data and "on" not in data:
        data["on"] = data.pop(True)
    return data


def triggers_of(workflow):
    on = workflow.get("on")
    if isinstance(on, str):
        return {on: None}
    if isinstance(on, list):
        return {name: None for name in on}
    return on or {}


def base_context(event_name, merge_sha, base_sha, branch_sha, number, title, body, runner_os):
    pr = {"number": number, "title": title, "body": body, "base": {"ref": "main", "sha": base_sha}, "head": {"sha": branch_sha, "ref": os.environ.get("BRANCH", "")},
          "user": {"login": "fork-upstream-checks"}}
    return {
        "github": {"event_name": event_name, "sha": merge_sha, "ref": "refs/heads/main", "ref_name": "main", "repository": UPSTREAM_SLUG, "base_ref": "main",
                   "head_ref": os.environ.get("BRANCH", ""), "actor": "fork-upstream-checks", "event": {"pull_request": pr, "repository": {"fork": False}},
                   "run_id": os.environ.get("GITHUB_RUN_ID", "0"), "workflow": ""},
        "runner": {"os": runner_os, "temp": os.environ.get("RUNNER_TEMP", tempfile.gettempdir()), "arch": "X64"},
        "env": {}, "matrix": {}, "needs": {}, "steps": {}, "inputs": {}, "secrets": {}, "job": {"status": "success"}, "vars": {},
    }


def needs_context(workflow_name, job, merge_sha):
    outputs = CONFIG.get("needs_outputs", {}).get(workflow_name, {})
    needs = {}
    for name in (job.get("needs") if isinstance(job.get("needs"), list) else [job.get("needs")] if job.get("needs") else []):
        given = outputs.get(name, {})
        needs[name] = {"result": "success", "outputs": {k: v.replace("$MERGE_SHA", merge_sha) if isinstance(v, str) else v for k, v in given.items()}}
    return needs


def scan_job(job):
    reasons = []
    runs_on = job.get("runs-on")
    if isinstance(runs_on, list) or (isinstance(runs_on, str) and "self-hosted" in runs_on):
        reasons.append("self-hosted runner")
    for step in job.get("steps") or []:
        uses = step.get("uses")
        if uses and not uses.startswith(KNOWN_USES):
            reasons.append(f"uses {uses.split('@')[0]}")
        shell = step.get("shell") or (job.get("defaults") or {}).get("run", {}).get("shell")
        if shell and shell not in ("bash", "sh", "pwsh", "python"):
            reasons.append(f"shell {shell}")
    return sorted(set(reasons))


def plan():
    base_sha, branch_sha, merge_sha = prepare()
    files = sorted(set(git("diff", "--name-only", "--no-renames", base_sha, merge_sha).splitlines()))
    units, inventory = [], []
    workflows = sorted(Path(".github/workflows").glob("*.yml"))
    for path in workflows:
        name = path.name
        merged_text = path.read_text(encoding="utf-8")
        base_text = git("show", f"{base_sha}:.github/workflows/{name}", check=False)
        try:
            merged = load_workflow(merged_text)
        except yaml.YAMLError as error:
            inventory.append(dict(workflow=name, job="*", triggers="?", runner="?", status="error", reason=f"cannot parse: {error}"))
            continue
        triggers = triggers_of(merged)
        pr_events = [e for e in ("pull_request", "pull_request_target") if e in triggers]
        for job_name, job in (merged.get("jobs") or {}).items():
            row = dict(workflow=name, job=job_name, triggers=",".join(sorted(triggers)), runner=json.dumps(job.get("runs-on")), minutes=job.get("timeout-minutes", ""))
            secrets = "secrets." in json.dumps(job) and "secrets.GITHUB_TOKEN" not in json.dumps(job)
            if not pr_events:
                row.update(status="not covered", reason="not triggered by a pull request: " + row["triggers"])
                inventory.append(row)
                continue
            event = "pull_request_target" if "pull_request_target" in pr_events else "pull_request"
            source_text = base_text if event == "pull_request_target" and base_text else merged_text
            source = load_workflow(source_text)
            job = (source.get("jobs") or {}).get(job_name)
            if job is None:
                row.update(status="not covered", reason="job missing in the base copy of the workflow")
                inventory.append(row)
                continue
            if not triggered(triggers.get(event), files):
                row.update(status="not triggered", reason="the changed files do not match the paths filter of " + event)
                inventory.append(row)
                continue
            allowed = CONFIG["run"].get(name, [])
            skip = CONFIG.get("skip", {}).get(name, {})
            if job_name in skip:
                row.update(status="not covered", reason=skip[job_name])
            elif job_name not in allowed:
                row.update(status="not covered", reason="not classified yet: add it to config.json (run or skip)")
            elif secrets:
                row.update(status="not covered", reason="uses secrets")
            else:
                reasons = scan_job(job)
                if reasons:
                    row.update(status="not covered", reason="cannot be reproduced outside GitHub: " + ", ".join(reasons))
                else:
                    ctx = base_context(event, merge_sha, base_sha, branch_sha, 0, "", "", "Linux")
                    ctx["needs"] = needs_context(name, job, merge_sha)
                    ctx["github"]["workflow"] = source.get("name", name)
                    ctx["env"] = dict(source.get("env") or {})
                    try:
                        combos = expand_matrix(job.get("strategy"), ctx)
                        for combo in combos:
                            ctx["matrix"] = combo
                            os_label = value_of(job.get("runs-on"), ctx)
                            node = ""
                            for step in job.get("steps") or []:
                                if (step.get("uses") or "").startswith("actions/setup-node"):
                                    node = stringify(value_of((step.get("with") or {}).get("node-version"), ctx))
                            suffix = "-".join(stringify(v) for v in combo.values())
                            uid = re.sub(r"[^A-Za-z0-9]+", "-", f"{name[:-4]}-{job_name}-{suffix}").strip("-")[:80]
                            units.append(dict(id=uid, workflow=name, job=job_name, event=event, matrix=combo, os=os_label, node=node))
                        row.update(status="run", reason=f"{len(combos)} unit(s) as {event}")
                    except Unsupported as error:
                        row.update(status="not covered", reason=f"expression not reproducible: {error}")
            inventory.append(row)
    Path("plan.json").write_text(json.dumps(dict(base=base_sha, branch=branch_sha, merge=merge_sha, files=files, inventory=inventory, units=units), indent=1), encoding="utf-8")
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as out:
        out.write("units=" + json.dumps({"include": units}) + "\n")
        out.write("has_units=" + ("true" if units else "false") + "\n")
        out.write(f"base_sha={base_sha}\nbranch_sha={branch_sha}\n")
    print(json.dumps(inventory, indent=1))


def read_env_file(path):
    values = {}
    if not Path(path).exists():
        return values
    lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^([^=<]+)<<(.+)$", line)
        if m:
            delimiter, body = m[2], []
            i += 1
            while i < len(lines) and lines[i] != delimiter:
                body.append(lines[i])
                i += 1
            values[m[1]] = "\n".join(body)
        elif "=" in line:
            key, _, value = line.partition("=")
            values[key] = value
        i += 1
    return values


def run_unit(unit):
    OUT.mkdir(parents=True, exist_ok=True)
    base_sha, branch_sha, merge_sha = prepare()
    workspace = Path.cwd()
    name, job_name, event = unit["workflow"], unit["job"], unit["event"]
    text = git("show", f"{base_sha}:.github/workflows/{name}") if event == "pull_request_target" else (workspace / ".github/workflows" / name).read_text(encoding="utf-8")
    workflow = load_workflow(text)
    job = workflow["jobs"][job_name]
    title, body = os.environ.get("PR_TITLE", "").strip(), os.environ.get("PR_BODY", "").strip()
    runner_os = {"Linux": "Linux", "Windows": "Windows", "macOS": "macOS"}.get(os.environ.get("RUNNER_OS", "Linux"), "Linux")
    ctx = base_context(event, merge_sha, base_sha, branch_sha, 0, title, body, runner_os)
    ctx["github"]["workflow"] = workflow.get("name", name)
    ctx["needs"] = needs_context(name, job, merge_sha)
    ctx["matrix"] = unit["matrix"]
    ctx["env"] = dict(workflow.get("env") or {})
    ctx["env"].update(job.get("env") or {})
    tmp = Path(tempfile.mkdtemp(prefix="fuc-"))
    event_file = tmp / "event.json"
    event_json = {"repository": {"fork": False}}
    if title or body:
        event_json["pull_request"] = ctx["github"]["event"]["pull_request"]
    event_file.write_text(json.dumps(event_json), encoding="utf-8")
    summary = tmp / "summary.md"
    summary.write_text("", encoding="utf-8")
    defaults = (job.get("defaults") or {}).get("run") or {}
    process_env = dict(os.environ)
    process_env.update({"GITHUB_WORKSPACE": str(workspace), "GITHUB_EVENT_NAME": event, "GITHUB_EVENT_PATH": str(event_file), "GITHUB_SHA": merge_sha,
                        "GITHUB_REF": "refs/heads/main", "GITHUB_REF_NAME": "main", "GITHUB_BASE_REF": "main", "GITHUB_REPOSITORY": UPSTREAM_SLUG,
                        "GITHUB_ACTIONS": "true", "CI": "true", "GITHUB_STEP_SUMMARY": str(summary)})
    results, failed, log_all = [], False, []
    for index, step in enumerate(job.get("steps") or []):
        ctx["__failed__"] = failed
        run_text = (step.get("run") or "").strip()
        label = step.get("name") or step.get("uses") or (run_text.splitlines()[0][:70] if run_text else f"step {index}")
        record = dict(name=label, status="success", seconds=0, excerpt="")
        started = time.time()
        try:
            if not condition(step.get("if"), ctx):
                record["status"] = "skipped"
                results.append(record)
                continue
            uses = step.get("uses") or ""
            with_ = {k: value_of(v, ctx) for k, v in (step.get("with") or {}).items()}
            outputs = {}
            if uses.startswith("actions/checkout"):
                ref = stringify(with_.get("ref"))
                target = base_sha if ref in (ctx["github"]["event"]["pull_request"]["base"]["ref"], base_sha) else merge_sha
                git("checkout", "-q", "--detach", "-f", target)
                git("clean", "-fdq", "-e", "ci", "-e", "out")
                record["name"] += f" (checked out {'the base' if target == base_sha else 'the merge of the branch into the base'} {target[:10]})"
            elif uses.startswith(("actions/setup-node", "actions/setup-python")):
                record["name"] += " (provided by the job)"
            elif uses.startswith("actions/cache"):
                outputs["cache-hit"] = "false"
                record["status"], record["name"] = "skipped", record["name"] + " (no cache on this run)"
            elif uses.startswith(("actions/upload-artifact", "actions/download-artifact")):
                record["status"], record["name"] = "skipped", record["name"] + " (artifacts are not exchanged)"
            elif uses:
                raise Unsupported(f"uses {uses}")
            elif step.get("run"):
                step_env = dict(process_env)
                step_env.update({k: stringify(interpolate(v, ctx)) for k, v in ctx["env"].items()})
                step_env.update({k: stringify(interpolate(v, ctx)) for k, v in (step.get("env") or {}).items()})
                env_file, path_file, out_file = tmp / f"env{index}", tmp / f"path{index}", tmp / f"out{index}"
                for f in (env_file, path_file, out_file):
                    f.write_text("", encoding="utf-8")
                step_env.update({"GITHUB_ENV": str(env_file), "GITHUB_PATH": str(path_file), "GITHUB_OUTPUT": str(out_file)})
                script = interpolate(step["run"], ctx)
                shell = step.get("shell") or defaults.get("shell") or ("pwsh" if runner_os == "Windows" else "bash")
                directory = step.get("working-directory") or defaults.get("working-directory") or "."
                if shell in ("bash", "sh"):
                    command = [bash_exe(), "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", script] if shell == "bash" else ["sh", "-e", "-c", script]
                elif shell == "pwsh":
                    path = tmp / f"step{index}.ps1"
                    path.write_text("$ErrorActionPreference = 'stop'\n" + script + "\nif ((Test-Path -LiteralPath variable:\\LASTEXITCODE)) { exit $LASTEXITCODE }", encoding="utf-8")
                    command = ["pwsh", "-command", f". '{path}'"]
                else:
                    path = tmp / f"step{index}.py"
                    path.write_text(script, encoding="utf-8")
                    command = [sys.executable, str(path)]
                print(f"::group::{label}", flush=True)
                proc = subprocess.run(command, cwd=directory, env=step_env, capture_output=True, text=True, encoding="utf-8", errors="replace")
                output = proc.stdout + proc.stderr
                print(output, flush=True)
                print("::endgroup::", flush=True)
                log_all.append(f"=== {label}\n{output}")
                ctx["env"].update(read_env_file(env_file))
                process_env.update(read_env_file(env_file))
                extra = [p for p in path_file.read_text(encoding="utf-8").splitlines() if p.strip()]
                if extra:
                    process_env["PATH"] = os.pathsep.join(extra + [process_env["PATH"]])
                outputs = read_env_file(out_file)
                if proc.returncode:
                    lines = output.strip().splitlines()
                    keep = [l for l in lines if re.search(r"::error|error:|Error|FAIL|failed|Traceback", l)][:15]
                    record["excerpt"] = "\n".join(keep + ["..."] + lines[-25:]) if keep else "\n".join(lines[-40:])
                    record["status"] = "failure"
                    if not step.get("continue-on-error"):
                        failed = True
            if step.get("id"):
                ctx["steps"][step["id"]] = {"outputs": outputs, "outcome": record["status"], "conclusion": record["status"]}
        except Unsupported as error:
            record["status"], record["excerpt"] = "unsupported", f"not reproducible outside GitHub: {error}"
            failed = True
        except Exception as error:
            record["status"], record["excerpt"] = "failure", f"driver error: {error!r}"
            failed = True
        record["seconds"] = round(time.time() - started, 1)
        results.append(record)
    status = "failure" if any(r["status"] in ("failure", "unsupported") for r in results) else "success"
    unit_result = dict(unit=unit, status=status, steps=results, summary=summary.read_text(encoding="utf-8", errors="replace")[-4000:])
    (OUT / "result.json").write_text(json.dumps(unit_result, indent=1), encoding="utf-8")
    (OUT / "log.txt").write_text("\n".join(log_all), encoding="utf-8")
    print(f"unit {unit['id']}: {status}")
    return 0


def main():
    mode = sys.argv[1]
    if mode == "plan":
        plan()
    elif mode == "run":
        sys.exit(run_unit(json.loads(os.environ["UNIT"])))


if __name__ == "__main__":
    main()

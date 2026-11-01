# Label store GPU compare: Windows and GTA kit

Not for upstream: drop this folder (and `LABEL_COMPARE_REPORT.md`) before any pull request. The
background, the design and every test are in `../LABEL_COMPARE_REPORT.md`.

The PowerShell scripts were written without a Windows machine to run them on; `summarize.py` has
a self-test (`py -3 summarize.py self-test`) and was checked against real Linux logs.

## What the run must answer

1. Does the GPU compare make the same decisions on GTA? (`shadow` run: mismatches must be 0.)
2. Where does the label wait time go? (`shadow` run: refusal reasons ranked by wait time.)
3. How much frame time does the GPU compare save? (`gpu` against `main` and `off`, same scene.)
4. Is the branch free with the switches off? (`off` must match `main`.)

## Switches

| Mode | Switch | Effect |
| --- | --- | --- |
| `main` | none, `main` libraries | baseline |
| `off` | none, branch libraries | must equal `main`: lease-table and late-stamp registration only run with one of the switches below |
| `shadow` | `APS5_LABEL_COMPARE_SHADOW=1` | still waits; classifies each waiting label store, checks the prediction after the wait, prints `[label-compare]` on stderr every 10 s. Classification costs CPU: do not use its frame times |
| `gpu` | `APS5_LABEL_GPU_COMPARE=1` | the GPU compare; prints the `[label-compare] GPU compare` line every 10 s; the performance candidate |

Neither switch needs `APS5_PROFILE_DRAW`. Do not add it to timed runs.

## 1. Two trees, two builds

Use short paths (BUILD.md: the 260-character limit). Toolchain and PATH as in
`docs/dev/BUILD.md`, "Windows setup and troubleshooting".

```powershell
git clone https://github.com/chitters2004/AnyPS5 C:\a5
cd C:\a5
git checkout draft/label-compare
git submodule update --init --recursive
git remote add upstream https://github.com/boykopovar/AnyPS5
git fetch upstream main
git worktree add C:\a5m upstream/main
cd C:\a5m
git submodule update --init --recursive
```

Build both with tests (once per tree; the second build of each is incremental):

```powershell
cmake -S C:\a5 -B C:\a5\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON
cmake --build C:\a5\build --target libs --parallel
cmake -S C:\a5m -B C:\a5m\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON
cmake --build C:\a5m\build --target libs --parallel
```

Copy the toolchain runtime DLLs beside the libraries of both builds (BUILD.md shows the loop).

## 2. Tests on this GPU (before the title)

Install the Vulkan SDK (for `VK_LAYER_KHRONOS_validation`), then:

```powershell
cd C:\a5\label-compare-kit
.\run-tests.ps1 -Branch C:\a5 -Main C:\a5m
```

It builds the focused test targets in both trees, runs them on `main`, on the branch and on the
branch with `APS5_LABEL_GPU_COMPARE=1`, with `ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, then prints the comparison by test name: changed
outcomes, new tests, `VUID-` increases, and whether the layer was missing. `-Wide` runs the 185
related tests instead (70 executables; long).

On a GPU whose import probe keeps imports watched, the settle and upload-before-completion cases
of `agc_driver_storage_flush_ordering_gpu_compare` run instead of skipping (on lavapipe they
skip). Look for `check skipped` in the branch log: no skip line there means the guards ran.

## 3. GTA runs

The converted `app.exe` is the same for both trees (the change is in the libraries only); the
script copies the chosen build's `.prx` and `.dll` files into the title's `libs\` before each
run. Pick a scene you can reach the same way each time (a save, then stand still), and a trace
window that starts after you are there.

```powershell
cd C:\a5\label-compare-kit
.\run-gta.ps1 -Title D:\gta -Libs C:\a5m\build\core\libs\libs -Mode main   -TraceStart 120 -TraceLength 30 -Tag 1
.\run-gta.ps1 -Title D:\gta -Libs C:\a5\build\core\libs\libs  -Mode off    -TraceStart 120 -TraceLength 30 -Tag 1
.\run-gta.ps1 -Title D:\gta -Libs C:\a5\build\core\libs\libs  -Mode gpu    -TraceStart 120 -TraceLength 30 -Tag 1
.\run-gta.ps1 -Title D:\gta -Libs C:\a5\build\core\libs\libs  -Mode shadow -TraceStart 120 -TraceLength 30 -Tag 1
```

Repeat `main`, `off` and `gpu` once more in another order (`-Tag 2`) to see the run-to-run spread.
Logs, traces and `aps5-summary.jsonl` go to `label-compare-kit\logs`. Quit the game after the
window closes (the trace is written when it closes).

## 4. Summaries to send back

```powershell
cd C:\a5\label-compare-kit
py -3 summarize.py gta (Get-ChildItem logs\gta-*.log -Exclude *.stdout.log) --summary logs\aps5-summary.jsonl
Get-ChildItem logs\aps5-trace-*.json.gz | ForEach-Object { py -3 ..\tools\trace_digest.py $_.FullName }
```

`summarize.py gta` prints, per log: the `[write-watch] host imports` line, the shadow totals and
mismatches (it stops with a `STOP` line and exit code 2 on any mismatch), the refusal reasons and
the waits the GPU compare kept, each ranked by wait time with the next step it points to, and a
frame-time table of every summary line with its delta from the first `off` run.

Send: both `summarize.py` outputs, the `trace_digest.py` outputs, any `STOP` lines, and the
`[label-compare] mismatch` lines if any.

## 5. Reading the result

- Mismatches above 0: the GPU decision differs from the waited one on this title. Do not use
  `gpu` until the cause is found; the mismatch lines name the image, block, mode and outcome.
- `off` slower than `main` beyond the spread: the registration gating failed; report it.
- `gpu` faster than `main`: the saving; compare with the eligible wait time of the shadow run.
- Large refusal reasons decide the next work (the `next:` column; the report's "Next steps"
  section explains each).

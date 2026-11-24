#!/usr/bin/env bash
# Repeatable in-game benchmark. Usage: tools/bench.sh [-s SECONDS] [-n NAME] [-e VAR=val ...] <game.exe> [loader args]
# Launch, walk to your spot, press Enter in this terminal: the next SECONDS (default 30) are measured.
# Results append to ~/.beer/bench/<NAME>.txt so builds can be compared on absolute numbers.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
secs=30; name=$(git rev-parse --short HEAD 2>/dev/null || echo build); extra=()
while getopts s:n:e: o; do case $o in s) secs=$OPTARG;; n) name=$OPTARG;; e) extra+=("$OPTARG");; esac; done
shift $((OPTIND-1))
out=~/.beer/bench; mkdir -p "$out"; log=$out/$name.log
env BEER_BD12_FPS=1 BEER_BD12_PROF=1 "${extra[@]}" ./loader --audio=fmod "$@" >"$log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
if [[ -t 0 ]]; then read -rp "Go to the spot, then press Enter to start the ${secs}s measurement... "
else echo "measuring in ${BENCH_DELAY:-45}s"; sleep "${BENCH_DELAY:-45}"; fi
start=$(wc -l <"$log"); sleep "$secs"; end=$(wc -l <"$log")
kill $pid 2>/dev/null || true
sed -n "$((start+1)),${end}p" "$log" >"$out/$name.window"
python3 - "$out/$name.window" "$name" "$secs" <<'P' | tee -a "$out/summary.txt"
import re,sys,statistics as st
w=open(sys.argv[1],errors='replace').read().splitlines()
fps=[(float(m.group(1)),float(m.group(2)),float(m.group(3))) for l in w if (m:=re.search(r'\[BD12 FPS\] ([\d.]+) fps \(([\d.]+) ms/frame.*worst (\d+) ms',l))]
if not fps: print("no FPS lines captured"); sys.exit(1)
f=[x[0] for x in fps]; ms=[x[1] for x in fps]
print(f"== {sys.argv[2]} ({sys.argv[3]}s, {len(f)} windows) avg {st.mean(f):.1f} fps, avg frame {st.mean(ms):.2f} ms, "
      f"min window {min(f):.1f} fps, worst frame {max(x[2] for x in fps):.0f} ms")
sec={}
for l in w:
    if '[BD12 PROF]' in l:
        for k,v in re.findall(r'([\w-]+)=(\d+)ms/',l): sec.setdefault(k,[]).append(float(v))
        for k,v in re.findall(r'(barriers|full|render-passes)=(\d+)',l): sec.setdefault(k+' (count)',[]).append(float(v))
for k,v in sorted(sec.items(),key=lambda kv:('(count)' in kv[0],-sum(kv[1]))):
    if sum(v)>0: print(f"   {k:<22} {st.mean(v):8.1f} {'per 2s window' if '(count)' in k else 'ms per 2s window'}")
P

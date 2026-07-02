#!/bin/bash
# Take one screenshot, retrying and polling until the file actually appears.
EDGE="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"
BASE="F:\Claude Code Arbeitsumgebung\dynamic-price-clock\docs\mockup-src\edge-profile"
DEST_WIN="F:\Claude Code Arbeitsumgebung\dynamic-price-clock\docs\mockup-src\shots"
DEST_UNIX="/f/Claude Code Arbeitsumgebung/dynamic-price-clock/docs/mockup-src/shots"

page="$1"
name="$2"
theme="$3"
idx="$4"

url="http://localhost:8934/${page}.html"
if [ "$theme" = "dark" ]; then url="${url}?theme=dark"; fi
outfile="${name}-${theme}.png"

for attempt in 1 2 3; do
  rm -rf "/f/Claude Code Arbeitsumgebung/dynamic-price-clock/docs/mockup-src/edge-profile${idx}"
  "$EDGE" --headless=new --disable-gpu --no-sandbox --disable-dev-shm-usage --hide-scrollbars \
    --user-data-dir="${BASE}${idx}" --virtual-time-budget=3000 --window-size=1280,900 \
    --screenshot="${DEST_WIN}\\${outfile}" "$url" >/dev/null 2>&1

  for poll in $(seq 1 15); do
    if [ -s "${DEST_UNIX}/${outfile}" ]; then
      echo "OK ${outfile} (attempt $attempt, poll $poll)"
      exit 0
    fi
    sleep 1
  done
  echo "retrying ${outfile} after failed attempt $attempt"
done

echo "FAILED ${outfile}"
exit 1

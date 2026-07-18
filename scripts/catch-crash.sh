#!/usr/bin/env bash
#
# catch-crash.sh — capture the REAL crash stack from a running Geany, faithfully.
#
# Why attach (not launch): launching Geany under a debugger enables the NT debug
# heap and alters SEH dispatch, which turns GTK's normally-handled startup
# first-chance exceptions into fatal crashes (a debugger artifact, not the real
# bug). Attaching to an already-running process keeps the normal heap and
# exception handling, so cdb sees the genuine crash.
#
# Usage:
#   1. Start Geany normally, open your Markdown file, enable the WebView preview.
#   2. Run this script in an MSYS2/Git-Bash terminal.
#   3. Reproduce the crash (select text -> Copy, paste, copy a longer string...).
#   4. The stack (all threads) + a full dump land in crashdumps/. Send me
#      crashdumps/cdb-out.log.
#
set -u
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

CDB="$(ls "/c/Program Files/WindowsApps/Microsoft.WinDbg_"*"/amd64/cdb.exe" 2>/dev/null | head -1)"
if [ -z "$CDB" ]; then echo "cdb.exe not found (install WinDbg)"; exit 1; fi

ROOT='C:\msys64\home\root\geany-webview'
BUILD="$ROOT\\build"
OUT="$ROOT\\crashdumps\\cdb-out.log"
DMP="$ROOT\\crashdumps\\copy-crash.dmp"
mkdir -p /c/msys64/home/root/geany-webview/crashdumps

echo ">>> Attaching cdb to geany.exe. Now reproduce the crash in Geany."
echo ">>> (This terminal will print 'Done' once the crash is captured.)"

# sxd av/eh: let benign first-chance exceptions pass; break only on the fatal
# (second-chance) one. On that break: dump all thread stacks + a full memory dump.
"$CDB" -pn geany.exe \
  -c ".sympath+ $BUILD; .reload /f geanywebview.dll; sxd av; sxd eh; g; .echo ============ CRASH CAUGHT ============; r; .echo ---- faulting thread ----; kbn 40; .echo ---- all threads ----; ~*kbn 25; .dump /ma $DMP; q" \
  > "/c/msys64/home/root/geany-webview/crashdumps/cdb-out.log" 2>&1

echo ">>> Done. Send me: crashdumps/cdb-out.log"

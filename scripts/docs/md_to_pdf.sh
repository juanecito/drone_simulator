#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Renders a Markdown document from docs/ to an A4 PDF, drawing its Mermaid diagrams.
#
# Usage: scripts/docs/md_to_pdf.sh [INPUT.md [OUTPUT.pdf]]
#   INPUT.md    defaults to docs/FRAME_PIPELINE.md
#   OUTPUT.pdf  defaults to INPUT with the .pdf extension
#
# Pipeline: pandoc (GitHub Markdown -> HTML) -> Mermaid in headless Chrome -> print to PDF.
# The page style is scripts/docs/pdf_print.css.
#
# Needs pandoc, curl and Google Chrome or Chromium (Debian: apt install pandoc chromium).
# Environment:
#   CHROME      browser binary (default: first of google-chrome, chromium, chromium-browser)
#   MERMAID_JS  local mermaid.min.js to use instead of the cached download
#
# Mermaid is downloaded once from jsDelivr into ${XDG_CACHE_HOME:-~/.cache}/drone_simulator
# and checked against a fixed SHA-256, so every PDF is drawn by the same Mermaid version.
set -euo pipefail

readonly MERMAID_VERSION=11.4.1
readonly MERMAID_SHA256=a43bc1afd446f9c4cc66ac5dd45d02e8d65e26fc5344ec0ef787f88d6ddb6f9e
readonly MERMAID_URL="https://cdn.jsdelivr.net/npm/mermaid@${MERMAID_VERSION}/dist/mermaid.min.js"
readonly RENDER_BUDGET_MS=30000

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

die() { echo "md_to_pdf: $*" >&2; exit 1; }

if [[ ${1:-} == -h || ${1:-} == --help ]]; then
    sed -n '5,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
fi
(( $# <= 2 )) || die "too many arguments (see --help)"

input=$(realpath "${1:-$repo_root/docs/FRAME_PIPELINE.md}")
[[ -f $input ]] || die "no such file: $input"
output=$(realpath -m "${2:-${input%.md}.pdf}")
css="$script_dir/pdf_print.css"

# --- Tools -------------------------------------------------------------------------------
command -v pandoc >/dev/null || die "pandoc not found (apt install pandoc)"
if [[ -z ${CHROME:-} ]]; then
    for candidate in google-chrome chromium chromium-browser; do
        if command -v "$candidate" >/dev/null; then CHROME=$candidate; break; fi
    done
fi
[[ -n ${CHROME:-} ]] || die "Chrome or Chromium not found (apt install chromium, or set CHROME)"

# --- Mermaid -----------------------------------------------------------------------------
if [[ -n ${MERMAID_JS:-} ]]; then
    mermaid_js=$(realpath "$MERMAID_JS")
    [[ -f $mermaid_js ]] || die "MERMAID_JS not found: $MERMAID_JS"
else
    cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/drone_simulator"
    mermaid_js="$cache_dir/mermaid-${MERMAID_VERSION}.min.js"
    if [[ ! -f $mermaid_js ]]; then
        command -v curl >/dev/null || die "curl not found (needed once to download Mermaid)"
        mkdir -p "$cache_dir"
        echo "Downloading Mermaid ${MERMAID_VERSION}..."
        curl -sSfL "$MERMAID_URL" -o "$mermaid_js.part"
        mv "$mermaid_js.part" "$mermaid_js"
    fi
    if ! echo "$MERMAID_SHA256  $mermaid_js" | sha256sum --check --status; then
        rm -f "$mermaid_js"
        die "Mermaid download failed its checksum and was removed; run again"
    fi
fi

# --- Work files --------------------------------------------------------------------------
# The HTML goes next to the input, so relative links and images resolve as in the Markdown.
work=$(mktemp -d)
html=$(mktemp "$(dirname "$input")/.md_to_pdf.XXXXXX.html")
trap 'rm -rf "$work" "$html"' EXIT

# Runs Mermaid on every diagram, then marks <body> with data-done="1", or "error" plus the
# message at the top of the page.
cat > "$work/after.html" <<EOF
<script src="file://${mermaid_js}"></script>
<script>
  for (const pre of document.querySelectorAll('pre.mermaid')) { pre.textContent = pre.textContent; }
  mermaid.initialize({ startOnLoad: false, theme: 'neutral', securityLevel: 'loose',
    flowchart: { useMaxWidth: true, htmlLabels: true, curve: 'basis' },
    sequence: { useMaxWidth: true }, fontFamily: 'DejaVu Sans, sans-serif' });
  mermaid.run({ querySelector: 'pre.mermaid' })
    .then(() => { document.body.dataset.done = '1'; })
    .catch(e => { document.body.dataset.done = 'error';
                  document.body.insertAdjacentText('afterbegin', 'MERMAID ERROR: ' + (e && e.message || e)); });
</script>
EOF

title=$(grep -m1 '^# ' "$input" | sed 's/^# *//')
title=${title:-$(basename "${input%.md}")}

# --- Markdown -> HTML --------------------------------------------------------------------
pandoc "$input" -f gfm -t html5 -s \
    --metadata title="$title" \
    --css "$css" \
    --include-after-body "$work/after.html" \
    -o "$html"

chrome() {
    "$CHROME" --headless=new --disable-gpu --no-sandbox --allow-file-access-from-files \
        --user-data-dir="$work/profile" --virtual-time-budget="$RENDER_BUDGET_MS" "$@" \
        2>"$work/chrome.log"
}

# --- Check the diagrams ------------------------------------------------------------------
diagrams=$(grep -c '<pre class="mermaid">' "$html" || true)
chrome --dump-dom "file://$html" > "$work/dom.html" \
    || die "Chrome failed, see its log:"$'\n'"$(cat "$work/chrome.log")"
if grep -q 'data-done="error"' "$work/dom.html"; then
    die "$(tr -s ' \n' '  ' < "$work/dom.html" | grep -o 'MERMAID ERROR: [^<]*' | head -1)"
fi
if (( diagrams > 0 )) && ! grep -q 'data-done="1"' "$work/dom.html"; then
    die "Mermaid did not finish within ${RENDER_BUDGET_MS} ms"
fi
if grep -q 'Unsupported markdown' "$work/dom.html"; then
    die "a diagram label starts like a Markdown list (\"1. \"); reword it"
fi

# --- HTML -> PDF -------------------------------------------------------------------------
mkdir -p "$(dirname "$output")"
chrome --no-pdf-header-footer --print-to-pdf="$output" "file://$html" \
    || die "Chrome failed, see its log:"$'\n'"$(cat "$work/chrome.log")"
[[ -s $output ]] || die "Chrome wrote no PDF"

pages=""
if command -v pdfinfo >/dev/null; then
    pages=", $(pdfinfo "$output" | awk '/^Pages:/ {print $2}') pages"
fi
echo "Wrote ${output#"$repo_root"/} (${diagrams} diagrams${pages})"

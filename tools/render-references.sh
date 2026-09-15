#!/bin/sh
# Renders every fixture to host/out/ref/<name>.png the way a browser would (resvg + Roboto).
set -e
cd "$(dirname "$0")/.."
mkdir -p host/out/ref
for f in test/fixtures/*.json; do
  n=$(basename "$f" .json)
  node tools/render-reference.mjs "$f" -o "host/out/ref/$n.svg"
  resvg --use-fonts-dir tools/fonts --font-family Roboto --width 1280 --height 720 "host/out/ref/$n.svg" "host/out/ref/$n.png"
done
echo "references in host/out/ref"

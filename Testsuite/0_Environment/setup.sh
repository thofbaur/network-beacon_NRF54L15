#!/bin/sh
# macOS / Linux: create the test environment in Testsuite/0_Environment/.venv
# and install the git pre-commit hook (regression check before every commit).
set -e
cd "$(dirname "$0")"
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -r requirements.txt

if hooks="$(git rev-parse --git-path hooks 2>/dev/null)"; then
    target="$hooks/pre-commit"
    if [ -f "$target" ] && ! cmp -s "$target" pre-commit; then
        cp "$target" "$target.bak"
        echo "Existing pre-commit hook saved as $target.bak"
    fi
    mkdir -p "$hooks"
    cp pre-commit "$target"
    chmod +x "$target"
    echo "Installed pre-commit hook: $target"
else
    echo "Not a git checkout: pre-commit hook not installed."
fi
echo "Done. From Testsuite/ run: 0_Environment/.venv/bin/python -m pytest"

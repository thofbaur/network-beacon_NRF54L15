#!/bin/sh
# macOS / Linux: create the test environment in Testsuite/0_Environment/.venv
set -e
cd "$(dirname "$0")"
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -r requirements.txt
echo "Done. From Testsuite/ run: 0_Environment/.venv/bin/python -m pytest"

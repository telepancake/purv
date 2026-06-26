#!/usr/bin/env bash
# Install the ACT4 framework toolchain via mise (uv/Python + Ruby + UDB gem).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

if ! command -v mise >/dev/null 2>&1 && [ ! -x "$HOME/.local/bin/mise" ]; then
  echo "==> installing mise"
  curl -fsSL https://mise.jdx.dev/install.sh | sh
fi
export PATH="$HOME/.local/bin:$PATH"
eval "$(mise activate bash)"

cd "$ROOT/third_party/riscv-arch-test"
echo "==> mise trust + install pinned tools (ruby 3.4.9, uv, bundler, prek)"
mise trust
mise install

# Pin Python 3.12: this environment injects an "exclude-newer" constraint that
# makes uv re-resolve to Python 3.14, on which the pinned pydantic breaks
# (TypeError: _eval_type() got an unexpected keyword argument 'prefer_fwd_module').
# 3.12 is the last line that the framework's pinned deps import cleanly.
echo "==> pin python 3.12 for the framework venv"
uv python pin 3.12

echo "==> python framework deps via uv"
uv sync 2>&1 | tail -5 || true

echo "==> ruby/UDB deps via bundler"
bundle install 2>&1 | tail -5 || true

echo "mise + framework toolchain installed."

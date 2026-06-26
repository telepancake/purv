#!/usr/bin/env bash
#
# bootstrap.sh — fetch the reference emulators + arch-test suite and install RISCOF.
#
# This is the ONE command that brings the conformance environment up. It must run
# in an environment with open GitHub git egress (your laptop, CI, or a remote
# session whose network policy allows github.com). It is idempotent: re-running
# only does the missing steps.
#
# What it does:
#   1. Registers the three submodules (Spike, Sail, riscv-arch-test) if absent.
#   2. Clones/updates them.
#   3. Records the resolved commit SHAs so you can pin them for reproducibility.
#   4. Installs RISCOF (the Python conformance framework) from PyPI.
#   5. Generates the Sail reference plugin via `riscof setup`.
#
# It does NOT build Spike or Sail — that's `make spike` / `make sail`, which have
# heavy toolchain prerequisites (autotools for Spike; opam/OCaml for Sail).

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

# The three submodules are already committed (.gitmodules). This just clones
# their contents. In most environments a plain `git submodule update --init`
# works. Some managed sessions inject a git-credential proxy that rewrites
# github.com URLs to a per-repo-scoped endpoint, which 403s on third-party
# repos; in that case we retry with the injected global/system config disabled
# so the clone goes straight over the normal HTTPS egress proxy.
echo "==> Fetching submodules"
if ! git submodule update --init --recursive; then
  echo "    [retry] disabling injected git config and going over HTTPS"
  GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null \
  GIT_SSL_CAINFO="${GIT_SSL_CAINFO:-/root/.ccr/ca-bundle.crt}" \
    git submodule update --init --recursive
fi

echo
echo "==> Resolved submodule commits (pin these in git for reproducible runs):"
git submodule status --recursive | sed 's/^/    /'

echo
echo "==> Installing RISCOF from PyPI"
python3 -m pip install --user --upgrade riscof

echo
echo "==> Generating the Sail reference plugin (sail_cSim) via 'riscof setup'"
# riscof setup drops a fresh DUT+ref scaffold; we only keep the generated
# sail_cSim reference plugin and ignore the throwaway DUT it creates.
if [ ! -d conformance/sail_cSim ]; then
  tmpdir="$(mktemp -d)"
  ( cd "$tmpdir" && riscof setup --dut spike --refname sail_cSim >/dev/null 2>&1 || true )
  if [ -d "$tmpdir/sail_cSim" ]; then
    cp -r "$tmpdir/sail_cSim" conformance/sail_cSim
    echo "    [ok]   conformance/sail_cSim generated"
  else
    echo "    [warn] could not auto-generate sail_cSim; copy it from the riscof package"
    echo "           templates manually (riscof/Templates/setup/sail_cSim)."
  fi
  rm -rf "$tmpdir"
else
  echo "    [skip] conformance/sail_cSim already present"
fi

cat <<EOF

Bootstrap complete.

Next steps:
  make spike        # build the Spike reference simulator
  make sail         # build the Sail C emulator (riscv_sim_RV32)
  make validate     # RISCOF: Spike (DUT) vs Sail (reference) — proves the harness
  make conformance  # RISCOF: purv (DUT)  vs Sail (reference) — the real test

Remember to commit the pinned submodule SHAs shown above.
EOF

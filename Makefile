# purv — RISC-V (RV32IMC) emulator, plus a conformance harness.
#
# Emulator & demos (the purv work lives in emu/):
#   make emu          build the purv emulator  (emu/purv)
#   make emu-test     build + run the emulator examples
#   make secure       tagged-memory pointer-safety demo (emu/purvs)
#   make sqlite       freestanding SQLite running on purv (fetch + build + run)
#   make compact      447-byte no-runtime demo: fixed-point Mandelbrot
#
# Conformance harness (RISCOF vs the Sail reference model):
#   make bootstrap    fetch submodules + install riscof  (needs open git egress)
#   make spike        build Spike (differential oracle)
#   make sail-dl      fetch the prebuilt Sail 0.12 reference model  (default `make sail`)
#   make sail-build   build the pinned submodule from source (needs the Sail toolchain)
#   make validate     RISCOF: Spike (DUT) vs Sail (reference) — proves the harness
#   make conformance  RISCOF: purv  (DUT) vs Sail (reference) — the real run

ROOT        := $(CURDIR)
TP          := $(ROOT)/third_party
SPIKE_DIR   := $(TP)/riscv-isa-sim
SAIL_SRC    := $(TP)/sail-riscv          # submodule (source); not used to build the model
ARCHTEST    := $(TP)/riscv-arch-test
CONF        := $(ROOT)/conformance
WORK        := $(ROOT)/riscof_work

# Reference model, two ways:
#   sail-dl    : the prebuilt Sail release 0.12 binary -- exactly the version the
#                ACT4/RISCOF golden signatures were computed with.
#   sail-build : build the pinned submodule from source. NOTE the submodule pin
#                (third_party/sail-riscv) is 0.12 + 19 commits, so the from-source
#                binary is NOT identical to the 0.12 the signatures use -- re-pin
#                the submodule to tag 0.12 (commit 65ddde80) if you need a match.
SAIL_VER       := 0.12
SAIL_DL_DIR    := $(ROOT)/tools/sail-$(SAIL_VER)
SAIL_DL_BIN    := $(SAIL_DL_DIR)/bin/sail_riscv_sim
SAIL_BUILD_BIN := $(SAIL_SRC)/build/c_emulator/sail_riscv_sim
# Which binary validate/conformance gate on (override to use the from-source one,
# e.g. `make conformance SAIL_BIN=$(SAIL_BUILD_BIN)`):
SAIL_BIN       ?= $(SAIL_DL_BIN)

# Built artifact Spike's plugin invokes.
SPIKE_BIN   := $(SPIKE_DIR)/build/spike

.PHONY: help emu emu-test secure sqlite compact bootstrap spike sail sail-dl sail-build validate conformance clean distclean

help:
	@sed -n '1,16p' $(MAKEFILE_LIST)

# --- The emulator and demos (the actual purv work) ------------------------

emu:
	$(MAKE) -C emu

emu-test:
	$(MAKE) -C emu test

secure:
	$(MAKE) -C emu/purvs test

sqlite:
	$(MAKE) -C emu sqlite

compact:
	$(MAKE) -C emu compact

# --- Reference emulators --------------------------------------------------

spike: $(SPIKE_BIN)
$(SPIKE_BIN):
	@echo "==> Building Spike (riscv-isa-sim)"
	mkdir -p $(SPIKE_DIR)/build
	cd $(SPIKE_DIR)/build && ../configure && $(MAKE) -j

# Default `make sail` is the download (lighter, version-matched to the harness).
sail: sail-dl

# Prebuilt Sail 0.12 release binary -> tools/sail-0.12/bin/sail_riscv_sim.
sail-dl: $(SAIL_DL_BIN)
$(SAIL_DL_BIN):
	@echo "==> Fetching prebuilt Sail $(SAIL_VER) reference model (sail_riscv_sim)"
	@mkdir -p $(SAIL_DL_DIR)
	@os=$$(uname); [ "$$os" = Darwin ] && os=Mac; \
	 asset="sail-riscv-$$os-$$(uname -m).tar.gz"; \
	 url="https://github.com/riscv/sail-riscv/releases/download/$(SAIL_VER)/$$asset"; \
	 echo "    $$url"; \
	 curl -fSL -o /tmp/sail-$(SAIL_VER).tgz "$$url" || { \
	   echo "ERROR: no prebuilt Sail $(SAIL_VER) asset '$$asset' for this platform."; \
	   echo "       Published: Linux-x86_64, Linux-aarch64, Mac-arm64. Use 'make sail-build'."; \
	   exit 1; }; \
	 tar xzf /tmp/sail-$(SAIL_VER).tgz --directory=$(SAIL_DL_DIR) --strip-components=1
	@$(SAIL_DL_BIN) --version

# Build the reference model from the pinned submodule. Needs the Sail compiler
# (>= 0.20.1) and a C/C++ toolchain; build_simulator.sh drives CMake and fetches
# libgmp. See the Arch package list in conformance/README.md. RV32 vs RV64 is a
# runtime --config, so this one binary serves both.
sail-build: $(SAIL_BUILD_BIN)
$(SAIL_BUILD_BIN):
	@echo "==> Building Sail reference model from the pinned submodule"
	cd $(ROOT) && git submodule update --init -- third_party/sail-riscv
	cd $(SAIL_SRC) && ./build_simulator.sh
	@$(SAIL_BUILD_BIN) --version

# --- RISCOF runs ----------------------------------------------------------

bootstrap:
	./scripts/bootstrap.sh

# Spike (DUT) vs Sail (reference): no purv required. This is the bring-up run
# that proves the suite, plugins, linker bracketing and signature extraction
# are all correct before purv exists.
validate: $(SPIKE_BIN) $(SAIL_BIN)
	cd $(CONF) && riscof run \
	    --config=config.bringup.ini \
	    --suite=$(ARCHTEST)/riscv-test-suite \
	    --env=$(ARCHTEST)/riscv-test-suite/env \
	    --work-dir=$(WORK)

# purv (DUT) vs Sail (reference): the actual conformance test.
conformance: $(SAIL_BIN)
	cd $(CONF) && riscof run \
	    --config=config.purv.ini \
	    --suite=$(ARCHTEST)/riscv-test-suite \
	    --env=$(ARCHTEST)/riscv-test-suite/env \
	    --work-dir=$(WORK)

clean:
	rm -rf $(WORK)

distclean: clean
	rm -rf $(SPIKE_DIR)/build $(SAIL_DIR)

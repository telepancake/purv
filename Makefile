# purv — RISC-V (RV32IMC) emulator, plus a conformance harness.
#
# Emulator & demos (the purv work lives in emu/):
#   make emu          build the purv emulator  (emu/purv)
#   make emu-test     build + run the emulator examples
#   make secure       tagged-memory pointer-safety demo (emu/purvs)
#   make sqlite       freestanding SQLite running on purv (fetch + build + run)
#
# Conformance harness (RISCOF vs the Sail reference model):
#   make bootstrap    fetch submodules + install riscof  (needs open git egress)
#   make spike        build Spike (differential oracle)
#   make sail         fetch the prebuilt Sail 0.12 reference model
#   make validate     RISCOF: Spike (DUT) vs Sail (reference) — proves the harness
#   make conformance  RISCOF: purv  (DUT) vs Sail (reference) — the real run

ROOT        := $(CURDIR)
TP          := $(ROOT)/third_party
SPIKE_DIR   := $(TP)/riscv-isa-sim
SAIL_SRC    := $(TP)/sail-riscv          # submodule (source); not used to build the model
ARCHTEST    := $(TP)/riscv-arch-test
CONF        := $(ROOT)/conformance
WORK        := $(ROOT)/riscof_work

# Reference model: the prebuilt Sail release 0.12, which is what ACT4/RISCOF
# actually consumes. The sail-riscv submodule HEAD is the newer CMake
# single-binary build and is NOT it (see conformance/STATUS.md), so `make sail`
# fetches the release binary rather than building the heavy OCaml/opam source.
SAIL_VER    := 0.12
SAIL_DIR    := $(ROOT)/tools/sail-$(SAIL_VER)
SAIL_BIN    := $(SAIL_DIR)/bin/sail_riscv_sim

# Built artifact Spike's plugin invokes.
SPIKE_BIN   := $(SPIKE_DIR)/build/spike

.PHONY: help emu emu-test secure sqlite bootstrap spike sail validate conformance clean distclean

help:
	@sed -n '1,14p' $(MAKEFILE_LIST)

# --- The emulator and demos (the actual purv work) ------------------------

emu:
	$(MAKE) -C emu

emu-test:
	$(MAKE) -C emu test

secure:
	$(MAKE) -C emu/purvs test

sqlite:
	$(MAKE) -C emu sqlite

# --- Reference emulators --------------------------------------------------

spike: $(SPIKE_BIN)
$(SPIKE_BIN):
	@echo "==> Building Spike (riscv-isa-sim)"
	mkdir -p $(SPIKE_DIR)/build
	cd $(SPIKE_DIR)/build && ../configure && $(MAKE) -j

sail: $(SAIL_BIN)
$(SAIL_BIN):
	@echo "==> Fetching prebuilt Sail $(SAIL_VER) reference model (sail_riscv_sim)"
	@mkdir -p $(SAIL_DIR)
	@os=$$(uname); [ "$$os" = Darwin ] && os=Mac; \
	 asset="sail-riscv-$$os-$$(uname -m).tar.gz"; \
	 url="https://github.com/riscv/sail-riscv/releases/download/$(SAIL_VER)/$$asset"; \
	 echo "    $$url"; \
	 curl -fSL -o /tmp/sail-$(SAIL_VER).tgz "$$url" || { \
	   echo "ERROR: no prebuilt Sail $(SAIL_VER) asset '$$asset' for this platform."; \
	   echo "       Published: Linux-x86_64, Linux-aarch64, Mac-arm64."; \
	   echo "       Build from source instead (needs opam + the sail compiler):"; \
	   echo "         git submodule update --init $(SAIL_SRC)"; \
	   echo "         cmake -S $(SAIL_SRC) -B $(SAIL_SRC)/build && cmake --build $(SAIL_SRC)/build"; \
	   exit 1; }; \
	 tar xzf /tmp/sail-$(SAIL_VER).tgz --directory=$(SAIL_DIR) --strip-components=1
	@$(SAIL_BIN) --version

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

# purv conformance harness
#
# Bring-up:   make bootstrap   (fetch submodules + install riscof; needs open git egress)
#             make spike sail  (build the two reference emulators)
# Validate:   make validate    (RISCOF: Spike-DUT vs Sail-ref  — proves the harness works)
# Conform:    make conformance (RISCOF: purv-DUT  vs Sail-ref  — the real conformance run)

ROOT        := $(CURDIR)
TP          := $(ROOT)/third_party
SPIKE_DIR   := $(TP)/riscv-isa-sim
SAIL_DIR    := $(TP)/sail-riscv
ARCHTEST    := $(TP)/riscv-arch-test
CONF        := $(ROOT)/conformance
WORK        := $(ROOT)/riscof_work

# Built artifacts the plugins invoke.
SPIKE_BIN   := $(SPIKE_DIR)/build/spike
SAIL_BIN    := $(SAIL_DIR)/c_emulator/riscv_sim_RV32

.PHONY: help bootstrap spike sail validate conformance clean distclean

help:
	@sed -n '1,8p' $(MAKEFILE_LIST)

bootstrap:
	./scripts/bootstrap.sh

# --- Reference emulators --------------------------------------------------

spike: $(SPIKE_BIN)
$(SPIKE_BIN):
	@echo "==> Building Spike (riscv-isa-sim)"
	mkdir -p $(SPIKE_DIR)/build
	cd $(SPIKE_DIR)/build && ../configure && $(MAKE) -j

# Sail emits a C model that compiles to riscv_sim_RV32. Needs opam/OCaml + the
# sail compiler on PATH; see third_party/sail-riscv/README for the toolchain.
sail: $(SAIL_BIN)
$(SAIL_BIN):
	@echo "==> Building Sail C emulator (riscv_sim_RV32)"
	$(MAKE) -C $(SAIL_DIR) ARCH=RV32 c_emulator/riscv_sim_RV32

# --- RISCOF runs ----------------------------------------------------------

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
	rm -rf $(SPIKE_DIR)/build
	-$(MAKE) -C $(SAIL_DIR) clean

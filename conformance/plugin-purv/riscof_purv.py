# RISCOF DUT plugin for the purv emulator.
#
# Follows the standard riscof pluginTemplate contract. The only purv-specific
# bits are marked TODO: how to invoke the emulator and where it dumps the
# signature. Everything else is the conventional flow shared by every DUT plugin.

import os
import shutil
import logging

import riscof.utils as utils
from riscof.pluginTemplate import pluginTemplate

logger = logging.getLogger()


class purv(pluginTemplate):
    __model__ = "purv"
    __version__ = "0.1.0"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        config = kwargs.get("config")
        if config is None:
            raise SystemExit("[purv] no [purv] section in config.ini")

        # Path to the purv binary. TODO: point this at the built emulator.
        self.dut_exe = os.path.join(config.get("PATH", ""), "purv")
        self.num_jobs = str(config.get("jobs", 1))
        self.pluginpath = os.path.abspath(config["pluginpath"])
        self.isa_spec = os.path.abspath(config["ispec"])
        self.platform_spec = os.path.abspath(config["pspec"])
        self.target_run = config.get("target_run", "1") != "0"

    def initialise(self, suite, work_dir, archtest_env):
        self.work_dir = work_dir
        self.suite_dir = suite
        # Compile command template. {0}=march, {1}=source.S, {2}=elf, {3}=defines
        self.compile_cmd = (
            "riscv{1}-unknown-elf-gcc -march={0} -mabi=ilp32 "
            "-static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles "
            "-T " + os.path.join(self.pluginpath, "env", "link.ld") + " "
            "-I " + os.path.join(self.pluginpath, "env") + " "
            "-I " + archtest_env + " {2} -o {3} {4}"
        )

    def build(self, isa_yaml, platform_yaml):
        ispec = utils.load_yaml(isa_yaml)["hart0"]
        self.xlen = "32" if 32 in ispec["supported_xlen"] else "64"
        self.isa = "rv" + self.xlen
        if "I" in ispec["ISA"]:
            self.isa += "i"
        if "M" in ispec["ISA"]:
            self.isa += "m"
        if "C" in ispec["ISA"]:
            self.isa += "c"
        self.compile_cmd = self.compile_cmd + (
            " -mabi=" + ("ilp32" if self.xlen == "32" else "lp64")
        )

    def runTests(self, testList):
        make = utils.makeUtil(
            makefilePath=os.path.join(self.work_dir, "Makefile." + self.name[:-1])
        )
        make.makeCommand = "make -k -j" + self.num_jobs

        for test, entry in testList.items():
            testname = entry["test_path"]
            test_dir = entry["work_dir"]
            elf = "purv.elf"
            sig_file = os.path.join(test_dir, self.name[:-1] + ".signature")

            march = entry["isa"].lower()
            defines = " ".join("-D" + d for d in entry["macros"])
            cmd_compile = self.compile_cmd.format(
                march, self.xlen, testname, elf, defines
            )

            # TODO(purv): replace with purv's real CLI. The contract is:
            #   run <elf>, write the signature memory range to <sig_file>,
            #   one 4-byte word per line, hex, no 0x prefix — matching what
            #   the Sail/Spike reference plugins emit.
            cmd_run = "{0} --signature={1} --signature-granularity=4 {2}".format(
                self.dut_exe, sig_file, elf
            )

            execute = "@cd {0}; {1}; {2}".format(
                test_dir, cmd_compile, cmd_run if self.target_run else "true"
            )
            make.add_target(execute)

        make.execute_all(self.work_dir)

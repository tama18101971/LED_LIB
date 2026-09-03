# SPDX-License-Identifier: MIT
#
# Custom PlatformIO test runner for LED_LIB host tests.
#
# The test suite (test/test_led_lib/test_led_lib.c) is self-contained:
# it has its own main() and reports failures via its exit code. This
# runner compiles it together with src/led.c using a host C compiler
# and executes the binary. Requires any C99 compiler in PATH (gcc /
# clang / mingw), overridden with the CC environment variable.
#
# Usage:  pio test -e native

import os
import subprocess

from platformio.test.result import TestCase, TestStatus

TEST_SOURCES = ["test/test_led_lib/test_led_lib.c", "src/led.c"]
BUILD_DIR = ".pio" + os.sep + "test_build"


class CustomTestRunner:
    """Duck-typed runner: PlatformIO calls start(cmd_ctx) only."""

    NAME = "custom"
    EXTRA_LIB_DEPS = None

    def __init__(self, test_suite, project_config, options=None):
        self.test_suite = test_suite
        self.project_config = project_config
        self.options = options

    # -- PlatformIO entry point --------------------------------------

    def start(self, cmd_ctx):  # pylint: disable=unused-argument
        self.test_suite.on_start()
        try:
            self.test_suite.add_case(self._build_and_run())
        except Exception as exc:  # pylint: disable=broad-except
            self.test_suite.add_case(
                TestCase(
                    name="%s:%s" % (self.test_suite.env_name, self.test_suite.test_name),
                    status=TestStatus.ERRORED,
                    exception=exc,
                )
            )
        finally:
            self.test_suite.on_finish()

    # -- helpers ------------------------------------------------------

    def _build_and_run(self):
        os.makedirs(BUILD_DIR, exist_ok=True)
        binary = BUILD_DIR + os.sep + self.test_suite.test_name
        if os.name == "nt":
            binary += ".exe"
        self._build(binary)
        process = subprocess.run(  # pylint: disable=unexpected-keyword-arg
            [binary], capture_output=True, text=True
        )
        if process.stdout:
            print(process.stdout, end="")
        if process.stderr:
            print(process.stderr, end="")

        return TestCase(
            name="%s:%s" % (self.test_suite.env_name, self.test_suite.test_name),
            status=TestStatus.PASSED if process.returncode == 0 else TestStatus.FAILED,
            stdout=process.stdout or "",
            message=None
            if process.returncode == 0
            else "exit code %d" % process.returncode,
        )

    @staticmethod
    def _build(binary):
        import click

        flags = ["-std=c99", "-Wall", "-Wextra", "-Wconversion", "-Wpedantic"]
        cmd = (
            [os.environ.get("CC", "gcc")]
            + flags
            + ["-Iinclude"]
            + TEST_SOURCES
            + ["-o", binary]
        )
        click.secho("Compiling %s..." % " ".join(cmd), bold=True)
        try:
            process = subprocess.run(  # pylint: disable=unexpected-keyword-arg
                cmd, capture_output=True, text=True
            )
        except FileNotFoundError:
            raise RuntimeError(
                "host C compiler '%s' not found in PATH. Install gcc/clang/mingw "
                "or set the CC environment variable." % os.environ.get("CC", "gcc")
            )
        if process.returncode != 0:
            raise RuntimeError(
                "host build failed:\n%s%s" % (process.stdout, process.stderr)
            )
        if process.stderr:
            print(process.stderr, end="")

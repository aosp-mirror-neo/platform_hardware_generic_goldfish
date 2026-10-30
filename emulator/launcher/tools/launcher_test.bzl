"""Bazel rules for launching and testing Android emulators."""

load("@rules_python//python:defs.bzl", "py_test")

def create_launch_emulator_test(
        name,
        target_log_line = None,
        timeout_seconds = None,
        repeat = 0,
        params = None,
        count_log_pattern = None,
        expected_occurrences = None,
        target_compatible_with = None):
    """Tests that the emulator can launch and that the given target_log_line regular expression is logged.

    This macro creates a `py_test` target configured to launch an Android emulator
    and verify its startup behavior. It automatically selects the correct
    architecture (ARM64 for macOS, x86_64 for others) and includes the necessary
    system images.

    Note that tests will use the tag: `resources:exclusive_test:1` this will force the test to be run
    in the "exclusive" mode if it is executed locally, but will run the test in parallel if it's
    executed remotely.

    Args:
        name: A unique name for this test target.
        target_log_line: A regular expression string. The
            test will pass only if this pattern is found in the emulator's logs.
        repeat: An optional integer specifying the number of times to repeat the test.
        timeout_seconds: An optional integer specifying the maximum time in seconds
            to wait for the emulator to launch and the log line (if specified) to appear.
        params: Additional parameters that need to be passed on to the emulator launcher.
        count_log_pattern: An optional regular expression pattern to count in emulator logs.
        expected_occurrences: An optional integer specifying the exact expected count of count_log_pattern.
        target_compatible_with: An optional target platform constraint list or select expression.
    """
    args = ["--target_log_line", target_log_line, "--repeat", str(repeat)]
    if count_log_pattern:
        args += ["--count_log_pattern", count_log_pattern]
    if expected_occurrences != None:
        args += ["--expected_occurrences", str(expected_occurrences)]
    if params:
        args += params

    # We don't want to launch the fishtank UI process for these tests.
    args.append("-no-window")
    if timeout_seconds:
        args = args + select({
            "@goldfish//emulator/tools:asan": ["--timeout_seconds", str(timeout_seconds * 3)],
            "@goldfish//emulator/tools:tsan": ["--timeout_seconds", str(timeout_seconds * 10)],
            "@platforms//os:macos": ["--timeout_seconds", str(max(timeout_seconds, 300))],
            "//conditions:default": ["--timeout_seconds", str(timeout_seconds)],
        })
    _create_launch_emulator_test(
        name,
        args,
        "@goldfish//emulator/launcher",
        target_compatible_with = target_compatible_with,
    )
    _create_launch_emulator_test(
        name + "_zip",
        args + ["--use_zip"],
        "@goldfish//emulator:release",
        target_compatible_with = target_compatible_with,
    )

def _create_launch_emulator_test(name, args, goldfish_dep, target_compatible_with = None):
    py_test(
        name = name,
        size = "large",
        tags = [
            # These tests are marked as manual so that they aren't included when using //...
            # Instead they must be explicitly named (including as part of a test_suite).
            "manual",
            "exclusive",
            # "exclusive-if-local" fails to parallelize on RBE
            # https://github.com/bazelbuild/bazel/issues/17834
            "resources:qemu_instances:1",
            "requires-network",
        ],
        args = select({
            "@platforms//os:macos": [
                "--abi",
                "arm64-v8a",
            ],
            "//conditions:default": [
                "--abi",
                "x86_64",
            ],
        }) + args + [
            "--disable-crash-reporting",
            "-verbose",
            "-read-only",
        ],
        main_module = "launch_emulator",
        deps = ["@goldfish//emulator/launcher:launch_emulator"],
        data = [goldfish_dep],
        imports = ["tools"],
        target_compatible_with = select({
            "@platforms//os:windows": ["@platforms//:incompatible"],
            "//conditions:default": [],
        }) + (target_compatible_with or select({
            "//conditions:default": [],
        })),
    )

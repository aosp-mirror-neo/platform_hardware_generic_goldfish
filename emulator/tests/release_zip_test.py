import argparse
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

from python.runfiles import Runfiles

ARGS = argparse.Namespace()

KNOWN_EXECUTABLES = {
    "emulator/emulator",
    "emulator/emulator-check",
    "emulator/mksdcard",
}

EMULATOR_CHECK_TIMEOUT_SEC = 30
POSIX_MODE_SHIFT = 16
POSIX_PERM_MASK = 0o777
DEFAULT_EXEC_MODE = 0o755
SHARED_LIB_SUFFIXES = (".dll", ".dylib", ".so")
LOADER_ENV_VARS = (
    "LD_LIBRARY_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FALLBACK_LIBRARY_PATH",
)


def find_symlinks(zf: zipfile.ZipFile) -> list[tuple[str, str]]:
    """Returns a list of (filename, link_target) for all symbolic link entries in the archive."""
    symlinks: list[tuple[str, str]] = []
    for info in zf.infolist():
        mode = info.external_attr >> 16
        if stat.S_ISLNK(mode):
            try:
                target = zf.read(info.filename).decode("utf-8", errors="replace")
            except Exception:
                target = "<unreadable>"
            symlinks.append((info.filename, target))
    return symlinks


def find_missing_exec_bits(zf: zipfile.ZipFile) -> list[tuple[str, int]]:
    """Returns a list of (filename, mode) for binaries missing executable permission bits."""
    missing: list[tuple[str, int]] = []
    for info in zf.infolist():
        mode = info.external_attr >> 16
        # Skip entries that do not carry POSIX file mode attributes
        if not mode:
            continue
        if (
            info.filename in KNOWN_EXECUTABLES
            or info.filename.startswith("emulator/bin/")
        ) and not info.filename.endswith("/"):
            if not (mode & 0o111):
                missing.append((info.filename, mode))
    return missing


def _resolve_path(path: str) -> str:
    """Resolves path using Bazel runfiles."""
    runfiles = Runfiles.Create()
    return runfiles.Rlocation(path)


def _is_root_shared_library(filename: str) -> bool:
    """Returns True if filename is a shared library directly under emulator/."""
    if not filename.startswith("emulator/") or filename.endswith("/"):
        return False
    rel_name = filename.removeprefix("emulator/")
    if "/" in rel_name:
        return False
    lower_name = rel_name.lower()
    return lower_name.endswith(SHARED_LIB_SUFFIXES) or ".so." in lower_name


def _is_emulator_check_runtime_entry(filename: str, binary_entry: str) -> bool:
    """Returns True if the zip entry is needed to load and run emulator-check."""
    if filename.endswith("/"):
        return False
    return (
        filename == binary_entry
        or filename.startswith("emulator/lib64/")
        or _is_root_shared_library(filename)
    )


def _restore_posix_permissions(
    extracted_path: Path, external_attr: int, is_binary: bool
) -> None:
    """Restores POSIX mode bits from ZipInfo.external_attr on non-Windows hosts."""
    if sys.platform == "win32":
        return
    mode = (external_attr >> POSIX_MODE_SHIFT) & POSIX_PERM_MASK
    if mode:
        os.chmod(extracted_path, mode)
    elif is_binary:
        os.chmod(extracted_path, DEFAULT_EXEC_MODE)


def _extract_emulator_check_layout(
    zf: zipfile.ZipFile, dest_dir: Path, binary_entry: str
) -> Path:
    """Extracts emulator-check and its runtime shared libraries into dest_dir."""
    for info in zf.infolist():
        if not _is_emulator_check_runtime_entry(info.filename, binary_entry):
            continue
        extracted = Path(zf.extract(info, path=dest_dir))
        _restore_posix_permissions(
            extracted,
            info.external_attr,
            is_binary=(info.filename == binary_entry),
        )
    return dest_dir / binary_entry


def _sanitized_loader_env() -> dict[str, str]:
    """Returns a copy of os.environ without dynamic-linker search path overrides."""
    env = os.environ.copy()
    for var in LOADER_ENV_VARS:
        env.pop(var, None)
    return env


class ReleaseZipTest(unittest.TestCase):
    """Integration test verifying contents and packaging integrity of the release zip."""

    def test_contains_expected_files(self):
        self.assertNotEqual(ARGS.expected_path, "", msg="--expected_path must be provided")
        self.assertNotEqual(ARGS.release_zip_path, "", msg="--release_zip_path must be provided")
        expected_path = _resolve_path(ARGS.expected_path)
        zip_path = _resolve_path(ARGS.release_zip_path)
        with open(expected_path) as ef:
            expected = set(l.strip() for l in ef)
        with zipfile.ZipFile(zip_path) as zf:
            got = set(zf.namelist())
        error_msgs = []
        missing = expected - got
        if missing:
            error_msgs.append("Missing files: " + ','.join(sorted(missing)))
        unexpected = got - expected
        if unexpected:
            error_msgs.append("Unexpected files: " + ','.join(sorted(unexpected)))
        if error_msgs:
            self.fail("\n".join(error_msgs))

    def test_directory_permissions(self):
        self.assertNotEqual(ARGS.release_zip_path, "", msg="--release_zip_path must be provided")
        zip_path = _resolve_path(ARGS.release_zip_path)
        bad_dirs = []
        with zipfile.ZipFile(zip_path) as zf:
            for info in zf.infolist():
                if info.filename.endswith("/"):
                    mode = (info.external_attr >> 16) & 0o7777
                    if mode != 0o755:
                        bad_dirs.append(f"{info.filename} (mode: {oct(mode)})")
        if bad_dirs:
            self.fail(
                "Directories in release zip must have 0o755 permissions:\n  "
                + "\n  ".join(sorted(bad_dirs))
            )

    def test_does_not_contain_symlinks(self):
        """Verifies that the release archive contains no symbolic links."""
        self.assertNotEqual(
            ARGS.release_zip_path, "", msg="--release_zip_path must be provided"
        )
        zip_path = _resolve_path(ARGS.release_zip_path)
        with zipfile.ZipFile(zip_path) as zf:
            symlinks = find_symlinks(zf)

        if symlinks:
            sample = "\n  ".join(f"{name} -> {target}" for name, target in symlinks[:20])
            total = len(symlinks)
            suffix = f"\n  ... and {total - 20} more" if total > 20 else ""
            self.fail(
                f"Release archive contains {total} symbolic link(s) "
                f"(expected regular files only):\n  {sample}{suffix}"
            )

    def test_executables_have_exec_bit(self):
        """Verifies that primary binaries retain executable permissions when POSIX mode is present."""
        if sys.platform == "win32":
            return
        self.assertNotEqual(
            ARGS.release_zip_path, "", msg="--release_zip_path must be provided"
        )
        zip_path = _resolve_path(ARGS.release_zip_path)
        with zipfile.ZipFile(zip_path) as zf:
            missing = find_missing_exec_bits(zf)

        if missing:
            sample = "\n  ".join(f"{name} (mode {oct(mode)})" for name, mode in missing)
            self.fail(
                f"Release archive binaries missing executable bit:\n  {sample}"
            )

    def test_emulator_check_help(self):
        """Verifies emulator-check -help loads and runs from the packaged layout."""
        self.assertNotEqual(
            ARGS.release_zip_path, "", msg="--release_zip_path must be provided"
        )
        zip_path = _resolve_path(ARGS.release_zip_path)
        binary_entry = (
            "emulator/emulator-check.exe"
            if sys.platform == "win32"
            else "emulator/emulator-check"
        )
        with tempfile.TemporaryDirectory(prefix="emulator_check_") as tmp_dir:
            with zipfile.ZipFile(zip_path) as zf:
                extracted_binary = _extract_emulator_check_layout(
                    zf, Path(tmp_dir), binary_entry
                )
            self.assertTrue(
                extracted_binary.is_file(),
                msg=(
                    f"Expected binary '{binary_entry}' was not found after "
                    f"extracting '{zip_path}'. Ensure emulator-check is "
                    "included in //emulator:release."
                ),
            )
            try:
                result = subprocess.run(
                    [str(extracted_binary), "-help"],
                    capture_output=True,
                    encoding="utf-8",
                    errors="replace",
                    env=_sanitized_loader_env(),
                    timeout=EMULATOR_CHECK_TIMEOUT_SEC,
                    check=False,
                )
            except OSError as exc:
                self.fail(
                    f"Failed to execute '{binary_entry} -help' from packaged "
                    f"layout: {exc}. Verify executable permissions and target "
                    "architecture in //emulator:release."
                )

        self.assertEqual(
            result.returncode,
            0,
            msg=(
                f"Running '{binary_entry} -help' from packaged layout failed "
                f"with exit code {result.returncode} (expected 0).\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}\n"
                "Verify RPATH/RUNPATH ($ORIGIN/lib64 or @loader_path/lib64) "
                "and packaged shared libraries under emulator/ and emulator/lib64/."
            ),
        )
        self.assertIn(
            "Usage: emulator-check",
            result.stdout,
            msg=(
                f"Running '{binary_entry} -help' exited 0 but stdout did not "
                "contain 'Usage: emulator-check'.\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            ),
        )


def parse_args():
    """Parses the test arguments, removing these from sys.argv."""
    parser = argparse.ArgumentParser(
        description="Test the release zip contains the expected files."
    )
    parser.add_argument(
        "--release_zip_path",
        help="The path to the release zip to inspect.",
    )
    parser.add_argument(
        "--expected_path",
        help="Path to the list of expected files.",
    )
    global ARGS
    ARGS, extra_args = parser.parse_known_args()
    sys.argv = sys.argv[:1] + extra_args


if __name__ == "__main__":
    parse_args()
    unittest.main()


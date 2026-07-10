"""Focused security regression tests for the VSIX installer."""

from __future__ import annotations

import json
import os
import stat
import tempfile
import unittest
import zipfile
from unittest.mock import Mock, patch

from ai_editor import extensions


class ExtensionInstallerSecurityTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="sao_vsix_security_")
        self._source_tmp = tempfile.TemporaryDirectory(
            prefix="sao_extension_source_")
        self.root = self._tmp.name
        self._root_patch = patch.object(extensions, "_extensions_dir", return_value=self.root)
        self._root_patch.start()

    def tearDown(self) -> None:
        self._root_patch.stop()
        self._source_tmp.cleanup()
        self._tmp.cleanup()

    def _archive(self, entries: list[tuple[str, bytes]], *, compression: int = zipfile.ZIP_STORED) -> str:
        fd, path = tempfile.mkstemp(suffix=".vsix", dir=self.root)
        os.close(fd)
        with zipfile.ZipFile(path, "w", compression=compression) as zf:
            for name, payload in entries:
                zf.writestr(name, payload)
        return path

    @staticmethod
    def _manifest(ext_id: str = "security.fixture") -> bytes:
        publisher, name = ext_id.split(".", 1)
        return json.dumps({
            "publisher": publisher,
            "name": name,
            "version": "1.0.0",
        }).encode("utf-8")

    def _install_archive(self, archive: str, ext_id: str = "security.fixture") -> dict:
        with patch.object(extensions, "download_vsix", return_value=archive):
            return extensions.install_extension(ext_id, "https://example.invalid/fixture.vsix")

    def _local_source(self, files: dict[str, bytes] | None = None) -> str:
        source = os.path.join(
            self._source_tmp.name, f"source-{len(os.listdir(self._source_tmp.name))}")
        os.makedirs(source)
        payloads = {"package.json": self._manifest(), **(files or {})}
        for relative, payload in payloads.items():
            path = os.path.join(source, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(payload)
        return source

    def _symlink_archive(self) -> str:
        fd, path = tempfile.mkstemp(suffix=".vsix", dir=self.root)
        os.close(fd)
        with zipfile.ZipFile(path, "w") as zf:
            zf.writestr("extension/package.json", self._manifest())
            link = zipfile.ZipInfo("extension/link")
            link.create_system = 3
            link.external_attr = (stat.S_IFLNK | 0o777) << 16
            zf.writestr(link, "target.txt")
        return path

    def test_install_rejects_parent_traversal_without_replacing_existing_extension(self) -> None:
        ext_dir = os.path.join(self.root, "security.fixture")
        os.makedirs(ext_dir)
        marker = os.path.join(ext_dir, "installed.txt")
        with open(marker, "w", encoding="utf-8") as fh:
            fh.write("keep")

        archive = self._archive([
            ("extension/package.json", self._manifest()),
            ("extension/../../escaped.txt", b"escape"),
        ])
        result = self._install_archive(archive)

        self.assertIn("error", result)
        self.assertTrue(os.path.isfile(marker))
        self.assertFalse(os.path.exists(os.path.join(self.root, "escaped.txt")))

    def test_unsafe_extension_ids_are_rejected_before_download_or_delete(self) -> None:
        downloader = Mock(side_effect=AssertionError("download should not run"))
        invalid_ids = (
            "../escape",
            "publisher/escape.name",
            "C:drive.name",
            "publisher..name",
            "CON.fixture",
            " publisher.name",
        )
        with patch.object(extensions, "download_vsix", downloader):
            for ext_id in invalid_ids:
                with self.subTest(ext_id=ext_id):
                    installed = extensions.install_extension(
                        ext_id, "https://example.invalid/fixture.vsix"
                    )
                    removed = extensions.uninstall_extension(ext_id)
                    self.assertIn("error", installed)
                    self.assertFalse(removed.get("ok"))
        downloader.assert_not_called()

    def test_install_rejects_absolute_drive_unc_device_and_symlink_members(self) -> None:
        bad_names = (
            "/absolute.txt",
            "C:/drive.txt",
            "C:drive-relative.txt",
            "\\\\server\\share\\unc.txt",
            "\\\\?\\C:\\device.txt",
            "extension/NUL.txt",
            "extension/stream.txt:secret",
        )
        for bad_name in bad_names:
            with self.subTest(member=bad_name):
                archive = self._archive([
                    ("extension/package.json", self._manifest()),
                    (bad_name, b"unsafe"),
                ])
                result = self._install_archive(archive)
                self.assertIn("error", result)

        result = self._install_archive(self._symlink_archive())
        self.assertIn("error", result)
        self.assertIn("symlink", result["error"].lower())

    def test_archive_limits_reject_file_count_single_total_and_ratio_bombs(self) -> None:
        manifest = self._manifest()
        cases = []

        archive = self._archive([
            ("extension/package.json", manifest),
            ("extension/a.txt", b"a"),
        ])
        cases.append((archive, {"MAX_VSIX_FILES": 1}, "entries"))

        archive = self._archive([
            ("extension/package.json", manifest),
            ("extension/large.bin", b"12345"),
        ])
        cases.append((archive, {"MAX_VSIX_SINGLE_FILE_BYTES": 4}, "per-file"))

        archive = self._archive([
            ("extension/package.json", manifest),
            ("extension/total.bin", b"12345"),
        ])
        cases.append((
            archive,
            {"MAX_VSIX_EXPANDED_BYTES": len(manifest) + 4},
            "expanded-size",
        ))

        fd, ratio_archive = tempfile.mkstemp(suffix=".vsix", dir=self.root)
        os.close(fd)
        with zipfile.ZipFile(ratio_archive, "w") as zf:
            zf.writestr("extension/package.json", manifest, compress_type=zipfile.ZIP_STORED)
            zf.writestr(
                "extension/bomb.bin",
                b"0" * 50_000,
                compress_type=zipfile.ZIP_DEFLATED,
            )
        cases.append((ratio_archive, {"MAX_VSIX_COMPRESSION_RATIO": 10.0}, "compression-ratio"))

        for archive, limits, error_text in cases:
            with self.subTest(error=error_text), patch.multiple(extensions, **limits):
                result = self._install_archive(archive)
                self.assertIn("error", result)
                self.assertIn(error_text, result["error"])

    def test_download_streams_and_removes_partial_file_when_size_limit_is_exceeded(self) -> None:
        class Response:
            headers = {}

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def raise_for_status(self) -> None:
                return None

            def iter_bytes(self, chunk_size: int):
                self.chunk_size = chunk_size
                yield b"123"
                yield b"456"

        class Client:
            response = Response()

            def stream(self, method: str, url: str):
                self.method = method
                self.url = url
                return self.response

        client = Client()
        with (
            patch.object(extensions, "_get_http_client", return_value=client),
            patch.object(extensions, "MAX_VSIX_DOWNLOAD_BYTES", 5),
        ):
            with self.assertRaisesRegex(ValueError, "compressed-size"):
                extensions.download_vsix(
                    "https://example.invalid/large.vsix", "security.fixture"
                )

        self.assertEqual(client.method, "GET")
        self.assertFalse(os.path.exists(os.path.join(self.root, "security.fixture.vsix")))
        self.assertFalse(any(name.endswith(".part") for name in os.listdir(self.root)))

    def test_atomic_commit_restores_previous_install_when_state_write_fails(self) -> None:
        ext_dir = os.path.join(self.root, "security.fixture")
        os.makedirs(ext_dir)
        marker = os.path.join(ext_dir, "old.txt")
        with open(marker, "w", encoding="utf-8") as fh:
            fh.write("old")
        state_path = os.path.join(self.root, "security.fixture.json")
        with open(state_path, "w", encoding="utf-8") as fh:
            json.dump({"id": "security.fixture", "version": "old"}, fh)
        archive = self._archive([
            ("extension/package.json", self._manifest()),
            ("extension/new.txt", b"new"),
        ])

        with patch.object(extensions, "_write_json_atomic", side_effect=OSError("disk full")):
            result = self._install_archive(archive)

        self.assertIn("error", result)
        self.assertTrue(os.path.isfile(marker))
        self.assertFalse(os.path.exists(os.path.join(ext_dir, "new.txt")))
        with open(state_path, "r", encoding="utf-8") as fh:
            self.assertEqual(json.load(fh).get("version"), "old")

    def test_uninstall_ignores_tampered_ext_dir_outside_extension_root(self) -> None:
        outside = tempfile.mkdtemp(prefix="sao_vsix_outside_")
        try:
            sentinel = os.path.join(outside, "keep.txt")
            with open(sentinel, "w", encoding="utf-8") as fh:
                fh.write("keep")
            expected = os.path.join(self.root, "security.fixture")
            os.makedirs(expected)
            state_path = os.path.join(self.root, "security.fixture.json")
            with open(state_path, "w", encoding="utf-8") as fh:
                json.dump({"id": "security.fixture", "ext_dir": outside}, fh)

            listed = extensions.list_installed()
            result = extensions.uninstall_extension("security.fixture")

            self.assertEqual(listed[0]["ext_dir"], expected)
            self.assertTrue(result.get("ok"))
            self.assertTrue(os.path.isfile(sentinel))
            self.assertFalse(os.path.exists(expected))
            self.assertFalse(os.path.exists(state_path))
            self.assertTrue(result.get("warnings"))
        finally:
            import shutil
            shutil.rmtree(outside, ignore_errors=True)

    def test_valid_vsix_installs_and_leaves_no_staging_directory(self) -> None:
        archive = self._archive([
            ("[Content_Types].xml", b"<Types />"),
            ("extension.vsixmanifest", b"<PackageManifest />"),
            ("extension/package.json", self._manifest()),
            ("extension/extension.js", b"module.exports = {};"),
        ])

        result = self._install_archive(archive)

        self.assertTrue(result.get("ok"), result)
        self.assertTrue(os.path.isfile(os.path.join(result["ext_dir"], "package.json")))
        self.assertTrue(os.path.isfile(os.path.join(result["ext_dir"], "extension.js")))
        self.assertFalse(any("staging" in name for name in os.listdir(self.root)))

    def test_root_level_package_layout_remains_compatible(self) -> None:
        archive = self._archive([
            ("package.json", self._manifest()),
            ("extension.js", b"module.exports = {};"),
        ])

        result = self._install_archive(archive)

        self.assertTrue(result.get("ok"), result)
        self.assertTrue(os.path.isfile(os.path.join(result["ext_dir"], "extension.js")))

    def test_local_directory_install_rejects_links_and_reparse_points(self) -> None:
        outside = os.path.join(self._source_tmp.name, "outside-secret.txt")
        with open(outside, "wb") as handle:
            handle.write(b"must-not-copy")
        source = self._local_source({"extension.js": b"module.exports = {};"})
        link_path = os.path.join(source, "outside-link.txt")
        try:
            os.symlink(outside, link_path)
        except OSError:
            # Windows without Developer Mode cannot create symlinks.  Exercise
            # the same rejection branch by marking this exact entry reparse.
            with open(link_path, "wb") as handle:
                handle.write(b"placeholder")
            original = extensions._path_is_link_or_reparse

            def fake_reparse(path: str, file_stat=None) -> bool:
                if os.path.normcase(path) == os.path.normcase(link_path):
                    return True
                return original(path, file_stat)

            context = patch.object(
                extensions, "_path_is_link_or_reparse",
                side_effect=fake_reparse)
        else:
            context = patch.object(
                extensions, "_path_is_link_or_reparse",
                wraps=extensions._path_is_link_or_reparse)

        with context:
            result = extensions.install_extension_from_local_dir(source)

        self.assertIn("error", result)
        self.assertRegex(result["error"].lower(), r"link|reparse|junction")
        self.assertFalse(os.path.exists(os.path.join(self.root, "security.fixture")))

    def test_local_directory_install_enforces_file_and_size_limits(self) -> None:
        cases = (
            ({"a.txt": b"a"}, {"MAX_VSIX_FILES": 1}, "entries"),
            ({"large.bin": b"12345"}, {"MAX_VSIX_SINGLE_FILE_BYTES": 4}, "per-file"),
            (
                {"total.bin": b"12345"},
                {"MAX_VSIX_EXPANDED_BYTES": len(self._manifest()) + 4},
                "expanded-size",
            ),
        )
        for files, limits, error_text in cases:
            source = self._local_source(files)
            with self.subTest(error=error_text), patch.multiple(
                    extensions, **limits):
                result = extensions.install_extension_from_local_dir(source)
                self.assertIn("error", result)
                self.assertIn(error_text, result["error"])

    def test_valid_local_directory_install_is_bounded_and_persistent(self) -> None:
        source = self._local_source({
            "extension.js": b"module.exports = {};",
            "nested/readme.txt": b"safe",
        })

        result = extensions.install_extension_from_local_dir(source)

        self.assertTrue(result.get("ok"), result)
        self.assertTrue(os.path.isfile(os.path.join(result["ext_dir"], "extension.js")))
        self.assertTrue(os.path.isfile(os.path.join(result["ext_dir"], "nested", "readme.txt")))
        self.assertFalse(any("staging" in name for name in os.listdir(self.root)))

    def test_local_directory_source_cannot_contain_managed_staging(self) -> None:
        with open(os.path.join(self.root, "package.json"), "wb") as handle:
            handle.write(self._manifest())

        result = extensions.install_extension_from_local_dir(self.root)

        self.assertIn("error", result)
        self.assertIn("contain", result["error"].lower())
        self.assertFalse(os.path.exists(os.path.join(self.root, "security.fixture")))


if __name__ == "__main__":
    unittest.main()

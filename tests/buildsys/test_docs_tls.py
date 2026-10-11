"""Verify the development server's HTTPS transport and browser isolation headers."""
from __future__ import annotations

import http.client
import gzip
import os
from pathlib import Path
import socketserver
import ssl
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock
from wsgiref.simple_server import make_server, WSGIServer, WSGIRequestHandler

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "buildsys"))
import mkdocs_hooks


class ThreadedServer(socketserver.ThreadingMixIn, WSGIServer):
    daemon_threads = True


class QuietHandler(WSGIRequestHandler):
    def log_message(self, *args):
        pass


class DocumentationTlsTests(unittest.TestCase):
    def test_auto_enables_https_for_remote_binds(self):
        with mock.patch.dict(os.environ, {"POSTGAMMA_DOCS_HTTPS": "auto",
                                         "POSTGAMMA_DOCS_TLS_CERT": "", "POSTGAMMA_DOCS_TLS_KEY": ""}):
            for host in ("localhost", "127.0.0.1", "127.0.0.2", "::1"):
                self.assertFalse(mkdocs_hooks.https_enabled(host))
            for host in ("0.0.0.0", "::", "192.0.2.10", "preview.example"):
                self.assertTrue(mkdocs_hooks.https_enabled(host))
            os.environ["POSTGAMMA_DOCS_HTTPS"] = "0"
            self.assertFalse(mkdocs_hooks.https_enabled("0.0.0.0"))
            os.environ["POSTGAMMA_DOCS_HTTPS"] = "invalid"
            with self.assertRaisesRegex(ValueError, "DOCS_HTTPS"):
                mkdocs_hooks.https_enabled("0.0.0.0")

    def test_partial_custom_certificate_configuration_fails(self):
        with mock.patch.dict(os.environ, {"POSTGAMMA_DOCS_TLS_CERT": "cert.pem",
                                         "POSTGAMMA_DOCS_TLS_KEY": ""}):
            with self.assertRaisesRegex(ValueError, "both DOCS_TLS_CERT and DOCS_TLS_KEY"):
                mkdocs_hooks.preview_certificate(Path("."), "0.0.0.0")

    def test_tls_get_verifies_certificate_and_isolation_headers(self):
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(os.environ, {
            "POSTGAMMA_DOCS_HTTPS": "1", "POSTGAMMA_DOCS_TLS_CERT": "",
            "POSTGAMMA_DOCS_TLS_KEY": "", "POSTGAMMA_DOCS_TLS_DIR": temporary,
            "POSTGAMMA_DOCS_TLS_HOSTS": "preview.example,192.0.2.10",
        }):
            root = Path(temporary)
            cert, key, names = mkdocs_hooks.preview_certificate(root, "127.0.0.1")
            self.assertIn("DNS:preview.example", names)
            self.assertIn("IP:192.0.2.10", names)
            self.assertEqual(key.stat().st_mode & 0o777, 0o600)
            identity = (cert.read_bytes(), key.read_bytes())
            mkdocs_hooks.preview_certificate(root, "127.0.0.1")
            self.assertEqual((cert.read_bytes(), key.read_bytes()), identity)
            asset = root / "assets/wasm/worker.mjs"
            asset.parent.mkdir(parents=True)
            asset.write_bytes(b"original module")

            def app(environ, respond):
                status = "404 Not Found" if environ["PATH_INFO"] == "/missing" else "200 OK"
                respond(status, [("Content-Type", "text/plain")])
                if environ["PATH_INFO"] == "/assets/wasm/worker.mjs":
                    return [asset.read_bytes()]
                return [b"wasm" * 10000 if environ["PATH_INFO"] == "/asset.wasm"
                        else environ["wsgi.url_scheme"].encode()]

            server = make_server("127.0.0.1", 0, app, ThreadedServer, QuietHandler)
            port = server.server_port
            server.url = f"http://127.0.0.1:{port}/"
            server.root = str(root)
            mkdocs_hooks.on_serve(server, config=SimpleNamespace(config_file_path=root / "mkdocs.yml"))
            self.assertEqual(server.url, f"https://127.0.0.1:{port}/")
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                # Trust this exact certificate; do not disable certificate or hostname verification.
                context = ssl.create_default_context(cafile=str(cert))
                for path, expected in (("/", 200), ("/asset.mjs", 200), ("/missing", 404)):
                    client = http.client.HTTPSConnection("127.0.0.1", port, timeout=5, context=context)
                    try:
                        client.request("GET", path)
                        response = client.getresponse()
                        self.assertEqual(response.status, expected)
                        self.assertEqual(response.getheader("Cross-Origin-Opener-Policy"), "same-origin")
                        self.assertEqual(response.getheader("Cross-Origin-Embedder-Policy"), "require-corp")
                        self.assertEqual(response.read(), b"https")
                    finally:
                        client.close()
                for encoding, compressed in (("gzip, deflate", True), ("gzip;q=0", False)):
                    client = http.client.HTTPSConnection("127.0.0.1", port, timeout=5, context=context)
                    try:
                        client.request("GET", "/asset.wasm", headers={"Accept-Encoding": encoding})
                        response = client.getresponse()
                        self.assertEqual(response.getheader("Vary"), "Accept-Encoding")
                        body = response.read()
                        if compressed:
                            self.assertEqual(response.getheader("Content-Encoding"), "gzip")
                            self.assertLess(len(body), 100)
                            body = gzip.decompress(body)
                        else:
                            self.assertIsNone(response.getheader("Content-Encoding"))
                        self.assertEqual(body, b"wasm" * 10000)
                    finally:
                        client.close()
                with mock.patch.object(server, "handle_error") as unhandled:
                    client = http.client.HTTPSConnection("127.0.0.1", port, timeout=5)
                    try:
                        with self.assertRaises(ssl.SSLCertVerificationError):
                            client.request("GET", "/")
                    finally:
                        client.close()
                etag = ""
                for expected in (200, 304, 200):
                    if expected == 200 and etag:
                        asset.write_bytes(b"rebuilt module")
                    client = http.client.HTTPSConnection("127.0.0.1", port, timeout=5, context=context)
                    try:
                        client.request("GET", "/assets/wasm/worker.mjs", headers={"If-None-Match": etag})
                        response = client.getresponse()
                        self.assertEqual(response.status, expected)
                        self.assertEqual(response.getheader("Cache-Control"), "no-cache")
                        self.assertEqual(response.getheader("Cross-Origin-Embedder-Policy"), "require-corp")
                        if expected == 304:
                            self.assertEqual(response.getheader("ETag"), etag)
                            self.assertEqual(response.read(), b"")
                        else:
                            self.assertNotEqual(response.getheader("ETag"), etag)
                            self.assertEqual(response.read(), asset.read_bytes())
                        etag = response.getheader("ETag")
                    finally:
                        client.close()
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)
            unhandled.assert_not_called()


if __name__ == "__main__":
    unittest.main()

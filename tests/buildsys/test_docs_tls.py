"""Verify the development server's HTTPS transport and browser isolation headers."""
from __future__ import annotations

import http.client
import gzip
import os
from pathlib import Path
import socket
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

    def test_certificate_renewal_without_server_restart(self):
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(os.environ, {
            "POSTGAMMA_DOCS_HTTPS": "1", "POSTGAMMA_DOCS_TLS_CERT": "",
            "POSTGAMMA_DOCS_TLS_KEY": "", "POSTGAMMA_DOCS_TLS_HOSTS": "",
        }):
            root = Path(temporary)
            pairs = []
            for generation in ("old", "new"):
                os.environ["POSTGAMMA_DOCS_TLS_DIR"] = str(root / generation)
                cert, key, _ = mkdocs_hooks.preview_certificate(root, "127.0.0.1")
                pairs.append((cert, key))
            cert, key = root / "fullchain.pem", root / "privkey.pem"
            cert.write_bytes(pairs[0][0].read_bytes())
            key.write_bytes(pairs[0][1].read_bytes())
            os.environ.update(POSTGAMMA_DOCS_TLS_CERT=str(cert), POSTGAMMA_DOCS_TLS_KEY=str(key))

            def app(environ, respond):
                respond("200 OK", [("Content-Type", "text/plain")])
                return [b"ready"]

            server = make_server("127.0.0.1", 0, app, ThreadedServer, QuietHandler)
            server.url = f"http://127.0.0.1:{server.server_port}/"
            server.root = str(root)
            mkdocs_hooks.on_serve(server, config=SimpleNamespace(config_file_path=root / "mkdocs.yml"))
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()

            def request(trusted):
                client = http.client.HTTPSConnection("127.0.0.1", server.server_port, timeout=5,
                                                    context=ssl.create_default_context(cafile=str(trusted)))
                try:
                    client.request("GET", "/")
                    response = client.getresponse()
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read(), b"ready")
                finally:
                    client.close()

            try:
                request(pairs[0][0])
                cert.write_bytes(pairs[1][0].read_bytes())
                with self.assertLogs("mkdocs.preview", level="WARNING") as messages:
                    request(pairs[0][0])
                self.assertIn("retaining previous certificate", messages.output[0])
                key.write_bytes(pairs[1][1].read_bytes())
                request(pairs[1][0])
                with self.assertRaises(ssl.SSLCertVerificationError):
                    request(pairs[0][0])
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)

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
            app_requests = []

            def app(environ, respond):
                app_requests.append(environ["PATH_INFO"])
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
                # HTTP on the HTTPS port must return a useful redirect, never
                # an empty response or the unisolated application contents.
                for method in ("GET", "HEAD"):
                    client = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                    try:
                        client.request(method, "/opfs/?probe=a%20b&client=opfs")
                        response = client.getresponse()
                        self.assertEqual(response.status, 307)
                        self.assertEqual(response.getheader("Location"),
                                         f"https://127.0.0.1:{port}/opfs/?probe=a%20b&client=opfs")
                        self.assertEqual(response.getheader("Cache-Control"), "no-store")
                        self.assertEqual(response.read(), b"")
                    finally:
                        client.close()
                for authority in ("example.com/other", "example.com:65536"):
                    client = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                    try:
                        client.request("GET", "/opfs/", headers={"Host": authority})
                        response = client.getresponse()
                        self.assertEqual(response.status, 400)
                        self.assertIsNone(response.getheader("Location"))
                        response.read()
                    finally:
                        client.close()
                self.assertEqual(app_requests, [])
                # An incomplete TLS handshake must not hold up other clients.
                with socket.create_connection(("127.0.0.1", port), timeout=5) as idle:
                    idle.sendall(b"\x16")
                    client = http.client.HTTPSConnection("127.0.0.1", port, timeout=5, context=context)
                    try:
                        client.request("GET", "/opfs/")
                        response = client.getresponse()
                        self.assertEqual(response.status, 200)
                        self.assertEqual(response.read(), b"https")
                    finally:
                        client.close()
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

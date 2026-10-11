"""MkDocs hooks for inserting fail-closed generated API references."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import logging
import os
from pathlib import Path
import re
import socket
import ssl
import subprocess
import tempfile
from typing import Any
from urllib.parse import urlsplit, urlunsplit
import zlib


GENERATED_REFERENCES = {
    "<!-- POSTGAMMA_GENERATED_C_API -->": "c-api.md",
    "<!-- POSTGAMMA_GENERATED_EXTENSION_API -->": "extension-api.md",
    "<!-- POSTGAMMA_GENERATED_PYTHON_API -->": "python-api.md",
}
VERSION_MARKER = "{{ POSTGAMMA_VERSION }}"
RELEASES_URL_MARKER = "{{ POSTGAMMA_RELEASES_URL }}"
RELEASE_STATE_MARKER = "{{ POSTGAMMA_RELEASE_STATE }}"


def https_enabled(host: str) -> bool:
    mode = os.environ.get("POSTGAMMA_DOCS_HTTPS", "0").lower()
    if mode in {"1", "yes", "true"}:
        return True
    if mode in {"0", "no", "false"}:
        return False
    if mode != "auto":
        raise ValueError("DOCS_HTTPS must be auto, 1 or 0")
    if os.environ.get("POSTGAMMA_DOCS_TLS_CERT") or os.environ.get("POSTGAMMA_DOCS_TLS_KEY"):
        return True
    if host.lower() == "localhost":
        return False
    try:
        return not ipaddress.ip_address(host.strip("[]")).is_loopback
    except ValueError:
        return True


def certificate_names(host: str) -> list[str]:
    names = {"localhost", "127.0.0.1", "::1", socket.gethostname()}
    names.update(os.environ.get("POSTGAMMA_DOCS_TLS_HOSTS", "").replace(",", " ").split())
    names.add(host.strip("[]"))
    # Read local interface addresses; no external IP-discovery service is used.
    try:
        addresses = subprocess.run(["ip", "-j", "address", "show"], check=True,
                                   capture_output=True, text=True, timeout=5)
        for interface in json.loads(addresses.stdout):
            names.update(item["local"] for item in interface.get("addr_info", []))
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    result = set()
    for name in names:
        try:
            address = ipaddress.ip_address(name)
        except ValueError:
            if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9.-]{0,252}", name):
                raise ValueError("Invalid DOCS_TLS_HOSTS name: " + name)
            result.add("DNS:" + name.lower())
        else:
            if not address.is_unspecified and not address.is_link_local:
                result.add("IP:" + str(address))
    return sorted(result)


def preview_certificate(root: Path, host: str) -> tuple[Path, Path, list[str]]:
    cert = os.environ.get("POSTGAMMA_DOCS_TLS_CERT", "")
    key = os.environ.get("POSTGAMMA_DOCS_TLS_KEY", "")
    if cert or key:
        if not cert or not key:
            raise ValueError("Set both DOCS_TLS_CERT and DOCS_TLS_KEY")
        return Path(cert).expanduser(), Path(key).expanduser(), []
    names = certificate_names(host)
    identifier = hashlib.sha256(",".join(names).encode()).hexdigest()[:16]
    directory = Path(os.environ.get("POSTGAMMA_DOCS_TLS_DIR", root / "build/docs/tls")) / identifier
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    directory.chmod(0o700)
    cert_path, key_path = directory / "cert.pem", directory / "key.pem"
    try:
        if cert_path.is_file() and key_path.is_file():
            valid = subprocess.run(["openssl", "x509", "-in", str(cert_path),
                                    "-noout", "-checkend", "86400"], capture_output=True, timeout=10)
            if valid.returncode == 0:
                return cert_path, key_path, names
        with tempfile.TemporaryDirectory(dir=directory) as temporary:
            generated = Path(temporary)
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256",
                            "-nodes", "-days", "30", "-subj", "/CN=PostGamma docs preview",
                            "-addext", "subjectAltName=" + ",".join(names),
                            "-addext", "extendedKeyUsage=serverAuth",
                            "-keyout", str(generated / "key.pem"),
                            "-out", str(generated / "cert.pem")],
                           check=True, capture_output=True, timeout=30)
            (generated / "key.pem").chmod(0o600)
            (generated / "key.pem").replace(key_path)
            (generated / "cert.pem").replace(cert_path)
    except (OSError, subprocess.SubprocessError) as error:
        raise RuntimeError("Cannot generate the HTTPS preview certificate. Install openssl, "
                           "or set DOCS_TLS_CERT and DOCS_TLS_KEY.") from error
    return cert_path, key_path, names


def on_config(config: Any) -> Any:
    # MkDocs initially assigns an HTTP site_url even when the serving socket
    # will use TLS. Keep generated canonical URLs consistent on every rebuild.
    if https_enabled(config.dev_addr[0]) and config.site_url:
        config.site_url = urlunsplit(urlsplit(config.site_url)._replace(scheme="https"))
    return config


def on_files(files: Any, config: Any) -> Any:
    """Copy a verified browser package without checking generated binaries in."""
    from mkdocs.structure.files import File

    root = Path(config.config_file_path).resolve().parent
    directory = Path(os.environ.get("POSTGAMMA_DOCS_WASM_DIR", root / "build/docs/browser"))
    if not (directory / "assets/wasm/demo.json").is_file():
        raise RuntimeError("browser demo metadata is missing; run make docs-wasm")
    for path in sorted(directory.rglob("*")):
        if path.is_file() and path.suffix not in {".md", ".ts"}:
            files.append(File(path.relative_to(directory).as_posix(), str(directory), config.site_dir, False))
    return files


def on_serve(server: Any, **kwargs: Any) -> Any:
    """Local preview uses native isolation headers, retaining MkDocs live reload."""
    host, port = server.server_address[:2]
    use_https = https_enabled(host)
    if use_https:
        root = Path(kwargs["config"].config_file_path).resolve().parent
        cert, key, names = preview_certificate(root, host)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(cert, key)
        logger = logging.getLogger("mkdocs.https")

        class TLSRequestHandler(server.RequestHandlerClass):
            def handle(self) -> None:
                previous_timeout = self.connection.gettimeout()
                try:
                    self.connection.settimeout(10)
                    self.connection.do_handshake()
                except (ssl.SSLError, ConnectionError, TimeoutError) as error:
                    # A browser may reject the development certificate or a
                    # visitor may still use HTTP. Neither is an app traceback.
                    logger.debug("TLS connection from %s closed: %s", self.client_address, error)
                    return
                finally:
                    self.connection.settimeout(previous_timeout)
                super().handle()

        server.RequestHandlerClass = TLSRequestHandler
        # Handshake in the request thread, so an idle connection does not block
        # acceptance of the other connections used by the browser's Workers.
        server.socket = context.wrap_socket(server.socket, server_side=True,
                                            do_handshake_on_connect=False)
        server.url = urlunsplit(urlsplit(server.url)._replace(scheme="https"))
        if names:
            logger.info("HTTPS development certificate: %s (self-signed; confirm the browser certificate prompt)", cert)
            for name in names:
                if name.startswith("IP:") and not ipaddress.ip_address(name[3:]).is_loopback:
                    address = name[3:]
                    if ":" in address:
                        address = "[" + address + "]"
                    logger.info("HTTPS preview: https://%s:%s/", address, port)
    app = server.get_app()

    def isolated(environ: Any, start_response: Any) -> Any:
        if use_https:
            environ["wsgi.url_scheme"] = "https"
        gzip_requested = False
        for encoding in environ.get("HTTP_ACCEPT_ENCODING", "").split(","):
            name, *parameters = encoding.strip().lower().split(";")
            if name.strip() == "gzip":
                try:
                    quality = next((float(p.split("=", 1)[1]) for p in parameters
                                    if p.strip().startswith("q=")), 1.0)
                    gzip_requested = quality > 0
                except ValueError:
                    pass
        compressed = False

        def respond(status: str, headers: list, exc_info: Any = None) -> Any:
            nonlocal compressed
            headers = [(k, v) for k, v in headers if k.lower() not in {
                "cross-origin-opener-policy", "cross-origin-embedder-policy"}]
            headers.extend([("Cross-Origin-Opener-Policy", "same-origin"),
                            ("Cross-Origin-Embedder-Policy", "require-corp")])
            # The Wasm kernel is over 10 MiB uncompressed. Stream gzip for
            # static assets while preserving MkDocs' live-reload responses.
            path = environ.get("PATH_INFO", "")
            eligible = (status.startswith("200 ") and environ.get("REQUEST_METHOD") == "GET"
                        and not path.startswith("/livereload/")
                        and (path.endswith("/") or Path(path).suffix in {
                            ".html", ".css", ".js", ".mjs", ".json", ".svg", ".wasm", ".data"})
                        and not any(k.lower() == "content-encoding" for k, _ in headers))
            if eligible:
                headers.append(("Vary", "Accept-Encoding"))
            if eligible and gzip_requested:
                compressed = True
                headers = [(k, v) for k, v in headers if k.lower() != "content-length"]
                headers.append(("Content-Encoding", "gzip"))
            return start_response(status, headers, exc_info)
        response = app(environ, respond)
        if not compressed:
            return response

        def gzip_body() -> Any:
            compressor = zlib.compressobj(level=5, wbits=31)
            try:
                for chunk in response:
                    yield compressor.compress(chunk)
                yield compressor.flush()
            finally:
                if hasattr(response, "close"):
                    response.close()

        return gzip_body()

    server.set_app(isolated)
    return server


def on_page_markdown(
    markdown: str, page: Any, config: Any, files: Any
) -> str:
    """Replace one reference marker with its build-generated Markdown."""

    del page, files
    root = Path(config.config_file_path).resolve().parent
    version = (root / "VERSION").read_text(encoding="ascii").strip()
    baseline = json.loads(
        (root / "manifests/api/python-api-v1.json").read_text(encoding="utf-8")
    )
    release_state = baseline["release"]["state"]
    releases_url = os.environ.get("POSTGAMMA_RELEASES_URL", "#release-status")
    markdown = markdown.replace(VERSION_MARKER, version)
    markdown = markdown.replace(RELEASE_STATE_MARKER, release_state)
    markdown = markdown.replace(RELEASES_URL_MARKER, releases_url)
    configured = os.environ.get("POSTGAMMA_DOCS_GENERATED_DIR")
    generated = (
        Path(configured).resolve()
        if configured
        else root / "build" / "docs" / "generated"
    )
    for marker, filename in GENERATED_REFERENCES.items():
        count = markdown.count(marker)
        if count == 0:
            continue
        if count != 1:
            raise RuntimeError(f"documentation marker occurs {count} times: {marker}")
        path = generated / filename
        if not path.is_file():
            raise RuntimeError(
                f"generated API reference is missing: {path}; run make docs-api"
            )
        markdown = markdown.replace(marker, path.read_text(encoding="utf-8"))
    return markdown

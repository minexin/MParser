"""Exercise real MExecServer REST execution and session-file export in isolation."""

import argparse
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import runpy
import socket
import stat
import subprocess
from threading import Thread
import time
from urllib.error import URLError
from urllib.request import ProxyHandler, Request, build_opener

from PIL import ImageFont


class Encoder(ThreadingHTTPServer):
    def __init__(self, helpers):
        super().__init__(("127.0.0.1", 0), EncodeRequest)
        self.helpers = helpers
        self.records = []
        self.wrong_format = False


class EncodeRequest(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        figure = self.headers.get("X-MParser-Figure")
        record = {"figure": figure, "route": self.path}
        try:
            fmt = {"/png": 1, "/pdf": 2}[self.path]
            length = int(self.headers["Content-Length"])
            if not 0 < length <= 4 * 1024 * 1024:
                raise ValueError("scene exceeds the probe limit")
            scene = json.loads(self.rfile.read(length))
            surface = self.server.helpers["projection"](scene, figure)
            data = self.server.helpers["encode"](1 if self.server.wrong_format else fmt, surface)
            record["encoded_sha256"] = hashlib.sha256(data).hexdigest()
            record["status"] = "wrong-signature" if self.server.wrong_format else "encoded"
            status = 200
        except (ValueError, KeyError, TypeError) as error:
            record.update(status="rejected", detail=str(error))
            status, data = 422, b""
        self.server.records.append(record)
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


@contextmanager
def launch(executable, root, port):
    root.mkdir()
    environment = os.environ.copy()
    environment.pop("MEXEC_EXPORT_PROBE_PORT", None)
    if port is not None:
        environment["MEXEC_EXPORT_PROBE_PORT"] = str(port)
    with socket.socket() as address:
        address.bind(("127.0.0.1", 0))
        api_port = address.getsockname()[1]
    with (root / "server.log").open("wb") as log:
        process = subprocess.Popen([str(executable), "--host", "127.0.0.1", "--port", str(api_port),
                                    "--file-root", str(root)], cwd=executable.parent,
                                   env=environment, stdout=log, stderr=log,
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        opener = build_opener(ProxyHandler({}))

        def api(path, body=None, expected=200):
            request = Request(f"http://127.0.0.1:{api_port}/api/v1{path}",
                              data=None if body is None else json.dumps(body).encode(),
                              headers={"Content-Type": "application/json"})
            with opener.open(request, timeout=12) as response:
                result = json.load(response)
                assert response.status == expected, (path, response.status, result)
            with (root / "rest.jsonl").open("a", encoding="utf-8") as evidence:
                evidence.write(json.dumps({"path": path, "request": body, "response": result}) + "\n")
            return result

        try:
            for _ in range(100):
                assert process.poll() is None, (process.returncode, root / "server.log")
                try:
                    api("/health")
                    break
                except URLError:
                    time.sleep(0.05)
            else:
                raise RuntimeError(f"MExecServer not ready: {root}")
            yield api
        finally:
            process.terminate()
            process.wait(timeout=10)


def execute(api, base, command, identifier=None):
    result = api(f"{base}/execute", {"command": command})
    if identifier:
        assert result["status"] == "runtime-failed", result
        assert result["diagnostics"][0]["identifier"] == f"MParser:{identifier}", result
    else:
        assert result["status"] == "succeeded", result
    return result


def run(executable, root, pdftoppm, renderer):
    enabled = root / "enabled"
    with launch(executable, enabled, renderer.server_port) as api:
        runtime = api("/info")["runtime"]
        assert runtime["version"] == "1.15.0" and runtime["abi_revision"] == 13, runtime
        session = api("/sessions", {"name": "real-export"}, 201)["id"]
        base = f"/sessions/{session}"
        files = enabled / "sessions" / session
        seeded = execute(api, base, "f=figure(); f.Name='selected'; r=plot([0 1 2],[0 2 0]); r.Color='r'; "
                         "g=figure(); g.Name='current'; b=plot([0 1 2],[2 0 2]); b.Color='b';")["graphics"]
        figures = {node["properties"]["Name"]: node["objectIdentity"] for node in seeded["objects"] if node["type"] == "Figure"}
        font = ImageFont.load_default()
        metrics = dict(width_pixels=640, height_pixels=400, pixels_per_inch=96,
                       character_width_pixels=font.getlength("0"), character_height_pixels=sum(font.getmetrics()))
        for figure in figures.values():
            layout = api(f"{base}/graphics-layout")
            api(f"{base}/graphics-layout-environment", dict(graph_identity=seeded["graphIdentity"],
                figure_identity=figure, expected_revision=layout["revision"], environment=metrics))

        stable = api(f"{base}/graphics-layout")["graphics"]
        execute(api, base, "saveas(r,'export-selected','png');")
        execute(api, base, "print('export-current','-dpdf');")
        assert api(f"{base}/graphics-layout")["graphics"] == stable
        assert [(record["figure"], record["route"]) for record in renderer.records] == [
            (figures["selected"], "/png"), (figures["current"], "/pdf")]
        png, pdf = files / "export-selected.png", files / "export-current.pdf"
        png_bytes, pdf_bytes = png.read_bytes(), pdf.read_bytes()
        assert png_bytes.startswith(b"\x89PNG\r\n\x1a\n") and pdf_bytes.startswith(b"%PDF-")
        entries = {entry["name"]: entry["size_bytes"] for entry in api(f"{base}/files")["entries"]}
        assert entries[png.name] == len(png_bytes) and entries[pdf.name] == len(pdf_bytes), entries
        sibling = api("/sessions", {"name": "separate-root"}, 201)["id"]
        assert not {png.name, pdf.name}.intersection(entry["name"] for entry in api(f"/sessions/{sibling}/files")["entries"])

        outside = files.parent / "export-outside.png"
        outside.write_bytes(b"outside session sentinel")
        for path in ("../export-outside", "/export-outside", "C:/export-outside", "export-dir/export-outside", "CON"):
            execute(api, base, f"print('{path}','-dpng');", "GraphicsExportDestinationRejected")
        execute(api, base, "print('export-current.pdf','-dpng');", "GraphicsExportExtensionMismatch")
        assert len(renderer.records) == 2 and outside.read_bytes() == b"outside session sentinel"

        renderer.wrong_format = True
        execute(api, base, "print('export-current','-dpdf');", "GraphicsExportFailed")
        renderer.wrong_format = False
        execute(api, base, "b.YData=[1 0 1];")
        pdf.chmod(stat.S_IREAD)
        try:
            execute(api, base, "print('export-current','-dpdf');", "GraphicsExportFailed")
            assert renderer.records[-1]["encoded_sha256"] != hashlib.sha256(pdf_bytes).hexdigest()
        finally:
            pdf.chmod(stat.S_IREAD | stat.S_IWRITE)
        assert pdf.read_bytes() == pdf_bytes
        execute(api, base, "print('export-current','-dpdf');")
        assert hashlib.sha256(pdf.read_bytes()).hexdigest() == renderer.records[-1]["encoded_sha256"]
        assert pdf.read_bytes() != pdf_bytes
        new_pdf = pdf.read_bytes()
        execute(api, base, "figure(); plot(1:3); print('export-current','-dpdf');", "GraphicsExportFailed")
        assert "explicit host metrics" in renderer.records[-1]["detail"]
        assert png.read_bytes() == png_bytes and pdf.read_bytes() == new_pdf
        assert not list(files.glob(".mexec-save-*.tmp"))

    disabled = root / "disabled"
    with launch(executable, disabled, None) as api:
        session = api("/sessions", {"name": "headless"}, 201)["id"]
        execute(api, f"/sessions/{session}", "plot(1:3); print('export-headless','-dpng');", "GraphicsRendererUnavailable")
        assert not (disabled / "sessions" / session / "export-headless.png").exists()

    subprocess.run([pdftoppm, "-png", "-r", "96", "-singlefile", str(pdf), str(root / "pdf-raster")],
                   check=True, timeout=30)
    pixels = {"png": renderer.helpers["verify_pixels"](png, "red"),
              "rasterized_pdf": renderer.helpers["verify_pixels"](root / "pdf-raster.png", "blue")}
    report = dict(runtime=runtime, figures=figures, pixels=pixels, render_requests=renderer.records,
                  sha256={path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (png, pdf)},
                  preserved_on_failure=True, sibling_session_isolated=True, unconfigured_headless=True)
    (root / "evidence.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS MExecServer REST export and rooted file publication: {root / 'evidence.json'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--pdftoppm", default="pdftoppm")
    args = parser.parse_args()
    root = args.evidence.resolve()
    root.mkdir(parents=True, exist_ok=False)
    helpers = runpy.run_path(str(Path(__file__).with_name("probe-graphics-export-render.py")))
    with Encoder(helpers) as renderer:
        thread = Thread(target=renderer.serve_forever)
        thread.start()
        try:
            run(args.server.resolve(strict=True), root, args.pdftoppm, renderer)
        finally:
            renderer.shutdown()
            thread.join()


if __name__ == "__main__":
    main()

"""Test-only isolated SDK renderer: Pillow PNG and ReportLab vector PDF.

This is not a general MExec renderer. It only accepts this probe's flat output
allowlist and visible normalized Axes with solid RGB Lines and finite data.
"""

import argparse
import ctypes as c
import hashlib
import io
import json
import math
import os
from pathlib import Path
import stat
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont
from reportlab.pdfgen import canvas


class View(c.Structure):
    _fields_ = [("data", c.c_void_p), ("size", c.c_size_t)]

    def read(self):
        return c.string_at(self.data, self.size).decode("utf-8") if self.size else ""


class Request(c.Structure):
    _fields_ = [("size", c.c_uint32), ("generation", c.c_uint32), ("format", c.c_uint32),
                ("version", c.c_uint32), ("figure", c.c_uint64), ("destination", View), ("scene", View)]


CALLBACK = c.CFUNCTYPE(c.c_uint32, c.c_void_p, c.POINTER(Request))


class Options(c.Structure):
    _fields_ = [("size", c.c_uint32), ("generation", c.c_uint32), ("entry", c.c_void_p),
                ("entry_size", c.c_size_t), ("arguments", c.c_void_p), ("arg_count", c.c_size_t),
                ("outputs", c.c_size_t), ("has_outputs", c.c_uint32), ("workspace", c.c_void_p),
                ("workspace_count", c.c_size_t), ("backend", c.c_uint32), ("profile", c.c_uint32),
                ("max_instructions", c.c_uint64), ("max_time", c.c_uint64), ("max_depth", c.c_uint64),
                ("max_array", c.c_uint64), ("max_diagnostics", c.c_uint64), ("cancellation", c.c_void_p),
                ("output", c.c_void_p), ("output_data", c.c_void_p), ("debugger", c.c_void_p),
                ("input", c.c_void_p), ("input_data", c.c_void_p), ("events", c.c_void_p),
                ("renderer", CALLBACK), ("renderer_data", c.c_void_p)]


class Layout(c.Structure):
    _fields_ = [("size", c.c_uint32), ("generation", c.c_uint32)] + [
        (name, c.c_double) for name in ("width", "height", "dpi", "char_width", "char_height")]


class NamedValue(c.Structure):
    _fields_ = [("name", c.c_char_p), ("size", c.c_size_t), ("value", c.c_void_p)]


def projection(scene, figure_identity):
    if scene["schema"] != "mparser.graphics" or scene["version"] != 4:
        raise ValueError("unsupported scene version")
    figure, = [node for node in scene["objects"] if node["objectIdentity"] == str(figure_identity)]
    if figure["type"] != "Figure" or figure["properties"]["Visible"] != "on" or not figure["layout"]:
        raise ValueError("requires a visible Figure with explicit host metrics")
    nodes = {node["id"]: node for node in scene["objects"]}
    width, height = figure["layout"]["contentSizePixels"]
    dpi = figure["layout"]["pixelsPerInch"]
    if (width, height, dpi) != (640, 400, 96):
        raise ValueError("outside the test surface contract")
    frames = []
    for axes_id in figure["children"]:
        axes = nodes[axes_id]
        p = axes["properties"]
        if axes["type"] != "Axes" or p["Units"] != "normalized" or axes.get("labels"):
            raise ValueError("only unlabelled normalized Axes are supported")
        if p.get("Visible", "on") != "on" or p.get("XGrid") != "off" or p.get("YGrid") != "off":
            raise ValueError("unsupported Axes properties")
        nx, ny, nw, nh = p["Position"]
        if not (0 <= nx < nx + nw <= 1 and 0 <= ny < ny + nh <= 1):
            raise ValueError("axes outside test surface")
        left, top, w, h = nx * width, (1 - ny - nh) * height, nw * width, nh * height
        x0, x1 = p["XLim"]
        y0, y1 = p["YLim"]
        curves = []
        for line_id in axes["children"]:
            line = nodes[line_id]
            lp = line["properties"]
            if (line["type"] != "Line" or lp["LineStyle"] != "-" or lp["Clipping"] != "on"
                    or lp.get("Visible", "on") != "on" or len(lp["Color"]) != 3):
                raise ValueError("only solid visible clipped RGB Lines are supported")
            points = []
            for x, y in zip(lp["XData"], lp["YData"], strict=True):
                if not (math.isfinite(x) and math.isfinite(y) and x0 <= x <= x1 and y0 <= y <= y1):
                    raise ValueError("out-of-limits or nonfinite Line data")
                points.append((left + w * (x - x0) / (x1 - x0), top + h * (y1 - y) / (y1 - y0)))
            if len(points) < 2:
                raise ValueError("requires a visible segment")
            curves.append((tuple(lp["Color"]), points))
        frames.append(((left, top, w, h), curves))
    if not frames or not any(curves for _, curves in frames):
        raise ValueError("empty test Figure")
    return int(width), int(height), dpi, frames


def encode(fmt, layout):
    width, height, dpi, frames = layout
    result = io.BytesIO()
    if fmt == 1:
        image = Image.new("RGB", (width, height), "white")
        draw = ImageDraw.Draw(image)
        for (left, top, w, h), curves in frames:
            draw.rectangle((left, top, left + w, top + h), outline="black")
            for color, points in curves:
                draw.line(points, fill=tuple(round(component * 255) for component in color), width=3)
        image.save(result, format="PNG", dpi=(dpi, dpi))
    elif fmt == 2:
        scale = 72 / dpi
        pdf = canvas.Canvas(result, pagesize=(width * scale, height * scale), invariant=1)
        pdf.scale(scale, scale)
        for (left, top, w, h), curves in frames:
            pdf.setStrokeColorRGB(0, 0, 0)
            pdf.setLineWidth(1)
            pdf.rect(left, height - top - h, w, h)
            for color, points in curves:
                pdf.setStrokeColorRGB(*color)
                pdf.setLineWidth(3)
                path = pdf.beginPath()
                path.moveTo(points[0][0], height - points[0][1])
                for x, y in points[1:]:
                    path.lineTo(x, height - y)
                pdf.drawPath(path)
        pdf.showPage()
        pdf.save()
    else:
        raise ValueError("unsupported encoder")
    return result.getvalue()


def publish(target, data, fail_before_commit):
    if target.exists() or target.is_symlink():
        info = target.lstat()
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1
                or getattr(info, "st_file_attributes", 0) & 0x400):
            return 2
    fd, temporary = tempfile.mkstemp(prefix=".render-", dir=target.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if fail_before_commit:
            raise OSError("injected before atomic replacement")
        os.replace(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return 0


def verify_pixels(path, color):
    with Image.open(path) as image:
        assert image.size == (640, 400), image.size
        colors = image.convert("RGB").getcolors(maxcolors=640 * 400)
        red = sum(count for count, (r, g, b) in colors if r > 180 and g < 90 and b < 90)
        blue = sum(count for count, (r, g, b) in colors if b > 180 and r < 90 and g < 90)
        assert (red > 500 and blue == 0) if color == "red" else (blue > 500 and red == 0), (red, blue)
        return {"red": red, "blue": blue}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("pdftoppm")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output.resolve()
    files = root / "files"
    files.mkdir()
    outside = root / "outside.png"
    outside.write_bytes(b"outside-root sentinel")
    dll = c.CDLL(str(args.library.resolve()))
    pointer, size, u32 = c.c_void_p, c.c_size_t, c.c_uint32

    def bind(name, result, *arguments):
        function = getattr(dll, "mparser_" + name)
        function.restype, function.argtypes = result, arguments
        return function

    abi = bind("c_abi_revision", u32)()
    assert abi == 13 and bind("c_abi_generation", u32)() == 2
    assert c.sizeof(Options) == 192 and c.sizeof(Request) == 56 and Options.renderer.offset == 176
    create = bind("runtime_create", u32, pointer, c.POINTER(pointer))
    execute = bind("runtime_execute", u32, pointer, pointer, c.POINTER(Options), c.POINTER(pointer))
    compile_source = bind("module_compile_utf8", u32, c.c_char_p, size, c.c_char_p, size, c.POINTER(pointer))
    init = bind("invocation_options_init_sized", u32, pointer, size, u32)
    succeeded = bind("result_succeeded", u32, pointer)
    diagnostic = bind("result_diagnostic", pointer, pointer, size)
    identifier = bind("diagnostic_identifier", View, pointer)
    graphics = bind("result_graphics_json_versioned", u32, pointer, u32, c.POINTER(View))
    set_layout = bind("runtime_graphics_set_layout_environment", u32, pointer, View, View, c.POINTER(Layout))
    release_runtime = bind("runtime_release", None, pointer)
    release_module = bind("module_release", None, pointer)
    release_result = bind("result_release", None, pointer)
    runtime = pointer()
    assert create(None, c.byref(runtime)) == 0
    records = []
    fail_before_commit = False

    @CALLBACK
    def render(_data, request_ptr):
        entry = {}
        records.append(entry)
        try:
            request = request_ptr.contents
            assert (request.size, request.generation, request.version) == (56, 2, 4)
            destination = request.destination.read()
            entry.update(path=destination, format=request.format, figure=str(request.figure))
            if (destination, request.format) not in {("selected.png", 1), ("current.pdf", 2)}:
                entry["status"] = "destination-rejected"
                return 2
            scene = json.loads(request.scene.read())
            (root / f"scene-{len(records)}.json").write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
            data = encode(request.format, projection(scene, request.figure))
            expected = b"\x89PNG\r\n\x1a\n" if request.format == 1 else b"%PDF-"
            assert data.startswith(expected)
            status = publish(files / destination, data, fail_before_commit)
            entry.update(status="written" if status == 0 else "destination-rejected", encoded_bytes=len(data))
            return status
        except Exception as error:
            entry.update(status="failed", detail=str(error))
            return 3

    def run(source, callback=False, expect=None, binding=None):
        module, result = pointer(), pointer()
        data = source.encode("utf-8")
        name = b"export_probe.m"
        assert compile_source(data, len(data), name, len(name), c.byref(module)) == 0
        try:
            options = Options()
            assert init(c.byref(options), c.sizeof(options), 2) == 0
            if callback:
                options.renderer = render
            if binding is not None:
                options.workspace, options.workspace_count = c.addressof(binding), 1
            assert execute(runtime, module, c.byref(options), c.byref(result)) == 0
            if expect:
                assert not succeeded(result) and identifier(diagnostic(result, 0)).read() == expect
            else:
                assert succeeded(result), identifier(diagnostic(result, 0)).read()
            view = View()
            assert graphics(result, 4, c.byref(view)) == 0
            return json.loads(view.read())
        finally:
            release_result(result)
            release_module(module)

    try:
        seeded = run("f=figure(); f.Name='selected'; r=plot([0 1 2],[0 2 0]); r.Color='r'; "
                     "g=figure(); g.Name='current'; b=plot([0 1 2],[2 0 2]); b.Color='b';")
        figures = [node for node in seeded["objects"] if node["type"] == "Figure"]
        first, second = [node["objectIdentity"] for node in figures]
        font = ImageFont.load_default()
        ascent, descent = font.getmetrics()
        metrics = Layout(c.sizeof(Layout), 2, 640, 400, 96, font.getlength("0"), ascent + descent)
        for figure in (first, second):
            graph_data = seeded["graphIdentity"].encode()
            figure_data = figure.encode()
            graph_buffer, figure_buffer = c.create_string_buffer(graph_data), c.create_string_buffer(figure_data)
            assert set_layout(runtime, View(c.addressof(graph_buffer), len(graph_data)),
                              View(c.addressof(figure_buffer), len(figure_data)), c.byref(metrics)) == 0
        # Keep all selections in the kernel: the current Figure and the Figure
        # owning a Line supplied through a public workspace handle.
        run("print('current','-dpdf');", True)
        lookup = bind("runtime_graphics_value", u32, pointer, View, View, c.POINTER(pointer))
        line = next(node for node in seeded["objects"] if node["type"] == "Line")
        line_data = line["objectIdentity"].encode()
        line_buffer = c.create_string_buffer(line_data)
        value = pointer()
        assert lookup(runtime, View(c.addressof(graph_buffer), len(graph_data)),
                      View(c.addressof(line_buffer), len(line_data)), c.byref(value)) == 0
        try:
            run("saveas(lineTarget,'selected','png');", True, binding=NamedValue(b"lineTarget", 10, value))
        finally:
            bind("value_release", None, pointer)(value)
        assert [(record["figure"], record["status"]) for record in records] == [(second, "written"), (first, "written")]
        pdf, png = files / "current.pdf", files / "selected.png"
        before = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (pdf, png)}
        run("print('current.pdf','-dpng');", True, "MParser:GraphicsExportExtensionMismatch")
        run("print('headless','-dpng');", False, "MParser:GraphicsRendererUnavailable")
        assert len(records) == 2 and not (files / "headless.png").exists()
        run("print('../outside','-dpng');", True, "MParser:GraphicsExportDestinationRejected")
        fail_before_commit = True
        run("print('current','-dpdf');", True, "MParser:GraphicsExportFailed")
        fail_before_commit = False
        run("figure(); plot(1:3); print('current','-dpdf');", True, "MParser:GraphicsExportFailed")
        assert "explicit host metrics" in records[-1]["detail"]
        assert {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in (pdf, png)} == before
        assert not list(files.glob(".render-*")) and outside.read_bytes() == b"outside-root sentinel"
        subprocess.run([args.pdftoppm, "-png", "-r", "96", "-singlefile", str(pdf), str(root / "current-raster")],
                       check=True, timeout=30)
        pixels = {"png": verify_pixels(png, "red"), "rasterized_pdf": verify_pixels(root / "current-raster.png", "blue")}
        report = dict(abi=abi, sha256=before, requests=records, pixels=pixels, preserved_on_failure=True,
                      encoder="Pillow PNG, ReportLab vector PDF", validation="Poppler rasterized PDF and Pillow pixel counts")
        (root / "evidence.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report))
    finally:
        release_runtime(runtime)


if __name__ == "__main__":
    main()

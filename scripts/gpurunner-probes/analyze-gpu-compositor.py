#!/usr/bin/env python3
"""analyze-gpu-compositor.py — independent semantic GPU/compositor verification.

Validates the two machine-readable facts a `gpu-compositor` capability probe
must establish.  Everything here is product-neutral and independent of the
implementation that produced the pixels:

  * `renderer` — the GL_RENDERER string reported by the private Weston/EGL
    stack must be a real hardware/paravirtual accelerator (VirGL, virtio-gpu,
    or a discrete/integrated GPU).  Software rasterizers are rejected by name
    (llvmpipe, softpipe, swrast, pixman, swiftshader, lavapipe, ...) and any
    renderer that matches neither list fails closed as unverified.

  * `diagram` — a compositor-level screenshot of the production manager UI
    must show a recognizable controller silhouette in the profile-editor
    diagram region.  The analysis is purely pixel-based: it never compares
    against a golden image, never checks a texture, never consults the asset
    that produced the diagram, and never asserts "nonblank".  Six independent
    structural assertions must all hold:

        contrast   the crop has a real luminance range and multiple colors
        occupancy  the silhouette mask covers a plausible area (blank and
                   saturated regions are rejected)
        edge       organized outline edges with bounded density (a thin
                   stroke or a noisy field is rejected)
        bbox       the silhouette forms one substantial, roughly centered,
                   landscape-ish blob
        symmetry   the horizontal column profile correlates with its mirror
                   (random noise is rejected here)
        regions    both side grips and the center body are all present,
                   side grips are balanced, and the grips extend below the
                   center body bottom (a lone smooth blob is rejected)

    Any failure yields the distinct negative marker `diagram-not-recognizable`,
    which is exactly what the current BUG-0014 behavior (blank production
    diagram) must produce so later live negative-control evidence can bind it.

Usage:
  analyze-gpu-compositor.py renderer --renderer STRING
  analyze-gpu-compositor.py diagram --screenshot FILE --geometry X,Y,W,H
      [--diagram X,Y,W,H] [--out VERDICT.json]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import subprocess
from pathlib import Path

SCHEMA = "gpu-compositor-analysis/v2"
PASS_MARKER = "controller-recognized"
# Digest of data/licensed-diagram-authority.json accepted by independent
# review.  Runtime files and fixture-provided self-hashes cannot alter it.
PINNED_AUTHORITY_SHA256 = "23cb0a91cdcde1ab7bb179b4fe5f6afc340dd9f2061b9d1222be94a3341c298d"
FAIL_MARKER = "diagram-not-recognizable"

# Renderer classification ----------------------------------------------------
# Software rasterizers are rejected by name (fail closed).  Anything that is
# not software must be a recognized hardware/paravirtual accelerator; an
# unrecognized string is `renderer-unverified`, never a pass.
SOFTWARE_RENDERER_RE = re.compile(
    r"llvmpipe|softpipe|swrast|pixman|swiftshader|lavapipe|"
    r"software\s*renderer|microsoft\s*basic|gdi\s*generic|"
    r"cairo|skia|offscreen\s*software",
    re.IGNORECASE,
)
HARDWARE_RENDERER_RE = re.compile(
    r"virgl|virtio|virito|nvidia|geforce|quadro|tesla|radeon|"
    r"amd\b|intel|adreno|mali|powervr|videocore|v3d|\bvc4\b|tegra|"
    r"freedreno|panfrost|qualcomm|venus|zink|apple|imagination|"
    r"mesa\s+gallium|d3d12",
    re.IGNORECASE,
)

# Diagram analysis thresholds ------------------------------------------------
TOL_BG = 18.0            # chebyshev channel distance from bg => foreground
CONTRAST_RANGE_MIN = 15.0
CONTRAST_COLORS_MIN = 2
OCCUPANCY_MIN = 0.02
OCCUPANCY_MAX = 0.75
BOUNDARY_DENSITY_MIN = 0.004
BOUNDARY_DENSITY_MAX = 0.20
BOUNDARY_HALF_MIN = 12    # minimum boundary pixels in each horizontal half
BBOX_AREA_MIN = 0.10      # of crop area
BBOX_W_MIN = 0.30         # of crop width
BBOX_H_MIN = 0.25         # of crop height
BBOX_CENTER_X_LO = 0.35   # fraction of crop width
BBOX_CENTER_X_HI = 0.65
BBOX_ASPECT_LO = 0.90
BBOX_ASPECT_HI = 2.80
SYMMETRY_CORR_MIN = 0.50
LARGEST_COMPONENT_MIN = 0.55
REGION_LEFT_MIN = 0.02    # occupancy of the left bbox band
REGION_CENTER_MIN = 0.01
REGION_RIGHT_MIN = 0.02
REGION_BALANCE_MAX = 0.60  # |L - R| <= REGION_BALANCE_MAX * max(L, R)
GRIP_EXTENSION_MIN = 0.05  # side grips extend below the center body bottom
                            # (fraction of crop height)


def fail(message: str) -> None:
    raise SystemExit(f"gpu-compositor-analysis: {message}")


def image_size(path: Path) -> tuple[int, int]:
    result = subprocess.run(
        ["identify", "-format", "%w %h", str(path)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False,
    )
    if result.returncode != 0:
        fail(f"cannot identify screenshot {path}: {result.stderr.strip()}")
    try:
        width, height = (int(part) for part in result.stdout.split())
    except ValueError:
        fail(f"cannot parse screenshot dimensions of {path}")
    return width, height


def read_crop_rgb(path: Path, x: int, y: int, w: int, h: int) -> list[bytes]:
    """Decode a rectangular crop as raw RGB via ImageMagick (nix-shell).

    Returns one bytes row per scanline; each row has exactly w * 3 bytes.
    """
    result = subprocess.run(
        ["convert", str(path), "-crop", f"{w}x{h}+{x}+{y}", "+repage",
         "-depth", "8", "rgb:-"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode != 0:
        fail(f"cannot decode screenshot crop: {result.stderr.strip()}")
    expected = w * h * 3
    if len(result.stdout) != expected:
        fail(f"screenshot crop decode returned {len(result.stdout)} bytes, expected {expected}")
    return [result.stdout[off:off + w * 3] for off in range(0, expected, w * 3)]


def luminance(row: bytes) -> list[float]:
    return [
        0.299 * row[i] + 0.587 * row[i + 1] + 0.114 * row[i + 2]
        for i in range(0, len(row), 3)
    ]


def background_color(rows: list[bytes], w: int, h: int) -> tuple[int, int, int]:
    """Dominant border color of the crop (the panel around the diagram)."""
    border: list[tuple[int, int, int]] = []
    def add(row: bytes) -> None:
        for i in range(0, w * 3, 3):
            border.append((row[i], row[i + 1], row[i + 2]))
    add(rows[0])
    add(rows[-1])
    for row in rows:
        for i in (0, (w - 1) * 3):
            border.append((row[i], row[i + 1], row[i + 2]))
    counts: dict[tuple[int, int, int], int] = {}
    for pixel in border:
        counts[pixel] = counts.get(pixel, 0) + 1
    return max(counts, key=counts.get)


def foreground_mask(rows: list[bytes], bg: tuple[int, int, int]) -> list[bytearray]:
    mask: list[bytearray] = []
    for row in rows:
        out = bytearray(len(row) // 3)
        idx = 0
        for i in range(0, len(row), 3):
            distance = max(
                abs(row[i] - bg[0]), abs(row[i + 1] - bg[1]), abs(row[i + 2] - bg[2])
            )
            out[idx] = 1 if distance >= TOL_BG else 0
            idx += 1
        mask.append(out)
    return mask


def luminance_map(rows: list[bytes]) -> list[list[float]]:
    return [luminance(row) for row in rows]


def boundary_mask(mask: list[bytearray], w: int, h: int) -> list[bytearray]:
    """Foreground pixels that touch a background pixel (or the crop edge).

    The boundary of a filled silhouette is a thin closed outline whose total
    length is proportional to its perimeter, independent of the palette
    contrast (the production diagram is a dark silhouette on a near-black
    panel).  A blank region has no boundary; a random noise field has a
    boundary that covers most of the region.
    """
    boundary = [bytearray(w) for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if not mask[y][x]:
                continue
            if (x == 0 or y == 0 or x == w - 1 or y == h - 1
                    or not mask[y][x - 1] or not mask[y][x + 1]
                    or not mask[y - 1][x] or not mask[y + 1][x]):
                boundary[y][x] = 1
    return boundary


def mask_bbox(mask: list[bytearray], w: int, h: int) -> tuple[int, int, int, int] | None:
    min_x = min_y = None
    max_x = max_y = None
    for y in range(h):
        row = mask[y]
        for x in range(w):
            if row[x]:
                if min_x is None or x < min_x:
                    min_x = x
                if max_x is None or x > max_x:
                    max_x = x
                if min_y is None or y < min_y:
                    min_y = y
                if max_y is None or y > max_y:
                    max_y = y
    if min_x is None:
        return None
    return min_x, min_y, max_x - min_x + 1, max_y - min_y + 1


def largest_component_fraction(mask: list[bytearray], w: int, h: int) -> float:
    """4-connectivity flood fill; fraction of foreground in the largest blob."""
    total = sum(sum(row) for row in mask)
    if total == 0:
        return 0.0
    seen = [[False] * w for _ in range(h)]
    largest = 0
    for start_y in range(h):
        for start_x in range(w):
            if not mask[start_y][start_x] or seen[start_y][start_x]:
                continue
            stack = [(start_x, start_y)]
            seen[start_y][start_x] = True
            size = 0
            while stack:
                x, y = stack.pop()
                size += 1
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and mask[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        stack.append((nx, ny))
            if size > largest:
                largest = size
    return largest / total


def pearson(a: list[float], b: list[float]) -> float:
    if len(a) != len(b) or len(a) < 2:
        return 0.0
    mean_a = sum(a) / len(a)
    mean_b = sum(b) / len(b)
    num = sum((x - mean_a) * (y - mean_b) for x, y in zip(a, b))
    den_a = math.sqrt(sum((x - mean_a) ** 2 for x in a))
    den_b = math.sqrt(sum((y - mean_b) ** 2 for y in b))
    if den_a == 0.0 or den_b == 0.0:
        return 0.0
    return num / (den_a * den_b)


def analyze_diagram(screenshot: Path, geometry: tuple[int, int, int, int],
                    diagram: tuple[int, int, int, int],
                    out: Path | None) -> dict:
    """Run the six independent semantic assertions on the diagram region."""
    result: dict = {
        "schema": SCHEMA,
        "kind": "diagram",
        "screenshot": str(screenshot),
        "window_geometry": {
            "x": geometry[0], "y": geometry[1], "w": geometry[2], "h": geometry[3],
        },
        "diagram_rect_window": {
            "x": diagram[0], "y": diagram[1], "w": diagram[2], "h": diagram[3],
        },
        "result": "fail",
        "marker": FAIL_MARKER,
        "assertions": {},
        "stats": {},
    }
    width, height = image_size(screenshot)
    win_x, win_y, win_w, win_h = geometry
    diag_x, diag_y, diag_w, diag_h = diagram
    crop_x = win_x + diag_x
    crop_y = win_y + diag_y
    if (crop_x < 0 or crop_y < 0 or crop_x + diag_w > width or crop_y + diag_h > height
            or diag_w < 32 or diag_h < 32):
        result["marker"] = "window-geometry-unexpected"
        result["reason"] = "diagram crop falls outside the compositor output"
        if out:
            out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        return result

    rows = read_crop_rgb(screenshot, crop_x, crop_y, diag_w, diag_h)
    bg = background_color(rows, diag_w, diag_h)
    lum = luminance_map(rows)
    mask = foreground_mask(rows, bg)
    boundary = boundary_mask(mask, diag_w, diag_h)

    all_lum = [value for row in lum for value in row]
    lum_range = max(all_lum) - min(all_lum)
    colors = set()
    for row in rows:
        for i in range(0, len(row), 3):
            colors.add(((row[i] >> 2) << 4) | ((row[i + 1] >> 2) << 2) | (row[i + 2] >> 2))
    occupancy = sum(sum(row) for row in mask) / (diag_w * diag_h)
    boundary_count = sum(sum(row) for row in boundary)
    boundary_density = boundary_count / (diag_w * diag_h)

    bbox = mask_bbox(mask, diag_w, diag_h)
    bbox_ok = False
    symmetry = 0.0
    largest_frac = 0.0
    if bbox is not None:
        bx, by, bw, bh = bbox
        bbox_area_frac = (bw * bh) / (diag_w * diag_h)
        aspect = bw / bh if bh else 0.0
        center_x = bx + bw / 2.0
        bbox_ok = (
            bbox_area_frac >= BBOX_AREA_MIN
            and bw >= BBOX_W_MIN * diag_w
            and bh >= BBOX_H_MIN * diag_h
            and BBOX_CENTER_X_LO * diag_w <= center_x <= BBOX_CENTER_X_HI * diag_w
            and BBOX_ASPECT_LO <= aspect <= BBOX_ASPECT_HI
        )
        profile = [
            sum(mask[y][bx + col] for y in range(by, by + bh))
            for col in range(bw)
        ]
        if bw >= 20:
            symmetry = pearson(profile, list(reversed(profile)))
        largest_frac = largest_component_fraction(mask, diag_w, diag_h)

    # Region assertions: left/center/right vertical bands of the bbox.  The
    # occupancy bands plus balance prove both side grips and the center body
    # are present and symmetric; the grip-extension check proves the grips
    # actually extend below the center body bottom (a gamepad trait that a
    # lone smooth blob such as an ellipse does not have).
    left_occ = center_occ = right_occ = 0.0
    bottom_left = bottom_center = bottom_right = -1
    regions_ok = False
    if bbox is not None:
        bx, by, bw, bh = bbox
        third = max(1, bw // 3)
        band_area = third * bh
        left_occ = sum(
            mask[y][bx + col]
            for y in range(by, by + bh) for col in range(0, third)
        ) / band_area
        center_occ = sum(
            mask[y][bx + col]
            for y in range(by, by + bh) for col in range(third, 2 * third)
        ) / band_area
        right_occ = sum(
            mask[y][bx + col]
            for y in range(by, by + bh) for col in range(2 * third, bw)
        ) / band_area
        bottom_left = max(
            (y for y in range(by, by + bh)
             for col in range(0, third) if mask[y][bx + col]),
            default=-1,
        )
        bottom_center = max(
            (y for y in range(by, by + bh)
             for col in range(third, 2 * third) if mask[y][bx + col]),
            default=-1,
        )
        bottom_right = max(
            (y for y in range(by, by + bh)
             for col in range(2 * third, bw) if mask[y][bx + col]),
            default=-1,
        )
        side_max = max(left_occ, right_occ)
        grip_extension = min(bottom_left, bottom_right) - bottom_center
        regions_ok = (
            left_occ >= REGION_LEFT_MIN
            and center_occ >= REGION_CENTER_MIN
            and right_occ >= REGION_RIGHT_MIN
            and side_max > 0.0
            and abs(left_occ - right_occ) <= REGION_BALANCE_MAX * side_max
            and bottom_center >= 0
            and grip_extension >= GRIP_EXTENSION_MIN * diag_h
        )

    edge_halves_ok = False
    if bbox is not None:
        bx, by, bw, bh = bbox
        half = max(1, bw // 2)
        left_edges = sum(
            1 for y in range(by, by + bh) for x in range(bx, bx + half)
            if boundary[y][x]
        )
        right_edges = sum(
            1 for y in range(by, by + bh) for x in range(bx + half, bx + bw)
            if boundary[y][x]
        )
        edge_halves_ok = left_edges >= BOUNDARY_HALF_MIN and right_edges >= BOUNDARY_HALF_MIN

    contrast_ok = (
        lum_range >= CONTRAST_RANGE_MIN
        and len(colors) >= CONTRAST_COLORS_MIN
        and occupancy >= 0.005
    )
    occupancy_ok = OCCUPANCY_MIN <= occupancy <= OCCUPANCY_MAX
    edge_ok = (
        BOUNDARY_DENSITY_MIN <= boundary_density <= BOUNDARY_DENSITY_MAX
        and edge_halves_ok
    )
    symmetry_ok = symmetry >= SYMMETRY_CORR_MIN and largest_frac >= LARGEST_COMPONENT_MIN

    assertions = {
        "contrast": bool(contrast_ok),
        "occupancy": bool(occupancy_ok),
        "edge": bool(edge_ok),
        "bbox": bool(bbox_ok),
        "symmetry": bool(symmetry_ok),
        "regions": bool(regions_ok),
    }
    stats = {
        "luminance_range": round(lum_range, 2),
        "distinct_colors": len(colors),
        "occupancy": round(occupancy, 4),
        "boundary_density": round(boundary_density, 4),
        "largest_component_fraction": round(largest_frac, 4),
        "symmetry_correlation": round(symmetry, 4),
        "region_occupancy": {
            "left": round(left_occ, 4),
            "center": round(center_occ, 4),
            "right": round(right_occ, 4),
        },
        "grip_extension_px": int(max(0, min(bottom_left, bottom_right) - bottom_center))
        if min(bottom_left, bottom_right) >= 0 and bottom_center >= 0 else -1,
    }
    result["assertions"] = assertions
    result["stats"] = stats
    result["background"] = list(bg)
    result["result"] = "pass" if all(assertions.values()) else "fail"
    result["marker"] = PASS_MARKER if result["result"] == "pass" else FAIL_MARKER
    if out:
        out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_licensed(result: dict, screenshot: Path, geometry: tuple[int,int,int,int],
                      diagram: tuple[int,int,int,int], args) -> tuple[bool,str]:
    """Validate installed bytes against the pinned, committed authority."""
    required={"asset":Path(args.asset),"license":Path(args.license),
              "map":Path(args.icon_map),"layout":Path(args.layout),
              "oracle":Path(args.oracle),"authority":Path(args.authority)}
    for label,path in required.items():
        if not path.is_file() or path.is_symlink():
            return False, f"installed-{label}-missing"
    if sha256(required["authority"]) != PINNED_AUTHORITY_SHA256:
        return False,"installed-authority-hash-mismatch"
    try:
        authority=json.loads(required["authority"].read_text())
        oracle_doc=json.loads(required["oracle"].read_text())
        layout=json.loads(required["layout"].read_text())
    except (OSError,json.JSONDecodeError): return False,"licensed-metadata-malformed"
    if (authority.get("schema") != "controller-box-licensed-diagram-authority/v1" or
        authority.get("status") != "accepted-machine-authority"):
        return False,"licensed-authority-malformed"
    expected_files=authority.get("files")
    names={"asset":"icons/svg/xbox-360.svg",
           "license":"icons/svg/LICENSE.controllercons",
           "map":"controller-icons.yaml",
           "layout":"controller-layouts/xbox-360.json",
           "oracle":"licensed-diagram-oracle.json"}
    if not isinstance(expected_files,dict) or set(expected_files) != set(names.values()):
        return False,"licensed-authority-incomplete"
    for label,rel in names.items():
        expected=expected_files.get(rel)
        if not isinstance(expected,str) or not re.fullmatch(r"[0-9a-f]{64}",expected):
            return False,"licensed-authority-malformed"
        if sha256(required[label]) != expected:
            return False,f"installed-{label}-hash-mismatch"
    if args.model!="xb360" or args.resolved_model!="xb360" or args.resolved_asset!="xbox-360.svg":
        return False,"wrong-licensed-model"
    if args.fallback_used!="no": return False,"generic-fallback-rejected"
    if layout.get("model")!="xb360" or layout.get("asset")!="xbox-360.svg":
        return False,"licensed-metadata-model-mismatch"
    try:
        oracle=oracle_doc["models"]["xb360"]
        required_controls=oracle["required_controls"]
        if set(required_controls) != set(oracle["controls"]) or len(required_controls) != 17:
            return False,"oracle-control-coverage-incomplete"
    except (KeyError,TypeError): return False,"licensed-metadata-model-mismatch"
    # The actual logged texture dimensions, not a fixture-declared ideal,
    # must cover both the authority floor and the current drawable need.
    need_w=max(oracle["minimum_raster"][0],diagram[2])
    need_h=max(oracle["minimum_raster"][1],diagram[3])
    if args.raster_width < need_w or args.raster_height < need_h:
        return False,"raster-density-insufficient"
    dw,dh=diagram[2],diagram[3]
    if abs(dw/dh-float(oracle["source_aspect"])) > float(oracle["aspect_tolerance"]):
        return False,"diagram-aspect-distorted"
    rows=read_crop_rgb(screenshot,geometry[0]+diagram[0],geometry[1]+diagram[1],dw,dh)
    # Correlate compositor pixels to the independently pinned canonical asset.
    # Generic/synthetic controller-like polygons may exercise the structural
    # analyzer, but can never satisfy installed-licensed-diagram.
    bg=background_color(rows,dw,dh)
    rendered=subprocess.run(
        ["convert","-background",f"rgb({bg[0]},{bg[1]},{bg[2]})",
         "-size",f"{dw}x{dh}",str(required["asset"]),"-depth","8","rgb:-"],
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=False)
    if rendered.returncode != 0 or len(rendered.stdout) != dw*dh*3:
        return False,"canonical-silhouette-unavailable"
    canonical=[rendered.stdout[o:o+dw*3] for o in range(0,len(rendered.stdout),dw*3)]
    observed_mask=foreground_mask(rows,bg)
    canonical_mask=foreground_mask(canonical,bg)
    intersection=union=0
    for y in range(dh):
        for x in range(dw):
            a=bool(observed_mask[y][x]); b=bool(canonical_mask[y][x])
            intersection += int(a and b); union += int(a or b)
    silhouette_iou=intersection/union if union else 0.0
    if silhouette_iou < 0.80:
        return False,"canonical-silhouette-mismatch"
    pts=[]
    for y,row in enumerate(rows):
        for x in range(dw):
            r,g,b=row[x*3:x*3+3]
            if b >= 110 and b-r >= 35 and b-g >= 15: pts.append((x,y))
    spec=oracle["controls"]["A"]
    if len(pts)<spec["minimum_pixels"]: return False,"highlight-missing"
    cx=sum(x for x,_ in pts)/len(pts); cy=sum(y for _,y in pts)/len(pts)
    if not (spec["centroid_x"][0] <= cx <= spec["centroid_x"][1] and spec["centroid_y"][0] <= cy <= spec["centroid_y"][1]):
        return False,"highlight-oracle-misaligned"
    result["licensed"]={"model":"xb360","asset":"xbox-360.svg","fallback":False,
        "authority_sha256":PINNED_AUTHORITY_SHA256,
        "hashes":{k:sha256(v) for k,v in required.items()},"raster":[args.raster_width,args.raster_height],
        "raster_need":[need_w,need_h],"canonical_silhouette_iou":round(silhouette_iou,4),
        "oracle_controls":required_controls,
        "highlight":{"control":"A","centroid":[round(cx,2),round(cy,2)],"pixels":len(pts),"oracle":"independent"}}
    return True,"installed-licensed-diagram-verified"


def validate_renderer(renderer: str) -> tuple[str, bool]:
    if not renderer or not renderer.strip():
        return "renderer-unverified", False
    if SOFTWARE_RENDERER_RE.search(renderer):
        return "software-renderer-rejected", False
    if not HARDWARE_RENDERER_RE.search(renderer):
        return "renderer-unverified", False
    return "renderer-accepted", True


def main() -> int:
    parser = argparse.ArgumentParser(description="GPU/compositor semantic analysis")
    sub = parser.add_subparsers(dest="command", required=True)

    renderer = sub.add_parser("renderer", help="validate a GL_RENDERER string")
    renderer.add_argument("--renderer", required=True, help="GL_RENDERER string")
    renderer.add_argument("--out", help="write verdict JSON here")

    diagram = sub.add_parser("diagram", help="analyze the diagram region of a screenshot")
    diagram.add_argument("--screenshot", required=True, help="compositor-level PNG")
    diagram.add_argument("--geometry", required=True, help="window rect X,Y,W,H in output coords")
    diagram.add_argument("--diagram", default="16,88,300,300", help="diagram rect relative to window")
    diagram.add_argument("--out", help="write verdict JSON here")
    diagram.add_argument("--model", default="")
    diagram.add_argument("--resolved-model", default="")
    diagram.add_argument("--resolved-asset", default="")
    diagram.add_argument("--fallback-used", default="yes")
    diagram.add_argument("--raster-width", type=int, default=0)
    diagram.add_argument("--raster-height", type=int, default=0)
    for name in ("asset","license","icon-map","layout","oracle","authority"):
        diagram.add_argument(f"--{name}")

    args = parser.parse_args()
    if args.command == "renderer":
        marker, ok = validate_renderer(args.renderer)
        verdict = {
            "schema": SCHEMA,
            "kind": "renderer",
            "renderer": args.renderer,
            "result": "pass" if ok else "fail",
            "marker": marker,
        }
        if args.out:
            Path(args.out).write_text(json.dumps(verdict, indent=2) + "\n", encoding="utf-8")
        print(f"gpu-compositor-analysis: renderer marker {marker}")
        return 0 if ok else 1

    try:
        geometry = tuple(int(part) for part in args.geometry.split(","))
        diagram_rect = tuple(int(part) for part in args.diagram.split(","))
    except ValueError:
        fail("geometry/diagram must be X,Y,W,H integers")
    if len(geometry) != 4 or len(diagram_rect) != 4:
        fail("geometry/diagram must be X,Y,W,H integers")
    out = Path(args.out) if args.out else None
    result = analyze_diagram(Path(args.screenshot), geometry, diagram_rect, None)
    licensed_requested = bool(args.model)
    if result["result"] == "pass" and licensed_requested:
        if not all((args.asset,args.license,args.icon_map,args.layout,args.oracle,args.authority)):
            result["result"]="fail"; result["marker"]="licensed-artifacts-missing"
        else:
            ok,marker=validate_licensed(result,Path(args.screenshot),geometry,diagram_rect,args)
            result["result"]="pass" if ok else "fail"; result["marker"]=marker
    if out:
        out.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(
        f"gpu-compositor-analysis: diagram marker {result['marker']} "
        f"(contrast={result['assertions']['contrast']}, "
        f"occupancy={result['assertions']['occupancy']}, "
        f"edge={result['assertions']['edge']}, "
        f"bbox={result['assertions']['bbox']}, "
        f"symmetry={result['assertions']['symmetry']}, "
        f"regions={result['assertions']['regions']})"
    )
    return 0 if result["result"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())

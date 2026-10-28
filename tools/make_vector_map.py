#!/usr/bin/env python3
"""Generate the compact Natural Earth vector map used by map mode.

The base map uses 1:50m ocean, country borders and rivers, plus 1:10m
first-order district boundaries and populated places. Coordinates are int16 at
0.005 degree resolution; long district segments are merged and simplified to
keep the generated header within the ESP32's 3 MB application partition.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import unicodedata
import urllib.request

SOURCES = {
    "ocean": "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "master/geojson/ne_50m_ocean.geojson",
    "borders": "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "master/geojson/ne_50m_admin_0_boundary_lines_land.geojson",
    "rivers": "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "master/geojson/ne_50m_rivers_lake_centerlines.geojson",
    "districts": "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "master/geojson/ne_10m_admin_1_states_provinces_lines.geojson",
    "district_labels": "https://raw.githubusercontent.com/nvkelso/"
    "natural-earth-vector/master/geojson/"
    "ne_10m_admin_1_states_provinces.geojson",
    "places": "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
    "master/geojson/ne_10m_populated_places_simple.geojson",
}

SCALE = 200  # 0.005 degrees per unit
SIMPLIFY = 2  # Douglas-Peucker tolerance, also in 0.005 degree units
DISTRICT_SIMPLIFY = 4
EUROPE = (-25.0, 34.0, 45.0, 72.0)  # lon min, lat min, lon max, lat max

SPECIAL_ASCII = {
    "Æ": "AE", "æ": "ae", "Œ": "OE", "œ": "oe",
    "Ø": "O", "ø": "o", "Ð": "D", "ð": "d",
    "Þ": "Th", "þ": "th", "ß": "ss",
    "Ł": "L", "ł": "l", "Đ": "D", "đ": "d",
    "İ": "I", "ı": "i", "Ğ": "G", "ğ": "g",
    "Ş": "S", "ş": "s", "Ŋ": "N", "ŋ": "n",
    "ſ": "s",
}


def to_ascii(text: str) -> str:
    expanded = "".join(SPECIAL_ASCII.get(ch, ch) for ch in text)
    decomposed = unicodedata.normalize("NFKD", expanded)
    return "".join(
        ch for ch in decomposed
        if not unicodedata.combining(ch) and ord(ch) < 128
    ).strip()


def download(name: str, cache: pathlib.Path) -> dict:
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / f"{name}.geojson"
    if not path.exists():
        request = urllib.request.Request(
            SOURCES[name], headers={"User-Agent": "eink-color-display/1.0"}
        )
        with urllib.request.urlopen(request, timeout=120) as response:
            path.write_bytes(response.read())
    return json.loads(path.read_text())


def lines_from_geometry(geometry: dict) -> list[list[list[float]]]:
    if not geometry:
        return []
    kind = geometry["type"]
    coordinates = geometry["coordinates"]
    if kind == "LineString":
        return [coordinates]
    if kind == "MultiLineString":
        return coordinates
    if kind == "Polygon":
        return coordinates
    if kind == "MultiPolygon":
        return [ring for polygon in coordinates for ring in polygon]
    return []


def quantise(value: float) -> int:
    return max(-32768, min(32767, int(round(value * SCALE))))


def simplify(points: list[tuple[int, int]], tolerance: float) -> list[tuple[int, int]]:
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        first, last = stack.pop()
        if last <= first + 1:
            continue
        x1, y1 = points[first]
        x2, y2 = points[last]
        dx, dy = x2 - x1, y2 - y1
        denominator = math.hypot(dx, dy)
        best = -1
        best_distance = -1.0
        for index in range(first + 1, last):
            x, y = points[index]
            if denominator == 0:
                distance = math.hypot(x - x1, y - y1)
            else:
                distance = abs(dy * x - dx * y + x2 * y1 - y2 * x1) / denominator
            if distance > best_distance:
                best_distance = distance
                best = index
        if best_distance > tolerance:
            keep[best] = True
            stack.append((first, best))
            stack.append((best, last))
    return [point for point, selected in zip(points, keep) if selected]


def in_europe(lon: float, lat: float) -> bool:
    lon_min, lat_min, lon_max, lat_max = EUROPE
    return lon_min <= lon <= lon_max and lat_min <= lat <= lat_max


def merge_segments(segments: list[list[tuple[int, int]]]) -> list[list[tuple[int, int]]]:
    """Greedily join line strings that share endpoints."""
    pending = [list(segment) for segment in segments if len(segment) >= 2]
    merged: list[list[tuple[int, int]]] = []
    while pending:
        chain = pending.pop()
        changed = True
        while changed:
            changed = False
            for index, segment in enumerate(pending):
                if chain[-1] == segment[0]:
                    chain += segment[1:]
                elif chain[-1] == segment[-1]:
                    chain += list(reversed(segment))[1:]
                elif chain[0] == segment[-1]:
                    chain = segment + chain[1:]
                elif chain[0] == segment[0]:
                    chain = list(reversed(segment)) + chain[1:]
                else:
                    continue
                pending.pop(index)
                changed = True
                break
        merged.append(chain)
    return merged


def add_table_entry(table: list[dict], points: list[tuple[int, int]],
                    line: list[tuple[int, int]]) -> None:
    if len(line) < 2:
        return
    offset = len(points)
    points.extend(line)
    xs = [point[0] for point in line]
    ys = [point[1] for point in line]
    table.append(
        {
            "offset": offset,
            "count": len(line),
            "min_lon": min(xs),
            "min_lat": min(ys),
            "max_lon": max(xs),
            "max_lat": max(ys),
        }
    )


def build_table(features: list[dict], points: list[tuple[int, int]]) -> list[dict]:
    table: list[dict] = []
    for feature in features:
        for line in lines_from_geometry(feature["geometry"]):
            if len(line) < 2:
                continue
            quantised = [(quantise(lon), quantise(lat)) for lon, lat in line]
            simplified = simplify(quantised, SIMPLIFY)
            if len(simplified) >= 2:
                quantised = simplified
            add_table_entry(table, points, quantised)
    return table


def build_district_table(features: list[dict],
                         points: list[tuple[int, int]]) -> list[dict]:
    table: list[dict] = []
    for feature in features:
        segments = []
        for line in lines_from_geometry(feature["geometry"]):
            if len(line) < 2:
                continue
            if not any(in_europe(lon, lat) for lon, lat in line):
                continue
            quantised = [(quantise(lon), quantise(lat)) for lon, lat in line]
            simplified = simplify(quantised, DISTRICT_SIMPLIFY)
            if len(simplified) >= 2:
                segments.append(simplified)
        for merged in merge_segments(segments):
            add_table_entry(table, points, merged)
    return table


def build_places(features: list[dict]) -> list[dict]:
    places = []
    for feature in features:
        geometry = feature.get("geometry")
        if not geometry or geometry.get("type") != "Point":
            continue
        lon, lat = geometry["coordinates"]
        properties = feature["properties"]
        name = to_ascii(properties.get("nameascii") or
                        properties.get("name") or "")
        rank = int(properties.get("scalerank", 99))
        if not name or rank > 8:
            continue
        places.append(
            {
                "lon": quantise(lon),
                "lat": quantise(lat),
                "rank": rank,
                "population": int(properties.get("pop_max") or 0),
                "name": name,
            }
        )
    places.sort(key=lambda place: (place["rank"], -place["population"],
                                   place["name"]))
    return places


def build_district_labels(features: list[dict]) -> list[dict]:
    labels = []
    for feature in features:
        properties = feature["properties"]
        lon = properties.get("longitude")
        lat = properties.get("latitude")
        if lon is None or lat is None or not in_europe(lon, lat):
            continue
        name = to_ascii(properties.get("name") or
                        properties.get("name_en") or "")
        if not name:
            continue
        labels.append(
            {
                "lon": quantise(lon),
                "lat": quantise(lat),
                "rank": int(properties.get("labelrank") or 10),
                "population": 0,
                "name": name,
            }
        )
    labels.sort(key=lambda label: (label["rank"], label["name"]))
    return labels


def write_polyline_table(lines: list[str], count_name: str, array_name: str,
                         entries: list[dict]) -> None:
    lines.extend([
        f"constexpr uint16_t {count_name} = {len(entries)};",
        f"const MapPolyline {array_name}[] = {{",
    ])
    for entry in entries:
        lines.append(
            "    {%d, %d, %d, %d, %d, %d},"
            % (
                entry["offset"],
                entry["count"],
                entry["min_lon"],
                entry["min_lat"],
                entry["max_lon"],
                entry["max_lat"],
            )
        )
    lines.append("};")
    lines.append("")


def write_place_table(lines: list[str], count_name: str, array_name: str,
                      entries: list[dict]) -> None:
    lines.extend([
        f"constexpr uint16_t {count_name} = {len(entries)};",
        f"const MapPlace {array_name}[] = {{",
    ])
    for entry in entries:
        lines.append(
            '    {%d, %d, %d, %s},'
            % (entry["lon"], entry["lat"], entry["rank"], json.dumps(entry["name"]))
        )
    lines.append("};")
    lines.append("")


def write_header(path: pathlib.Path, ocean: list[dict], borders: list[dict],
                 rivers: list[dict], districts: list[dict],
                 district_labels: list[dict], places: list[dict],
                 points: list[tuple[int, int]]) -> None:
    lines = [
        "// Generated by tools/make_vector_map.py. Do not edit by hand.",
        "// Natural Earth: 1:50m ocean/borders/rivers, 1:10m districts/places.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "struct MapPolyline {",
        "  uint32_t offset;",
        "  uint16_t count;",
        "  int16_t min_lon;",
        "  int16_t min_lat;",
        "  int16_t max_lon;",
        "  int16_t max_lat;",
        "};",
        "",
        "struct MapPlace {",
        "  int16_t lon;",
        "  int16_t lat;",
        "  uint8_t rank;",
        "  const char* name;",
        "};",
        "",
    ]
    write_polyline_table(lines, "kOceanRingCount", "kOceanRings", ocean)
    write_polyline_table(lines, "kBorderLineCount", "kBorderLines", borders)
    write_polyline_table(lines, "kRiverLineCount", "kRiverLines", rivers)
    write_polyline_table(lines, "kDistrictLineCount", "kDistrictLines", districts)
    write_place_table(lines, "kDistrictLabelCount", "kDistrictLabels",
                      district_labels)
    write_place_table(lines, "kMapPlaceCount", "kMapPlaces", places)
    lines.extend([
        f"constexpr uint32_t kVectorMapPointCount = {len(points)};",
        "const int16_t kVectorMapPoints[] = {",
    ])
    for start in range(0, len(points), 8):
        chunk = ", ".join(f"{lon},{lat}" for lon, lat in points[start : start + 8])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    path.write_text("\n".join(lines))
    print(
        f"wrote {path}: {len(ocean)} ocean rings, {len(borders)} border lines, "
        f"{len(rivers)} rivers, {len(districts)} district lines, "
        f"{len(district_labels)} district labels, {len(places)} places, "
        f"{len(points)} points ({len(points) * 4} coordinate bytes)"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cache",
        type=pathlib.Path,
        default=pathlib.Path("/tmp/opencode/vector-map-cache"),
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent / "src"
        / "vector_map.h",
    )
    args = parser.parse_args()

    points: list[tuple[int, int]] = []
    ocean = build_table(download("ocean", args.cache)["features"], points)
    borders = build_table(download("borders", args.cache)["features"], points)
    rivers = build_table(download("rivers", args.cache)["features"], points)
    districts = build_district_table(download("districts", args.cache)["features"],
                                     points)
    district_labels = build_district_labels(
        download("district_labels", args.cache)["features"]
    )
    places = build_places(download("places", args.cache)["features"])

    write_header(args.output, ocean, borders, rivers, districts,
                 district_labels, places, points)


if __name__ == "__main__":
    main()

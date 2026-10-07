#!/usr/bin/env python3
"""
Build pipeline: web/index.html → gzip → APP/web_assets.h (C byte array)
Usage: python tools/build_web_assets.py [--input web/index.html] [--output APP/web_assets.h]
"""
import argparse
import gzip
import os
import sys


def html_to_c_header(html_path: str, output_path: str) -> None:
    with open(html_path, "rb") as f:
        raw = f.read()

    compressed = gzip.compress(raw, compresslevel=9)
    length = len(compressed)

    print(f"  Raw HTML:  {len(raw):>8} bytes")
    print(f"  Gzipped:   {length:>8} bytes ({100*length/len(raw):.1f}%)")

    lines = [
        "/**",
        " * @file    web_assets.h",
        " * @brief   Kontrx — Embedded SPA Web Assets (GZIPPED HTML/CSS/JS)",
        f" *          Auto-generated from {os.path.basename(html_path)} ({length} bytes gzipped)",
        " */",
        "",
        "#ifndef WEB_ASSETS_H",
        "#define WEB_ASSETS_H",
        "",
        "#include <stddef.h>",
        "",
        f"#define KONTRX_HTML_LEN {length}",
        "",
        f"static const uint8_t KONTRX_HTML[{length}] = {{",
    ]

    for i in range(0, length, 16):
        chunk = compressed[i:i+16]
        hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
        comma = "," if i + 16 < length else ""
        lines.append(f"    {hex_vals}{comma}")

    lines.append("};")
    lines.append("")
    lines.append("#endif /* WEB_ASSETS_H */")
    lines.append("")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"  Output:    {output_path}")


def extract_html(header_path: str, output_path: str) -> None:
    """Extract gzipped HTML from existing web_assets.h for editing."""
    import re

    with open(header_path, "r", encoding="utf-8", errors="replace") as f:
        content = f.read()

    hex_values = re.findall(r"0x([0-9a-fA-F]{2})", content)
    data = bytes(int(h, 16) for h in hex_values)
    html = gzip.decompress(data)

    with open(output_path, "wb") as f:
        f.write(html)

    print(f"  Extracted: {len(html)} bytes -> {output_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Kontrx web assets build tool")
    sub = parser.add_subparsers(dest="cmd")

    build = sub.add_parser("build", help="Compile HTML → gzipped C header")
    build.add_argument("--input", default="web/index.html")
    build.add_argument("--output", default="APP/web_assets.h")

    extract = sub.add_parser("extract", help="Extract HTML from existing header")
    extract.add_argument("--input", default="APP/web_assets.h")
    extract.add_argument("--output", default="web/index.html")

    args = parser.parse_args()

    if args.cmd == "build":
        html_to_c_header(args.input, args.output)
    elif args.cmd == "extract":
        extract_html(args.input, args.output)
    else:
        parser.print_help()

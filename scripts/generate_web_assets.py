"""Compress karute's web UI into read-only firmware assets.

The editable sources live in web/*.html. Browsers understand gzip directly,
so the ESP32 only serves these bytes and does not spend RAM or CPU inflating
them. The generated C++ header lives under .pio and is never hand-edited.
"""

from pathlib import Path
import gzip


PAGES = (
    ("index.html", "kIndexHtmlGz"),
    ("settings.html", "kSettingsHtmlGz"),
    ("wifi.html", "kWifiSettingsHtmlGz"),
    ("help.html", "kHelpHtmlGz"),
)


def byte_array(name, compressed):
    lines = [f"static const uint8_t {name}[] PROGMEM = {{"]
    for offset in range(0, len(compressed), 16):
        chunk = compressed[offset:offset + 16]
        lines.append("  " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    lines.append("};")
    lines.append(f"static constexpr size_t {name}Size = sizeof({name});")
    return lines


def generate(project_dir, output_dir):
    project_dir = Path(project_dir)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "// Generated from web/*.html; do not edit.",
        "#pragma once",
        "#include <Arduino.h>",
        "namespace karute_web {",
    ]
    totals = []
    for filename, symbol in PAGES:
        source = (project_dir / "web" / filename).read_bytes()
        compressed = gzip.compress(source, compresslevel=9, mtime=0)
        lines.extend(byte_array(symbol, compressed))
        totals.append((filename, len(source), len(compressed)))
    lines.extend(["}  // namespace karute_web", ""])
    content = "\n".join(lines)
    target = output_dir / "karute_web_assets.h"
    if not target.exists() or target.read_text(encoding="utf-8") != content:
        target.write_text(content, encoding="utf-8")
    return totals


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("project_dir")
    parser.add_argument("output_dir")
    args = parser.parse_args()
    for page, source_size, compressed_size in generate(
            args.project_dir, args.output_dir):
        print(f"{page}: {source_size} -> {compressed_size} bytes")
else:
    Import("env")
    project = Path(env.subst("$PROJECT_DIR"))
    generated = Path(env.subst("$BUILD_DIR")) / "generated"
    totals = generate(project, generated)
    env.Append(CPPPATH=[str(generated)])
    summary = ", ".join(
        f"{page} {source_size}->{compressed_size}"
        for page, source_size, compressed_size in totals)
    print(f"Web assets: {summary}")

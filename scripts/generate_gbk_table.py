"""Generate a read-only Unicode -> GBK table as a build artifact.

No downloaded library, clinical text, or user configuration is involved.
Python's strict GBK codec is the source; unmappable characters are rejected.
The generated header lives under .pio, never in the stable firmware source.
"""

from pathlib import Path


def mappings():
    result = []
    for codepoint in range(0x80, 0x10000):
        try:
            encoded = chr(codepoint).encode("gbk", errors="strict")
        except UnicodeEncodeError:
            continue
        if len(encoded) == 2:
            result.append((codepoint << 16) | int.from_bytes(encoded, "big"))
    return result


def generate(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    entries = mappings()
    lines = [
        "// Generated from Python's strict GBK codec; do not edit.",
        "#pragma once", "#include <stdint.h>", "#include <stddef.h>",
        "namespace karute_alt {",
        "static constexpr uint32_t kGbkPairs[] = {",
    ]
    for offset in range(0, len(entries), 8):
        lines.append("  " + ", ".join(
            f"0x{entry:08x}u" for entry in entries[offset:offset + 8]) + ",")
    lines.extend(["};", "}  // namespace karute_alt", ""])
    content = "\n".join(lines)
    target = directory / "karute_gbk_table.h"
    if not target.exists() or target.read_text(encoding="utf-8") != content:
        target.write_text(content, encoding="utf-8")
    return len(entries)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("directory")
    args = parser.parse_args()
    print(f"GBK table: {generate(args.directory)} mappings")
else:
    # PlatformIO evaluates this file with SCons' Import helper.
    Import("env")
    generated = Path(env.subst("$BUILD_DIR")) / "generated"
    count = generate(generated)
    env.Append(CPPPATH=[str(generated)])
    print(f"Alt test: generated {count} strict GBK mappings")

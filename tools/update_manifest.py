#!/usr/bin/env python3
"""
Update ESP Web Tools manifest.json from Arduino build output.

Parses flash_args for filenames/offsets, reads version from library.properties,
and optionally copies firmware files to the website directory.

Usage:
    python3 tools/update_manifest.py                    # update all builds
    python3 tools/update_manifest.py --board touch      # update specific board
    python3 tools/update_manifest.py --deploy            # also copy to website dir
"""

import json
import re
import shutil
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent
BUILD_DIR = PROJECT_DIR / "build"
LIBRARY_PROPERTIES = PROJECT_DIR / "library.properties"

BOARD_CONFIG = {
    "esp32.esp32.waveshare_esp32_s3_touch_amoled_164": {
        "name": "Homewind Touch",
        "chipFamily": "ESP32-S3",
        "alias": "touch",
        "website_dir": "homewind-touch",
    },
    "esp32.esp32.XIAO_ESP32S3": {
        "name": "Homewind Basic",
        "chipFamily": "ESP32-S3",
        "alias": "basic",
        "website_dir": "homewind-basic",
    },
}

WEBSITE_FIRMWARE_DIR = Path.home() / "Documents/Works/Homewind/Website/src/firmware"


def read_version() -> str:
    """Read version from library.properties."""
    for line in LIBRARY_PROPERTIES.read_text().splitlines():
        if line.startswith("version="):
            return line.split("=", 1)[1].strip()
    raise ValueError(f"No version= found in {LIBRARY_PROPERTIES}")


def parse_flash_args(build_path: Path) -> list[dict]:
    """Parse flash_args to extract filenames and offsets.

    Format:
        --flash-mode dio --flash-freq 80m --flash-size 16MB
        0x0 Homewind.ino.bootloader.bin
        0x8000 Homewind.ino.partitions.bin
        ...
    """
    flash_args = build_path / "flash_args"
    if not flash_args.exists():
        raise FileNotFoundError(f"Missing {flash_args}")

    parts = []
    for line in flash_args.read_text().splitlines():
        match = re.match(r"^(0x[0-9a-fA-F]+)\s+(\S+)$", line.strip())
        if match:
            offset = int(match.group(1), 16)
            filename = match.group(2)
            bin_file = build_path / filename
            if not bin_file.exists():
                print(f"  WARNING: {filename} not found in {build_path}")
            parts.append({"path": filename, "offset": offset})
    return parts


def build_manifest(name: str, version: str, chip_family: str, parts: list[dict]) -> dict:
    return {
        "name": name,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": chip_family,
                "parts": [{"path": p["path"], "offset": p["offset"]} for p in parts],
            }
        ],
    }


def write_manifest(manifest: dict, path: Path) -> None:
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"  Written: {path}")


def deploy_to_website(build_path: Path, website_dir_name: str, manifest: dict) -> None:
    """Copy manifest + .bin files to the website firmware directory."""
    dest = WEBSITE_FIRMWARE_DIR / website_dir_name
    if not dest.parent.exists():
        print(f"  SKIP deploy: {dest.parent} does not exist")
        return

    dest.mkdir(parents=True, exist_ok=True)

    write_manifest(manifest, dest / "manifest.json")

    for part in manifest["builds"][0]["parts"]:
        src = build_path / part["path"]
        if src.exists():
            shutil.copy2(src, dest / part["path"])
            size_kb = src.stat().st_size / 1024
            print(f"  Copied: {part['path']} ({size_kb:.1f} KB)")
        else:
            print(f"  WARNING: {src} not found, skipping copy")


def process_board(board_dir: str, config: dict, version: str, deploy: bool) -> bool:
    build_path = BUILD_DIR / board_dir
    if not build_path.exists():
        print(f"  SKIP: {build_path} does not exist")
        return False

    print(f"\n[{config['name']}] ({board_dir})")

    parts = parse_flash_args(build_path)
    if not parts:
        print("  ERROR: No flash parts found in flash_args")
        return False

    manifest = build_manifest(config["name"], version, config["chipFamily"], parts)

    write_manifest(manifest, build_path / "manifest.json")

    if deploy:
        deploy_to_website(build_path, config["website_dir"], manifest)

    return True


def main():
    import argparse

    parser = argparse.ArgumentParser(description="Update ESP Web Tools manifest.json")
    parser.add_argument("--board", choices=["touch", "basic", "all"], default="all",
                        help="Which board to update (default: all)")
    parser.add_argument("--deploy", action="store_true",
                        help="Also copy firmware files to website directory")
    args = parser.parse_args()

    version = read_version()
    print(f"Version: {version}")

    success = 0
    for board_dir, config in BOARD_CONFIG.items():
        if args.board != "all" and config["alias"] != args.board:
            continue
        if process_board(board_dir, config, version, args.deploy):
            success += 1

    print(f"\nDone: {success} board(s) updated.")
    return 0 if success > 0 else 1


if __name__ == "__main__":
    sys.exit(main())

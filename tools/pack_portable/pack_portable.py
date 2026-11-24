#!/usr/bin/env python3
"""Pack Pulse into a portable ZIP archive:

    Pulse-Portable/
      pulse.exe               (root launcher)
      portable                (marker file activating portable mode)
      Program/
        dialog-current.json
        DialogBridge/<sha256>/...
      Data/                   (all user data stored here in portable mode)
"""
from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--build", required=True, help="CMake build directory")
    ap.add_argument("--version", required=True, help="Pulse version string")
    ap.add_argument("--out", required=True, help="Output .zip path")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    build = Path(args.build)
    actual_build = build if build.is_absolute() else (root / build).resolve()

    sys.path.insert(0, str(root / "tools" / "pack_installer"))
    sys.path.insert(0, str(root / "tools" / "pack_dialog_experiment"))
    import pack_installer
    import pack as dialog_pack

    items = pack_installer.collect(root, actual_build)
    machine = dialog_pack.PE(actual_build / "pulse.exe").machine
    arch = next((name for name, value in dialog_pack.MACHINES.items() if value == machine), None)
    if arch is None:
        raise SystemExit("unsupported Pulse architecture for portable bundle")

    out_zip = Path(args.out)
    out_zip.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="pulse-portable-") as staging:
        staging_path = Path(staging)
        installed = staging_path / "installed"
        dialog_pack.package_installed(actual_build, installed, args.version, arch, root, items)
        dialog_pack.verify_installed(installed)

        portable_root = staging_path / "Pulse-Portable"
        program_dir = portable_root / "Program"
        data_dir = portable_root / "Data"
        program_dir.mkdir(parents=True, exist_ok=True)
        data_dir.mkdir(parents=True, exist_ok=True)

        # Root launcher
        shutil.copy2(installed / "pulse.exe", portable_root / "pulse.exe")
        # Portable activation marker file
        (portable_root / "portable").write_bytes(b"")
        # Runtime bundle inside Program/
        shutil.move(str(installed / "dialog-current.json"), str(program_dir / "dialog-current.json"))
        shutil.move(str(installed / "DialogBridge"), str(program_dir / "DialogBridge"))

        file_count = 0
        with zipfile.ZipFile(out_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            # Ensure empty Data/ directory is present in the archive
            zf.writestr("Pulse-Portable/Data/", b"")
            for p in sorted(portable_root.rglob("*")):
                if p.is_file():
                    arcname = p.relative_to(staging_path).as_posix()
                    zf.write(p, arcname)
                    file_count += 1

    size_mb = out_zip.stat().st_size / (1024 * 1024)
    print(f"Packed {file_count} files ({size_mb:.2f} MiB) -> {out_zip}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

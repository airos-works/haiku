#!/usr/bin/env python3
"""Build, but do not install, a one-time Linux reference boot on a FAT ESP.

The embedded GRUB script must consume and read back the trial flag before
starting Linux. Later boots and pre-boot failures chainload the saved Haiku
loader. This changes no partition tables, firmware or EFI variables.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urlsplit


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--haiku-loader", type=Path, required=True)
    parser.add_argument("--expected-loader-sha256", required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--initrd", type=Path, required=True)
    parser.add_argument("--microcode", type=Path)
    parser.add_argument("--esp-uuid", required=True)
    parser.add_argument("--http-root", required=True,
                        help="HTTP directory containing sysresccd/ and autorun0")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-fA-F]{4}-[0-9a-fA-F]{4}", args.esp_uuid):
        parser.error("expected FAT volume UUID")
    if not re.fullmatch(r"[0-9a-f]{64}", args.expected_loader_sha256):
        parser.error("expected a lowercase SHA-256 digest")
    if digest(args.haiku_loader) != args.expected_loader_sha256:
        parser.error("Haiku recovery loader digest mismatch")
    url = urlsplit(args.http_root)
    if (url.scheme not in ("http", "https") or not url.hostname
            or url.username or url.password or url.query or url.fragment
            or not re.fullmatch(r"[A-Za-z0-9./:_%-]+", args.http_root)):
        parser.error("HTTP root must be an uncredentialed URL without shell syntax")
    http_root = args.http_root.rstrip("/") + "/"
    args.output.mkdir(parents=True, exist_ok=False)
    trial = args.output / "EFI/WX5100"
    boot = args.output / "EFI/BOOT"
    trial.mkdir(parents=True)
    boot.mkdir(parents=True)
    shutil.copyfile(args.haiku_loader, trial / "HAIKU.EFI")
    shutil.copyfile(args.kernel, trial / "VMLINUZ")
    with (trial / "INITRD.IMG").open("wb") as target:
        for source in (args.microcode, args.initrd):
            if source:
                with source.open("rb") as stream:
                    shutil.copyfileobj(stream, target)
    env = trial / "GRUBENV"
    subprocess.run(["grub-editenv", str(env), "create"], check=True)
    subprocess.run(["grub-editenv", str(env), "set", "wx_trial=armed"], check=True)
    # All interpolated values are validated above. File paths in the ESP are
    # fixed, and the original loader is independently hash-checked.
    config = f'''serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
terminal_output console serial
search --no-floppy --fs-uuid --set=wx_esp {args.esp_uuid}
set wx_env=($wx_esp)/EFI/WX5100/GRUBENV
set wx_boot_linux=0
if load_env --file=$wx_env wx_trial; then
    if [ "$wx_trial" = "armed" ]; then
        set wx_trial=consumed
        if save_env --file=$wx_env wx_trial; then
            unset wx_trial
            if load_env --file=$wx_env wx_trial; then
                if [ "$wx_trial" = "consumed" ]; then
                    set wx_boot_linux=1
                fi
            fi
        fi
    fi
fi
if [ "$wx_boot_linux" = "1" ]; then
    echo WX5100_TRIAL_CONSUMED_STARTING_LINUX
    if linux ($wx_esp)/EFI/WX5100/VMLINUZ archisobasedir=sysresccd ip=dhcp archiso_http_srv={http_root} checksum nomdlvm nofirewall ar_source={http_root.rstrip('/')} ar_suffixes=0 ar_nowait iomem=relaxed console=tty0 console=ttyS0,115200n8; then
        if initrd ($wx_esp)/EFI/WX5100/INITRD.IMG; then
            boot
        fi
    fi
fi
echo WX5100_CHAINLOADING_HAIKU_RECOVERY
chainloader ($wx_esp)/EFI/WX5100/HAIKU.EFI
boot
'''
    config_path = args.output / "grub.cfg"
    config_path.write_text(config)
    subprocess.run([
        "grub-mkstandalone", "-O", "x86_64-efi", "--locales=", "--fonts=",
        "--modules=part_gpt part_msdos fat search search_fs_uuid chain linux loadenv test serial normal",
        "-o", str(boot / "BOOTX64.EFI"),
        "boot/grub/grub.cfg=" + str(config_path),
    ], check=True)
    manifest = {
        "purpose": "One-time Linux reference boot with persistent Haiku fallback",
        "esp_uuid": args.esp_uuid,
        "http_root": http_root,
        "haiku_loader_sha256": args.expected_loader_sha256,
        "files": {str(p.relative_to(args.output)): {
            "bytes": p.stat().st_size, "sha256": digest(p)
        } for p in sorted(args.output.rglob("*")) if p.is_file()},
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()

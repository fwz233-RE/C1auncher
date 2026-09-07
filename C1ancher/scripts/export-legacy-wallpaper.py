"""One-time migration of the existing display asset out of C source."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
source = subprocess.check_output(["git", "show", "v1.3.0:src/ui/wallpaper.c"], cwd=root, text=True)
frame = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})U", source))
assert len(frame) == 296 * 152 // 8
output = root.parent / "Pic" / "wallpaper.raw"
output.parent.mkdir(parents=True, exist_ok=True)
if output.exists():
    raise SystemExit("Refusing to overwrite existing wallpaper")
output.write_bytes(frame)
print(f"Exported original wallpaper: {output} ({len(frame)} bytes)")

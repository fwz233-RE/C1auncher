"""Normalize only workspace media directory names, without replacing contents."""
import argparse
import hashlib
import json
import os
from pathlib import Path

GROUPS = (("Pic", {"pic"}), ("Music", {"music"}), ("Book", {"book", "books"}))

def snapshot(directory):
    result = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Refusing symlink: {path}")
        if path.is_file():
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            result[str(path.relative_to(directory))] = {"size": path.stat().st_size, "sha256": digest.hexdigest()}
        elif not path.is_dir():
            raise ValueError(f"Refusing special file: {path}")
    return result


def normalize(root, apply=False):
    plans = []
    children = list(root.iterdir())
    for canonical, aliases in GROUPS:
        matches = [p for p in children if p.name.lower() in aliases]
        if len(matches) > 1:
            raise ValueError(f"Multiple {canonical} directories; manual conflict review required: {matches}")
        source = matches[0] if matches else None
        if source and (source.is_symlink() or not source.is_dir()):
            raise ValueError(f"Not a regular directory: {source}")
        plans.append((canonical, source, snapshot(source) if source else {}))
    report = []
    for canonical, source, before in plans:
        target = root / canonical
        action = "keep" if source and source.name == canonical else "rename" if source else "create"
        if apply:
            if action == "create":
                target.mkdir()
            elif action == "rename":
                # Two-step rename handles Windows/NTFS case-only renames reliably.
                temporary = root / (".c1-media-rename-" + canonical)
                if temporary.exists():
                    raise ValueError(f"Interrupted migration requires review: {temporary}")
                source.rename(temporary)
                try:
                    temporary.rename(target)
                except Exception:
                    temporary.rename(source)
                    raise
            after = snapshot(target)
            if before != after:
                raise ValueError(f"Content verification failed: {target}")
            if canonical not in [p.name for p in root.iterdir()]:
                raise ValueError(f"Exact spelling was not preserved: {target}")
        report.append({"from": source.name if source else None, "to": canonical,
                       "action": action, "files": len(before), "contents": before})
    return report

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report = normalize(args.root.resolve(strict=True), args.apply)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    for item in report:
        print(f"{item['action']}: {item['from']} -> {item['to']} ({item['files']} files)")
    print("Applied and verified" if args.apply else "Read-only inspection; no directories changed")

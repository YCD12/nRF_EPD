"""Check the application BIN, excluding ELF debug sections and RAM-only data."""
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--limit", type=int, default=57344)
    parser.add_argument("--reserve", type=int, default=4096)
    args = parser.parse_args()
    if args.limit <= 0 or not 0 <= args.reserve <= args.limit:
        parser.error("require limit > 0 and 0 <= reserve <= limit")
    try:
        used = args.binary.stat().st_size
    except OSError as exc:
        parser.exit(1, f"Flash budget: cannot read {args.binary}: {exc}\n")
    free = args.limit - used
    print(f"Flash: {used} / {args.limit} bytes; free {free} bytes")
    if used == 0 or free < 0:
        parser.exit(1, "ERROR: application BIN is empty or exceeds the Flash partition\n")
    if free < args.reserve:
        print(f"WARNING: application Flash reserve below {args.reserve} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

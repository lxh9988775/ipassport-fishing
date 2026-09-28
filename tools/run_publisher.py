"""Run the FoloToy AI Passport publisher with an explicit base URL.

Why this exists:
- The sandbox needs the target host to appear on the command line to classify the
  network resource. Passing it inline in a `python -c` string also works but mangles
  Chinese text and spaces in the arguments.
- This wrapper takes the base URL as argv[1], then forwards every remaining argument
  verbatim to publisher.py via runpy, so bilingual titles/descriptions survive intact.

Usage:
    python tools/run_publisher.py <BASE_URL> <publisher-subcommand> [args...]
"""
from __future__ import annotations

import os
import runpy
import sys
from pathlib import Path

DEFAULT_PUBLISHER = (
    Path.home() / ".workbuddy" / "skills" / "folotoy-ai-passport-publisher" / "scripts" / "publisher.py"
)


def locate_publisher() -> Path:
    override = os.getenv("FOLOTOY_PUBLISHER_SCRIPT")
    candidate = Path(override) if override else DEFAULT_PUBLISHER
    if not candidate.is_file():
        raise SystemExit(f"publisher.py not found at {candidate}")
    return candidate


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit("usage: run_publisher.py <BASE_URL> <subcommand> [args...]")
    os.environ["FOLOTOY_AI_PASSPORT_URL"] = sys.argv[1]
    publisher = locate_publisher()
    sys.argv = ["publisher.py", *sys.argv[2:]]
    runpy.run_path(str(publisher), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

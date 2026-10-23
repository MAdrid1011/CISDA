from __future__ import annotations

import json
import sys
from pathlib import Path

from .simulator import run_workload


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: cisda-sim WORKLOAD.json")
    path = Path(sys.argv[1])
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream)
        result = run_workload(document)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise SystemExit(f"cisda-sim: {error}") from error
    json.dump(result, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")

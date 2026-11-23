#!/usr/bin/env python3
"""Export MQTT v2 schemas; --check verifies committed snapshots."""
import argparse
import json
from pathlib import Path

from binbot_server.transport.control_protocol import CONTROL_COMMAND, ControlResult, ControlState

ROOT = Path(__file__).resolve().parents[2] / "contracts/mqtt/v2/schemas"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    schemas = {
        "command": CONTROL_COMMAND.json_schema(),
        "state": ControlState.model_json_schema(),
        "result": ControlResult.model_json_schema(),
    }
    for name, schema in schemas.items():
        data = (
            json.dumps(
                {"$schema": "https://json-schema.org/draft/2020-12/schema", **schema},
                ensure_ascii=False,
                indent=2,
            )
            + "\n"
        )
        path = ROOT / f"{name}.schema.json"
        if args.check:
            if not path.exists() or path.read_text() != data:
                raise SystemExit(
                    f"Schema changed: run scripts/export-control-contracts.py ({name})"
                )
        else:
            ROOT.mkdir(parents=True, exist_ok=True)
            path.write_text(data)


if __name__ == "__main__":
    main()

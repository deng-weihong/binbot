import json
import subprocess
import sys
from pathlib import Path

import pytest
from pydantic import ValidationError

from binbot_server.transport.control_protocol import CONTROL_COMMAND

CONTRACTS = Path(__file__).resolve().parents[2] / "contracts/mqtt/v2"
VECTORS = json.loads((CONTRACTS / "vectors/commands.json").read_text())


@pytest.mark.parametrize("vector", VECTORS, ids=[row["name"] for row in VECTORS])
def test_shared_command_vectors(vector):
    if vector["valid"]:
        assert CONTROL_COMMAND.validate_python(vector["command"]).model_dump() == vector["command"]
    else:
        with pytest.raises(ValidationError):
            CONTROL_COMMAND.validate_python(vector["command"])


def test_exported_schema_matches_runtime_models():
    script = Path(__file__).resolve().parents[1] / "scripts/export-control-contracts.py"
    subprocess.run([sys.executable, str(script), "--check"], check=True)

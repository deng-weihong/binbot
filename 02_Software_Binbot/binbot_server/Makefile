.PHONY: check test test-integration run

check:
	PYTHONPATH=src python3 -m compileall -q src
	PYTHONPATH=src flake8 src tests
	PYTHONPATH=src mypy src
	PYTHONPATH=src python3 -m binbot_server --config config.example.toml --check-config

test:
	PYTHONPATH=src pytest

test-integration:
	PYTHONPATH=src pytest -m integration -o addopts=-q

run:
	PYTHONPATH=src python3 -m binbot_server --config config.example.toml

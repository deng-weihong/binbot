from __future__ import annotations

import argparse
import json

from aiohttp import web

from binbot_server.bootstrap import create_application
from binbot_server.config import load_settings
from binbot_server.infrastructure.logging import configure_logging


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="BinBot Voice Server")
    parser.add_argument("--config", default=None, help="TOML configuration file")
    parser.add_argument(
        "--check-config",
        action="store_true",
        help="validate configuration and exit",
    )
    return parser.parse_args()


def main() -> None:
    args = _arguments()
    settings = load_settings(args.config)
    configure_logging(settings.server.log_level)
    if args.check_config:
        print(json.dumps({"status": "ok", "config": args.config or "config.toml"}))
        return
    application = create_application(settings)
    web.run_app(
        application.http,
        host=settings.server.host,
        port=settings.server.port,
        access_log=None,
        print=None,
    )


if __name__ == "__main__":
    main()

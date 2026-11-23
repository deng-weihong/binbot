"""Start an isolated local Server + MCP pair with private generated credentials."""
import argparse
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.request import urlopen


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-port", type=int, default=18767)
    parser.add_argument("--mcp-port", type=int, default=8770)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--restart", action="store_true")
    actions.add_argument("--stop", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    state = root / ".local/mcp"
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    state.chmod(0o700)
    pid_file = state / "processes.json"
    if (args.restart or args.stop) and pid_file.exists():
        for name, pid in json.loads(pid_file.read_text()).items():
            proc = Path(f"/proc/{pid}/cmdline")
            if not proc.exists():
                continue
            command = proc.read_bytes().split(b"\0")
            module = b"binbot_server" if name == "server" else b"binbot_server.transport.mcp"
            if module not in command or b"-m" not in command:
                raise RuntimeError(f"PID {pid} no longer belongs to the local {name}; refusing to stop it")
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                continue
        for _ in range(100):
            still_running = False
            for pid in json.loads(pid_file.read_text()).values():
                proc = Path(f"/proc/{pid}/stat")
                if proc.exists() and proc.read_text().split(")", 1)[1].split()[0] != "Z":
                    still_running = True
            if not still_running:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("local services did not stop within ten seconds")
        pid_file.unlink()
    if args.stop:
        print("Local Server and MCP stopped")
        return
    for port in (args.server_port, args.mcp_port):
        with socket.socket() as check:
            check.bind(("127.0.0.1", port))
    credentials = state / "credentials.json"
    if credentials.exists():
        tokens = json.loads(credentials.read_text())
    else:
        tokens = {name: secrets.token_urlsafe(32) for name in ("service", "mcp")}
        credentials.write_text(json.dumps(tokens, indent=2) + "\n")
    credentials.chmod(0o600)
    origin = f"http://127.0.0.1:{args.server_port}"
    config = state / "server.toml"
    config.write_text(f'''[server]
host = "127.0.0.1"
port = {args.server_port}
[auth]
allow_anonymous = false
[admin]
allow_unauthenticated_loopback = false
[storage]
kind = "sqlite"
sqlite_path = {json.dumps(str(state / "server.sqlite3"))}
[component]
directory = {json.dumps(str(state / "component-ota"))}
public_origin = "{origin}"
''')
    client_config = state / "mcp-client.json"
    client_config.write_text(json.dumps({"mcpServers": {"binbot": {
        "url": f"http://127.0.0.1:{args.mcp_port}/mcp",
        "headers": {"Authorization": "Bearer " + tokens["mcp"]}}}}, indent=2) + "\n")
    client_config.chmod(0o600)
    env = {**os.environ, "BINBOT_MCP_SERVICE_TOKEN": tokens["service"], "BINBOT_MCP_TOKEN": tokens["mcp"]}
    processes = []
    try:
        commands = [[sys.executable, "-m", "binbot_server", "--config", str(config)],
                    [sys.executable, "-m", "binbot_server.transport.mcp", "--server-url", origin,
                     "--port", str(args.mcp_port)]]
        for name, command in zip(("server", "mcp"), commands):
            with (state / f"{name}.log").open("ab") as log:
                processes.append(subprocess.Popen(command, cwd=root, env=env, stdout=log,
                                                   stderr=log, start_new_session=True))
        for _ in range(100):
            if any(p.poll() is not None for p in processes):
                raise RuntimeError(f"startup failed; inspect logs in {state}")
            try:
                with urlopen(origin + "/healthz", timeout=0.5):
                    pass
                try:
                    urlopen(f"http://127.0.0.1:{args.mcp_port}/mcp", timeout=0.5).close()
                except HTTPError as exc:
                    if exc.code != 401:
                        raise
                break
            except (URLError, TimeoutError):
                time.sleep(0.1)
        else:
            raise RuntimeError("local service startup timed out")
        pid_file.write_text(json.dumps({"server": processes[0].pid, "mcp": processes[1].pid}))
        print(f"Server: {origin}\nMCP: http://127.0.0.1:{args.mcp_port}/mcp\nClient config: {client_config}")
    except BaseException:
        for process in processes:
            if process.poll() is None:
                process.terminate()
            process.wait()
        raise


if __name__ == "__main__":
    main()

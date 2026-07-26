#!/usr/bin/env python3
"""Behavioral regression test for the Windows/WSL-only auth runner branch.

The test uses fake X/VNC/noVNC processes. It deliberately does not create a
filesystem X11 socket, while xdpyinfo succeeds, matching the WSLg behavior
observed on the affected Windows machine. It also injects Wayland variables and
verifies that only the x11vnc child receives the sanitized X11 environment.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import textwrap

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "tools/slicer_linux_auth_helper/run_auth_browser.sh"


def write_executable(path: Path, content: str) -> None:
    path.write_text(textwrap.dedent(content).lstrip(), encoding="utf-8", newline="\n")
    path.chmod(0o755)


def allocate_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def main() -> int:
    if os.name == "nt":
        print("Windows WSL auth runner behavioral test skipped on native Windows")
        return 0
    if not RUNNER.is_file():
        raise SystemExit(f"missing runner: {RUNNER}")

    source = RUNNER.read_text(encoding="utf-8")
    native_xvfb_guard = (
        'for _ in {1..100}; do [[ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ]] '
        '&& break; sleep 0.1; done'
    )
    native_x11vnc = (
        'x11vnc -display "$DISPLAY" -rfbport "$VNC_PORT" -localhost -forever '
        '-shared -noxdamage -noshm -nowf -noscr -rfbauth "$PASS_FILE"'
    )
    for invariant in (
        'case "$(uname -r)" in',
        '*microsoft*|*Microsoft*)',
        'WINDOWS_WSL_RUNTIME=1',
        'xdpyinfo -display "$DISPLAY"',
        '-u WAYLAND_DISPLAY',
        'XDG_SESSION_TYPE=x11',
        native_xvfb_guard,
        native_x11vnc,
    ):
        if invariant not in source:
            raise SystemExit(f"runner is missing invariant: {invariant}")

    with tempfile.TemporaryDirectory(prefix="orcastudio-wsl-auth-runner-") as temp:
        root = Path(temp)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        state = root / "state"
        profile = root / "profile"
        novnc = root / "novnc"
        novnc.mkdir()
        (novnc / "vnc.html").write_text("<!doctype html><title>noVNC</title>", encoding="utf-8")
        evidence = root / "x11vnc-env.json"
        browser_evidence = root / "browser-ran.txt"

        write_executable(
            fake_bin / "uname",
            """
            #!/usr/bin/env bash
            case "${1:-}" in
                -m) printf '%s\\n' x86_64 ;;
                -r) printf '%s\\n' 6.6.87.2-microsoft-standard-WSL2 ;;
                *) exec /usr/bin/uname "$@" ;;
            esac
            """,
        )
        write_executable(
            fake_bin / "Xvfb",
            """
            #!/usr/bin/env bash
            exec sleep 60
            """,
        )
        write_executable(
            fake_bin / "xdpyinfo",
            """
            #!/usr/bin/env bash
            [[ "${1:-}" == "-display" && "${2:-}" == ":193" ]]
            """,
        )
        write_executable(
            fake_bin / "openbox",
            """
            #!/usr/bin/env bash
            exec sleep 60
            """,
        )
        write_executable(
            fake_bin / "dbus-run-session",
            """
            #!/usr/bin/env bash
            [[ "${1:-}" != "--" ]] || shift
            exec "$@"
            """,
        )
        write_executable(
            fake_bin / "x11vnc",
            """
            #!/usr/bin/env python3
            import json
            import os
            from pathlib import Path
            import socket
            import sys

            args = sys.argv[1:]
            if args and args[0] == "-storepasswd":
                Path(args[-1]).write_text("fake-vnc-password", encoding="utf-8")
                raise SystemExit(0)

            evidence = Path(os.environ["AUTH_TEST_X11VNC_EVIDENCE"])
            evidence.write_text(json.dumps({
                "DISPLAY": os.environ.get("DISPLAY"),
                "WAYLAND_DISPLAY": os.environ.get("WAYLAND_DISPLAY"),
                "WAYLAND_SOCKET": os.environ.get("WAYLAND_SOCKET"),
                "DESKTOP_SESSION": os.environ.get("DESKTOP_SESSION"),
                "XDG_CURRENT_DESKTOP": os.environ.get("XDG_CURRENT_DESKTOP"),
                "XDG_SESSION_TYPE": os.environ.get("XDG_SESSION_TYPE"),
            }, sort_keys=True), encoding="utf-8")
            port = int(args[args.index("-rfbport") + 1])
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind(("127.0.0.1", port))
                server.listen(8)
                while True:
                    client, _ = server.accept()
                    client.close()
            """,
        )
        write_executable(
            fake_bin / "websockify",
            """
            #!/usr/bin/env python3
            import socket
            import sys

            args = sys.argv[1:]
            listen = args[args.index("--web") + 2]
            host, port_text = listen.rsplit(":", 1)
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind((host, int(port_text)))
                server.listen(8)
                while True:
                    client, _ = server.accept()
                    client.close()
            """,
        )
        browser = root / "slicer_linux_auth_browser_x86_64"
        write_executable(
            browser,
            """
            #!/usr/bin/env python3
            import os
            from pathlib import Path
            import sys

            args = sys.argv[1:]
            ready = Path(args[args.index("--ready-file") + 1])
            ready.write_text("ready", encoding="utf-8")
            Path(os.environ["AUTH_TEST_BROWSER_EVIDENCE"]).write_text(
                os.environ.get("WAYLAND_DISPLAY", "<unset>"), encoding="utf-8"
            )
            """,
        )

        password_file = root / "password.txt"
        password_file.write_text("0123456789abcdef", encoding="utf-8")
        command_file = state / "browser-command.json"
        event_file = state / "browser-events.ndjson"
        result_file = state / "result.json"
        novnc_port = allocate_loopback_port()
        vnc_port = allocate_loopback_port()
        while vnc_port == novnc_port:
            vnc_port = allocate_loopback_port()

        env = os.environ.copy()
        env.update(
            {
                "PATH": str(fake_bin) + os.pathsep + env.get("PATH", ""),
                "SLICER_LINUX_RUNTIME_COMPONENT_DIR": str(root),
                "SLICER_LINUX_RUNTIME_AUTH_BROWSER": str(browser),
                "SLICER_LINUX_RUNTIME_NOVNC_WEB": str(novnc),
                "SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT": str(novnc_port),
                "SLICER_LINUX_RUNTIME_AUTH_VNC_PORT": str(vnc_port),
                "SLICER_LINUX_RUNTIME_AUTH_DISPLAY": "193",
                "SLICER_LINUX_RUNTIME_AUTH_LISTEN_HOST": "127.0.0.1",
                "AUTH_TEST_X11VNC_EVIDENCE": str(evidence),
                "AUTH_TEST_BROWSER_EVIDENCE": str(browser_evidence),
                "WAYLAND_DISPLAY": "wayland-0",
                "WAYLAND_SOCKET": "wayland-test-socket",
                "DESKTOP_SESSION": "wslg",
                "XDG_CURRENT_DESKTOP": "WSLg",
                "XDG_SESSION_TYPE": "wayland",
            }
        )

        completed = subprocess.run(
            [
                "bash",
                str(RUNNER),
                "browse",
                str(state),
                "data:text/html,auth-test",
                "-",
                str(result_file),
                str(profile),
                str(password_file),
                str(command_file),
                str(event_file),
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=20,
            check=False,
        )
        if completed.returncode != 0:
            raise SystemExit(
                "WSL auth runner behavioral test failed\n"
                f"stdout:\n{completed.stdout}\n"
                f"stderr:\n{completed.stderr}\n"
                f"session.log:\n{(state / 'session.log').read_text(encoding='utf-8', errors='replace') if (state / 'session.log').exists() else '<missing>'}"
            )
        if not evidence.is_file():
            raise SystemExit("fake x11vnc did not start")
        observed = json.loads(evidence.read_text(encoding="utf-8"))
        expected = {
            "DISPLAY": ":193",
            "WAYLAND_DISPLAY": None,
            "WAYLAND_SOCKET": None,
            "DESKTOP_SESSION": None,
            "XDG_CURRENT_DESKTOP": None,
            "XDG_SESSION_TYPE": "x11",
        }
        if observed != expected:
            raise SystemExit(f"x11vnc environment was not sanitized correctly: {observed!r}")
        if browser_evidence.read_text(encoding="utf-8") != "wayland-0":
            raise SystemExit("Wayland variables leaked out of the x11vnc-only sanitization scope")
        if Path("/tmp/.X11-unix/X193").exists():
            raise SystemExit("test unexpectedly created a filesystem X11 socket")

    print("Windows WSL auth runner behavioral test OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

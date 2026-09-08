"""Auto discovery helpers for USB DBI/MTP and the new NRO WiFi metadata server."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path


def _run_powershell(script: str) -> str:
    system_root = os.environ.get("SystemRoot", "C:\\Windows")
    powershell = Path(system_root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    proc = subprocess.run(
        [
            str(powershell),
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-NonInteractive",
            "-Command",
            "[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; " + script,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"PowerShell exited with {proc.returncode}")
    return proc.stdout


def dbi_installed_games() -> list[dict]:
    """Return games and users visible under DBI MTP 7: Saves > Installed games."""

    script = r"""
$shell = New-Object -ComObject Shell.Application
$switch = $shell.NameSpace(17).Items() | Where-Object { $_.Name -eq "Switch" } | Select-Object -First 1
if (-not $switch) { throw "未检测到 Switch，请确认 DBI MTP 已连接" }
$folder = $switch.GetFolder()
foreach ($part in @("7: Saves", "Installed games")) {
    $folder = $folder.Items() | Where-Object { $_.Name -eq $part } | Select-Object -First 1
    if (-not $folder) { throw "DBI 目录中找不到 $part" }
    $folder = $folder.GetFolder()
}
foreach ($game in @($folder.Items())) {
    if (-not $game.IsFolder) { continue }
    $users = @()
    foreach ($child in @($game.GetFolder().Items())) {
        if ($child.IsFolder) { $users += $child.Name }
    }
    Write-Output ($game.Name + "|" + ($users -join ","))
}
"""
    lines = _run_powershell(script).splitlines()
    games = []
    for line in lines:
        if "|" not in line:
            continue
        name, users = line.split("|", 1)
        if name:
            games.append({"name": name, "users": users.split(",") if users else []})
    if not games:
        raise RuntimeError("DBI 存档目录中没有列出 Installed games。")
    return games


def wifi_catalog(host: str, path: str = "/") -> dict:
    """Fetch the catalog JSON exposed by the new NRO WiFi info server."""

    import urllib.request

    url = f"http://{host}:8080{path}"
    try:
        with urllib.request.urlopen(url, timeout=15) as response:
            return json.loads(response.read().decode("utf-8"))
    except Exception as exc:
        raise RuntimeError(f"无法连接新版 NRO 的 WiFi 信息服务器：{exc}") from exc

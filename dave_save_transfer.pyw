#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Dave the Diver save converter between PC SteamSData and Switch JKSV backups."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk


APP_NAME = "DaveDiverSaveTransfer"
STEAM_SDATA = Path("nexon") / "DAVE THE DIVER" / "SteamSData"
SAVE_MARKERS = ("gamesave",)


def app_local_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or (Path.home() / "AppData" / "Local")
    return Path(base) / APP_NAME


def backup_root() -> Path:
    return app_local_dir() / "backups"


def config_path() -> Path:
    return app_local_dir() / "config.json"


def load_config() -> dict:
    try:
        with config_path().open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_config(pc_dir: str, switch_dir: str) -> None:
    config_path().parent.mkdir(parents=True, exist_ok=True)
    payload = {"pc_dir": pc_dir, "switch_dir": switch_dir}
    try:
        with config_path().open("w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
    except OSError:
        pass


def find_pc_save_dir() -> Path | None:
    user_profile = os.environ.get("USERPROFILE") or str(Path.home())
    local_low = Path(user_profile) / "AppData" / "LocalLow" / STEAM_SDATA
    if not local_low.is_dir():
        return None
    candidates = []
    for child in local_low.iterdir():
        if not child.is_dir():
            continue
        if any(_is_pc_save_file(f) for f in child.iterdir() if f.is_file()):
            candidates.append((child.stat().st_mtime, child))
    if not candidates:
        return None
    candidates.sort(key=lambda item: item[0], reverse=True)
    return candidates[0][1]


def _looks_like_save_name(name: str) -> bool:
    lowered = name.lower()
    return any(marker in lowered for marker in SAVE_MARKERS)


def _is_pc_save_file(path: Path) -> bool:
    return path.is_file() and path.suffix.lower() == ".sav" and _looks_like_save_name(path.name)


def _is_switch_save_file(path: Path) -> bool:
    if not path.is_file() or path.name.startswith("."):
        return False
    if not _looks_like_save_name(path.name):
        return False
    return path.suffix.lower() in ("", ".sav")


@dataclass(frozen=True)
class TransferItem:
    source: Path
    target: Path
    action: str


def plan_pc_to_switch(pc_dir: Path, switch_dir: Path) -> list[TransferItem]:
    if not pc_dir.is_dir():
        raise ValueError(f"PC 存档目录不存在：{pc_dir}")
    if not switch_dir.is_dir():
        raise ValueError(f"Switch JKSV 备份目录不存在：{switch_dir}")
    sources = sorted(
        (path for path in pc_dir.iterdir() if _is_pc_save_file(path)),
        key=lambda path: path.name,
    )
    if not sources:
        raise ValueError(f"PC 存档目录里没有可转换的 .sav 文件：{pc_dir}")
    items = []
    for source in sources:
        target = switch_dir / source.stem
        items.append(TransferItem(source, target, "去掉 .sav 后缀"))
    return items


def plan_switch_to_pc(switch_dir: Path, pc_dir: Path) -> list[TransferItem]:
    if not switch_dir.is_dir():
        raise ValueError(f"Switch JKSV 备份目录不存在：{switch_dir}")
    if not pc_dir.is_dir():
        raise ValueError(f"PC 存档目录不存在：{pc_dir}")
    sources = sorted(
        (path for path in switch_dir.iterdir() if _is_switch_save_file(path)),
        key=lambda path: path.name,
    )
    if not sources:
        raise ValueError(
            f"Switch JKSV 备份目录里没有可转换的 GameSave 文件：{switch_dir}\n"
            "请选择 JKSV 导出的具体备份文件夹（形如 JKSV/DAVE THE DIVER/baseline/）。"
        )
    items = []
    for source in sources:
        target_name = source.name if source.suffix.lower() == ".sav" else source.name + ".sav"
        target = pc_dir / target_name
        items.append(TransferItem(source, target, "补上 .sav 后缀"))
    return items


def backup_target(target_dir: Path, backup_under: Path | None = None) -> Path:
    if not target_dir.is_dir():
        raise ValueError(f"目标目录不存在，无法备份：{target_dir}")
    root = backup_under if backup_under is not None else backup_root()
    root.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S") + "_" + str(int(time.time() * 1000) % 1000)
    backup_dir = root / f"{target_dir.name}_{stamp}"
    shutil.copytree(target_dir, backup_dir)
    return backup_dir


def _ps_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _run_powershell(script: str) -> str:
    system_root = os.environ.get("SystemRoot", "C:\\Windows")
    powershell = Path(system_root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    try:
        proc = subprocess.run(
            [
                str(powershell),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-NonInteractive",
                "-Command",
                script,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=180,
            creationflags=creationflags,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("操作 Switch 超时，请检查 DBI/MTP 连接后重试。") from exc
    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip()
        raise RuntimeError(f"DBI/MTP 操作失败：{detail or f'退出码 {proc.returncode}'}")
    return proc.stdout


DBI_NAVIGATE = r"""
$ErrorActionPreference = "Stop"
$shell = New-Object -ComObject Shell.Application
$switch = $shell.NameSpace(17).Items() | Where-Object { $_.Name -eq "Switch" } | Select-Object -First 1
if (-not $switch) {
    throw "未检测到 Switch，请确认 DBI 已开启 MTP Responder 且已连接电脑"
}
$folder = $switch.GetFolder()
foreach ($part in @("7: Saves", "Installed games", "DAVE THE DIVER")) {
    $next = $folder.Items() | Where-Object { $_.Name -eq $part } | Select-Object -First 1
    if (-not $next) {
        throw "DBI 存档目录中找不到 $part"
    }
    $folder = $next.GetFolder()
}
"""


def _dbi_profile_header(profile: str) -> str:
    quoted = _ps_quote(profile)
    return (
        DBI_NAVIGATE
        + "\n"
        + f"""
$profileItem = $folder.Items() | Where-Object {{ $_.Name -eq {quoted} }} | Select-Object -First 1
if (-not $profileItem) {{
    throw "DBI 存档目录中找不到用户目录 {quoted}"
}}
$target = $profileItem.GetFolder()
"""
    )


def dbi_profiles() -> list[str]:
    script = DBI_NAVIGATE + r"""
$found = $false
foreach ($child in @($folder.Items())) {
    if (-not $child.IsFolder) {
        continue
    }
    $hasSave = $false
    foreach ($file in @($child.GetFolder().Items())) {
        if ($file.Name -like "GameSave*") {
            $hasSave = $true
            break
        }
    }
    if ($hasSave) {
        Write-Output $child.Name
        $found = $true
    }
}
if (-not $found) {
    throw "在 DBI 存档目录中没有找到包含 GameSave 文件的用户目录"
}
"""
    output = _run_powershell(script)
    return [line.strip() for line in output.splitlines() if line.strip()]


def dbi_list_files(profile: str) -> list[str]:
    script = _dbi_profile_header(profile) + r"""
foreach ($file in @($target.Items())) {
    if (-not $file.IsFolder) {
        Write-Output $file.Name
    }
}
"""
    output = _run_powershell(script)
    return [line.strip() for line in output.splitlines() if line.strip()]


def dbi_export_to(profile: str, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    dest_quoted = _ps_quote(str(destination))
    script = _dbi_profile_header(profile) + f"""
$dst = $shell.NameSpace({dest_quoted})
foreach ($file in @($target.Items())) {{
    if ($file.IsFolder) {{
        continue
    }}
    $dst.CopyHere($file, 16)
    $outPath = Join-Path {dest_quoted} $file.Name
    $deadline = (Get-Date).AddSeconds(45)
    $ok = $false
    while ((Get-Date) -lt $deadline) {{
        if (Test-Path $outPath) {{
            if ((Get-Item $outPath).Length -gt 0) {{
                $ok = $true
                break
            }}
        }}
        Start-Sleep -Milliseconds 200
    }}
    if (-not $ok) {{
        throw "从 Switch 导出存档失败: $($file.Name)"
    }}
}}
Write-Output "EXPORT_OK"
"""
    _run_powershell(script)


def dbi_upload_from(profile: str, source_dir: Path) -> None:
    source_quoted = _ps_quote(str(source_dir))
    script = _dbi_profile_header(profile) + f"""
$src = $shell.NameSpace({source_quoted})
foreach ($file in @($src.Items())) {{
    if ($file.IsFolder) {{
        continue
    }}
    $target.CopyHere($file, 1556)
    Start-Sleep -Milliseconds 1200
}}
Write-Output "UPLOAD_OK"
"""
    _run_powershell(script)


def dbi_upload_and_verify(profile: str, source_dir: Path) -> None:
    expected = {path.name: path.read_bytes() for path in source_dir.iterdir() if path.is_file()}
    if not expected:
        raise ValueError("没有可写入 Switch 的转换文件。")
    dbi_upload_from(profile, source_dir)
    verify_root = app_local_dir() / "dbi_verify"
    verify_root.mkdir(parents=True, exist_ok=True)
    for attempt in range(4):
        verify_dir = verify_root / f"verify_{time.strftime('%Y%m%d_%H%M%S')}_{int(time.time() * 1000) % 1000}"
        dbi_export_to(profile, verify_dir)
        actual = {
            path.name: path.read_bytes()
            for path in verify_dir.iterdir()
            if path.is_file() and path.name in expected
        }
        if actual == expected:
            return
        time.sleep(1)
    missing = sorted(set(expected) - set(actual))
    raise RuntimeError(f"写入 Switch 后校验失败，缺少或不一致的文件：{', '.join(missing) or '未知'}")


def execute_plan(items: list[TransferItem], target_dir: Path, backup_under: Path | None = None) -> tuple[Path, list[str]]:
    backup_dir = backup_target(target_dir, backup_under)
    log_lines = []
    for item in items:
        if item.source.resolve() == item.target.resolve():
            continue
        item.target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item.source, item.target)
        log_lines.append(f"{item.source.name} -> {item.target.name}：{item.action}")
    return backup_dir, log_lines


def run_self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="dave_save_transfer_test_") as tmp:
        base = Path(tmp)
        pc_dir = base / "pc"
        switch_dir = base / "switch"
        backups = base / "backups"
        pc_dir.mkdir()
        switch_dir.mkdir()

        samples = {
            "GameSave_00_GD.sav": b"pc-game-data-gd-00",
            "GameSave_01_PZ.sav": b"pc-game-data-pz-01",
            "m_GameSave_00_UO.sav": b"pc-game-data-uo-00",
        }
        for name, data in samples.items():
            (pc_dir / name).write_bytes(data)
        (pc_dir / "readme.txt").write_text("not a save", encoding="utf-8")

        pc_to_switch = plan_pc_to_switch(pc_dir, switch_dir)
        assert len(pc_to_switch) == len(samples)
        execute_plan(pc_to_switch, switch_dir, backups)
        assert (switch_dir / "GameSave_00_GD").read_bytes() == samples["GameSave_00_GD.sav"]
        assert not (switch_dir / "GameSave_00_GD.sav").exists()

        execute_plan(plan_pc_to_switch(pc_dir, switch_dir), switch_dir, backups)
        (switch_dir / "notes.json").write_text("jksv metadata", encoding="utf-8")

        pc_roundtrip = base / "pc_roundtrip"
        pc_roundtrip.mkdir()
        switch_to_pc = plan_switch_to_pc(switch_dir, pc_roundtrip)
        assert len(switch_to_pc) == len(samples)
        execute_plan(switch_to_pc, pc_roundtrip, backups)
        for name, data in samples.items():
            assert (pc_roundtrip / name).read_bytes() == data

        print(f"self-test passed; backup folders: {len(list(backups.glob('*')))}")


class SaveTransferApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("潜水员戴夫 PC↔Switch 存档转换")
        self.root.geometry("980x760")
        self.root.minsize(860, 640)

        self.pc_var = tk.StringVar()
        self.switch_var = tk.StringVar()
        self.direction_var = tk.StringVar(value="to_switch")
        self.dbi_var = tk.BooleanVar(value=False)

        self._build_ui()
        self._load_saved_paths()
        self.log("就绪：请选择两侧目录，然后点击“预览”或“开始转换”。")

    def _build_ui(self) -> None:
        style = ttk.Style(self.root)
        try:
            style.theme_use("vista")
        except tk.TclError:
            pass
        self.root.option_add("*Font", ("Microsoft YaHei UI", 10))

        main = ttk.Frame(self.root, padding=14)
        main.pack(fill="both", expand=True)
        main.columnconfigure(1, weight=1)

        ttk.Label(main, text="PC 存档目录", width=18).grid(row=0, column=0, sticky="w", pady=6)
        pc_entry = ttk.Entry(main, textvariable=self.pc_var)
        pc_entry.grid(row=0, column=1, sticky="ew", pady=6)
        ttk.Button(main, text="自动检测", command=self.detect_pc).grid(row=0, column=2, padx=6)
        ttk.Button(main, text="浏览…", command=self.browse_pc).grid(row=0, column=3)

        ttk.Label(main, text="Switch 目录", width=18).grid(row=1, column=0, sticky="w", pady=6)
        self.switch_entry = ttk.Entry(main, textvariable=self.switch_var)
        self.switch_entry.grid(row=1, column=1, sticky="ew", pady=6)
        self.switch_browse_button = ttk.Button(main, text="浏览…", command=self.browse_switch)
        self.switch_browse_button.grid(row=1, column=2, columnspan=2, padx=6)

        direction = ttk.Frame(main)
        direction.grid(row=2, column=0, columnspan=4, sticky="w", pady=10)
        ttk.Radiobutton(
            direction,
            text="PC → Switch（去掉 .sav）",
            variable=self.direction_var,
            value="to_switch",
            command=self.clear_preview,
        ).pack(side="left", padx=(0, 18))
        ttk.Radiobutton(
            direction,
            text="Switch → PC（补上 .sav）",
            variable=self.direction_var,
            value="to_pc",
            command=self.clear_preview,
        ).pack(side="left")
        ttk.Checkbutton(
            direction,
            text="直连 DBI/MTP Switch",
            variable=self.dbi_var,
            command=self._on_dbi_toggle,
        ).pack(side="right")

        actions = ttk.Frame(main)
        actions.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(4, 10))
        self.summary_var = tk.StringVar(value="尚未预览")
        ttk.Label(actions, textvariable=self.summary_var).pack(side="left", fill="x", expand=True)
        ttk.Button(actions, text="预览", command=self.preview).pack(side="right", padx=(8, 0))
        ttk.Button(actions, text="开始转换", command=self.convert).pack(side="right")

        columns = ("source", "target", "action")
        self.tree = ttk.Treeview(main, columns=columns, show="headings", selectmode="none")
        self.tree.heading("source", text="来源文件")
        self.tree.heading("target", text="目标文件")
        self.tree.heading("action", text="操作")
        self.tree.column("source", width=260, anchor="w")
        self.tree.column("target", width=260, anchor="w")
        self.tree.column("action", width=140, anchor="w")
        tree_frame = ttk.Frame(main)
        tree_frame.grid(row=4, column=0, columnspan=4, sticky="nsew")
        tree_frame.columnconfigure(0, weight=1)
        tree_frame.rowconfigure(0, weight=1)
        scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.grid(row=0, column=0, sticky="nsew")
        scroll.grid(row=0, column=1, sticky="ns")
        main.rowconfigure(4, weight=1)

        log_frame = ttk.LabelFrame(main, text="运行日志", padding=6)
        log_frame.grid(row=5, column=0, columnspan=4, sticky="ew", pady=(10, 0))
        log_frame.columnconfigure(0, weight=1)
        self.log_text = tk.Text(log_frame, height=8, wrap="word", state="disabled")
        log_scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=log_scroll.set)
        self.log_text.grid(row=0, column=0, sticky="ew")
        log_scroll.grid(row=0, column=1, sticky="ns")

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def _load_saved_paths(self) -> None:
        config = load_config()
        pc_dir = config.get("pc_dir") or ""
        switch_dir = config.get("switch_dir") or ""
        if not pc_dir:
            detected = find_pc_save_dir()
            if detected:
                pc_dir = str(detected)
                self.log(f"已自动找到 PC 存档目录：{detected}")
        if not pc_dir and not switch_dir:
            self.log("未找到现成 PC 存档，可手动点击“浏览…”选择。")
        self.pc_var.set(pc_dir)
        self.switch_var.set(switch_dir)
        self._on_dbi_toggle()

    def detect_pc(self) -> None:
        detected = find_pc_save_dir()
        if not detected:
            messagebox.showerror("未找到", "未在 AppData\\LocalLow\\nexon\\DAVE THE DIVER\\SteamSData 下找到 .sav 存档。")
            return
        self.pc_var.set(str(detected))
        self.log(f"PC 存档目录：{detected}")
        self.clear_preview()

    def browse_pc(self) -> None:
        initial = self.pc_var.get() or str(Path.home())
        chosen = filedialog.askdirectory(initialdir=initial, title="选择 PC 存档目录（含 .sav 的数字子目录）")
        if chosen:
            self.pc_var.set(chosen)
            self.log(f"PC 存档目录：{chosen}")
            self.clear_preview()

    def browse_switch(self) -> None:
        initial = self.switch_var.get() or str(Path.home())
        chosen = filedialog.askdirectory(initialdir=initial, title="选择 JKSV 导出的 Switch 备份文件夹")
        if chosen:
            self.switch_var.set(chosen)
            self.log(f"Switch 备份目录：{chosen}")
            self.clear_preview()

    def _on_dbi_toggle(self) -> None:
        if self.dbi_var.get():
            self.switch_entry.configure(state="disabled")
            self.switch_browse_button.configure(state="disabled")
            self.log("已启用 DBI/MTP 直连模式：转换时会自动检测 Switch 存档。")
        else:
            self.switch_entry.configure(state="normal")
            self.switch_browse_button.configure(state="normal")
        self.clear_preview()

    def log(self, message: str) -> None:
        self.log_text.configure(state="normal")
        self.log_text.insert("end", f"[{time.strftime('%H:%M:%S')}] {message}\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def clear_preview(self) -> None:
        for item in self.tree.get_children():
            self.tree.delete(item)
        self.summary_var.set("尚未预览")

    def _current_plan(self) -> tuple[list[TransferItem], Path]:
        pc_dir = Path(self.pc_var.get().strip())
        switch_dir = Path(self.switch_var.get().strip())
        if self.direction_var.get() == "to_switch":
            items = plan_pc_to_switch(pc_dir, switch_dir)
            target = switch_dir
        else:
            items = plan_switch_to_pc(switch_dir, pc_dir)
            target = pc_dir
        return items, target

    def preview(self) -> None:
        self.clear_preview()
        if self.dbi_var.get():
            self._dbi_preview()
            return
        try:
            items, _ = self._current_plan()
        except Exception as exc:
            self.log(f"预览失败：{exc}")
            messagebox.showerror("预览失败", str(exc))
            return
        for item in items:
            self.tree.insert(
                "",
                "end",
                values=(item.source.name, item.target.name, item.action),
            )
        self.summary_var.set(f"共 {len(items)} 个存档文件")
        self.log(f"预览完成：将处理 {len(items)} 个存档文件。")

    def convert(self) -> None:
        if self.dbi_var.get():
            self._convert_dbi()
            return
        try:
            items, target = self._current_plan()
        except Exception as exc:
            self.log(f"转换失败：{exc}")
            messagebox.showerror("转换失败", str(exc))
            return

        direction_text = "PC → Switch" if self.direction_var.get() == "to_switch" else "Switch → PC"
        if not messagebox.askyesno(
            "确认转换",
            f"方向：{direction_text}\n"
            f"目标目录：{target}\n"
            f"将处理 {len(items)} 个存档文件。\n\n"
            "转换前会自动备份目标目录，是否继续？",
        ):
            return

        try:
            backup_dir, log_lines = execute_plan(items, target)
            self.pc_var.set(str(Path(self.pc_var.get().strip())))
            self.switch_var.set(str(Path(self.switch_var.get().strip())))
            for line in log_lines:
                self.log(line)
            self.log(f"转换完成：{len(items)} 个文件已写入 {target}")
            self.log(f"转换前备份：{backup_dir}")
            self.summary_var.set(f"完成：{len(items)} 个文件")
            messagebox.showinfo("完成", f"转换成功。\n\n备份位于：\n{backup_dir}")
        except Exception as exc:
            self.log(f"转换失败：{exc}")
            messagebox.showerror("转换失败", str(exc))

    def _pc_save_dir(self) -> Path:
        pc_text = self.pc_var.get().strip()
        if not pc_text:
            raise ValueError("PC 存档目录为空，请先点击“自动检测”或“浏览…”。")
        pc_dir = Path(pc_text)
        if not pc_dir.is_dir():
            raise ValueError(f"PC 存档目录不存在：{pc_dir}")
        return pc_dir

    def _resolve_dbi_profile(self) -> str:
        profiles = dbi_profiles()
        if not profiles:
            raise ValueError("DBI 存档目录中没有找到《潜水员戴夫》的 GameSave 存档。")
        if len(profiles) > 1:
            self.log(f"检测到多个用户存档目录，将使用第一个：{profiles[0]}")
        profile = profiles[0]
        self.log(f"已连接 Switch DBI 存档：DAVE THE DIVER / {profile}")
        return profile

    def _dbi_preview(self) -> None:
        try:
            profile = self._resolve_dbi_profile()
            pc_dir = self._pc_save_dir()
            if self.direction_var.get() == "to_switch":
                sources = sorted(
                    (path for path in pc_dir.iterdir() if _is_pc_save_file(path)),
                    key=lambda path: path.name,
                )
                if not sources:
                    raise ValueError(f"PC 存档目录里没有可转换的 .sav 文件：{pc_dir}")
                for source in sources:
                    self.tree.insert(
                        "",
                        "end",
                        values=(source.name, source.stem, "写入 Switch DBI"),
                    )
                self.summary_var.set(f"DBI {profile}：将写入 {len(sources)} 个文件")
            else:
                files = dbi_list_files(profile)
                if not files:
                    raise ValueError("Switch DBI 存档目录中没有文件。")
                for name in files:
                    target = name if name.lower().endswith(".sav") else name + ".sav"
                    self.tree.insert(
                        "",
                        "end",
                        values=(name, target, "写入 PC"),
                    )
                self.summary_var.set(f"DBI {profile}：将读取 {len(files)} 个文件")
            self.log(f"DBI/MTP 预览完成：{profile}")
        except Exception as exc:
            self.log(f"DBI 预览失败：{exc}")
            messagebox.showerror("DBI 预览失败", str(exc))

    def _convert_dbi(self) -> None:
        try:
            profile = self._resolve_dbi_profile()
            pc_dir = self._pc_save_dir()
        except Exception as exc:
            self.log(f"DBI 转换失败：{exc}")
            messagebox.showerror("DBI 转换失败", str(exc))
            return

        direction_text = "PC → Switch" if self.direction_var.get() == "to_switch" else "Switch → PC"
        if not messagebox.askyesno(
            "确认 DBI 转换",
            f"方向：{direction_text}\n"
            f"Switch 用户目录：DAVE THE DIVER / {profile}\n\n"
            "PC → Switch 会先自动备份当前 Switch 存档，再写入并校验。\n"
            "Switch → PC 会先导出 Switch 存档，再覆盖 PC 存档并自动备份 PC 原存档。\n\n"
            "是否继续？",
        ):
            return

        stamp = time.strftime("%Y%m%d_%H%M%S") + "_" + str(int(time.time() * 1000) % 1000)
        work_root = app_local_dir() / "dbi_work"
        work_root.mkdir(parents=True, exist_ok=True)
        try:
            if self.direction_var.get() == "to_switch":
                staging = work_root / f"pc_to_switch_{stamp}"
                staging.mkdir(parents=True, exist_ok=True)
                items = plan_pc_to_switch(pc_dir, staging)
                for item in items:
                    item.target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(item.source, item.target)
                self.log(f"已生成 Switch 格式文件：{staging}")

                backup_dir = backup_root() / f"DBI_Switch_{stamp}"
                self.log("正在导出当前 Switch 存档作为备份…")
                dbi_export_to(profile, backup_dir)
                self.log(f"Switch 原存档备份完成：{backup_dir}")

                self.log(f"正在写入并校验 {len(items)} 个文件到 Switch…")
                dbi_upload_and_verify(profile, staging)
                self.summary_var.set(f"DBI 完成：{len(items)} 个文件")
                self.log("DBI/MTP 写入校验通过。")
                messagebox.showinfo(
                    "DBI 转换完成",
                    f"PC 存档已写入 Switch 并校验通过。\n\n备份位于：\n{backup_dir}",
                )
            else:
                source_dir = work_root / f"switch_to_pc_{stamp}"
                self.log("正在从 Switch 导出存档…")
                dbi_export_to(profile, source_dir)
                items = plan_switch_to_pc(source_dir, pc_dir)
                self.log(f"正在转换并写入 PC：{pc_dir}")
                backup_dir, log_lines = execute_plan(items, pc_dir)
                for line in log_lines:
                    self.log(line)
                self.summary_var.set(f"DBI 完成：{len(items)} 个文件")
                self.log(f"PC 原存档备份：{backup_dir}")
                messagebox.showinfo(
                    "DBI 转换完成",
                    f"Switch 存档已写入 PC 并校验完成。\n\nPC 原存档备份位于：\n{backup_dir}",
                )
        except Exception as exc:
            self.log(f"DBI 转换失败：{exc}")
            messagebox.showerror("DBI 转换失败", str(exc))

    def on_close(self) -> None:
        save_config(self.pc_var.get().strip(), self.switch_var.get().strip())
        self.root.destroy()


def main() -> None:
    if "--self-test" in sys.argv:
        try:
            run_self_test()
            raise SystemExit(0)
        except Exception:
            import traceback

            traceback.print_exc()
            raise SystemExit(1)

    if sys.platform.startswith("win"):
        try:
            import ctypes

            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass

    root = tk.Tk()
    SaveTransferApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()

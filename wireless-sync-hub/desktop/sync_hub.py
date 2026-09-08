#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Switch Save Sync Hub: modern PC side for NX-Save-Sync NRO."""

from __future__ import annotations

import json
import os
import shutil
import socket
import tempfile
import threading
import time
import uuid
import zipfile
from pathlib import Path
from urllib.parse import quote

from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from discovery import dbi_installed_games, wifi_catalog


APP_NAME = "SwitchSaveSyncHub"
PORT = 8080


def app_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or (Path.home() / "AppData" / "Local")
    return Path(base) / APP_NAME


def config_path() -> Path:
    return app_dir() / "config.json"


def backup_root() -> Path:
    return app_dir() / "backups"


def load_config() -> dict:
    path = config_path()
    if not path.exists():
        return {"switch_ip": "", "games": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        data.setdefault("switch_ip", "")
        data.setdefault("games", [])
        return data
    except (OSError, json.JSONDecodeError):
        return {"switch_ip": "", "games": []}


def save_config(config: dict) -> None:
    config_path().parent.mkdir(parents=True, exist_ok=True)
    config_path().write_text(
        json.dumps(config, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )


def lan_ip() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return "0.0.0.0"


def backup_directory(source: Path) -> Path:
    stamp = time.strftime("%Y%m%d_%H%M%S")
    dest = backup_root() / f"{source.name}_{stamp}"
    shutil.copytree(source, dest)
    return dest


def transform_switch_to_pc(source: Path, pc_dir: Path, suffix_sav: bool) -> int:
    pc_dir.mkdir(parents=True, exist_ok=True)
    copied = 0
    for item in source.rglob("*"):
        if not item.is_file():
            continue
        rel = item.relative_to(source)
        parts = [part for part in rel.parts if part != "Title_Name"]
        if not parts:
            continue
        name = parts[-1]
        if suffix_sav and not name.lower().endswith(".sav"):
            name += ".sav"
        target_rel = Path(*parts[:-1], name) if len(parts) > 1 else Path(name)
        target = pc_dir / target_rel
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, target)
        copied += 1
    return copied


def transform_pc_to_switch(pc_dir: Path, title_dir: Path, suffix_sav: bool) -> int:
    title_dir.mkdir(parents=True, exist_ok=True)
    copied = 0
    for item in pc_dir.rglob("*"):
        if not item.is_file():
            continue
        rel = item.relative_to(pc_dir)
        name = rel.name
        if suffix_sav:
            if not name.lower().endswith(".sav"):
                continue
            name = name[:-4]
        target = title_dir / rel.parent / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, target)
        copied += 1
    return copied


def clear_pc_sav_files(pc_dir: Path) -> int:
    removed = 0
    if not pc_dir.exists():
        return 0
    for item in pc_dir.rglob("*.sav"):
        if item.is_file():
            item.unlink()
            removed += 1
    return removed


def http_download_from_switch(host: str, dest_zip: Path, log) -> bool:
    try:
        sock = socket.create_connection((host, PORT), timeout=15)
    except OSError as exc:
        log(f"无法连接 Switch：{exc}")
        return False
    sock.settimeout(60)
    try:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: " + host.encode() + b"\r\nConnection: close\r\n\r\n")
        header = b""
        body = b""
        content_length = 0
        header_done = False
        while True:
            chunk = sock.recv(64 * 1024)
            if not chunk:
                break
            if not header_done:
                header += chunk
                marker = header.find(b"\r\n\r\n")
                if marker >= 0:
                    header_text = header[:marker].decode("utf-8", errors="ignore")
                    for line in header_text.splitlines():
                        if line.lower().startswith("content-length:"):
                            content_length = int(line.split(":", 1)[1].strip())
                    body = header[marker + 4:]
                    header_done = True
            else:
                body += chunk
    except (OSError, TimeoutError) as exc:
        log(f"下载失败：{exc}")
        sock.close()
        return False
    sock.close()

    if not header_done:
        log("Switch 没有返回有效 HTTP 响应。")
        return False
    if content_length <= 0:
        log("Switch 返回的存档包为空。")
        return False

    dest_zip.write_bytes(b"")
    received = 0
    try:
        with dest_zip.open("wb") as handle:
            handle.write(body)
            received = len(body)
            sock2 = socket.create_connection((host, PORT), timeout=10)
            sock2.sendall(b"SHUTDOWN")
            sock2.close()
            log("已通知 Switch 停止存档服务器。")
    except OSError as exc:
        log(f"写入或关闭 Switch 服务器失败：{exc}")
        return False

    if received < content_length:
        log(f"下载不完整：{received}/{content_length} 字节。")
        return False
    log(f"存档包下载完成：{received} 字节")
    return True


class ZipUploadServer(threading.Thread):
    def __init__(self, zip_path: Path, log) -> None:
        super().__init__(daemon=True)
        self.zip_path = zip_path
        self.log = log
        self._stop = threading.Event()
        self._server: socket.socket | None = None

    def stop(self) -> None:
        self._stop.set()

    def _send_file(self, client: socket.socket) -> None:
        if not self.zip_path.exists():
            client.sendall(b"HTTP/1.1 404 Not Found\r\n\r\n")
            return
        size = self.zip_path.stat().st_size
        client.sendall(
            (
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/zip\r\n"
                f"Content-Length: {size}\r\n"
                "Connection: close\r\n\r\n"
            ).encode()
        )
        with self.zip_path.open("rb") as handle:
            while True:
                chunk = handle.read(64 * 1024)
                if not chunk:
                    break
                client.sendall(chunk)
        self.log(f"已向 Switch 发送存档包：{size} 字节")

    def run(self) -> None:
        try:
            self._server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self._server.bind(("0.0.0.0", PORT))
            self._server.listen(5)
            self._server.settimeout(1)
            self.log(f"PC 服务器已启动，等待 Switch 连接端口 {PORT}。")
        except OSError as exc:
            self.log(f"启动 PC 服务器失败：{exc}")
            self._stop.set()
            return
        while not self._stop.is_set():
            try:
                client, _ = self._server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with client:
                try:
                    request = client.recv(4096)
                    if b"SHUTDOWN" in request:
                        client.sendall(b"HTTP/1.1 200 OK\r\n\r\n")
                        self._stop.set()
                        self.log("Switch 已完成下载并通知关闭服务器。")
                    elif request:
                        self._send_file(client)
                except OSError as exc:
                    self.log(f"传输过程中出错：{exc}")
        if self._server:
            self._server.close()


def stage_pc_zip(game: dict, staging_root: Path, log) -> Path:
    pc_dir = Path(game["pc_dir"])
    if not pc_dir.is_dir():
        raise RuntimeError(f"PC 存档目录不存在：{pc_dir}")
    title_dir = staging_root / "temp" / game["title_id"].upper()
    suffix_sav = game.get("mode") == "sav"
    count = transform_pc_to_switch(pc_dir, title_dir, suffix_sav)
    if count == 0:
        raise RuntimeError(f"没有从 PC 存档目录生成可发送的文件：{pc_dir}")
    log(f"已生成 {count} 个 Switch 格式文件：{title_dir}")
    zip_path = staging_root / "temp.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for file_path in (staging_root / "temp").rglob("*"):
            if file_path.is_file():
                archive.write(file_path, file_path.relative_to(staging_root).as_posix())
    return zip_path


class SyncWorker(QThread):
    log = Signal(str)
    succeeded = Signal(str)
    failed = Signal(str)

    def __init__(self, config: dict, game: dict, direction: str) -> None:
        super().__init__()
        self.config = config
        self.game = game
        self.direction = direction

    def run(self) -> None:
        try:
            if not self.game.get("title_id", "").strip():
                raise RuntimeError("Title ID 为空，请先通过 WiFi 自动扫描补全。")
            if self.direction == "from_switch":
                self._run_from_switch()
            else:
                self._run_to_switch()
        except Exception as exc:
            self.failed.emit(str(exc))

    def _run_from_switch(self) -> None:
        host = self.config.get("switch_ip", "").strip()
        if not host:
            raise RuntimeError("请先在顶部填写 Switch 的 IP 地址。")
        pc_dir = Path(self.game["pc_dir"])
        if not pc_dir.is_dir():
            raise RuntimeError(f"PC 存档目录不存在：{pc_dir}")

        self.log.emit("正在从 Switch 下载存档，请在 Switch 新版 Hub NRO 中选择游戏并点 Export selected to PC。")
        with tempfile.TemporaryDirectory(prefix="sync_from_switch_") as tmp:
            root = Path(tmp)
            dest_zip = root / "temp.zip"
            if not http_download_from_switch(host, dest_zip, self.log.emit):
                raise RuntimeError("下载失败，请确认 Switch 上的存档服务器正在运行。")
            with zipfile.ZipFile(dest_zip) as archive:
                archive.extractall(root)
            source = root / "temp" / self.game["title_id"].upper()
            if not source.is_dir():
                raise RuntimeError(f"下载包中没有找到 Title ID {self.game['title_id']} 的存档目录。")

            backup = backup_directory(pc_dir)
            self.log.emit(f"PC 原存档已备份：{backup}")
            removed = 0
            if self.game.get("mode") == "sav":
                removed = clear_pc_sav_files(pc_dir)
                self.log.emit(f"已清理 {removed} 个旧 .sav 文件。")
            copied = transform_switch_to_pc(source, pc_dir, self.game.get("mode") == "sav")
            if copied == 0:
                raise RuntimeError("没有从 Switch 存档写入任何 PC 文件。")
            self._record_success(f"Switch → PC：{copied} 个文件")

    def _run_to_switch(self) -> None:
        host = self.config.get("switch_ip", "").strip()
        if not host:
            raise RuntimeError("请先在顶部填写 Switch 的 IP 地址。")
        pc_ip = self.config.get("pc_ip") or lan_ip()
        self.log.emit(f"请在 Switch 新版 Hub NRO 中进入 Receive from PC，并确认 PC IP 为 {pc_ip}。")
        with tempfile.TemporaryDirectory(prefix="sync_to_switch_") as tmp:
            root = Path(tmp)
            zip_path = stage_pc_zip(self.game, root, self.log.emit)
            server = ZipUploadServer(zip_path, self.log.emit)
            server.start()
            deadline = time.time() + 600
            while time.time() < deadline and server.is_alive() and not server._stop.is_set():
                time.sleep(0.2)
            server.stop()
            server.join(timeout=2)
            if not server._stop.is_set():
                raise RuntimeError("等待 Switch 连接超时，请在 Switch 上选择 Receive 后重试。")
            self._record_success("PC → Switch 已发送")

    def _record_success(self, label: str) -> None:
        now = time.strftime("%Y-%m-%d %H:%M:%S")
        self.game["last_sync"] = now
        history = self.game.setdefault("history", [])
        history.insert(0, {"time": now, "label": label})
        del history[20:]
        save_config(self.config)
        self.succeeded.emit(label)


class ProfileDialog(QDialog):
    def __init__(self, parent=None, game: dict | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("添加/编辑游戏")
        self.setMinimumWidth(520)
        self._game = dict(game) if game else {
            "id": uuid.uuid4().hex,
            "name": "",
            "title_id": "",
            "pc_dir": "",
            "mode": "sav",
            "history": [],
            "last_sync": "",
        }

        layout = QVBoxLayout(self)
        form = QFormLayout()
        self.name_edit = QLineEdit(self._game.get("name", ""))
        self.tid_edit = QLineEdit(self._game.get("title_id", ""))
        self.tid_edit.setPlaceholderText("例如 010097F018538000")
        self.tid_edit.setToolTip("16 位十六进制 Title ID，NX-Save-Sync 的存档文件夹名。")
        self.path_edit = QLineEdit(self._game.get("pc_dir", ""))
        browse_button = QPushButton("浏览")
        browse_button.clicked.connect(self._browse_pc)
        path_row = QHBoxLayout()
        path_row.addWidget(self.path_edit, 1)
        path_row.addWidget(browse_button)
        self.mode_combo = QComboBox()
        self.mode_combo.addItem("PC 需要 .sav 后缀", "sav")
        self.mode_combo.addItem("直接复制（无后缀转换）", "direct")
        index = self.mode_combo.findData(self._game.get("mode", "sav"))
        self.mode_combo.setCurrentIndex(max(0, index))

        form.addRow("游戏名称", self.name_edit)
        form.addRow("Switch Title ID", self.tid_edit)
        form.addRow("PC 存档目录", path_row)
        form.addRow("转换规则", self.mode_combo)
        layout.addLayout(form)

        hint = QLabel(
            "提示：第一次建议先让 Switch 端 Send 一次，确认下载包里的 Title ID；"
            "之后该配置会被记住。"
        )
        hint.setWordWrap(True)
        layout.addWidget(hint)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _browse_pc(self) -> None:
        chosen = QFileDialog.getExistingDirectory(self, "选择 PC 存档目录")
        if chosen:
            self.path_edit.setText(chosen)

    def _accept(self) -> None:
        name = self.name_edit.text().strip()
        tid = self.tid_edit.text().strip().upper()
        pc_dir = self.path_edit.text().strip()
        if not name or not pc_dir:
            QMessageBox.warning(self, "缺少信息", "游戏名称和 PC 目录需要填写。")
            return
        self._game.update(
            {
                "name": name,
                "title_id": tid,
                "pc_dir": pc_dir,
                "mode": self.mode_combo.currentData(),
            }
        )
        self.accept()

    def game(self) -> dict:
        return self._game


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Switch Save Sync Hub")
        self.resize(1050, 620)
        self.config = load_config()
        self.worker: SyncWorker | None = None
        self._build_ui()
        self._refresh_table()

    def _build_ui(self) -> None:
        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)

        top = QHBoxLayout()
        top.addWidget(QLabel("Switch IP"))
        self.switch_ip_edit = QLineEdit(self.config.get("switch_ip", ""))
        self.switch_ip_edit.setPlaceholderText("192.168.1.100")
        self.switch_ip_edit.setFixedWidth(150)
        top.addWidget(self.switch_ip_edit)
        top.addWidget(QLabel("PC IP"))
        self.pc_ip_edit = QLineEdit(self.config.get("pc_ip") or lan_ip())
        self.pc_ip_edit.setFixedWidth(150)
        top.addWidget(self.pc_ip_edit)
        save_ip = QPushButton("保存 IP")
        save_ip.clicked.connect(self._save_ip)
        top.addWidget(save_ip)
        top.addStretch(1)
        layout.addLayout(top)

        splitter = QSplitter(Qt.Horizontal)
        layout.addWidget(splitter, 1)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["游戏", "Title ID", "PC 目录", "上次同步"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch)
        self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
        left_layout.addWidget(self.table)

        button_row = QHBoxLayout()
        add_button = QPushButton("添加游戏")
        add_button.clicked.connect(self._add_game)
        edit_button = QPushButton("编辑")
        edit_button.clicked.connect(self._edit_game)
        remove_button = QPushButton("删除")
        remove_button.clicked.connect(self._remove_game)
        button_row.addWidget(edit_button)
        button_row.addWidget(remove_button)
        button_row.addStretch(1)
        left_layout.addLayout(button_row)

        sync_row = QHBoxLayout()
        usb_button = QPushButton("USB 自动扫描 (DBI)")
        usb_button.clicked.connect(self._usb_scan)
        wifi_button = QPushButton("WiFi 自动扫描 (NRO)")
        wifi_button.clicked.connect(self._wifi_scan)
        self.from_switch_button = QPushButton("同步：Switch → PC")
        self.from_switch_button.clicked.connect(lambda: self._sync("from_switch"))
        self.to_switch_button = QPushButton("同步：PC → Switch")
        self.to_switch_button.clicked.connect(lambda: self._sync("to_switch"))
        for button in (usb_button, wifi_button, self.from_switch_button, self.to_switch_button):
            sync_row.addWidget(button)
        left_layout.addLayout(sync_row)
        splitter.addWidget(left)

        right = QGroupBox("运行日志")
        right_layout = QVBoxLayout(right)
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        right_layout.addWidget(self.log_view)
        splitter.addWidget(right)
        splitter.setSizes([680, 360])

        self.append_log("就绪：请先添加游戏并填写 Switch IP。")

    def append_log(self, message: str) -> None:
        self.log_view.appendPlainText(f"[{time.strftime('%H:%M:%S')}] {message}")

    def _save_ip(self) -> None:
        self.config["switch_ip"] = self.switch_ip_edit.text().strip()
        self.config["pc_ip"] = self.pc_ip_edit.text().strip()
        save_config(self.config)
        self.append_log("IP 配置已保存。")

    def _refresh_table(self) -> None:
        self.table.setRowCount(len(self.config["games"]))
        for row, game in enumerate(self.config["games"]):
            values = [
                game.get("name", ""),
                game.get("title_id", ""),
                game.get("pc_dir", ""),
                game.get("last_sync", ""),
            ]
            for col, value in enumerate(values):
                item = QTableWidgetItem(value)
                item.setToolTip(value)
                self.table.setItem(row, col, item)
        self.table.resizeRowsToContents()

    def _selected_game(self) -> dict | None:
        row = self.table.currentRow()
        if row < 0 or row >= len(self.config["games"]):
            QMessageBox.information(self, "未选择", "请先在列表中选择一个游戏。")
            return None
        return self.config["games"][row]

    def _add_game(self) -> None:
        dialog = ProfileDialog(self)
        if dialog.exec() == QDialog.Accepted:
            self.config["games"].append(dialog.game())
            save_config(self.config)
            self._refresh_table()
            self.table.selectRow(len(self.config["games"]) - 1)

    def _edit_game(self) -> None:
        game = self._selected_game()
        if not game:
            return
        dialog = ProfileDialog(self, game)
        if dialog.exec() == QDialog.Accepted:
            edited = dialog.game()
            index = self.config["games"].index(game)
            self.config["games"][index] = edited
            save_config(self.config)
            self._refresh_table()
            self.table.selectRow(index)

    def _remove_game(self) -> None:
        game = self._selected_game()
        if not game:
            return
        answer = QMessageBox.question(self, "确认删除", f"删除 {game['name']} 的同步配置？")
        if answer == QMessageBox.Yes:
            self.config["games"].remove(game)
            save_config(self.config)
            self._refresh_table()

    def _find_game(self, name: str, title_id: str = "") -> dict | None:
        for game in self.config["games"]:
            if title_id and game.get("title_id", "").upper() == title_id.upper():
                return game
            if name and game.get("name", "") == name:
                return game
        return None

    def _select_row_for_game(self, game: dict) -> None:
        if game in self.config["games"]:
            self.table.selectRow(self.config["games"].index(game))

    def _usb_scan(self) -> None:
        try:
            games = dbi_installed_games()
        except Exception as exc:
            QMessageBox.critical(self, "USB 扫描失败", str(exc))
            self.append_log(f"USB 扫描失败：{exc}")
            return
        if not games:
            QMessageBox.information(self, "USB 扫描", "没有发现已安装游戏。")
            return

        labels = [game["name"] for game in games]
        choice, ok = QInputDialog.getItem(
            self,
            "USB 自动扫描",
            "选择要从 Switch 添加的游戏：",
            labels,
            0,
            False,
        )
        if not ok:
            return
        index = labels.index(choice)
        found = games[index]
        existing = self._find_game(found["name"])
        if existing:
            self._select_row_for_game(existing)
            self.append_log(f"游戏已存在：{existing['name']}，已选中。")
            return

        new_game = {
            "id": uuid.uuid4().hex,
            "name": found["name"],
            "title_id": "",
            "pc_dir": "",
            "mode": "sav",
            "switch_user": found["users"][0] if found["users"] else "",
            "history": [],
            "last_sync": "",
        }
        dialog = ProfileDialog(self, new_game)
        if dialog.exec() == QDialog.Accepted:
            self.config["games"].append(dialog.game())
            save_config(self.config)
            self._refresh_table()
            self.table.selectRow(len(self.config["games"]) - 1)
            self.append_log("已从 USB 添加游戏；Title ID 可稍后用 WiFi 自动扫描补全。")

    def _wifi_scan(self) -> None:
        host = self.switch_ip_edit.text().strip()
        if not host:
            QMessageBox.warning(self, "缺少 Switch IP", "请先填写 Switch 的 IP 地址。")
            return
        try:
            pc_ip = self.pc_ip_edit.text().strip() or lan_ip()
            catalog = wifi_catalog(host, f"/?pc={quote(pc_ip)}")
        except Exception as exc:
            QMessageBox.critical(self, "WiFi 扫描失败", str(exc))
            self.append_log(f"WiFi 扫描失败：{exc}")
            return

        titles = catalog.get("titles", [])
        if not titles:
            QMessageBox.information(self, "WiFi 扫描", "NRO 没有返回可同步的游戏存档。")
            return
        self.append_log(
            f"WiFi 扫描成功：系统 {catalog.get('system', {}).get('firmware', '')}，"
            f"发现 {len(titles)} 个游戏。"
        )

        for title in titles:
            existing = self._find_game("", title.get("title_id", ""))
            if existing and not existing.get("title_id"):
                existing["title_id"] = title["title_id"].upper()
            elif existing and not existing.get("name"):
                existing["name"] = title.get("name", "")
        save_config(self.config)
        self._refresh_table()

        labels = [f"{title.get('name', '')} | {title.get('title_id', '')}" for title in titles]
        choice, ok = QInputDialog.getItem(
            self,
            "WiFi 自动扫描",
            "选择要从 NRO 添加/选中的游戏：",
            labels,
            0,
            False,
        )
        if not ok:
            return
        index = labels.index(choice)
        title = titles[index]
        name = title.get("name", "")
        tid = title.get("title_id", "").upper()
        existing = self._find_game(name, tid)
        if existing:
            if not existing.get("title_id"):
                existing["title_id"] = tid
                save_config(self.config)
                self._refresh_table()
            self._select_row_for_game(existing)
            self.append_log(f"已选中现有游戏：{name} | {tid}")
            return

        new_game = {
            "id": uuid.uuid4().hex,
            "name": name,
            "title_id": tid,
            "pc_dir": "",
            "mode": "sav",
            "history": [],
            "last_sync": "",
        }
        dialog = ProfileDialog(self, new_game)
        if dialog.exec() == QDialog.Accepted:
            self.config["games"].append(dialog.game())
            save_config(self.config)
            self._refresh_table()
            self.table.selectRow(len(self.config["games"]) - 1)

    def _sync(self, direction: str) -> None:
        if self.worker and self.worker.isRunning():
            QMessageBox.information(self, "正在同步", "已经有同步任务在运行。")
            return
        self._save_ip()
        game = self._selected_game()
        if not game:
            return
        direction_text = "Switch → PC" if direction == "from_switch" else "PC → Switch"
        answer = QMessageBox.question(
            self,
            "确认同步",
            f"游戏：{game['name']}\n方向：{direction_text}\n\n"
            "同步前会自动备份目标目录。是否继续？",
        )
        if answer != QMessageBox.Yes:
            return

        self.worker = SyncWorker(self.config, game, direction)
        self.worker.log.connect(self.append_log)
        self.worker.succeeded.connect(self._sync_succeeded)
        self.worker.failed.connect(self._sync_failed)
        self.from_switch_button.setEnabled(False)
        self.to_switch_button.setEnabled(False)
        self.worker.finished.connect(lambda: (self.from_switch_button.setEnabled(True), self.to_switch_button.setEnabled(True)))
        self.worker.start()

    def _sync_succeeded(self, label: str) -> None:
        self.append_log(f"同步成功：{label}")
        self._refresh_table()
        QMessageBox.information(self, "完成", f"同步成功：{label}\n\n备份和配置已保存在\n{app_dir()}")

    def _sync_failed(self, message: str) -> None:
        self.append_log(f"同步失败：{message}")
        QMessageBox.critical(self, "同步失败", message)


def main() -> None:
    app = QApplication([])
    window = MainWindow()
    window.show()
    app.exec()


if __name__ == "__main__":
    main()

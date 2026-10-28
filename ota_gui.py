#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32 蓝牙 OTA 固件升级上位机（单文件脚本 ota_gui.py）
==========================================================
运行方式:
    uv run --with pyserial --with requests ota_gui.py

依赖:
    - tkinter / ttk        (Python 标准库，界面)
    - requests             (云端版本检测与固件下载)
    - pyserial             (蓝牙串口通信)

功能:
    1. 串口连接配置，打开后发送 'V' 查询实际版本（Bootloader 需支持忽略 'V'）
    2. 云端固件更新检测（真实 API 或内置 Mock 模拟云端）
    3. 固件来源切换（云端下载 / 本地 .bin）
    4. 发送升级请求、16 字节固件信息和固件数据：
       阶段一发送 'U' 请求进入 Bootloader，记录设备响应并检查进入结果
       固件信息通过 'H' / 'h' 握手发送，保存成功收到 'H' 后继续
       阶段二 'E' 外部 Flash 多扇区擦除握手
       阶段三 128 字节停等流控分包传输（ACK == b'0'）
       阶段四发送 'C' 请求搬运，等待 APP 启动提示，再发送 'V' 查询实际版本

线程模型:
    云端请求和 OTA 串口收发在后台线程执行，端口扫描和打开/关闭仍在主线程。
    后台使用主线程启动时传入的参数；UI 更新通过 queue.Queue + root.after() 调度。
"""

from __future__ import annotations

import datetime
import os
import queue
import re
import struct
import threading
import time
import traceback
import zlib
from typing import Any, Callable, List, Optional

import requests
import serial
from serial.tools import list_ports

import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk


# ================================================================
# 常量配置
# ================================================================
CHUNK_SIZE = 128                 # 上位机每包最多 128 字节并等待应答；不保证设备端接收事件边界一致
MAX_FIRMWARE_SIZE = 16 * 1024     # 与 Bootloader 当前固定擦除的暂存范围一致
DEFAULT_BAUD = 9600              # 默认波特率
BAUD_LIST = ["4800", "9600", "19200", "38400", "57600", "115200", "230400"]

DEFAULT_API_URL = "https://raw.githubusercontent.com/Yzsheeep/STM32_OTA_Project/main/firmware/latest.json"
DEFAULT_DOWNLOAD_URL = "https://github.com/Yzsheeep/STM32_OTA_Project/releases/download/v1.0.1/app.bin"
UNKNOWN_DEVICE_VERSION = "未查询"

# Mock 模式演示元数据；更新日志及大小不代表本地固件实际功能
MOCK_LATEST_VERSION = "1.0.1"
MOCK_SIZE = 0
MOCK_CHANGELOG = "本地文件模拟云端返回，仅用于界面演示。"

FIRMWARE_FILENAME = "app.bin"

APP_TITLE = "STM32 蓝牙 OTA 固件升级上位机"
FONT_FAMILY = "Microsoft YaHei UI"

UI_QUEUE_POLL_MS = 50            # UI 任务队列轮询间隔
CLOUD_TIMEOUT_S = 15             # 云端 HTTP 超时
DOWNLOAD_TIMEOUT_S = 60          # 固件下载 HTTP 超时
APP_BOOT_TIMEOUT_S = 20          # 搬运、校验及 APP 启动的等待上限
VERSION_TIMEOUT_S = 3            # APP 主循环可能延迟约 1 秒才响应 V
APP_START_MESSAGE = b"Hello! I am App in 2nd Floor!\r\n"


# ================================================================
# 工具函数
# ================================================================
def parse_version(ver: str) -> tuple:
    """将 'V1.0.0' / '1.2.3' 等版本字符串解析为可比较的整数元组。"""
    text = (ver or "").strip().lstrip("Vv")
    parts: List[int] = []
    for seg in text.split("."):
        seg = seg.strip()
        try:
            parts.append(int(seg))
        except ValueError:
            # 忽略非法后缀，例如 1.0.1-beta -> 1.0.1
            digits = ""
            for ch in seg:
                if ch.isdigit():
                    digits += ch
                else:
                    break
            parts.append(int(digits) if digits else 0)
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:4])


def is_newer_version(remote: str, local: str) -> bool:
    """判断云端版本是否高于本地版本。"""
    return parse_version(remote) > parse_version(local)


def firmware_version_code(ver: str) -> int:
    """将主版本.次版本.修订号编码为现有元数据中的 uint32_t。"""
    parts = ver.strip().lstrip("Vv").split(".")
    if len(parts) != 3 or not all(part.isascii() and part.isdigit() for part in parts):
        raise ValueError(f"版本格式应为 1.0.1：{ver}")
    major, minor, patch = (int(part) for part in parts)
    if any(part > 255 for part in (major, minor, patch)):
        raise ValueError("版本号每一段必须在 0～255 之间。")
    return (major << 16) | (minor << 8) | patch


def firmware_version_from_bin(data: bytes) -> str:
    """读取当前 APP 已有的版本回复字符串，不增加新的固件头。"""
    versions = set(re.findall(rb"APP_VERSION=([0-9]+\.[0-9]+\.[0-9]+)\r\n\x00", data))
    if len(versions) != 1:
        raise ValueError("固件中未找到唯一的 APP_VERSION 版本标识，请使用当前 APP 工程生成的 .bin。")
    version = versions.pop().decode("ascii")
    firmware_version_code(version)
    return version


def validate_firmware_size(size: int) -> None:
    if not 8 <= size <= MAX_FIRMWARE_SIZE or size % 4:
        raise ValueError("当前固件长度必须为 8 字节到 16 KB，且为 4 字节的整数倍。")


def format_size(num_bytes: int) -> str:
    """格式化固件大小显示。"""
    return f"{num_bytes} 字节 ({num_bytes / 1024:.2f} KB)"


def timestamp_now() -> str:
    """返回精确到毫秒的时间戳 HH:MM:SS.mmm。"""
    now = datetime.datetime.now()
    return f"{now.strftime('%H:%M:%S')}.{now.microsecond // 1000:03d}"


# ================================================================
# 主界面类
# ================================================================
class OTAGuiApp:
    """STM32 蓝牙 OTA 固件升级上位机主程序。"""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("820x700")
        self.root.minsize(760, 640)

        # ---- 线程安全 UI 队列 ----
        self._ui_queue: "queue.Queue[tuple]" = queue.Queue()

        # ---- 运行时状态 ----
        self.ser: Optional[serial.Serial] = None
        self.firmware_data: Optional[bytes] = None   # 云端下载 / 缓存的固件
        self.cloud_info: Optional[dict] = None       # 云端 JSON 信息
        self.ota_running = False
        self.checking_cloud = False
        self.querying_version = False
        self.device_version: Optional[str] = None   # 仅保存设备实际回复的版本

        self._build_styles()
        self._build_ui()
        self._scan_ports(initial=True)
        self._auto_find_local_bin()

        # 启动 UI 队列轮询
        self.root.after(UI_QUEUE_POLL_MS, self._poll_ui_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ------------------------------------------------------------ 风格
    def _build_styles(self) -> None:
        style = ttk.Style(self.root)
        # Windows 下优先使用 vista 主题，其余平台回退默认
        for theme in ("vista", "clam", "default"):
            try:
                style.theme_use(theme)
                break
            except tk.TclError:
                continue

        base = (FONT_FAMILY, 9)
        style.configure(".", font=base)
        style.configure("TLabel", font=base)
        style.configure("TButton", font=base)
        style.configure("TCheckbutton", font=base)
        style.configure("TRadiobutton", font=base)
        style.configure("TCombobox", font=base)
        style.configure("TEntry", font=base)
        style.configure("TLabelframe", font=(FONT_FAMILY, 9, "bold"))
        style.configure("TLabelframe.Label", font=(FONT_FAMILY, 9, "bold"))

        style.configure("Header.TLabel", font=(FONT_FAMILY, 11, "bold"))
        style.configure("InfoKey.TLabel", font=(FONT_FAMILY, 9, "bold"))
        style.configure("InfoVal.TLabel", font=(FONT_FAMILY, 9))
        style.configure("Start.TButton", font=(FONT_FAMILY, 11, "bold"), padding=(12, 8))
        style.configure("Status.TLabel", font=(FONT_FAMILY, 10, "bold"))

    # ------------------------------------------------------------ UI 搭建
    def _build_ui(self) -> None:
        pad = {"padx": 8, "pady": 4}
        outer = ttk.Frame(self.root, padding=10)
        outer.pack(fill=tk.BOTH, expand=True)

        # ========== 1. 连接配置栏 ==========
        conn_frame = ttk.LabelFrame(outer, text="连接配置")
        conn_frame.pack(fill=tk.X, **pad)

        row = ttk.Frame(conn_frame, padding=(8, 6))
        row.pack(fill=tk.X)

        ttk.Label(row, text="串口:").pack(side=tk.LEFT)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(
            row, textvariable=self.port_var, width=12, state="readonly"
        )
        self.port_combo.pack(side=tk.LEFT, padx=(4, 6))

        self.btn_refresh = ttk.Button(row, text="刷新", width=7, command=self._scan_ports)
        self.btn_refresh.pack(side=tk.LEFT, padx=(0, 12))

        ttk.Label(row, text="波特率:").pack(side=tk.LEFT)
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.baud_combo = ttk.Combobox(
            row, textvariable=self.baud_var, values=BAUD_LIST, width=9, state="readonly"
        )
        self.baud_combo.pack(side=tk.LEFT, padx=(4, 12))

        self.btn_serial = ttk.Button(row, text="打开串口", width=12, command=self._toggle_serial)
        self.btn_serial.pack(side=tk.LEFT, padx=(0, 12))

        self.conn_status_var = tk.StringVar(value="● 未连接")
        self.lbl_conn_status = ttk.Label(
            row, textvariable=self.conn_status_var, style="Status.TLabel", foreground="#c0392b"
        )
        self.lbl_conn_status.pack(side=tk.LEFT)

        # ========== 2. 云端固件更新区 ==========
        cloud_frame = ttk.LabelFrame(outer, text="云端固件更新")
        cloud_frame.pack(fill=tk.X, **pad)

        api_row = ttk.Frame(cloud_frame, padding=(8, 6))
        api_row.pack(fill=tk.X)
        ttk.Label(api_row, text="云端 API:").pack(side=tk.LEFT)
        self.api_url_var = tk.StringVar(value=DEFAULT_API_URL)
        self.api_entry = ttk.Entry(api_row, textvariable=self.api_url_var)
        self.api_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 10))

        self.mock_var = tk.BooleanVar(value=False)  # 默认访问真实更新源
        self.mock_chk = ttk.Checkbutton(
            api_row, text="内置模拟云端 (Mock 模式)", variable=self.mock_var
        )
        self.mock_chk.pack(side=tk.LEFT)

        btn_row = ttk.Frame(cloud_frame, padding=(8, 0, 8, 6))
        btn_row.pack(fill=tk.X)
        self.btn_check = ttk.Button(
            btn_row, text="检查云端更新", width=16, command=self._check_cloud
        )
        self.btn_check.pack(side=tk.LEFT)

        self.update_hint_var = tk.StringVar(value="尚未检查云端更新")
        self.lbl_hint = tk.Label(
            btn_row,
            textvariable=self.update_hint_var,
            font=(FONT_FAMILY, 10, "bold"),
            fg="#7f8c8d",
            anchor="w",
        )
        self.lbl_hint.pack(side=tk.LEFT, padx=14)

        info_grid = ttk.Frame(cloud_frame, padding=(8, 2, 8, 6))
        info_grid.pack(fill=tk.X)
        info_grid.columnconfigure(1, weight=1)
        info_grid.columnconfigure(3, weight=1)

        ttk.Label(info_grid, text="设备当前版本:", style="InfoKey.TLabel").grid(
            row=0, column=0, sticky="w", padx=(0, 4), pady=2
        )
        self.device_ver_var = tk.StringVar(value=UNKNOWN_DEVICE_VERSION)
        ttk.Label(info_grid, textvariable=self.device_ver_var, style="InfoVal.TLabel").grid(
            row=0, column=1, sticky="w", pady=2
        )

        ttk.Label(info_grid, text="云端最新版本:", style="InfoKey.TLabel").grid(
            row=0, column=2, sticky="w", padx=(16, 4), pady=2
        )
        self.cloud_ver_var = tk.StringVar(value="—")
        ttk.Label(info_grid, textvariable=self.cloud_ver_var, style="InfoVal.TLabel").grid(
            row=0, column=3, sticky="w", pady=2
        )

        ttk.Label(info_grid, text="固件大小:", style="InfoKey.TLabel").grid(
            row=1, column=0, sticky="w", padx=(0, 4), pady=2
        )
        self.fw_size_var = tk.StringVar(value="—")
        ttk.Label(info_grid, textvariable=self.fw_size_var, style="InfoVal.TLabel").grid(
            row=1, column=1, sticky="w", pady=2
        )

        ttk.Label(info_grid, text="更新日志 (Changelog):", style="InfoKey.TLabel").grid(
            row=2, column=0, sticky="nw", padx=(0, 4), pady=(6, 2)
        )
        self.changelog_text = tk.Text(
            info_grid,
            height=4,
            wrap=tk.WORD,
            relief=tk.SOLID,
            borderwidth=1,
            font=(FONT_FAMILY, 9),
            bg="#f7f9fa",
            state=tk.DISABLED,
        )
        self.changelog_text.grid(row=3, column=0, columnspan=4, sticky="ew", pady=(0, 4))

        # ========== 3. 固件来源选择区 ==========
        src_frame = ttk.LabelFrame(outer, text="固件来源")
        src_frame.pack(fill=tk.X, **pad)

        radio_row = ttk.Frame(src_frame, padding=(8, 6))
        radio_row.pack(fill=tk.X)
        self.src_var = tk.StringVar(value="cloud")
        ttk.Radiobutton(
            radio_row, text="使用云端下载的最新固件", variable=self.src_var, value="cloud"
        ).pack(side=tk.LEFT, padx=(0, 18))
        ttk.Radiobutton(
            radio_row, text="手动选择本地 .bin 文件", variable=self.src_var, value="local"
        ).pack(side=tk.LEFT)

        file_row = ttk.Frame(src_frame, padding=(8, 0, 8, 6))
        file_row.pack(fill=tk.X)
        ttk.Label(file_row, text="本地路径:").pack(side=tk.LEFT)
        self.local_path_var = tk.StringVar()
        self.path_entry = ttk.Entry(file_row, textvariable=self.local_path_var)
        self.path_entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 10))
        ttk.Button(file_row, text="浏览选择 .bin", command=self._browse_bin).pack(side=tk.LEFT)

        # ========== 4. 升级控制与监控区 ==========
        ota_frame = ttk.LabelFrame(outer, text="升级控制与监控")
        ota_frame.pack(fill=tk.BOTH, expand=True, **pad)

        start_row = ttk.Frame(ota_frame, padding=(8, 8, 8, 4))
        start_row.pack(fill=tk.X)
        self.btn_start = ttk.Button(
            start_row,
            text="开始一键 OTA 升级",
            style="Start.TButton",
            command=self._start_ota,
        )
        self.btn_start.pack(fill=tk.X)

        prog_row = ttk.Frame(ota_frame, padding=(8, 2, 8, 4))
        prog_row.pack(fill=tk.X)
        self.progressbar = ttk.Progressbar(prog_row, maximum=100, value=0, mode="determinate")
        self.progressbar.pack(fill=tk.X, pady=(0, 4))
        self.progress_text_var = tk.StringVar(value="进度: 0.0% | 等待升级 ...")
        ttk.Label(prog_row, textvariable=self.progress_text_var).pack(anchor="w")

        log_row = ttk.Frame(ota_frame, padding=(8, 2, 8, 8))
        log_row.pack(fill=tk.BOTH, expand=True)
        ttk.Label(log_row, text="实时日志:").pack(anchor="w", pady=(0, 3))
        self.log_text = scrolledtext.ScrolledText(
            log_row,
            height=12,
            wrap=tk.WORD,
            font=("Consolas", 9),
            state=tk.DISABLED,
            bg="#1e1e1e",
            fg="#d4d4d4",
            insertbackground="#d4d4d4",
        )
        self.log_text.pack(fill=tk.BOTH, expand=True)
        self.log_text.tag_configure("INFO", foreground="#d4d4d4")
        self.log_text.tag_configure("OK", foreground="#6ee7a0")
        self.log_text.tag_configure("WARN", foreground="#f0c674")
        self.log_text.tag_configure("ERROR", foreground="#f07178")

        self.log(f"{APP_TITLE} 已启动，等待操作 ...")

    # ------------------------------------------------------------ 线程安全 UI 更新
    def ui_call(self, fn: Callable[..., Any], *args: Any, **kwargs: Any) -> None:
        """任意线程调用：把 UI 更新函数投入队列，由主线程执行。"""
        self._ui_queue.put((fn, args, kwargs))

    def _poll_ui_queue(self) -> None:
        """主线程定时消费 UI 队列，执行通过 ui_call 提交的回调；不涵盖其他位置直接访问 Tk 的情况。"""
        try:
            while True:
                fn, args, kwargs = self._ui_queue.get_nowait()
                try:
                    fn(*args, **kwargs)
                except Exception:  # noqa: BLE001 - 单个 UI 回调异常不影响其余回调
                    traceback.print_exc()
        except queue.Empty:
            pass
        self.root.after(UI_QUEUE_POLL_MS, self._poll_ui_queue)

    def log(self, msg: str, level: str = "INFO") -> None:
        """线程安全地向日志框追加一条带毫秒时间戳的日志。"""
        def _append() -> None:
            self.log_text.configure(state=tk.NORMAL)
            self.log_text.insert(tk.END, f"[{timestamp_now()}] {msg}\n", level)
            self.log_text.see(tk.END)
            self.log_text.configure(state=tk.DISABLED)

        self.ui_call(_append)

    def _set_progress(self, percent: float, text: str) -> None:
        self.progressbar["value"] = max(0.0, min(100.0, percent))
        self.progress_text_var.set(text)

    def _set_hint(self, text: str, color: str) -> None:
        self.update_hint_var.set(text)
        self.lbl_hint.configure(fg=color)

    def _set_device_version(self, ver: str) -> None:
        self.device_version = None if ver == UNKNOWN_DEVICE_VERSION else ver
        self.device_ver_var.set(ver)

    def _set_cloud_fields(self, version: str, size_text: str, changelog: str) -> None:
        self.cloud_ver_var.set(version)
        self.fw_size_var.set(size_text)
        self.changelog_text.configure(state=tk.NORMAL)
        self.changelog_text.delete("1.0", tk.END)
        self.changelog_text.insert(tk.END, changelog if changelog else "（无更新日志）")
        self.changelog_text.configure(state=tk.DISABLED)

    # ------------------------------------------------------------ 串口管理
    def _scan_ports(self, initial: bool = False) -> None:
        """扫描当前电脑可用串口并刷新下拉框。"""
        try:
            ports = [p.device for p in list_ports.comports()]
            ports.sort()
        except Exception as e:  # noqa: BLE001
            ports = []
            self.log(f"串口扫描失败: {e}", "ERROR")

        self.port_combo["values"] = ports
        current = self.port_var.get()
        if ports:
            if current not in ports:
                self.port_var.set(ports[0])
            if not initial:
                self.log(f"已扫描到 {len(ports)} 个串口: {', '.join(ports)}")
        else:
            self.port_var.set("")
            if not initial:
                self.log("未扫描到可用串口，请检查蓝牙适配器是否已连接", "WARN")

    def _auto_find_local_bin(self) -> None:
        """默认自动查找脚本目录 / 当前目录下的 app.bin。"""
        candidates = [
            os.path.join(os.path.dirname(os.path.abspath(__file__)), FIRMWARE_FILENAME),
            os.path.join(os.getcwd(), FIRMWARE_FILENAME),
        ]
        for path in candidates:
            if os.path.isfile(path):
                self.local_path_var.set(os.path.abspath(path))
                self.log(f"已自动定位本地固件: {os.path.abspath(path)}")
                return
        self.log(f"当前目录未发现 {FIRMWARE_FILENAME}，可手动浏览选择本地 .bin", "WARN")

    def _browse_bin(self) -> None:
        path = filedialog.askopenfilename(
            title="选择固件二进制文件",
            filetypes=[("固件二进制", "*.bin"), ("所有文件", "*.*")],
        )
        if path:
            self.local_path_var.set(path)
            self.src_var.set("local")
            try:
                size = os.path.getsize(path)
                self.log(f"已选择本地固件: {path} ({format_size(size)})")
            except OSError as e:
                self.log(f"读取文件信息失败: {e}", "ERROR")

    def _toggle_serial(self) -> None:
        if self.querying_version:
            return
        if self.ser is not None and self.ser.is_open:
            self._close_serial()
        else:
            self._open_serial()

    def _open_serial(self) -> None:
        if self.ota_running:
            messagebox.showwarning("提示", "OTA 升级进行中，请勿切换串口连接。")
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("提示", "请先选择串口号，或点击“刷新”扫描端口。")
            return
        try:
            baud = int(self.baud_var.get())
            self.ser = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1.0,
                write_timeout=2.0,
            )
        except (serial.SerialException, ValueError, OSError) as e:
            self.ser = None
            self.log(f"串口打开失败 [{port}]: {e}", "ERROR")
            messagebox.showerror("串口错误", f"打开串口失败:\n{e}")
            return

        self.btn_serial.configure(text="关闭串口")
        self.conn_status_var.set("● 已连接")
        self.lbl_conn_status.configure(foreground="#27ae60")
        self.log(f"串口已打开: {port} @ {baud} bps", "OK")
        self._set_device_version(UNKNOWN_DEVICE_VERSION)
        self.querying_version = True
        self._set_controls_enabled(False)
        self.btn_start.configure(state=tk.DISABLED)
        self._set_hint("正在查询设备实际版本 ...", "#2980b9")
        threading.Thread(target=self._query_connected_version, daemon=True).start()

    def _query_connected_version(self) -> None:
        """连接后只发送 V；Bootloader 必须已加入忽略 V 的分支。"""
        version = None
        try:
            version = self._query_app_version()
            self.log(f"连接查询成功，设备当前版本：{version}", "OK")
        except Exception as e:  # noqa: BLE001
            self.log(f"设备版本查询失败：{e}", "WARN")

        def _done() -> None:
            self.querying_version = False
            self._set_device_version(version or UNKNOWN_DEVICE_VERSION)
            if version is not None:
                self._set_hint("设备版本已查询，请点击检查云端更新", "#27ae60")
            else:
                self.device_ver_var.set("查询失败")
                self._set_hint("版本查询失败，请确认 APP 正在运行及串口连接正常", "#c0392b")
            self._set_controls_enabled(True)
            self.btn_start.configure(state=tk.NORMAL)

        self.ui_call(_done)

    def _close_serial(self) -> None:
        if self.ota_running:
            messagebox.showwarning("提示", "OTA 升级进行中，请勿关闭串口。")
            return
        self._close_serial_quiet()
        self.btn_serial.configure(text="打开串口")
        self.conn_status_var.set("● 未连接")
        self.lbl_conn_status.configure(foreground="#c0392b")
        self.log("串口已关闭")
        self._set_device_version(UNKNOWN_DEVICE_VERSION)

    def _close_serial_quiet(self) -> None:
        try:
            if self.ser is not None and self.ser.is_open:
                self.ser.close()
        except Exception:  # noqa: BLE001
            pass
        self.ser = None

    def _require_serial(self) -> bool:
        if self.ser is None or not self.ser.is_open:
            messagebox.showerror("串口未连接", "请先打开串口，再执行 OTA 升级。")
            self.log("串口未连接，操作已取消", "ERROR")
            return False
        return True

    # ------------------------------------------------------------ 云端检测与固件拉取
    def _check_cloud(self) -> None:
        if self.checking_cloud or self.ota_running or self.querying_version:
            return
        self.checking_cloud = True
        self.cloud_info = None
        self.firmware_data = None
        self._set_cloud_fields("—", "—", "")
        self._set_controls_enabled(False)
        self.btn_start.configure(state=tk.DISABLED)
        self._set_hint("正在检查云端更新 ...", "#2980b9")
        self.log("开始检查云端固件更新 ...")
        # 在主线程读取 Tk 变量，后台只接收普通 Python 参数。
        args = (
            self.mock_var.get(), self.api_url_var.get().strip(),
            self.local_path_var.get().strip(), self.device_version,
        )
        threading.Thread(target=self._check_cloud_worker, args=args, daemon=True).start()

    def _check_cloud_worker(
        self, use_mock: bool, url: str, local_path: str, local_ver: Optional[str]
    ) -> None:
        try:
            if use_mock:
                info, data = self._fetch_mock_cloud(local_path)
            else:
                info, data = self._fetch_real_cloud(url, local_ver)
        except Exception as e:  # noqa: BLE001
            msg = f"云端检查失败: {e}"
            self.log(msg, "ERROR")
            self.ui_call(self._set_hint, msg, "#c0392b")
            self.ui_call(messagebox.showerror, "云端更新检查失败", str(e))
            self._finish_cloud_check()
            return

        self.cloud_info = info
        self.firmware_data = data

        latest = str(info.get("latest_version", ""))
        size_text = format_size(len(data)) if data else format_size(int(info.get("size", 0)))
        changelog = str(info.get("changelog", ""))
        self.ui_call(self._set_cloud_fields, latest, size_text, changelog)
        self.log(
            f"云端信息 -> 最新版本: {latest} | 大小: {size_text} | 更新日志:\n{changelog}"
        )

        if local_ver is None:
            self.log("云端固件已下载并核验；设备版本未查询，本次不判断是否有新版本。", "WARN")
            self.ui_call(self._set_hint, "云端固件已就绪，设备版本未查询", "#7f8c8d")
        elif is_newer_version(latest, local_ver):
            self.log(f"发现新版本: {local_ver} -> {latest}，固件已缓存至内存", "OK")
            self.ui_call(self._set_hint, "发现新版本，固件已就绪", "#27ae60")
        else:
            self.log(f"当前已是最新版本 ({local_ver})")
            self.ui_call(self._set_hint, "当前已是最新版本", "#7f8c8d")

        self._finish_cloud_check()

    def _finish_cloud_check(self) -> None:
        def _done() -> None:
            self.checking_cloud = False
            if not self.ota_running:
                self._set_controls_enabled(True)
                self.btn_start.configure(state=tk.NORMAL)

        self.ui_call(_done)

    def _fetch_mock_cloud(self, local_path: str) -> tuple:
        """Mock 模式：等待 0.5 秒，返回模拟元数据字典并读取本地 app.bin；不访问真实云端。"""
        self.log("Mock 模式: 模拟 0.5 秒网络延迟 ...")
        time.sleep(0.5)

        info = {
            "latest_version": MOCK_LATEST_VERSION,
            "download_url": DEFAULT_DOWNLOAD_URL,
            "size": MOCK_SIZE,
            "changelog": MOCK_CHANGELOG,
        }
        self.log(f"Mock 模式: 收到云端 JSON -> {info}")

        candidates = []
        if local_path:
            candidates.append(local_path)
        candidates.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), FIRMWARE_FILENAME))
        candidates.append(os.path.join(os.getcwd(), FIRMWARE_FILENAME))

        for cand in candidates:
            if cand and os.path.isfile(cand):
                with open(cand, "rb") as f:
                    data = f.read()
                if not data:
                    raise RuntimeError(f"固件文件为空: {cand}")
                validate_firmware_size(len(data))
                info["latest_version"] = firmware_version_from_bin(data)
                info["crc32"] = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
                self.log(f"Mock 模式: 已读取本地固件 {cand} 共 {len(data)} 字节")
                info["size"] = len(data)
                self.ui_call(self.local_path_var.set, os.path.abspath(cand))
                return info, data

        raise FileNotFoundError(
            f"Mock 模式需要本地固件文件，但未找到 {FIRMWARE_FILENAME}。\n"
            f"请将固件放置到程序目录后重试。"
        )

    def _fetch_real_cloud(self, url: str, local_ver: Optional[str]) -> tuple:
        """真实模式：请求 API 解析 JSON，并通过 download_url 下载固件。"""
        if not url:
            raise RuntimeError("云端 API 地址不能为空")
        self.log(f"请求云端 API: {url}")
        try:
            with requests.get(url, timeout=CLOUD_TIMEOUT_S, headers={"Cache-Control": "no-cache"}) as resp:
                resp.raise_for_status()
                info = resp.json()
        except requests.RequestException as e:
            raise RuntimeError(f"HTTP 请求失败: {e}") from e
        except ValueError as e:
            raise RuntimeError(f"云端返回的 JSON 解析失败: {e}") from e

        if not isinstance(info, dict):
            raise RuntimeError("云端 JSON 格式错误，应为对象")

        for key in ("latest_version", "download_url", "size", "crc32"):
            if key not in info:
                raise RuntimeError(f"云端 JSON 缺少必要字段: {key}")

        if not isinstance(info["latest_version"], str):
            raise RuntimeError("云端 latest_version 必须为版本字符串。")
        firmware_version_code(info["latest_version"])
        if type(info["size"]) is not int:
            raise RuntimeError("云端 size 必须为固件字节数。")
        validate_firmware_size(info["size"])
        if not isinstance(info["crc32"], str) or not re.fullmatch(r"[0-9a-fA-F]{8}", info["crc32"]):
            raise RuntimeError("云端 crc32 必须为 8 位十六进制字符串。")
        if not isinstance(info["download_url"], str) or not info["download_url"].startswith("https://"):
            raise RuntimeError("云端 download_url 必须为 HTTPS 固件下载地址。")
        self.log(f"云端 JSON 解析成功: {info}")

        data: Optional[bytes] = None
        if local_ver is None or is_newer_version(str(info.get("latest_version", "")), local_ver):
            dl_url = str(info.get("download_url", "")).strip()
            if not dl_url:
                raise RuntimeError("download_url 为空，无法下载固件")
            self.log(f"开始下载云端固件: {dl_url}")
            try:
                with requests.get(dl_url, timeout=DOWNLOAD_TIMEOUT_S, stream=True) as response:
                    response.raise_for_status()
                    downloaded = bytearray()
                    for chunk in response.iter_content(chunk_size=4096):
                        downloaded.extend(chunk)
                        if len(downloaded) > info["size"]:
                            raise RuntimeError("下载数据超过云端声明的固件大小，已停止下载。")
                    data = bytes(downloaded)
            except requests.RequestException as e:
                raise RuntimeError(f"固件下载失败: {e}") from e
            if len(data) != info["size"]:
                raise RuntimeError(f"下载大小不一致：期望 {info['size']}，收到 {len(data)} 字节。")
            actual_crc = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
            if actual_crc != info["crc32"].upper():
                raise RuntimeError(f"云端固件 CRC32 不一致：期望 {info['crc32']}，收到 {actual_crc}。")
            binary_version = firmware_version_from_bin(data)
            if firmware_version_code(binary_version) != firmware_version_code(info["latest_version"]):
                raise RuntimeError(f"固件版本 {binary_version} 与云端声明 {info['latest_version']} 不一致。")
            self.log(f"下载核验通过：{len(data)} 字节，CRC32={actual_crc}，APP 版本={binary_version}", "OK")
        return info, data

    # ------------------------------------------------------------ OTA 升级主流程
    def _start_ota(self) -> None:
        if self.ota_running or self.checking_cloud or self.querying_version:
            return

        # 固件数据准备
        source = self.src_var.get()
        if source == "cloud":
            if not self.firmware_data:
                messagebox.showerror(
                    "缺少固件",
                    "尚未获取云端固件。\n请先点击“检查云端更新”，或切换到本地 .bin 文件。",
                )
                self.log("未获取云端固件，无法开始升级", "ERROR")
                return
            data = self.firmware_data
            self.log(f"固件来源: 云端下载的最新固件（{len(data)} 字节）")
        else:
            path = self.local_path_var.get().strip()
            if not path or not os.path.isfile(path):
                messagebox.showerror(
                    "文件不存在",
                    f"本地固件文件不存在:\n{path or '(未选择)'}\n请通过“浏览选择 .bin”指定固件。",
                )
                self.log(f"本地固件文件不存在: {path}", "ERROR")
                return
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError as e:
                messagebox.showerror("文件读取失败", str(e))
                self.log(f"读取本地固件失败: {e}", "ERROR")
                return
            if not data:
                messagebox.showerror("文件无效", "本地固件文件为空。")
                self.log("本地固件文件为空", "ERROR")
                return
            self.log(f"固件来源: 本地文件 {path}（{len(data)} 字节）")

        try:
            validate_firmware_size(len(data))
            target_version = firmware_version_from_bin(data)
            if source == "cloud":
                declared_version = str(self.cloud_info["latest_version"])
                if firmware_version_code(target_version) != firmware_version_code(declared_version):
                    raise ValueError("缓存固件版本与云端信息不一致，请重新检查云端更新。")
                target_version = declared_version
        except (ValueError, KeyError, TypeError) as e:
            self.log(f"固件检查失败: {e}", "ERROR")
            messagebox.showerror("固件检查失败", str(e))
            return

        if not self._require_serial():
            return

        self.ota_running = True
        self._set_device_version(UNKNOWN_DEVICE_VERSION)
        self.btn_start.configure(state=tk.DISABLED, text="OTA 升级进行中 ...")
        self._set_controls_enabled(False)
        self._set_progress(0.0, "进度: 0.0% | 准备升级 ...")

        threading.Thread(
            target=self._ota_worker,
            args=(data, target_version),
            daemon=True,
            name="ota-worker",
        ).start()

    def _set_controls_enabled(self, enabled: bool) -> None:
        state = tk.NORMAL if enabled else tk.DISABLED
        for widget in (
            self.btn_refresh,
            self.btn_serial,
            self.btn_check,
            self.port_combo,
            self.baud_combo,
            self.api_entry,
            self.mock_chk,
            self.path_entry,
        ):
            widget.configure(state=state)
        if enabled:
            self.port_combo.configure(state="readonly")
            self.baud_combo.configure(state="readonly")

    def _ota_worker(self, data: bytes, target_version: str) -> None:
        """发送固件后，等待 APP 启动并查询设备实际版本。"""
        success = False
        try:
            self.log("=" * 60)
            self.log(f"开始 OTA 升级，固件总大小 {len(data)} 字节，分包大小 {CHUNK_SIZE} 字节")

            self._phase_wake_bootloader()
            self._phase_send_metadata(data, target_version)
            self._phase_erase_flash()
            self._phase_stream_chunks(data)
            self._phase_copy_and_boot(data, target_version)

            success = True
        except Exception as e:  # noqa: BLE001
            self.log(f"OTA 升级失败: {e}", "ERROR")
            self.ui_call(messagebox.showerror, "OTA 升级失败", str(e))
        finally:
            self.ota_running = False

            def _restore() -> None:
                self.btn_start.configure(state=tk.NORMAL, text="开始一键 OTA 升级")
                self._set_controls_enabled(True)

            self.ui_call(_restore)
            if success:
                self.log("升级流程线程已安全退出", "OK")
            else:
                self.log("升级流程已中断，可排查后重试", "WARN")

    # ---- 阶段一：请求进入 Bootloader，并记录设备响应 ----
    def _phase_wake_bootloader(self) -> None:
        self.log("阶段一: 发送 U，等待 Bootloader 响应 ...")
        received = bytearray()
        previous_timeout = self.ser.timeout
        error_messages = (b"OTA flag write failed\r\n", b"EEPROM not ready\r\n", b"OTA flag read failed\r\n", b"OTA flag verify failed\r\n", b"Meta Read Err!\r\n")
        try:
            self.ser.reset_input_buffer()  # 发送前清除旧数据
            self.ser.timeout = 0.1
            self.ser.write(b"U")
            self.ser.flush()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                received.extend(self.ser.read(self.ser.in_waiting or 1))
                if any(message in received for message in error_messages):
                    response = received.decode("utf-8", errors="replace").strip()
                    self.log(f"设备唤醒输出: {response}", "ERROR")
                    raise RuntimeError(f"进入升级模式失败，设备返回: {response}")
                if b"Wait for OTA update...\r\n" in received or b"Set Flag OK!\r\n" in received:
                    for line in received.decode("utf-8", errors="replace").splitlines():
                        self.log(f"< {line}")
                    self.log("阶段一完成: 已收到 Bootloader 响应", "OK")
                    return
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"唤醒串口通信失败: {e}") from e
        finally:
            self.ser.timeout = previous_timeout

        response = received.decode("utf-8", errors="replace").strip() or "没有收到输出"
        self.log(f"设备唤醒输出: {response}", "WARN")
        raise RuntimeError(f"5 秒内未确认进入 Bootloader，设备输出: {response}")

    def _phase_send_metadata(self, data: bytes, target_version: str) -> None:
        """按 Bootloader 已有格式发送四个小端 uint32_t。"""
        crc = zlib.crc32(data) & 0xFFFFFFFF
        version = firmware_version_code(target_version)
        metadata = struct.pack("<IIII", 0x55AA55AA, len(data), crc, version)
        self.log(f"固件信息: {len(data)} 字节，CRC32={crc:08X}，版本字段={version:08X}")

        try:
            self.ser.timeout = 3
            self.ser.write(b"H")
            self.ser.flush()
            ack = self.ser.read(1)
            if ack != b"h":
                raise RuntimeError(f"固件信息握手失败：期望小写 h，收到 {ack!r}")

            self.ser.write(metadata)
            self.ser.flush()
            ack = self.ser.read(1)
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"固件信息发送失败: {e}") from e

        if ack == b"N":
            raise RuntimeError("设备拒绝固件信息，请检查 EEPROM 写入与读回校验。")
        if ack != b"H":
            raise RuntimeError(f"固件信息保存未确认：期望大写 H，收到 {ack!r}")
        self.log("固件信息保存已确认（收到 H）；CRC32 仅记录，设备尚未执行整包校验", "OK")

    # ---- 阶段二：擦除外部 Flash 握手（指令 'E'） ----
    def _phase_erase_flash(self) -> None:
        self.log("阶段二: 发送擦除指令 'E'，等待外部 Flash 多扇区擦除 ACK ...")
        try:
            self.ser.timeout = 5  # 擦除耗时较长，设置 5 秒读取超时
            self.ser.write(b"E")
            self.ser.flush()
            ack = self.ser.read(1)
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"串口通信失败（可能已断开）: {e}") from e

        if ack == b"E":
            self.log("外部 Flash 多扇区擦除完成！", "OK")
        elif ack == b"":
            raise RuntimeError("阶段二握手超时：5 秒内未收到 'E' 确认，升级终止")
        else:
            raise RuntimeError(f"阶段二握手失败：期望 b'E'，实际收到 {ack!r}，升级终止")

    # ---- 阶段三：每包最多 128 字节，末包可更短；等待字符 0 应答，无包序号、重传或独立包 CRC ----
    def _phase_stream_chunks(self, data: bytes) -> None:
        total = len(data)
        chunks = [data[i:i + CHUNK_SIZE] for i in range(0, total, CHUNK_SIZE)]
        total_pkts = len(chunks)
        self.log(f"阶段三: 开始 128 字节停等流控传输，共 {total_pkts} 个分包 / {total} 字节")

        for idx, chunk in enumerate(chunks, start=1):
            try:
                self.ser.write(chunk)
                self.ser.flush()
                ack = self.ser.read(1)
            except (serial.SerialException, OSError) as e:
                raise RuntimeError(
                    f"分包 #{idx}/{total_pkts} 串口通信失败（可能已断开）: {e}"
                ) from e

            if ack != b"0":
                if ack == b"":
                    raise RuntimeError(f"分包 #{idx}/{total_pkts} 确认超时：未收到 ACK '0'，升级中断")
                raise RuntimeError(
                    f"分包 #{idx}/{total_pkts} ACK 校验失败：期望 b'0'，实际收到 {ack!r}，升级中断"
                )

            sent = min(idx * CHUNK_SIZE, total)
            percent = sent * 100.0 / total
            text = f"进度: {percent:.1f}% | 包 #{idx}/{total_pkts} ({sent}/{total} 字节)"
            self.ui_call(self._set_progress, percent, text)
            self.log(f"分包 #{idx}/{total_pkts} 写入 W25Q64 成功（{sent}/{total} 字节）")

        self.log(f"阶段三完成: 全部 {total_pkts} 个分包均收到 ACK '0'", "OK")

    # ---- 阶段四：请求搬运，确认 APP 启动后查询版本 ----
    def _phase_copy_and_boot(self, data: bytes, target_version: str) -> None:
        self.log("阶段四: 所有分包传输完毕，准备片内 Flash 搬迁 ...")
        time.sleep(0.5)

        try:
            self.ser.reset_input_buffer()  # 后续只接受本次 C 指令之后的启动输出
            self.ser.write(b"C")
            self.ser.flush()
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"串口写入搬迁指令失败（可能已断开）: {e}") from e

        self.log("阶段四: 已发送 C，等待固件校验、片内 Flash 擦写及 APP 启动 ...")
        self.ui_call(self._set_progress, 100.0, "固件已发送，正在等待安装及 APP 启动确认 ...")
        self._wait_for_app_start()
        new_ver = self._query_app_version()
        self.ui_call(self._set_device_version, new_ver)
        if firmware_version_code(new_ver) != firmware_version_code(target_version):
            raise RuntimeError(f"APP 已启动，但实际版本 {new_ver} 与目标版本 {target_version} 不一致。")
        self.log(f"设备实际返回的当前版本: {new_ver}", "OK")

        total = len(data)
        self.ui_call(
            self._set_progress,
            100.0,
            f"进度: 100.0% | 已确认 APP 启动，设备版本 {new_ver} ({total}/{total} 字节)",
        )

        self.log("已确认设备进入 APP 并回复版本。", "OK")
        self.ui_call(
            messagebox.showinfo,
            "APP 启动已确认",
            f"设备已进入 APP，并回复当前版本：{new_ver}",
        )

    def _wait_for_app_start(self) -> None:
        """安装阶段先等 APP 启动提示，随后查询版本以确认安装结果。"""
        received = bytearray()
        previous_timeout = self.ser.timeout
        errors = (b"CRC check failed", b"APP CRC failed", b"Erase Failed!", b"Meta Read Err!")
        try:
            self.ser.timeout = 0.1
            deadline = time.monotonic() + APP_BOOT_TIMEOUT_S
            while time.monotonic() < deadline:
                received.extend(self.ser.read(self.ser.in_waiting or 1))
                if any(message in received for message in errors):
                    output = received.decode("utf-8", errors="replace").replace("\x00", "").strip()
                    raise RuntimeError(f"设备安装未完成，返回：{output}")
                if APP_START_MESSAGE in received:
                    for line in received.decode("utf-8", errors="replace").replace("\x00", "").splitlines():
                        if line.strip():
                            self.log(f"< {line}")
                    self.log("已收到 APP 启动提示，准备查询设备版本。", "OK")
                    return
                # 保留足够的尾部以识别被串口拆开的消息，避免异常输出无限累积。
                if len(received) > 8192:
                    del received[:-4096]
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"等待 APP 启动时串口通信失败: {e}") from e
        finally:
            self.ser.timeout = previous_timeout

        output = received.decode("utf-8", errors="replace").replace("\x00", "").strip() or "没有收到输出"
        raise RuntimeError(
            f"{APP_BOOT_TIMEOUT_S} 秒内未确认 APP 启动，不发送 V；设备输出：{output}"
        )

    def _query_app_version(self) -> str:
        """发送一个 V 并等待版本回复；Bootloader 需能安全忽略 V。"""
        ser = self.ser
        if ser is None or not ser.is_open:
            raise RuntimeError("串口未连接，无法查询设备版本。")
        received = bytearray()
        previous_timeout = ser.timeout
        try:
            ser.timeout = 0.1
            ser.reset_input_buffer()
            ser.write(b"V")
            ser.flush()
            self.log("发送 V，查询 APP 实际版本 ...")
            deadline = time.monotonic() + VERSION_TIMEOUT_S
            while time.monotonic() < deadline:
                received.extend(ser.read(ser.in_waiting or 1))
                while b"\n" in received:
                    line, _, remainder = received.partition(b"\n")
                    received = bytearray(remainder)
                    text = line.decode("ascii", errors="replace").strip("\r\x00 ")
                    if not text:
                        continue
                    self.log(f"< {text}")
                    if text.startswith("APP_VERSION="):
                        version = text[len("APP_VERSION="):].strip()
                        parts = version.split(".")
                        if len(version) > 32 or len(parts) != 3 or not all(
                            part.isascii() and part.isdigit() for part in parts
                        ):
                            raise RuntimeError(f"APP 版本回复格式无效：{text}")
                        return f"V{version}"
                if len(received) > 1024:
                    raise RuntimeError("APP 版本回复过长，无法确认设备版本。")
        except (serial.SerialException, OSError) as e:
            raise RuntimeError(f"查询 APP 版本时串口通信失败: {e}") from e
        finally:
            ser.timeout = previous_timeout

        raise RuntimeError(
            f"{VERSION_TIMEOUT_S} 秒内未收到完整的 APP_VERSION 回复，设备可能未进入 APP 或串口链路未就绪。"
        )

    # ------------------------------------------------------------ 退出
    def _on_close(self) -> None:
        if self.ota_running:
            if not messagebox.askyesno("确认退出", "OTA 升级正在进行中，确定要退出吗？"):
                return
        self._close_serial_quiet()
        self.root.destroy()


# ================================================================
# 程序入口
# ================================================================
def main() -> None:
    root = tk.Tk()
    OTAGuiApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()

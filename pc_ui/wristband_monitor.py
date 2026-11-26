"""STM32 智能手环桌面监控与通信验证上位机（纯 Python 标准库，免安装 pyserial）."""

from collections import deque
import ctypes
from ctypes import wintypes
from datetime import datetime
import queue
import re
import subprocess
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk
import winreg


class DCB(ctypes.Structure):
    """Win32 串口设备控制块 (28 字节)."""

    _fields_ = [
        ("DCBlength", wintypes.DWORD),
        ("BaudRate", wintypes.DWORD),
        ("fFlags", wintypes.DWORD),
        ("wReserved", wintypes.WORD),
        ("XonLim", wintypes.WORD),
        ("XoffLim", wintypes.WORD),
        ("ByteSize", wintypes.BYTE),
        ("Parity", wintypes.BYTE),
        ("StopBits", wintypes.BYTE),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", wintypes.WORD),
    ]


class COMMTIMEOUTS(ctypes.Structure):
    """Win32 串口超时配置 (20 字节)."""

    _fields_ = [
        ("ReadIntervalTimeout", wintypes.DWORD),
        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
        ("ReadTotalTimeoutConstant", wintypes.DWORD),
        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
        ("WriteTotalTimeoutConstant", wintypes.DWORD),
    ]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_INVALID_HANDLE = ctypes.c_void_p(-1).value

_k32.CreateFileW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    ctypes.c_void_p,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.HANDLE,
]
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.CloseHandle.restype = wintypes.BOOL
_k32.GetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(DCB)]
_k32.GetCommState.restype = wintypes.BOOL
_k32.BuildCommDCBW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(DCB)]
_k32.BuildCommDCBW.restype = wintypes.BOOL
_k32.SetCommState.argtypes = [wintypes.HANDLE, ctypes.POINTER(DCB)]
_k32.SetCommState.restype = wintypes.BOOL
_k32.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.POINTER(COMMTIMEOUTS)]
_k32.SetCommTimeouts.restype = wintypes.BOOL
_k32.EscapeCommFunction.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_k32.EscapeCommFunction.restype = wintypes.BOOL
_k32.PurgeComm.argtypes = [wintypes.HANDLE, wintypes.DWORD]
_k32.PurgeComm.restype = wintypes.BOOL
_k32.ReadFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
_k32.ReadFile.restype = wintypes.BOOL
_k32.WriteFile.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.c_void_p,
]
_k32.WriteFile.restype = wintypes.BOOL


def list_com_ports() -> list[str]:
    """从 Windows 注册表枚举当前可用串口号."""
    ports: list[str] = []
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM"
        ) as key:
            idx = 0
            while True:
                try:
                    _, val, _ = winreg.EnumValue(key, idx)
                    if isinstance(val, str) and val.upper().startswith("COM"):
                        ports.append(val.upper())
                    idx += 1
                except OSError:
                    break
    except OSError:
        return []

    def _sort_key(name: str) -> tuple[int, str]:
        m = re.search(r"(\d+)$", name)
        return (int(m.group(1)) if m else 9999, name)

    return sorted(set(ports), key=_sort_key)


class Win32Serial:
    """基于 Win32 API 的轻量异步安全串口封装."""

    def __init__(self) -> None:
        self.handle: wintypes.HANDLE | None = None
        self._lock = threading.Lock()

    @property
    def is_open(self) -> bool:
        return self.handle is not None

    def open(self, port: str, baud: int = 115200) -> None:
        self.close()
        path = rf"\\.\{port}"
        handle = _k32.CreateFileW(path, 0xC0000000, 0, None, 3, 0x80, None)
        if not handle or handle == _INVALID_HANDLE:
            err = ctypes.get_last_error()
            raise OSError(err, f"打开 {port} 失败（WinError={err}），请确认端口未被其他串口助手占用")

        try:
            dcb = DCB()
            dcb.DCBlength = ctypes.sizeof(DCB)
            if not _k32.GetCommState(handle, ctypes.byref(dcb)):
                raise ctypes.WinError(ctypes.get_last_error())
            if not _k32.BuildCommDCBW(f"baud={baud} parity=N data=8 stop=1", ctypes.byref(dcb)):
                raise ctypes.WinError(ctypes.get_last_error())

            # 强制二进制模式、关闭流控并拉高 DTR/RTS 以便 STM32 USB CDC 识别已连接 PC
            dcb.fFlags |= 0x0001
            dcb.fFlags &= ~((1 << 2) | (1 << 3) | (1 << 6) | (1 << 8) | (1 << 9) | (1 << 11))
            dcb.fFlags = (dcb.fFlags & ~(0x3 << 4)) | (1 << 4)
            dcb.fFlags = (dcb.fFlags & ~(0x3 << 12)) | (1 << 12)
            if not _k32.SetCommState(handle, ctypes.byref(dcb)):
                raise ctypes.WinError(ctypes.get_last_error())

            timeouts = COMMTIMEOUTS(20, 0, 50, 0, 500)
            if not _k32.SetCommTimeouts(handle, ctypes.byref(timeouts)):
                raise ctypes.WinError(ctypes.get_last_error())

            _k32.EscapeCommFunction(handle, 5)  # SETDTR
            _k32.EscapeCommFunction(handle, 3)  # SETRTS
            _k32.PurgeComm(handle, 0x000C)
            self.handle = handle
        except Exception:
            _k32.CloseHandle(handle)
            raise

    def read_chunk(self, max_bytes: int = 256) -> bytes:
        with self._lock:
            h = self.handle
        if not h:
            return b""
        buf = ctypes.create_string_buffer(max_bytes)
        read_len = wintypes.DWORD(0)
        ok = _k32.ReadFile(h, buf, max_bytes, ctypes.byref(read_len), None)
        if not ok:
            err = ctypes.get_last_error()
            raise OSError(err, f"串口读取中断（WinError={err}）")
        return buf.raw[: read_len.value]

    def write_line(self, text: str) -> int:
        payload = (text.rstrip("\r\n") + "\r\n").encode("ascii", errors="replace")
        with self._lock:
            h = self.handle
            if not h:
                raise OSError("串口未打开")
            written = wintypes.DWORD(0)
            ok = _k32.WriteFile(h, payload, len(payload), ctypes.byref(written), None)
            if not ok:
                err = ctypes.get_last_error()
                raise OSError(err, f"串口发送失败（WinError={err}）")
            return int(written.value)

    def close(self) -> None:
        with self._lock:
            if self.handle is not None:
                _k32.CloseHandle(self.handle)
                self.handle = None


ALARM_MAP = (
    (0x01, "心率过高"),
    (0x02, "心率过低"),
    (0x04, "血氧过低"),
    (0x08, "体温过高"),
    (0x10, "体温过低"),
)

PAGE_NAMES = {0: "主页", 1: "阈值/报警页", 2: "历史记录页"}


def decode_alarm_mask(alm: int) -> str:
    """解码固件报警原因位掩码."""
    reasons = [label for bit, label in ALARM_MAP if alm & bit]
    return "、".join(reasons) if reasons else "正常"


def parse_data_line(line: str) -> dict | None:
    """解析手环每秒推送的 DATA 行."""
    s = line.strip()
    if not s.startswith("DATA,"):
        return None
    kv: dict[str, str] = {}
    for part in s[5:].split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
    try:
        temp_str = kv["TEMP"]
        return {
            "hr": int(kv["HR"]),
            "spo2": int(kv["SPO2"]),
            "temp": temp_str,
            "temp_val": float(temp_str),
            "step": int(kv["STEP"]),
            "alm": int(kv["ALM"]),
            "sos": int(kv["SOS"]),
            "page": int(kv["PAGE"]),
            "time": kv["T"],
        }
    except (KeyError, ValueError):
        return None


_HIST_RE = re.compile(
    r"^#(\d+),([TU])(\d{2}:\d{2}),HRavg=(\d+),HRmax=(\d+),SPO2min=(\d+),Tmax=([\d.]+),S=(\d+),F=(\d+),A=(\d+)$"
)


def parse_hist_line(line: str) -> dict | None:
    """解析 DUMP 导出的单条 Flash 历史记录."""
    m = _HIST_RE.match(line.strip())
    if not m:
        return None
    seq, tag, hhmm, hr_avg, hr_max, spo2_min, tmax, steps, falls, alarms = m.groups()
    time_str = hhmm if tag == "T" else f"运行 {hhmm}"
    return {
        "seq": int(seq),
        "time_str": time_str,
        "hr_avg": int(hr_avg),
        "hr_max": int(hr_max),
        "spo2_min": int(spo2_min),
        "temp_max": f"{tmax} ℃",
        "steps": int(steps),
        "falls": int(falls),
        "alarms": int(alarms),
    }


_PNPUTIL_FIELD_MAP = {
    "Instance ID": "instance_id",
    "实例 ID": "instance_id",
    "Device Description": "description",
    "设备描述": "description",
    "Class Name": "class_name",
    "类名": "class_name",
    "Status": "status",
    "状态": "status",
    "Problem Code": "problem",
    "问题代码": "problem",
    "Problem Status": "problem_status",
    "问题状态": "problem_status",
}
_USB_ERROR_MARKERS = ("设备描述符请求失败", "Unknown", "DEVICE_DESCRIPTOR_FAILURE")


def _get_oem_encoding() -> str:
    """获取 Windows 原生控制台命令使用的 OEM 代码页编码."""
    try:
        cp = ctypes.windll.kernel32.GetOEMCP()
        if cp:
            return f"cp{cp}"
    except Exception:
        pass
    return "gbk"


def _run_pnputil(args: list[str]) -> list[dict[str, str]]:
    """调用 pnputil 并解析已连接设备信息块."""
    res = subprocess.run(
        ["pnputil", *args],
        capture_output=True,
        timeout=5,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    enc = _get_oem_encoding()
    stdout = res.stdout.decode(enc, errors="replace").replace("\r\n", "\n")
    stderr = res.stderr.decode(enc, errors="replace").replace("\r\n", "\n").strip()
    if res.returncode != 0:
        detail = stderr or stdout.strip() or f"exit code {res.returncode}"
        raise RuntimeError(f"pnputil {' '.join(args)} 失败: {detail}")

    devices: list[dict[str, str]] = []
    for block in re.split(r"\n\s*\n", stdout.strip()):
        record: dict[str, str] = {}
        for raw_line in block.splitlines():
            if ":" not in raw_line:
                continue
            key, val = raw_line.split(":", 1)
            field = _PNPUTIL_FIELD_MAP.get(key.strip())
            if field:
                record[field] = val.strip()
        if "instance_id" in record:
            problem = record.get("problem", "")
            pstatus = record.get("problem_status", "")
            if pstatus and pstatus != problem:
                problem = f"{problem} ({pstatus})".strip()
            devices.append(
                {
                    "instance_id": record["instance_id"],
                    "description": record.get("description") or "未知设备",
                    "class_name": record.get("class_name") or "",
                    "status": record.get("status") or "未知",
                    "problem": problem or "异常",
                }
            )
    return devices


def diagnose_usb_environment() -> str:
    """排查当前电脑已接入的串口及异常 USB 描述符，辅助定位线材/硬件问题."""
    ports = list_com_ports()
    lines = [f"[USB诊断] 注册表活跃串口: {', '.join(ports) if ports else '无'}"]
    try:
        port_devs = _run_pnputil(["/enum-devices", "/connected", "/class", "Ports"])
        usb_devs = _run_pnputil(["/enum-devices", "/connected", "/class", "USB"])
        prob_devs = _run_pnputil(["/enum-devices", "/connected", "/problem"])

        sections = (
            ("[串口类设备 (Ports)]", port_devs, False),
            ("[已连接 USB 设备]", usb_devs, False),
            ("[异常/未识别设备]", prob_devs, True),
        )
        for title, devs, include_problem in sections:
            lines.append(title)
            if not devs:
                lines.append("  无")
                continue
            for dev in devs:
                parts = [dev["description"], dev["status"]]
                if include_problem:
                    parts.append(dev["problem"])
                parts.append(dev["instance_id"])
                lines.append("  - " + " | ".join(parts))

        has_usb_prob = any(
            dev["instance_id"].upper().startswith("USB\\")
            for dev in prob_devs
        ) or any(
            marker in f"{dev['description']} {dev['problem']} {dev['instance_id']}"
            for dev in prob_devs + usb_devs
            for marker in _USB_ERROR_MARKERS
        )

        if has_usb_prob:
            lines.append(
                "-> 检测到异常或未识别 USB 设备：请检查数据线接触、Blue Pill 核心板 PA12(D+) 1.5kΩ 上拉电阻，以及设备管理器中的错误代码。"
            )
        elif prob_devs:
            lines.append("-> 检测到其他硬件异常设备，请核对设备管理器错误代码。")
        elif ports or port_devs:
            lines.append("-> 已检测到串口设备，请在上方点击「刷新」后选择对应 COM 口并点击「连接」。")
        else:
            lines.append(
                "-> 未检测到任何串口及异常 USB 设备，请按以下 3 项排查：\n"
                "   1) 确认所用 Micro-USB 线为可传数据的四芯数据线（若板子亮灯但电脑无任何 USB/未知设备提示，多半是纯供电线，请更换手机数据线）；\n"
                "   2) 确认已通过 ST-Link 将带 USB CDC 的最新固件（28,244 B / 27.58 KB）下载到 STM32 核心板；\n"
                "   3) 备选方案：用 USB 转 TTL 模块接核心板 PA9(TX)/PA10(RX)/GND，选择 115200 波特率连接。"
            )
    except Exception as exc:
        lines.append(f"[USB诊断] 查询设备列表异常: {exc}")
    return "\n".join(lines)


class WristbandApp:
    """智能手环通信验证与实时监控桌面 GUI."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("智能手环串口/USB 监控助手")
        self.root.geometry("1020x740")
        self.root.minsize(920, 660)
        self.root.configure(bg="#F3F5F8")

        self.serial = Win32Serial()
        self.rx_queue: queue.Queue[tuple[str, str]] = queue.Queue()
        self.stop_event = threading.Event()
        self.reader_thread: threading.Thread | None = None

        self.hr_history: deque[int] = deque(maxlen=60)
        self.temp_history: deque[float] = deque(maxlen=60)
        self.pending_cmds: list[str] = []

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.conn_status_var = tk.StringVar(value="未连接")
        self.banner_var = tk.StringVar(value="系统状态：等待连接手环（插上 USB 数据线或 USB-TTL 后点刷新）")

        self.hr_var = tk.StringVar(value="--")
        self.spo2_var = tk.StringVar(value="--")
        self.temp_var = tk.StringVar(value="--.-")
        self.step_var = tk.StringVar(value="--")
        self.meta_var = tk.StringVar(value="手环页面：--   |   手环时间：--")

        self.hr_min_var = tk.StringVar(value="50")
        self.hr_max_var = tk.StringVar(value="120")
        self.spo2_min_var = tk.StringVar(value="90")
        self.t_min_var = tk.StringVar(value="20")
        self.t_max_var = tk.StringVar(value="40")
        self.cmd_var = tk.StringVar()

        self._setup_style()
        self._build_ui()
        self.refresh_ports()
        self.root.after(60, self._poll_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _setup_style(self) -> None:
        style = ttk.Style()
        try:
            style.theme_use("vista")
        except tk.TclError:
            pass
        style.configure(".", font=("Microsoft YaHei UI", 9))
        style.configure("TLabelframe.Label", font=("Microsoft YaHei UI", 9, "bold"), foreground="#1F2937")
        style.configure("Treeview", rowheight=22, font=("Microsoft YaHei UI", 9))

    def _build_ui(self) -> None:
        # 1. 顶部连接栏
        top = tk.Frame(self.root, bg="#FFFFFF", bd=0, highlightthickness=1, highlightbackground="#E5E7EB")
        top.pack(fill="x", padx=10, pady=(10, 6))

        tk.Label(top, text="串口:", bg="#FFFFFF").pack(side="left", padx=(10, 4), pady=8)
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=10, state="readonly")
        self.port_combo.pack(side="left", padx=2)
        ttk.Button(top, text="刷新", width=6, command=self.refresh_ports).pack(side="left", padx=4)

        tk.Label(top, text="波特率:", bg="#FFFFFF").pack(side="left", padx=(8, 4))
        ttk.Combobox(
            top, textvariable=self.baud_var, values=("115200", "9600"), width=8, state="readonly"
        ).pack(side="left", padx=2)

        self.conn_btn = ttk.Button(top, text="连接", width=8, command=self.toggle_connect)
        self.conn_btn.pack(side="left", padx=(8, 4))
        ttk.Button(top, text="一键诊断USB", command=self.run_usb_diag).pack(side="left", padx=4)

        self.conn_badge = tk.Label(
            top, textvariable=self.conn_status_var, bg="#FEE2E2", fg="#991B1B",
            font=("Microsoft YaHei UI", 9, "bold"), padx=10, pady=3
        )
        self.conn_badge.pack(side="right", padx=10)

        # 2. 状态横幅
        self.banner_label = tk.Label(
            self.root, textvariable=self.banner_var, bg="#E0F2FE", fg="#075985",
            font=("Microsoft YaHei UI", 10, "bold"), anchor="w", padx=12, pady=6
        )
        self.banner_label.pack(fill="x", padx=10, pady=(0, 6))

        # 3. 四大实时体征卡片
        cards = tk.Frame(self.root, bg="#F3F5F8")
        cards.pack(fill="x", padx=10, pady=(0, 6))
        for i in range(4):
            cards.columnconfigure(i, weight=1)

        self._make_card(cards, 0, "心率 (BPM)", self.hr_var, "#DC2626")
        self._make_card(cards, 1, "血氧 (%)", self.spo2_var, "#0284C7")
        self._make_card(cards, 2, "体温 (℃)", self.temp_var, "#D97706")
        self._make_card(cards, 3, "步数 (步)", self.step_var, "#059669")

        tk.Label(
            self.root, textvariable=self.meta_var, bg="#F3F5F8", fg="#4B5563", anchor="w"
        ).pack(fill="x", padx=12, pady=(0, 6))

        # 4. 中部左右分栏
        mid = tk.Frame(self.root, bg="#F3F5F8")
        mid.pack(fill="both", expand=True, padx=10, pady=(0, 6))
        mid.columnconfigure(0, weight=5)
        mid.columnconfigure(1, weight=6)
        mid.rowconfigure(0, weight=1)

        # 左栏：实时波形 + 遥控按钮
        left = tk.Frame(mid, bg="#F3F5F8")
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 6))

        wave_box = ttk.LabelFrame(left, text="实时心率趋势图（近 60 秒）")
        wave_box.pack(fill="both", expand=True, pady=(0, 6))
        self.canvas = tk.Canvas(wave_box, bg="#FFFFFF", height=160, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=6, pady=6)
        self.canvas.bind("<Configure>", lambda _e: self._redraw_chart())

        ctrl_box = ttk.LabelFrame(left, text="手环遥控与通信自检")
        ctrl_box.pack(fill="x")
        r1 = tk.Frame(ctrl_box)
        r1.pack(fill="x", padx=6, pady=(6, 3))
        ttk.Button(r1, text="切到主页", command=lambda: self.send_cmd("PAGE 0")).pack(side="left", expand=True, fill="x", padx=2)
        ttk.Button(r1, text="阈值/报警页", command=lambda: self.send_cmd("PAGE 1")).pack(side="left", expand=True, fill="x", padx=2)
        ttk.Button(r1, text="历史页", command=lambda: self.send_cmd("PAGE 2")).pack(side="left", expand=True, fill="x", padx=2)

        r2 = tk.Frame(ctrl_box)
        r2.pack(fill="x", padx=6, pady=3)
        ttk.Button(r2, text="确认消警/取消SOS", command=lambda: self.send_cmd("ACK")).pack(side="left", expand=True, fill="x", padx=2)
        ttk.Button(r2, text="远程触发SOS", command=lambda: self.send_cmd("SOS")).pack(side="left", expand=True, fill="x", padx=2)

        r3 = tk.Frame(ctrl_box)
        r3.pack(fill="x", padx=6, pady=(3, 6))
        ttk.Button(r3, text="一键对时 (电脑时间)", command=self.sync_time).pack(side="left", expand=True, fill="x", padx=2)
        ttk.Button(r3, text="发送 HELP 测连通", command=lambda: self.send_cmd("HELP")).pack(side="left", expand=True, fill="x", padx=2)

        # 右栏：阈值配置 + Flash 历史表格
        right = tk.Frame(mid, bg="#F3F5F8")
        right.grid(row=0, column=1, sticky="nsew")

        cfg_box = ttk.LabelFrame(right, text="报警阈值修改（写入手环 Flash 掉电保存）")
        cfg_box.pack(fill="x", pady=(0, 6))
        cfg_row = tk.Frame(cfg_box)
        cfg_row.pack(fill="x", padx=6, pady=6)
        fields = [
            ("心率下限", self.hr_min_var),
            ("心率上限", self.hr_max_var),
            ("血氧下限%", self.spo2_min_var),
            ("体温下限", self.t_min_var),
            ("体温上限", self.t_max_var),
        ]
        for label, var in fields:
            tk.Label(cfg_row, text=label).pack(side="left", padx=(4, 2))
            ttk.Entry(cfg_row, textvariable=var, width=4).pack(side="left", padx=(0, 6))
        ttk.Button(cfg_row, text="下发保存", command=self.apply_thresholds).pack(side="right", padx=2)

        hist_box = ttk.LabelFrame(right, text="Flash 历史记录管理")
        hist_box.pack(fill="both", expand=True)
        hbar = tk.Frame(hist_box)
        hbar.pack(fill="x", padx=6, pady=4)
        ttk.Button(hbar, text="立即保存一条", command=lambda: self.send_cmd("SAVE")).pack(side="left", padx=2)
        ttk.Button(hbar, text="读取全部历史", command=lambda: self.send_cmd("DUMP")).pack(side="left", padx=2)
        ttk.Button(hbar, text="清空历史", command=self.confirm_clear_hist).pack(side="right", padx=2)

        cols = ("seq", "time", "hr_avg", "hr_max", "spo2", "temp", "steps", "falls", "alarms")
        self.tree = ttk.Treeview(hist_box, columns=cols, show="headings", height=6)
        headers = [
            ("seq", "编号", 42),
            ("time", "时间", 78),
            ("hr_avg", "均心率", 54),
            ("hr_max", "高心率", 54),
            ("spo2", "低血氧", 54),
            ("temp", "高体温", 60),
            ("steps", "步数", 52),
            ("falls", "跌倒", 42),
            ("alarms", "报警", 42),
        ]
        for col, title, w in headers:
            self.tree.heading(col, text=title)
            self.tree.column(col, width=w, anchor="center")
        self.tree.pack(fill="both", expand=True, padx=6, pady=(0, 6))

        # 5. 底部通信日志与手动指令栏
        log_box = ttk.LabelFrame(self.root, text="原始串口收发日志（通信验证区）")
        log_box.pack(fill="x", padx=10, pady=(0, 10))
        self.log_text = scrolledtext.ScrolledText(
            log_box, height=7, font=("Consolas", 9), state="disabled", bg="#111827", fg="#E5E7EB"
        )
        self.log_text.pack(fill="x", padx=6, pady=(4, 4))

        sbar = tk.Frame(log_box)
        sbar.pack(fill="x", padx=6, pady=(0, 6))
        tk.Label(sbar, text="自定义指令:").pack(side="left")
        ent = ttk.Entry(sbar, textvariable=self.cmd_var)
        ent.pack(side="left", fill="x", expand=True, padx=6)
        ent.bind("<Return>", lambda _e: self.send_custom())
        ttk.Button(sbar, text="发送", width=8, command=self.send_custom).pack(side="left", padx=2)
        ttk.Button(sbar, text="清空日志", width=8, command=self.clear_log).pack(side="left", padx=2)

    def _make_card(self, parent: tk.Frame, col: int, title: str, var: tk.StringVar, color: str) -> None:
        card = tk.Frame(parent, bg="#FFFFFF", highlightthickness=1, highlightbackground="#E5E7EB")
        card.grid(row=0, column=col, sticky="ew", padx=4)
        tk.Label(card, text=title, bg="#FFFFFF", fg="#6B7280", font=("Microsoft YaHei UI", 9)).pack(
            anchor="w", padx=10, pady=(6, 0)
        )
        tk.Label(card, textvariable=var, bg="#FFFFFF", fg=color, font=("Microsoft YaHei UI", 18, "bold")).pack(
            anchor="w", padx=10, pady=(0, 6)
        )

    def log(self, msg: str) -> None:
        ts = datetime.now().strftime("%H:%M:%S")
        self.log_text.configure(state="normal")
        self.log_text.insert("end", f"[{ts}] {msg}\n")
        if int(self.log_text.index("end-1c").split(".")[0]) > 500:
            self.log_text.delete("1.0", "100.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def clear_log(self) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def refresh_ports(self) -> None:
        ports = list_com_ports()
        self.port_combo["values"] = ports
        if ports:
            if self.port_var.get() not in ports:
                self.port_var.set(ports[-1])
        else:
            self.port_var.set("")

    def run_usb_diag(self) -> None:
        self.log("正在扫描本机 USB 与串口设备...")

        def _worker() -> None:
            report = diagnose_usb_environment()
            self.rx_queue.put(("DIAG", report))

        threading.Thread(target=_worker, daemon=True).start()

    def toggle_connect(self) -> None:
        if self.serial.is_open:
            self.disconnect()
        else:
            self.connect()

    def connect(self) -> None:
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("未选择串口", "当前未检测到可用串口，请先插好数据线并点“刷新”（或点“一键诊断USB”排查）。")
            return
        try:
            baud = int(self.baud_var.get())
            self.serial.open(port, baud)
        except Exception as exc:
            messagebox.showerror("连接失败", str(exc))
            self.log(f"[ERR] {exc}")
            return

        self.stop_event.clear()
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()
        self.conn_btn.configure(text="断开")
        self.conn_status_var.set(f"已连接 {port}")
        self.conn_badge.configure(bg="#DCFCE7", fg="#166534")
        self.banner_var.set(f"系统状态：已连接 {port}，等待接收手环每秒 DATA 数据...")
        self.banner_label.configure(bg="#DBEAFE", fg="#1E40AF")
        self.log(f"[SYS] 已连接 {port} ({baud} bps)，已拉高 DTR（手环主页应显示“已连PC”）")

    def disconnect(self, reason: str = "") -> None:
        self.stop_event.set()
        self.serial.close()
        self.conn_btn.configure(text="连接")
        self.conn_status_var.set("未连接")
        self.conn_badge.configure(bg="#FEE2E2", fg="#991B1B")
        self.banner_var.set("系统状态：串口未连接")
        self.banner_label.configure(bg="#F3F4F6", fg="#374151")
        if reason:
            self.log(f"[DISCONNECT] {reason}")

    def _reader_loop(self) -> None:
        buf = bytearray()
        while not self.stop_event.is_set() and self.serial.is_open:
            try:
                chunk = self.serial.read_chunk(256)
            except OSError as exc:
                self.rx_queue.put(("ERR", f"{exc}（如刚按过手环刷新键，请等 2 秒后点刷新重连）"))
                break
            if not chunk:
                continue
            buf.extend(chunk)
            while b"\n" in buf:
                raw_line, _, rest = buf.partition(b"\n")
                buf = bytearray(rest)
                text = raw_line.decode("utf-8", errors="replace").strip()
                if text:
                    self.rx_queue.put(("RX", text))

    def _poll_queue(self) -> None:
        try:
            while True:
                kind, payload = self.rx_queue.get_nowait()
                if kind == "RX":
                    self.handle_rx_line(payload)
                elif kind == "ERR":
                    if self.serial.is_open:
                        self.disconnect(payload)
                elif kind == "DIAG":
                    self.log(payload)
                    self.refresh_ports()
        except queue.Empty:
            pass
        self.root.after(60, self._poll_queue)

    def handle_rx_line(self, line: str) -> None:
        """处理单行来自手环的数据（与 UI 逻辑解耦便于测试）."""
        data = parse_data_line(line)
        if data is not None:
            self.hr_var.set(str(data["hr"]))
            self.spo2_var.set(str(data["spo2"]))
            self.temp_var.set(data["temp"])
            self.step_var.set(str(data["step"]))
            pg_name = PAGE_NAMES.get(data["page"], f"第{data['page']}页")
            self.meta_var.set(f"手环当前页：{pg_name}   |   手环时间：{data['time']}")

            if data["sos"]:
                self.banner_var.set("紧急警报：手环处于 SOS / 跌倒求救状态！（可点“确认消警/取消SOS”）")
                self.banner_label.configure(bg="#FEE2E2", fg="#991B1B")
            elif data["alm"]:
                reasons = decode_alarm_mask(data["alm"])
                self.banner_var.set(f"越界报警：{reasons}（可点“确认消警/取消SOS”）")
                self.banner_label.configure(bg="#FEF3C7", fg="#92400E")
            else:
                self.banner_var.set("系统状态：通信正常，体征监控中")
                self.banner_label.configure(bg="#DCFCE7", fg="#166534")

            self.hr_history.append(data["hr"])
            self.temp_history.append(data["temp_val"])
            self._redraw_chart()
            self.log(f"RX: {line}")
            return

        if line.startswith("HIST_COUNT="):
            for item in self.tree.get_children():
                self.tree.delete(item)
            self.log(f"RX: {line}")
            return

        hist = parse_hist_line(line)
        if hist is not None:
            self.tree.insert(
                "",
                "end",
                values=(
                    hist["seq"],
                    hist["time_str"],
                    hist["hr_avg"],
                    hist["hr_max"],
                    f"{hist['spo2_min']}%",
                    hist["temp_max"],
                    hist["steps"],
                    hist["falls"],
                    hist["alarms"],
                ),
            )
            self.log(f"RX: {line}")
            return

        self.log(f"RX: {line}")

    def _redraw_chart(self) -> None:
        self.canvas.delete("all")
        w = max(self.canvas.winfo_width(), 240)
        h = max(self.canvas.winfo_height(), 120)
        pad_l, pad_r, pad_t, pad_b = 36, 12, 14, 18
        pw = w - pad_l - pad_r
        ph = h - pad_t - pad_b

        for val in (60, 100, 140):
            y = pad_t + ph - ((val - 40) / 140.0) * ph
            self.canvas.create_line(pad_l, y, w - pad_r, y, fill="#E5E7EB", dash=(3, 3))
            self.canvas.create_text(pad_l - 4, y, text=str(val), anchor="e", fill="#9CA3AF", font=("Consolas", 8))

        pts = list(self.hr_history)
        if len(pts) < 2:
            self.canvas.create_text(w // 2, h // 2, text="连接后将自动绘制实时心率曲线", fill="#9CA3AF")
            return

        coords: list[float] = []
        n = len(pts)
        for idx, hr_val in enumerate(pts):
            x = pad_l + (idx / max(n - 1, 1)) * pw
            clamped = max(40, min(180, hr_val))
            y = pad_t + ph - ((clamped - 40) / 140.0) * ph
            coords.extend((x, y))
        self.canvas.create_line(*coords, fill="#EF4444", width=2, smooth=False)
        self.canvas.create_text(
            w - pad_r, 8, text=f"最新心率: {pts[-1]} bpm", anchor="ne", fill="#DC2626", font=("Microsoft YaHei UI", 8, "bold")
        )

    def send_cmd(self, cmd: str) -> None:
        if not self.serial.is_open:
            messagebox.showinfo("未连接", "请先连接手环串口后再发送指令。")
            return
        try:
            self.serial.write_line(cmd)
            self.log(f"TX: {cmd}")
        except OSError as exc:
            self.disconnect(str(exc))

    def send_custom(self) -> None:
        cmd = self.cmd_var.get().strip()
        if cmd:
            self.send_cmd(cmd)
            self.cmd_var.set("")

    def sync_time(self) -> None:
        now_str = datetime.now().strftime("%H:%M:%S")
        self.send_cmd(f"TIME={now_str}")

    def confirm_clear_hist(self) -> None:
        if not self.serial.is_open:
            messagebox.showinfo("未连接", "请先连接手环串口。")
            return
        if messagebox.askyesno("确认清空", "确定要清空手环片内 Flash 保存的全部历史记录吗？"):
            self.send_cmd("CLR")

    def apply_thresholds(self) -> None:
        if not self.serial.is_open:
            messagebox.showinfo("未连接", "请先连接手环串口后再修改阈值。")
            return
        try:
            h_min = int(self.hr_min_var.get())
            h_max = int(self.hr_max_var.get())
            s_min = int(self.spo2_min_var.get())
            t_min = int(self.t_min_var.get())
            t_max = int(self.t_max_var.get())
        except ValueError:
            messagebox.showerror("格式错误", "全部阈值必须填写整数。")
            return
        if not (20 <= h_min <= h_max - 5 and h_max <= 220 and 50 <= s_min <= 100 and 0 <= t_min <= t_max - 2 and t_max <= 60):
            messagebox.showerror("范围无效", "请检查阈值范围：心率 20~220(间隔>=5)，血氧 50~100，体温 0~60(间隔>=2)。")
            return

        # 逐条间隔 150ms 下发，给手环第 61 页 Flash 擦写留出充足时间
        self.pending_cmds = [
            f"SET HR_MIN={h_min}",
            f"SET HR_MAX={h_max}",
            f"SET SPO2={s_min}",
            f"SET T_MIN={t_min}",
            f"SET T_MAX={t_max}",
        ]
        self._flush_pending_cmd()

    def _flush_pending_cmd(self) -> None:
        if not self.pending_cmds or not self.serial.is_open:
            self.pending_cmds.clear()
            return
        cmd = self.pending_cmds.pop(0)
        self.send_cmd(cmd)
        if self.pending_cmds:
            self.root.after(150, self._flush_pending_cmd)

    def _on_close(self) -> None:
        self.disconnect()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    WristbandApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()

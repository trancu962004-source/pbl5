#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
USB-CAN Patient Monitor Dashboard
Đọc CAN Extended ID từ STM32 qua USB-CAN CANable SLCAN, giải mã VITAL/LOADCELL,
xử lý cảnh báo tại Python, và hiển thị web realtime.

CAN Extended ID 29-bit:
  bit 28..26 : msg_type   1=VITAL, 2=LOADCELL
  bit 25..24 : wing       0=A, 1=B
  bit 23..16 : room       0..255
  bit 15..8  : bed        0..255
  bit 7..0   : patient    0..255

VITAL data:
  byte 0: HR low
  byte 1: HR high
  byte 2: SpO2
  byte 3: Temp x10 low
  byte 4: Temp x10 high
  byte 5: Battery
  byte 6: Seq
  byte 7: Status

LOADCELL data:
  byte 0: Volume ml low
  byte 1: Volume ml high
  byte 2: Drops/min low
  byte 3: Drops/min high
  byte 4: Seq
  byte 5: Status
  byte 6: Reserved
  byte 7: Reserved

Cài đặt:
  pip install python-can fastapi uvicorn openpyxl

Chạy Windows CANable SLCAN:
  python patient_can_web_dashboard.py --channel COM27 --bitrate 125000 --web

Mở web:
  http://127.0.0.1:8000
"""

from __future__ import annotations

import argparse
import asyncio
import json
import threading
import time
import urllib.parse
from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Any, Dict, List, Optional

import can

MSG_TYPE_VITAL = 1
MSG_TYPE_LOADCELL = 2

WING_CODE_TO_TEXT = {
    0: "A",
    1: "B",
}

# =========================
# NGƯỠNG CẢNH BÁO
# Có thể chỉnh theo yêu cầu đồ án/bác sĩ
# =========================

# Sinh hiệu
HR_LOW_CRITICAL = 45
HR_LOW_WARNING = 55
HR_HIGH_WARNING = 110
HR_HIGH_CRITICAL = 130

SPO2_WARNING = 94
SPO2_CRITICAL = 90

TEMP_WARNING = 38.0
TEMP_CRITICAL = 39.0

BATTERY_WARNING = 20
BATTERY_CRITICAL = 10

# Loadcell / dịch truyền
INFUSION_EMPTY_WARNING_ML = 80
INFUSION_EMPTY_CRITICAL_ML = 30
DROPS_LOW_WARNING = 5
DROPS_HIGH_WARNING = 90
DROPS_HIGH_CRITICAL = 120

# Mất kết nối
OFFLINE_WARNING_SEC = 8
OFFLINE_CRITICAL_SEC = 15

# Tự xóa bệnh nhân khỏi dashboard nếu offline quá lâu
# 300 giây = 5 phút. Có thể đổi thành 60 nếu muốn xóa sau 1 phút.
AUTO_REMOVE_PATIENT_SEC = 300

# Lưu lịch sử các ca/bệnh nhân đã được bác sĩ xử lý xong.
HANDLED_HISTORY_FILE = Path("handled_patients_log.json")
HANDLED_EXCEL_FILE = Path("handled_patients_export.xlsx")
HANDLED_BACKUP_DIR = Path("handled_history_backup")
MAX_HANDLED_HISTORY = 300

# Xác nhận ngưng chảy: DPM = 0 liên tục bao lâu mới cảnh báo
INFUSION_STOP_CONFIRM_SEC = 20

# Giảm báo động giả: cần cùng cảnh báo lặp lại N lần mới hiện critical/warning
ALERT_CONFIRM_COUNT = 2

# Hệ số dây truyền: 20 giọt = 1 ml
DROP_FACTOR_GTT_PER_ML = 20

# Status byte từ STM32/ESP:
# bit0: nhịp tim bất thường, bit1: SpO2 thấp, bit2: nhiệt độ cao
# bit3: té ngã, bit4: nút SOS/cầu cứu
STATUS_HR_ABNORMAL = 1 << 0
STATUS_SPO2_LOW = 1 << 1
STATUS_TEMP_HIGH = 1 << 2
STATUS_FALL = 1 << 3
STATUS_SOS = 1 << 4

IMMEDIATE_ALERT_CODES = {
    "FALL_DETECTED",
    "SOS_BUTTON",
    "VITAL_COMBINED_CRITICAL",
}

# Bộ lọc/AI rule-based nhẹ để giảm báo giả và gợi ý ngưỡng dịch theo từng bệnh nhân.
# Đây là heuristic, không phải model y tế đã được huấn luyện/kiểm định lâm sàng.
EMA_ALPHA = 0.25
DPM_BASELINE_MIN_SAMPLES = 5
DPM_DYNAMIC_LOW_RATIO = 0.50
DPM_DYNAMIC_HIGH_RATIO = 1.50
DPM_DYNAMIC_CRITICAL_RATIO = 2.00


@dataclass
class Vital:
    hr: int
    spo2: int
    temp: float
    battery: int
    seq: int
    status: int
    updated_at: float


@dataclass
class Loadcell:
    volume_ml: int
    drops_per_min: int
    seq: int
    status: int
    updated_at: float


@dataclass
class Alert:
    level: str          # "warning" hoặc "critical"
    code: str
    message: str
    value: Any
    timestamp: float


@dataclass
class PatientState:
    wing: str
    room: int
    bed: int
    patient: int
    vital: Optional[Vital] = None
    loadcell: Optional[Loadcell] = None
    first_seen: float = field(default_factory=time.time)
    last_seen: float = field(default_factory=time.time)
    zero_dpm_since: Optional[float] = None
    alerts: List[Alert] = field(default_factory=list)
    alert_counter: Dict[str, int] = field(default_factory=dict)

    # Các giá trị học/ước lượng đơn giản theo từng bệnh nhân.
    # Dùng để so sánh HR, SpO2 và điều chỉnh ngưỡng dịch truyền theo thực tế.
    hr_ema: Optional[float] = None
    spo2_ema: Optional[float] = None
    dpm_ema: Optional[float] = None
    dpm_baseline: Optional[float] = None
    dpm_baseline_samples: int = 0
    last_smart_note: str = ""

    # Quy trình xử lý cảnh báo:
    # - active: bệnh nhân đang cần bác sĩ xử lý, được giữ lại trên dashboard
    # - handled: bác sĩ đã bấm "Đã xử lý xong", cảnh báo cùng loại sẽ được tắt
    # - normal: không có cảnh báo đang chờ xử lý
    handling_status: str = "normal"
    needs_handling_since: Optional[float] = None
    handled_at: Optional[float] = None
    handled_note: str = ""
    handled_alert_signature: str = ""
    current_alert_signature: str = ""
    handling_alerts: List[Alert] = field(default_factory=list)

    @property
    def key(self) -> str:
        return f"{self.wing}-R{self.room:03d}-B{self.bed:03d}-P{self.patient:03d}"


patients: Dict[str, PatientState] = {}
patients_lock = threading.Lock()
handled_history: List[dict] = []

websocket_clients = set()
web_loop: Optional[asyncio.AbstractEventLoop] = None


def parse_can_id(can_id: int):
    msg_type = (can_id >> 26) & 0x07
    wing_code = (can_id >> 24) & 0x03
    room = (can_id >> 16) & 0xFF
    bed = (can_id >> 8) & 0xFF
    patient = can_id & 0xFF
    wing = WING_CODE_TO_TEXT.get(wing_code, f"X{wing_code}")
    return msg_type, wing, room, bed, patient


def make_patient_key(wing: str, room: int, bed: int, patient: int) -> str:
    return f"{wing}-R{room:03d}-B{bed:03d}-P{patient:03d}"


def get_or_create_patient(wing: str, room: int, bed: int, patient: int) -> PatientState:
    key = make_patient_key(wing, room, bed, patient)
    if key not in patients:
        patients[key] = PatientState(wing=wing, room=room, bed=bed, patient=patient)
    return patients[key]


def parse_vital_data(data: bytes) -> Vital:
    if len(data) < 8:
        raise ValueError("VITAL DLC must be 8")

    hr = data[0] | (data[1] << 8)
    spo2 = data[2]

    temp_x10 = data[3] | (data[4] << 8)
    if temp_x10 >= 32768:
        temp_x10 -= 65536

    return Vital(
        hr=hr,
        spo2=spo2,
        temp=temp_x10 / 10.0,
        battery=data[5],
        seq=data[6],
        status=data[7],
        updated_at=time.time(),
    )


def parse_loadcell_data(data: bytes) -> Loadcell:
    if len(data) < 8:
        raise ValueError("LOADCELL DLC must be 8")

    volume_ml = data[0] | (data[1] << 8)
    drops_per_min = data[2] | (data[3] << 8)

    return Loadcell(
        volume_ml=volume_ml,
        drops_per_min=drops_per_min,
        seq=data[4],
        status=data[5],
        updated_at=time.time(),
    )




def calculate_infusion_time(volume_ml: int, drops_per_min: int) -> dict:
    """
    Tính tốc độ ml/phút và thời gian truyền còn lại.
    """
    if volume_ml <= 0:
        return {
            "flow_ml_per_min": 0,
            "remaining_minutes": 0,
            "remaining_text": "Đã hết dịch"
        }

    if drops_per_min <= 0:
        return {
            "flow_ml_per_min": 0,
            "remaining_minutes": None,
            "remaining_text": "Đang ngưng chảy"
        }

    flow_ml_per_min = drops_per_min / DROP_FACTOR_GTT_PER_ML
    remaining_minutes = volume_ml / flow_ml_per_min

    hours = int(remaining_minutes // 60)
    minutes = int(remaining_minutes % 60)

    if hours > 0:
        remain_text = f"Còn khoảng {hours} giờ {minutes} phút"
    else:
        remain_text = f"Còn khoảng {minutes} phút"

    return {
        "flow_ml_per_min": round(flow_ml_per_min, 2),
        "remaining_minutes": round(remaining_minutes, 1),
        "remaining_text": remain_text
    }



def decode_status_bits(status: int) -> dict:
    """
    Giải mã byte status từ CAN.
    Hai data mới của đồng hồ:
      - fall = bit3
      - sos  = bit4
    """
    return {
        "hr_abnormal": bool(status & STATUS_HR_ABNORMAL),
        "spo2_low": bool(status & STATUS_SPO2_LOW),
        "temp_high": bool(status & STATUS_TEMP_HIGH),
        "fall": bool(status & STATUS_FALL),
        "sos": bool(status & STATUS_SOS),
    }


def update_ema(old: Optional[float], value: float, alpha: float = EMA_ALPHA) -> float:
    if old is None:
        return float(value)
    return (alpha * float(value)) + ((1.0 - alpha) * old)


def update_patient_models(p: PatientState):
    """
    Cập nhật mô hình nhỏ theo từng bệnh nhân:
    - EMA HR/SpO2 để so sánh xu hướng.
    - Baseline DPM để cảnh báo dịch truyền theo ngưỡng động thay vì cố định.
    """
    if p.vital:
        if p.vital.hr > 0:
            p.hr_ema = update_ema(p.hr_ema, p.vital.hr)
        if p.vital.spo2 > 0:
            p.spo2_ema = update_ema(p.spo2_ema, p.vital.spo2)

    if p.loadcell:
        dpm = p.loadcell.drops_per_min
        if dpm > 0:
            p.dpm_ema = update_ema(p.dpm_ema, dpm)

            # Học baseline khi tốc độ đang ở vùng hợp lý.
            # Sau đủ mẫu, baseline sẽ bám chậm theo EMA.
            if dpm <= DROPS_HIGH_CRITICAL:
                if p.dpm_baseline is None:
                    p.dpm_baseline = float(dpm)
                    p.dpm_baseline_samples = 1
                elif p.dpm_baseline_samples < DPM_BASELINE_MIN_SAMPLES:
                    p.dpm_baseline = (
                        (p.dpm_baseline * p.dpm_baseline_samples) + float(dpm)
                    ) / (p.dpm_baseline_samples + 1)
                    p.dpm_baseline_samples += 1
                else:
                    p.dpm_baseline = update_ema(p.dpm_baseline, dpm, alpha=0.08)


def dynamic_infusion_thresholds(p: PatientState) -> dict:
    """
    Tạo ngưỡng dịch truyền theo bệnh nhân.
    Nếu chưa có baseline đủ tin cậy thì dùng ngưỡng mặc định.
    """
    if p.dpm_baseline is not None and p.dpm_baseline_samples >= DPM_BASELINE_MIN_SAMPLES:
        base = max(1.0, p.dpm_baseline)
        return {
            "mode": "dynamic",
            "baseline_dpm": round(base, 1),
            "low_warning": max(1, int(round(base * DPM_DYNAMIC_LOW_RATIO))),
            "high_warning": max(2, int(round(base * DPM_DYNAMIC_HIGH_RATIO))),
            "high_critical": max(3, int(round(base * DPM_DYNAMIC_CRITICAL_RATIO))),
        }

    return {
        "mode": "fixed_until_enough_data",
        "baseline_dpm": None,
        "low_warning": DROPS_LOW_WARNING,
        "high_warning": DROPS_HIGH_WARNING,
        "high_critical": DROPS_HIGH_CRITICAL,
    }


def smart_recommendations(p: PatientState) -> dict:
    """
    Thuật toán gợi ý dạng rule-based:
    - So sánh HR và SpO2 hiện tại với EMA của chính bệnh nhân.
    - Gợi ý ngưỡng dịch truyền động dựa trên baseline DPM.
    """
    notes: List[str] = []
    actions: List[str] = []
    risk_score = 0

    if p.vital:
        v = p.vital
        flags = decode_status_bits(v.status)

        if flags["sos"]:
            risk_score += 100
            actions.append("Bệnh nhân bấm SOS: ưu tiên kiểm tra ngay.")
        if flags["fall"]:
            risk_score += 90
            actions.append("Có tín hiệu té ngã: kiểm tra bệnh nhân và vị trí giường.")

        if v.hr > 0 and p.hr_ema:
            hr_delta = v.hr - p.hr_ema
            if abs(hr_delta) >= 20:
                risk_score += 20
                notes.append(f"HR lệch {hr_delta:+.0f} bpm so với trung bình gần đây ({p.hr_ema:.0f}).")
        if v.spo2 > 0 and p.spo2_ema:
            spo2_delta = v.spo2 - p.spo2_ema
            if spo2_delta <= -3:
                risk_score += 25
                notes.append(f"SpO2 giảm {abs(spo2_delta):.0f}% so với trung bình gần đây ({p.spo2_ema:.0f}%).")

        if v.hr > HR_HIGH_WARNING and 0 < v.spo2 < SPO2_WARNING:
            risk_score += 60
            actions.append("HR cao kèm SpO2 thấp: ưu tiên đo lại và hỗ trợ hô hấp nếu cần.")
        elif v.hr < HR_LOW_WARNING and 0 < v.spo2 < SPO2_WARNING:
            risk_score += 60
            actions.append("HR thấp kèm SpO2 thấp: ưu tiên kiểm tra cảm biến và tình trạng bệnh nhân.")

    infusion_thresholds = dynamic_infusion_thresholds(p)
    if p.loadcell:
        l = p.loadcell
        if infusion_thresholds["mode"] == "dynamic":
            notes.append(
                "Ngưỡng dịch đang dùng theo baseline bệnh nhân: "
                f"{infusion_thresholds['baseline_dpm']} giọt/phút "
                f"(thấp≤{infusion_thresholds['low_warning']}, "
                f"cao≥{infusion_thresholds['high_warning']}, "
                f"rất cao≥{infusion_thresholds['high_critical']})."
            )
        else:
            notes.append("Chưa đủ dữ liệu để học ngưỡng dịch riêng, tạm dùng ngưỡng mặc định.")

        if l.drops_per_min == 0 and l.volume_ml > INFUSION_EMPTY_WARNING_ML:
            risk_score += 45
            actions.append("Dịch còn nhưng DPM = 0: kiểm tra dây truyền, kẹp truyền hoặc cảm biến giọt.")
        elif l.drops_per_min > 0 and infusion_thresholds["mode"] == "dynamic":
            base = infusion_thresholds["baseline_dpm"] or 0
            if base and l.drops_per_min >= infusion_thresholds["high_warning"]:
                actions.append("Tốc độ dịch cao hơn ngưỡng động: nên kiểm tra/điều chỉnh khóa dịch.")
            elif base and l.drops_per_min <= infusion_thresholds["low_warning"]:
                actions.append("Tốc độ dịch thấp hơn ngưỡng động: kiểm tra tắc dây hoặc hết dịch.")

    if not notes:
        notes.append("Chưa phát hiện xu hướng bất thường rõ ràng.")
    if not actions:
        actions.append("Tiếp tục theo dõi.")

    return {
        "risk_score": risk_score,
        "notes": notes,
        "actions": actions,
        "infusion_thresholds": infusion_thresholds,
    }


def calculate_priority(p: PatientState) -> dict:
    """
    Xếp hạng bệnh nhân ưu tiên hỗ trợ từ cao xuống thấp.
    """
    score = 0
    reasons: List[str] = []

    for a in p.alerts:
        if a.level == "critical":
            score += 45
        elif a.level == "warning":
            score += 18
        reasons.append(a.message)

    if p.vital:
        flags = decode_status_bits(p.vital.status)
        if flags["sos"]:
            score += 100
            reasons.append("SOS")
        if flags["fall"]:
            score += 90
            reasons.append("Té ngã")

        if p.vital.spo2 > 0:
            if p.vital.spo2 < SPO2_CRITICAL:
                score += 70
            elif p.vital.spo2 < SPO2_WARNING:
                score += 35

        if p.vital.hr > 0:
            if p.vital.hr < HR_LOW_CRITICAL or p.vital.hr > HR_HIGH_CRITICAL:
                score += 55
            elif p.vital.hr < HR_LOW_WARNING or p.vital.hr > HR_HIGH_WARNING:
                score += 25

    if p.loadcell:
        if p.loadcell.volume_ml <= INFUSION_EMPTY_CRITICAL_ML:
            score += 35
        if p.loadcell.drops_per_min == 0 and p.loadcell.volume_ml > INFUSION_EMPTY_WARNING_ML:
            score += 40

    smart = smart_recommendations(p)
    score += min(50, int(smart["risk_score"] * 0.25))

    if score >= 130:
        level = "Rất cao"
    elif score >= 80:
        level = "Cao"
    elif score >= 35:
        level = "Trung bình"
    else:
        level = "Thấp"

    # bỏ trùng reason nhưng vẫn giữ thứ tự
    unique_reasons = list(dict.fromkeys(reasons))[:6]

    return {
        "score": int(score),
        "level": level,
        "reasons": unique_reasons,
    }

def new_alert(level: str, code: str, message: str, value: Any) -> Alert:
    return Alert(
        level=level,
        code=code,
        message=message,
        value=value,
        timestamp=time.time(),
    )


def alert_signature(alerts: List[Alert]) -> str:
    """
    Tạo chữ ký cảnh báo dựa trên code.
    Khi bác sĩ đã xử lý xong một chữ ký, dashboard sẽ tắt cảnh báo đó
    cho đến khi cảnh báo biến mất hoàn toàn hoặc có loại cảnh báo mới.
    """
    if not alerts:
        return ""
    codes = sorted({a.code for a in alerts})
    return "|".join(codes)


def load_handled_history():
    global handled_history
    try:
        if HANDLED_HISTORY_FILE.exists():
            data = json.loads(HANDLED_HISTORY_FILE.read_text(encoding="utf-8"))
            if isinstance(data, list):
                handled_history = data[-MAX_HANDLED_HISTORY:]
    except Exception as exc:
        print("Cannot load handled history:", exc, flush=True)
        handled_history = []


def save_handled_history():
    try:
        HANDLED_HISTORY_FILE.write_text(
            json.dumps(handled_history[-MAX_HANDLED_HISTORY:], ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
    except Exception as exc:
        print("Cannot save handled history:", exc, flush=True)


def clear_handled_history(doctor: str = "", shift: str = "", reason: str = "") -> dict:
    """
    Xóa lịch sử đã xử lý trong handled_patients_log.json.
    Trước khi xóa sẽ tự sao lưu sang thư mục handled_history_backup để tránh mất dữ liệu.
    Hàm này nên được gọi khi đang giữ patients_lock.
    """
    now = time.time()
    records = list(handled_history)
    backup_file = ""

    if records:
        HANDLED_BACKUP_DIR.mkdir(parents=True, exist_ok=True)
        backup_path = HANDLED_BACKUP_DIR / f"handled_patients_log_backup_{time.strftime('%Y%m%d_%H%M%S')}.json"
        backup_payload = {
            "cleared_at": now,
            "cleared_by": doctor or "Bác sĩ/nhân viên",
            "shift": shift or "Ca trực chưa nhập",
            "reason": reason or "Xóa lịch sử khi sang ngày/ca mới",
            "deleted_count": len(records),
            "records": records,
        }
        backup_path.write_text(
            json.dumps(backup_payload, ensure_ascii=False, indent=2),
            encoding="utf-8",
        )
        backup_file = str(backup_path)

    handled_history.clear()
    save_handled_history()

    print(
        f"CLEAR HANDLED HISTORY | count={len(records)} | by={doctor or '-'} | shift={shift or '-'} | backup={backup_file or '-'}",
        flush=True,
    )

    return {
        "ok": True,
        "deleted_count": len(records),
        "backup_file": backup_file,
        "cleared_at": now,
    }


def _fmt_ts_excel(ts: Any) -> str:
    try:
        if not ts:
            return ""
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(ts)))
    except Exception:
        return str(ts or "")


def _alerts_to_text(alerts: Any) -> str:
    if not isinstance(alerts, list):
        return ""
    parts = []
    for a in alerts:
        if isinstance(a, dict):
            level = str(a.get("level", "")).upper()
            msg = str(a.get("message", ""))
            value = a.get("value", "")
            code = str(a.get("code", ""))
            parts.append(f"{level} {code}: {msg} ({value})")
        else:
            parts.append(str(a))
    return "; ".join(parts)


def export_handled_history_excel(output_path: Path = HANDLED_EXCEL_FILE) -> Path:
    """Xuất lịch sử đã xử lý từ handled_patients_log.json/handled_history ra file Excel."""
    from openpyxl import Workbook
    from openpyxl.styles import Font, PatternFill, Border, Side, Alignment
    from openpyxl.utils import get_column_letter

    # Ưu tiên dữ liệu đang nằm trong RAM; nếu RAM trống thì đọc lại JSON trên ổ đĩa.
    records = list(handled_history)
    if not records and HANDLED_HISTORY_FILE.exists():
        try:
            data = json.loads(HANDLED_HISTORY_FILE.read_text(encoding="utf-8"))
            if isinstance(data, list):
                records = data
        except Exception:
            records = []

    wb = Workbook()
    ws = wb.active
    ws.title = "Lich su xu ly"

    headers = [
        "STT", "Mã bệnh nhân", "Dãy", "Phòng", "Giường", "BN",
        "Thời gian xử lý", "Bác sĩ xử lý", "Ca trực", "Ghi chú",
        "Điểm ưu tiên", "Cảnh báo",
        "HR", "SpO2", "Nhiệt độ", "Pin", "FALL", "SOS",
        "Dịch còn (ml)", "Giọt/phút", "Seq vital", "Seq loadcell",
    ]
    ws.append(headers)

    for idx, r in enumerate(records, start=1):
        vital = r.get("vital") or {}
        loadcell = r.get("loadcell") or {}
        flags = decode_status_bits(int(vital.get("status") or 0)) if isinstance(vital, dict) else {}
        priority = r.get("priority") or {}
        priority_text = priority.get("score", "") if isinstance(priority, dict) else priority

        ws.append([
            idx,
            r.get("patient_key", ""),
            r.get("wing", ""),
            r.get("room", ""),
            r.get("bed", ""),
            r.get("patient", ""),
            _fmt_ts_excel(r.get("handled_at")),
            r.get("handled_by", ""),
            r.get("shift", ""),
            r.get("note", ""),
            priority_text,
            _alerts_to_text(r.get("alerts", [])),
            vital.get("hr", "") if isinstance(vital, dict) else "",
            vital.get("spo2", "") if isinstance(vital, dict) else "",
            vital.get("temp", "") if isinstance(vital, dict) else "",
            vital.get("battery", "") if isinstance(vital, dict) else "",
            int(bool(flags.get("fall"))) if flags else "",
            int(bool(flags.get("sos"))) if flags else "",
            loadcell.get("volume_ml", "") if isinstance(loadcell, dict) else "",
            loadcell.get("drops_per_min", "") if isinstance(loadcell, dict) else "",
            vital.get("seq", "") if isinstance(vital, dict) else "",
            loadcell.get("seq", "") if isinstance(loadcell, dict) else "",
        ])

    # Style nhẹ cho Excel dễ đọc.
    header_fill = PatternFill("solid", fgColor="D9EAF7")
    thin = Side(style="thin", color="D0D7DE")
    border = Border(left=thin, right=thin, top=thin, bottom=thin)
    for cell in ws[1]:
        cell.font = Font(bold=True)
        cell.fill = header_fill
        cell.alignment = Alignment(horizontal="center", vertical="center", wrap_text=True)
        cell.border = border
    for row in ws.iter_rows(min_row=2):
        for cell in row:
            cell.border = border
            cell.alignment = Alignment(vertical="top", wrap_text=True)

    widths = {
        1: 6, 2: 20, 3: 8, 4: 8, 5: 8, 6: 8, 7: 20,
        8: 22, 9: 18, 10: 34, 11: 14, 12: 48,
        13: 8, 14: 8, 15: 10, 16: 8, 17: 8, 18: 8,
        19: 14, 20: 12, 21: 10, 22: 12,
    }
    for col_idx, width in widths.items():
        ws.column_dimensions[get_column_letter(col_idx)].width = width
    ws.freeze_panes = "A2"
    ws.auto_filter.ref = ws.dimensions

    output_path.parent.mkdir(parents=True, exist_ok=True)
    wb.save(output_path)
    return output_path


def normalize_patient_key(patient_key: str) -> str:
    """Chuẩn hóa key nhận từ web/API để tránh lỗi URL encode hoặc dư khoảng trắng."""
    key = urllib.parse.unquote(str(patient_key or "")).strip()
    key = key.replace(" ", "")
    return key


def find_patient_by_key(patient_key: str) -> Optional[PatientState]:
    """Tìm bệnh nhân bằng key, có fallback để tránh lỗi route/encode."""
    key = normalize_patient_key(patient_key)
    if key in patients:
        return patients[key]

    # Fallback 1: so sánh không phân biệt hoa thường
    key_upper = key.upper()
    for k, p in patients.items():
        if k.upper() == key_upper:
            return p

    # Fallback 2: chấp nhận key dạng B-R1-B2-P3 nếu có
    import re
    m = re.match(r"^([A-Za-z])-R0*(\d+)-B0*(\d+)-P0*(\d+)$", key)
    if m:
        wing = m.group(1).upper()
        room = int(m.group(2))
        bed = int(m.group(3))
        patient = int(m.group(4))
        fixed_key = make_patient_key(wing, room, bed, patient)
        return patients.get(fixed_key)

    return None


def mark_patient_handled(patient_key: str, note: str = "", doctor: str = "", shift: str = "") -> dict:
    """
    Bác sĩ bấm đã xử lý xong:
    - Lưu lại bệnh nhân vào lịch sử handled_patients_log.json
    - Kết thúc cảnh báo đang hiển thị cho bệnh nhân đó
    - Chặn hiện lại cùng nhóm cảnh báo cho đến khi có cảnh báo mới/khác
    """
    now = time.time()
    clean_key = normalize_patient_key(patient_key)

    with patients_lock:
        p = find_patient_by_key(clean_key)
        if p is None:
            available = sorted(list(patients.keys()))
            return {
                "ok": False,
                "error": "Không tìm thấy bệnh nhân trong bộ nhớ dashboard. Có thể bệnh nhân đã bị xóa offline hoặc bạn đang chạy sai file/server.",
                "requested_key": clean_key,
                "available_keys": available[:20],
            }

        # Nếu hiện tại không còn p.alerts nhưng ca vẫn active, dùng handling_alerts đã lưu.
        active_alerts = p.alerts if p.alerts else p.handling_alerts
        sig = p.current_alert_signature or alert_signature(active_alerts)

        # Nếu sig rỗng nhưng bác sĩ vẫn bấm xử lý, vẫn cho xử lý và lưu log.
        if not sig:
            sig = "MANUAL_HANDLED"

        p.handled_alert_signature = sig
        p.current_alert_signature = sig
        p.handling_status = "handled"
        p.handled_at = now
        p.handled_note = note or "Đã xử lý xong"
        p.needs_handling_since = None

        record = {
            "patient_key": p.key,
            "wing": p.wing,
            "room": p.room,
            "bed": p.bed,
            "patient": p.patient,
            "handled_at": now,
            "handled_by": doctor or "Bác sĩ/nhân viên",
            "shift": shift or "Ca trực chưa nhập",
            "note": p.handled_note,
            "alert_signature": sig,
            "alerts": [asdict(a) for a in active_alerts],
            "priority": calculate_priority(p),
            "vital": asdict(p.vital) if p.vital else None,
            "loadcell": asdict(p.loadcell) if p.loadcell else None,
        }

        handled_history.append(record)
        while len(handled_history) > MAX_HANDLED_HISTORY:
            handled_history.pop(0)

        # Tắt cảnh báo hiển thị ngay lập tức.
        p.alerts = []
        p.handling_alerts = []
        save_handled_history()

        print(f"{p.key} | HANDLED by {record['handled_by']} | note={p.handled_note}", flush=True)
        return {"ok": True, "record": record, "patient": patient_to_dict(p)}

def calculate_raw_alerts(p: PatientState) -> List[Alert]:
    now = time.time()
    alerts: List[Alert] = []

    # Mất kết nối
    offline_sec = now - p.last_seen
    if offline_sec >= OFFLINE_CRITICAL_SEC:
        return [new_alert("critical", "OFFLINE_CRITICAL", "Mất kết nối bệnh nhân", round(offline_sec, 1))]
    if offline_sec >= OFFLINE_WARNING_SEC:
        alerts.append(new_alert("warning", "OFFLINE_WARNING", "Node lâu chưa gửi dữ liệu", round(offline_sec, 1)))

    # Sinh hiệu + 2 data mới: FALL/SOS nằm trong status byte.
    if p.vital:
        v = p.vital
        flags = decode_status_bits(v.status)

        if flags["sos"]:
            alerts.append(new_alert("critical", "SOS_BUTTON", "Bệnh nhân bấm nút SOS/cầu cứu", 1))
        if flags["fall"]:
            alerts.append(new_alert("critical", "FALL_DETECTED", "Phát hiện té ngã", 1))

        # HR = 0 thường là cảm biến chưa đo được, không cảnh báo nhịp tim thấp ngay.
        if v.hr > 0:
            if v.hr < HR_LOW_CRITICAL:
                alerts.append(new_alert("critical", "HR_LOW_CRITICAL", "Nhịp tim rất thấp", v.hr))
            elif v.hr < HR_LOW_WARNING:
                alerts.append(new_alert("warning", "HR_LOW_WARNING", "Nhịp tim thấp", v.hr))
            elif v.hr > HR_HIGH_CRITICAL:
                alerts.append(new_alert("critical", "HR_HIGH_CRITICAL", "Nhịp tim rất cao", v.hr))
            elif v.hr > HR_HIGH_WARNING:
                alerts.append(new_alert("warning", "HR_HIGH_WARNING", "Nhịp tim cao", v.hr))

        # SpO2 = 0 thường là chưa có kết quả hợp lệ từ MAX30100, không cảnh báo SpO2.
        if v.spo2 > 0:
            if v.spo2 < SPO2_CRITICAL:
                alerts.append(new_alert("critical", "SPO2_CRITICAL", "SpO2 rất thấp", v.spo2))
            elif v.spo2 < SPO2_WARNING:
                alerts.append(new_alert("warning", "SPO2_WARNING", "SpO2 thấp", v.spo2))

        # Cảnh báo phối hợp HR + SpO2: không thay thế bác sĩ, chỉ là gợi ý ưu tiên.
        if v.hr > 0 and v.spo2 > 0:
            if (v.hr > HR_HIGH_WARNING and v.spo2 < SPO2_WARNING) or \
               (v.hr < HR_LOW_WARNING and v.spo2 < SPO2_WARNING):
                alerts.append(new_alert(
                    "critical",
                    "VITAL_COMBINED_CRITICAL",
                    "HR và SpO2 cùng bất thường, cần ưu tiên kiểm tra",
                    f"HR={v.hr}, SpO2={v.spo2}"
                ))

        # Temp = 0 đang là giá trị mặc định trong node watch, không cảnh báo.
        if v.temp > 0:
            if v.temp >= TEMP_CRITICAL:
                alerts.append(new_alert("critical", "TEMP_CRITICAL", "Sốt cao", v.temp))
            elif v.temp >= TEMP_WARNING:
                alerts.append(new_alert("warning", "TEMP_WARNING", "Sốt", v.temp))

        if v.battery > 0:
            if v.battery <= BATTERY_CRITICAL:
                alerts.append(new_alert("critical", "BATTERY_CRITICAL", "Pin rất yếu", v.battery))
            elif v.battery <= BATTERY_WARNING:
                alerts.append(new_alert("warning", "BATTERY_WARNING", "Pin yếu", v.battery))

    # Loadcell: cảnh báo dịch truyền theo ngưỡng động nếu đã học đủ baseline.
    if p.loadcell:
        l = p.loadcell
        th = dynamic_infusion_thresholds(p)

        if l.volume_ml <= INFUSION_EMPTY_CRITICAL_ML:
            alerts.append(new_alert("critical", "INFUSION_EMPTY_CRITICAL", "Dịch truyền gần cạn", l.volume_ml))
        elif l.volume_ml <= INFUSION_EMPTY_WARNING_ML:
            alerts.append(new_alert("warning", "INFUSION_EMPTY_WARNING", "Dịch truyền sắp hết", l.volume_ml))

        if l.drops_per_min >= th["high_critical"]:
            alerts.append(new_alert(
                "critical",
                "DROPS_HIGH_CRITICAL",
                "Tốc độ nhỏ giọt quá nhanh so với ngưỡng động",
                {"dpm": l.drops_per_min, "threshold": th["high_critical"], "mode": th["mode"]}
            ))
        elif l.drops_per_min >= th["high_warning"]:
            alerts.append(new_alert(
                "warning",
                "DROPS_HIGH_WARNING",
                "Tốc độ nhỏ giọt nhanh so với ngưỡng động",
                {"dpm": l.drops_per_min, "threshold": th["high_warning"], "mode": th["mode"]}
            ))
        elif 0 < l.drops_per_min <= th["low_warning"]:
            alerts.append(new_alert(
                "warning",
                "DROPS_LOW_WARNING",
                "Tốc độ nhỏ giọt thấp so với ngưỡng động",
                {"dpm": l.drops_per_min, "threshold": th["low_warning"], "mode": th["mode"]}
            ))

        if l.volume_ml > INFUSION_EMPTY_WARNING_ML and l.drops_per_min == 0:
            if p.zero_dpm_since is not None:
                zero_sec = now - p.zero_dpm_since
                if zero_sec >= INFUSION_STOP_CONFIRM_SEC:
                    alerts.append(new_alert("critical", "INFUSION_STOP", "Dịch truyền có thể đã ngưng chảy", round(zero_sec, 1)))

    return alerts

def debounce_alerts(p: PatientState, raw_alerts: List[Alert]) -> List[Alert]:
    """
    Chống báo giả: cùng một code phải xuất hiện ALERT_CONFIRM_COUNT lần liên tiếp.
    Offline critical thì cho hiện ngay.
    """
    raw_codes = {a.code for a in raw_alerts}

    # reset counter cho cảnh báo đã hết
    for code in list(p.alert_counter.keys()):
        if code not in raw_codes:
            p.alert_counter[code] = 0

    confirmed: List[Alert] = []
    for a in raw_alerts:
        p.alert_counter[a.code] = p.alert_counter.get(a.code, 0) + 1
        if (
            a.code.startswith("OFFLINE")
            or a.code in IMMEDIATE_ALERT_CODES
            or p.alert_counter[a.code] >= ALERT_CONFIRM_COUNT
        ):
            confirmed.append(a)

    return confirmed


def update_alerts(p: PatientState):
    raw = calculate_raw_alerts(p)
    confirmed = debounce_alerts(p, raw)
    sig = alert_signature(confirmed)
    p.current_alert_signature = sig

    if not confirmed:
        p.alerts = []
        # Nếu đã từng có cảnh báo active nhưng bác sĩ chưa bấm xử lý,
        # vẫn giữ bệnh nhân trong danh sách cần xử lý cùng alert snapshot cũ.
        if p.handling_status == "active":
            return

        p.handling_alerts = []
        p.handling_status = "normal"
        p.needs_handling_since = None
        p.handled_alert_signature = ""
        return

    # Bác sĩ đã xử lý đúng nhóm cảnh báo này -> tắt cảnh báo hiển thị.
    # Nếu có loại cảnh báo mới, sig sẽ khác và hệ thống sẽ active lại.
    if sig and sig == p.handled_alert_signature:
        p.alerts = []
        p.handling_alerts = []
        p.handling_status = "handled"
        return

    # Cảnh báo mới/chưa xử lý: lưu bệnh nhân vào danh sách cần xử lý.
    p.alerts = confirmed
    p.handling_alerts = confirmed
    if p.handling_status != "active" or p.needs_handling_since is None:
        p.needs_handling_since = time.time()
    p.handling_status = "active"
    p.handled_at = None
    p.handled_note = ""


def cleanup_offline_patients(now: Optional[float] = None):
    """
    Xóa bệnh nhân khỏi danh sách nếu không nhận được CAN frame quá AUTO_REMOVE_PATIENT_SEC.
    Hàm này phải được gọi khi đang giữ patients_lock.
    """
    if now is None:
        now = time.time()

    for key in list(patients.keys()):
        p = patients[key]
        offline_sec = now - p.last_seen

        if offline_sec >= AUTO_REMOVE_PATIENT_SEC:
            print(f"{p.key} | REMOVE: offline {offline_sec:.1f}s", flush=True)
            del patients[key]


def update_patient_from_can(msg: can.Message) -> Optional[PatientState]:
    if not msg.is_extended_id:
        return None

    if len(msg.data) < 8:
        return None

    msg_type, wing, room, bed, patient_id = parse_can_id(msg.arbitration_id)

    with patients_lock:
        p = get_or_create_patient(wing, room, bed, patient_id)
        p.last_seen = time.time()

        if msg_type == MSG_TYPE_VITAL:
            p.vital = parse_vital_data(bytes(msg.data))
        elif msg_type == MSG_TYPE_LOADCELL:
            loadcell = parse_loadcell_data(bytes(msg.data))
            p.loadcell = loadcell

            if loadcell.drops_per_min == 0:
                if p.zero_dpm_since is None:
                    p.zero_dpm_since = time.time()
            else:
                p.zero_dpm_since = None
        else:
            return None

        update_patient_models(p)
        update_alerts(p)
        return p


def patient_to_dict(p: PatientState) -> dict:
    vital_dict = None
    if p.vital:
        vital_dict = {
            **asdict(p.vital),
            "status_flags": decode_status_bits(p.vital.status),
        }

    loadcell_dict = None
    if p.loadcell:
        loadcell_dict = {
            **asdict(p.loadcell),
            "status_flags": decode_status_bits(p.loadcell.status),
            "infusion": calculate_infusion_time(
                p.loadcell.volume_ml,
                p.loadcell.drops_per_min
            ),
            "dynamic_thresholds": dynamic_infusion_thresholds(p),
        }

    smart = smart_recommendations(p)
    priority = calculate_priority(p)

    return {
        "key": p.key,
        "wing": p.wing,
        "room": p.room,
        "bed": p.bed,
        "patient": p.patient,
        "vital": vital_dict,
        "loadcell": loadcell_dict,
        "first_seen": p.first_seen,
        "last_seen": p.last_seen,
        "alerts": [asdict(a) for a in p.alerts],
        "handling": {
            "status": p.handling_status,
            "needs_handling_since": p.needs_handling_since,
            "handled_at": p.handled_at,
            "handled_note": p.handled_note,
            "current_alert_signature": p.current_alert_signature,
            "handled_alert_signature": p.handled_alert_signature,
            "alerts": [asdict(a) for a in p.handling_alerts],
        },
        "smart": smart,
        "priority": priority,
    }

def print_patient(p: PatientState):
    vital_text = "-"
    if p.vital:
        flags = decode_status_bits(p.vital.status)
        vital_text = (
            f"HR={p.vital.hr} SpO2={p.vital.spo2} "
            f"T={p.vital.temp:.1f}C Bat={p.vital.battery}% Seq={p.vital.seq} "
            f"FALL={int(flags['fall'])} SOS={int(flags['sos'])}"
        )

    loadcell_text = "-"
    if p.loadcell:
        th = dynamic_infusion_thresholds(p)
        loadcell_text = (
            f"ML={p.loadcell.volume_ml} DPM={p.loadcell.drops_per_min} "
            f"Seq={p.loadcell.seq} TH={th['mode']} "
            f"L/H/HC={th['low_warning']}/{th['high_warning']}/{th['high_critical']}"
        )

    if p.alerts:
        alert_text = " | ".join([f"{a.level.upper()}:{a.message}({a.value})" for a in p.alerts])
    else:
        alert_text = "OK"

    priority = calculate_priority(p)
    print(
        f"{p.key} | PRIORITY={priority['level']}({priority['score']}) "
        f"| VITAL: {vital_text} | LOADCELL: {loadcell_text} | ALERT: {alert_text}"
    )


async def broadcast_patient_update(p: PatientState):
    if not websocket_clients:
        return

    payload = json.dumps({
        "type": "patient_update",
        "data": patient_to_dict(p),
    }, ensure_ascii=False)

    dead = []
    for ws in list(websocket_clients):
        try:
            await ws.send_text(payload)
        except Exception:
            dead.append(ws)

    for ws in dead:
        websocket_clients.discard(ws)


def schedule_broadcast(p: PatientState):
    if web_loop is not None:
        asyncio.run_coroutine_threadsafe(broadcast_patient_update(p), web_loop)


def can_reader_thread(interface: str, channel: str, bitrate: int, print_raw: bool):
    bus = can.interface.Bus(interface=interface, channel=channel, bitrate=bitrate)
    print(f"CAN reader started: interface={interface}, channel={channel}, bitrate={bitrate}")

    while True:
        try:
            msg = bus.recv(timeout=1.0)

            if msg is None:
                # Cập nhật cảnh báo offline mỗi giây + tự xóa bệnh nhân offline quá lâu
                now = time.time()

                with patients_lock:
                    cleanup_offline_patients(now)

                    for p in patients.values():
                        old_codes = [a.code for a in p.alerts]
                        update_alerts(p)
                        new_codes = [a.code for a in p.alerts]
                        if old_codes != new_codes:
                            print_patient(p)
                            # polling web sẽ tự cập nhật mỗi 1 giây
                continue

            if print_raw:
                print(
                    f"RAW ext={msg.is_extended_id} id=0x{msg.arbitration_id:X} "
                    f"dlc={msg.dlc} data={bytes(msg.data).hex(' ')}"
                )

            p = update_patient_from_can(msg)
            if p:
                print_patient(p)
                # polling web sẽ tự cập nhật mỗi 1 giây

            # Nếu vẫn còn CAN frame từ node khác thì msg không bao giờ None.
            # Vì vậy vẫn cần cleanup ở đây để xóa bệnh nhân đã offline.
            with patients_lock:
                cleanup_offline_patients(time.time())

        except KeyboardInterrupt:
            break
        except Exception as exc:
            print("CAN read error:", exc)
            time.sleep(0.5)


def run_web_server(host: str, port: int):
    import uvicorn
    from fastapi import FastAPI, Body, Query
    from fastapi.responses import HTMLResponse, JSONResponse, FileResponse

    app = FastAPI(title="Patient CAN Dashboard")

    html = r"""
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<title>Patient CAN Dashboard V4 - Priority & SOS/Fall</title>
<style>
    body {
        font-family: Arial, sans-serif;
        background: #f3f4f6;
        margin: 0;
        padding: 20px;
    }
    h1 { margin: 0 0 8px 0; }
    .topbar {
        display: flex;
        gap: 12px;
        align-items: center;
        margin-bottom: 16px;
        flex-wrap: wrap;
    }
    input, select {
        padding: 8px;
        border-radius: 8px;
        border: 1px solid #ccc;
    }
    .summary {
        display: flex;
        gap: 10px;
        flex-wrap: wrap;
        margin-bottom: 16px;
    }
    .summary-card {
        background: white;
        border-radius: 12px;
        padding: 12px 16px;
        box-shadow: 0 1px 6px #0001;
        min-width: 130px;
    }
    .grid {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(350px, 1fr));
        gap: 12px;
    }
    .card {
        background: white;
        border-radius: 14px;
        padding: 14px;
        box-shadow: 0 2px 8px #0001;
        border-left: 8px solid #22c55e;
    }
    .warning { border-left-color: #f59e0b; }
    .critical { border-left-color: #ef4444; animation: blink 1s infinite alternate; }
    @keyframes blink { from { box-shadow: 0 0 0 #ef4444; } to { box-shadow: 0 0 14px #ef444477; } }
    .title {
        font-size: 18px;
        font-weight: bold;
        margin-bottom: 8px;
    }
    .row { margin: 5px 0; }
    .muted { color: #666; font-size: 13px; }
    .alert {
        padding: 6px 8px;
        border-radius: 8px;
        margin-top: 5px;
        background: #fee2e2;
        color: #991b1b;
        font-weight: bold;
    }
    .ok {
        color: #15803d;
        font-weight: bold;
    }
    .priority-row {
        display: flex;
        gap: 8px;
        align-items: center;
        flex-wrap: wrap;
        margin: 8px 0;
    }
    .priority-badge {
        padding: 5px 9px;
        border-radius: 999px;
        font-weight: bold;
        background: #e5e7eb;
    }
    .priority-very-high { background: #fee2e2; color: #991b1b; }
    .priority-high { background: #ffedd5; color: #9a3412; }
    .priority-medium { background: #fef3c7; color: #92400e; }
    .priority-low { background: #dcfce7; color: #166534; }
    .status-pill {
        display: inline-block;
        padding: 4px 8px;
        border-radius: 999px;
        margin-right: 5px;
        background: #e5e7eb;
        font-weight: bold;
        font-size: 12px;
    }
    .status-on { background: #fee2e2; color: #991b1b; }
    .smart-box {
        background: #f8fafc;
        border: 1px dashed #cbd5e1;
        border-radius: 10px;
        padding: 8px;
        margin-top: 8px;
        font-size: 13px;
    }
    .smart-box ul {
        margin: 5px 0 0 18px;
        padding: 0;
    }
    .priority-panel {
        background: white;
        border-radius: 14px;
        padding: 14px;
        box-shadow: 0 2px 8px #0001;
        margin-bottom: 16px;
    }
    .priority-list {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
        gap: 8px;
        margin-top: 8px;
    }
    .priority-item {
        border: 1px solid #e5e7eb;
        border-radius: 10px;
        padding: 8px;
        background: #fafafa;
    }
    .handling-panel {
        background: white;
        border-radius: 14px;
        padding: 14px;
        box-shadow: 0 2px 8px #0001;
        margin-bottom: 16px;
    }
    .handling-list {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
        gap: 8px;
        margin-top: 8px;
    }
    .handling-item {
        border: 1px solid #fecaca;
        border-left: 6px solid #ef4444;
        border-radius: 10px;
        padding: 10px;
        background: #fff7f7;
    }
    .handled-item {
        border: 1px solid #bbf7d0;
        border-left: 6px solid #22c55e;
        border-radius: 10px;
        padding: 10px;
        background: #f0fdf4;
    }
    .handle-btn {
        padding: 8px 12px;
        border-radius: 9px;
        border: 1px solid #16a34a;
        background: #dcfce7;
        color: #166534;
        font-weight: bold;
        cursor: pointer;
        margin-top: 8px;
    }
    .handle-btn:hover { background: #bbf7d0; }
    .doctor-box {
        display: flex;
        gap: 8px;
        align-items: center;
        flex-wrap: wrap;
        background: white;
        border-radius: 14px;
        padding: 12px 14px;
        box-shadow: 0 2px 8px #0001;
        margin-bottom: 16px;
    }
    .doctor-box label { font-weight: bold; }
    .export-btn {
        padding: 8px 12px;
        border-radius: 9px;
        border: 1px solid #2563eb;
        background: #dbeafe;
        color: #1d4ed8;
        font-weight: bold;
        cursor: pointer;
    }
    .export-btn:hover { background: #bfdbfe; }
    .handled-badge {
        display: inline-block;
        padding: 5px 9px;
        border-radius: 999px;
        background: #dcfce7;
        color: #166534;
        font-weight: bold;
        font-size: 12px;
    }

    .chart-panel {
        background: white;
        border-radius: 14px;
        padding: 14px;
        box-shadow: 0 2px 8px #0001;
        margin: 16px 0;
    }
    .chart-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(330px, 1fr));
        gap: 12px;
    }
    .chart-box {
        border: 1px solid #e5e7eb;
        border-radius: 12px;
        padding: 10px;
    }
    .chart-box h3 {
        margin: 0 0 8px 0;
        font-size: 15px;
    }
    canvas {
        width: 100%;
        height: 210px;
        display: block;
    }

    .chart-close-btn:hover {
        filter: brightness(0.96);
    }


    .close-chart-btn {
        padding: 9px 14px;
        border-radius: 9px;
        border: 1px solid #ef4444;
        background: #fee2e2;
        color: #991b1b;
        font-weight: bold;
        cursor: pointer;
        white-space: nowrap;
    }
    .close-chart-btn:hover {
        background: #fecaca;
    }
    .danger-btn {
        padding: 9px 14px;
        border-radius: 9px;
        border: 1px solid #dc2626;
        background: #fee2e2;
        color: #991b1b;
        font-weight: bold;
        cursor: pointer;
        white-space: nowrap;
    }
    .danger-btn:hover {
        background: #fecaca;
    }

</style>
</head>
<body>
<h1>Patient CAN Dashboard V6 - SOS/Té ngã, ca trực & Excel</h1>

<div class="topbar">
    <input id="search" placeholder="Tìm bệnh nhân, phòng, dãy..." oninput="render()">
    <select id="filterLevel" onchange="render()">
        <option value="all">Tất cả</option>
        <option value="alert">Chỉ có cảnh báo</option>
        <option value="critical">Chỉ critical</option>
        <option value="warning">Chỉ warning</option>
        <option value="chart">Hiện biểu đồ</option>
    </select>
</div>

<div class="doctor-box">
    <label for="doctorName">Bác sĩ trực:</label>
    <input id="doctorName" placeholder="Nhập tên bác sĩ" style="min-width:180px;" oninput="saveShiftInfo()">
    <label for="shiftName">Ca trực:</label>
    <input id="shiftName" placeholder="ca trực" style="min-width:160px;" oninput="saveShiftInfo()">
    <button class="export-btn" onclick="exportHandledExcel()">Xuất Excel lịch sử xử lý</button>
    <button class="danger-btn" onclick="clearHandledHistory()">Xóa lịch sử đã xử lý</button>
</div>

<div class="summary">
    <div class="summary-card"><b id="total">0</b><br><span class="muted">Bệnh nhân</span></div>
    <div class="summary-card"><b id="ok">0</b><br><span class="muted">Ổn định</span></div>
    <div class="summary-card"><b id="warning">0</b><br><span class="muted">Warning</span></div>
    <div class="summary-card"><b id="critical">0</b><br><span class="muted">Critical</span></div>
    <div class="summary-card"><b id="urgent">0</b><br><span class="muted">Ưu tiên cao</span></div>
    <div class="summary-card"><b id="needHandling">0</b><br><span class="muted">Cần xử lý</span></div>
</div>

<div class="handling-panel">
    <b>Bệnh nhân đang cần xử lý</b>
    <div class="muted">Khi có cảnh báo, bệnh nhân được giữ tại đây cho đến khi bác sĩ bấm “Đã xử lý xong”.</div>
    <div id="handlingList" class="handling-list"></div>
</div>

<div class="handling-panel">
    <b>Lịch sử đã xử lý gần đây</b>
    <div class="muted">Lưu trong file <code>handled_patients_log.json</code>. Nên xuất Excel trước khi xóa; khi xóa hệ thống tự sao lưu vào thư mục <code>handled_history_backup</code>.</div>
    <div id="handledHistoryList" class="handling-list"></div>
</div>

<div class="priority-panel">
    <b>Bệnh nhân ưu tiên hỗ trợ từ cao đến thấp</b>
    <div class="muted">Sắp xếp dựa trên SOS, té ngã, HR, SpO2, dịch truyền, pin và mất kết nối.</div>
    <div id="priorityList" class="priority-list"></div>
</div>


<div id="chartPanel" class="chart-panel" style="display:none;">
    <div class="topbar" style="justify-content:space-between; align-items:center;">
        <div style="display:flex; gap:12px; align-items:center; flex-wrap:wrap;">
            <b>Biểu đồ trạng thái</b>
            <select id="patientSelect" onchange="selectedKey=this.value; renderCharts();"></select>
            <span class="muted">Lưu lịch sử tạm thời trong trình duyệt, tối đa 120 điểm gần nhất.</span>
        </div>
        <button onclick="hideCharts()" class="close-chart-btn">✕ Tắt biểu đồ</button>
    </div>
    <div class="chart-grid">
        <div class="chart-box">
            <h3>Sinh hiệu: HR / SpO2 / Temp</h3>
            <canvas id="vitalChart"></canvas>
        </div>
        <div class="chart-box">
            <h3>Dịch truyền: Volume / DPM</h3>
            <canvas id="loadcellChart"></canvas>
        </div>
        <div class="chart-box">
            <h3>Trạng thái cảnh báo: OK / Warning / Critical</h3>
            <canvas id="statusChart"></canvas>
        </div>
    </div>
</div>

<div id="grid" class="grid"></div>

<script>
const patients = {};
const history = {}; // key -> [{t, hr, spo2, temp, volume, dpm, alertLevel}]
let lastUpdateText = "-";
let selectedKey = null;
let currentDoctorName = localStorage.getItem("doctorName") || "";
let currentShiftName = localStorage.getItem("shiftName") || "";
const MAX_HISTORY_POINTS = 120; // 120 giây nếu polling 1s

function saveShiftInfo() {
    const d = document.getElementById("doctorName");
    const s = document.getElementById("shiftName");
    currentDoctorName = d ? d.value.trim() : currentDoctorName;
    currentShiftName = s ? s.value.trim() : currentShiftName;
    localStorage.setItem("doctorName", currentDoctorName);
    localStorage.setItem("shiftName", currentShiftName);
}

function loadShiftInfoToUI() {
    const d = document.getElementById("doctorName");
    const s = document.getElementById("shiftName");
    if (d) d.value = currentDoctorName;
    if (s) s.value = currentShiftName;
}

function exportHandledExcel() {
    window.location.href = "/api/export-handled-excel?ts=" + Date.now();
}

async function clearHandledHistory() {
    let doctor = currentDoctorName || prompt("Nhập tên bác sĩ trực/xác nhận xóa lịch sử:", "");
    if (doctor === null) return;
    doctor = String(doctor || "Bác sĩ/nhân viên").trim();
    currentDoctorName = doctor;
    localStorage.setItem("doctorName", currentDoctorName);
    const d = document.getElementById("doctorName");
    if (d) d.value = currentDoctorName;

    let shift = currentShiftName || prompt("Nhập ca trực/ngày trực:", "Ca sáng");
    if (shift === null) return;
    shift = String(shift || "Ca trực chưa nhập").trim();
    currentShiftName = shift;
    localStorage.setItem("shiftName", currentShiftName);
    const s = document.getElementById("shiftName");
    if (s) s.value = currentShiftName;

    const ok = confirm(
        "Bạn có chắc muốn xóa danh sách lịch sử đã xử lý đang hiển thị không?\n\n" +
        "Nên bấm Xuất Excel trước khi xóa.\n" +
        "Hệ thống vẫn tự sao lưu file JSON vào thư mục handled_history_backup."
    );
    if (!ok) return;

    const reason = prompt("Lý do xóa lịch sử:", "Sang ngày mới / bàn giao ca trực") || "Sang ngày mới / bàn giao ca trực";

    try {
        const r = await fetch("/api/clear-handled-history", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({doctor: doctor, shift: shift, reason: reason})
        });
        const data = await r.json();
        if (!r.ok || !data.ok) {
            alert("Không xóa được lịch sử: " + (data.error || JSON.stringify(data)));
            return;
        }

        alert(
            "Đã xóa " + data.deleted_count + " bản ghi lịch sử đã xử lý.\n" +
            (data.backup_file ? "Đã sao lưu: " + data.backup_file : "Không có bản ghi để sao lưu.")
        );
        refreshHandledHistory();
        refreshPatients();
    } catch (e) {
        alert("Lỗi khi xóa lịch sử: " + e);
    }
}

function showCharts() {
    const chartPanel = document.getElementById("chartPanel");
    if (chartPanel) {
        chartPanel.style.display = "block";
        setTimeout(renderCharts, 50);
        chartPanel.scrollIntoView({behavior: "smooth", block: "start"});
    }
}

function hideCharts() {
    const chartPanel = document.getElementById("chartPanel");
    if (chartPanel) {
        chartPanel.style.display = "none";
    }
    const filterEl = document.getElementById("filterLevel");
    if (filterEl && filterEl.value === "chart") {
        filterEl.value = "all";
    }
}

function activeHandlingAlerts(p) {
    if (!p || !p.handling || p.handling.status !== "active") return [];
    return p.handling.alerts || [];
}

function displayAlerts(p) {
    const normalAlerts = p.alerts || [];
    const handlingAlerts = activeHandlingAlerts(p);
    return normalAlerts.length ? normalAlerts : handlingAlerts;
}

function levelOf(p) {
    const alerts = displayAlerts(p);
    if (!alerts || alerts.length === 0) return "ok";
    if (alerts.some(a => a.level === "critical")) return "critical";
    return "warning";
}

function priorityScore(p) {
    return p.priority ? Number(p.priority.score || 0) : 0;
}

function priorityClass(level) {
    if (level === "Rất cao") return "priority-badge priority-very-high";
    if (level === "Cao") return "priority-badge priority-high";
    if (level === "Trung bình") return "priority-badge priority-medium";
    return "priority-badge priority-low";
}

function statusPill(label, on) {
    return `<span class="status-pill ${on ? "status-on" : ""}">${label}: ${on ? "1" : "0"}</span>`;
}

function esc(x) {
    return String(x ?? "").replace(/[&<>"']/g, s => ({
        "&": "&amp;",
        "<": "&lt;",
        ">": "&gt;",
        "\x22": "&quot;",
        "'": "&#39;"
    }[s]));
}

function renderPriorityList(arr) {
    const el = document.getElementById("priorityList");
    if (!el) return;

    const sorted = [...arr].sort((a, b) => {
        const d = priorityScore(b) - priorityScore(a);
        if (d !== 0) return d;
        return a.key.localeCompare(b.key);
    });

    const top = sorted.slice(0, 8);
    if (top.length === 0) {
        el.innerHTML = `<div class="muted">Chưa có bệnh nhân.</div>`;
        return;
    }

    el.innerHTML = top.map(p => {
        const pr = p.priority || {level: "Thấp", score: 0, reasons: []};
        const reasons = pr.reasons && pr.reasons.length ? pr.reasons.join(", ") : "Ổn định";
        return `
            <div class="priority-item">
                <div><b>${esc(p.key)}</b></div>
                <div class="${priorityClass(pr.level)}">${esc(pr.level)} - ${pr.score} điểm</div>
                <div class="muted">${esc(reasons)}</div>
            </div>
        `;
    }).join("");
}

function alertText(alerts) {
    if (!alerts || alerts.length === 0) return "Không có cảnh báo";
    return alerts.map(a => `${a.level?.toUpperCase?.() || "ALERT"}: ${a.message} (${JSON.stringify(a.value)})`).join("; ");
}

function normalizePatientKeyForApi(input) {
    // Tránh lỗi gửi [object Object] khi onclick/cache JS truyền nhầm object thay vì chuỗi key.
    if (input === null || input === undefined) return "";
    if (typeof input === "string") return input.trim();
    if (typeof input === "object") {
        if (typeof input.key === "string") return input.key.trim();
        if (typeof input.patient_key === "string") return input.patient_key.trim();
        if (input.dataset && typeof input.dataset.key === "string") return input.dataset.key.trim();
    }
    return String(input).trim();
}

function prettyApiError(data) {
    if (!data) return "Không có phản hồi từ server";
    if (typeof data === "string") return data;
    if (data.error) return data.error;
    if (data.detail) {
        if (typeof data.detail === "string") return data.detail;
        return JSON.stringify(data.detail, null, 2);
    }
    return JSON.stringify(data, null, 2);
}

async function markHandled(keyInput, ev) {
    if (ev) ev.stopPropagation();

    const key = normalizePatientKeyForApi(keyInput);
    if (!key || key === "[object Object]") {
        alert("Không xác định được mã bệnh nhân. Hãy bấm Ctrl+F5 để tải lại giao diện mới.");
        console.error("Invalid patient key", keyInput);
        return;
    }

    saveShiftInfo();
    let doctor = currentDoctorName || prompt("Nhập tên bác sĩ trực/xử lý:", "");
    if (doctor === null) return;
    doctor = String(doctor || "Bác sĩ/nhân viên").trim();
    currentDoctorName = doctor;
    localStorage.setItem("doctorName", currentDoctorName);
    loadShiftInfoToUI();

    let shift = currentShiftName || prompt("Nhập ca trực:", "Ca trực");
    if (shift === null) return;
    shift = String(shift || "Ca trực chưa nhập").trim();
    currentShiftName = shift;
    localStorage.setItem("shiftName", currentShiftName);
    loadShiftInfoToUI();

    const note = prompt(`Nhập ghi chú xử lý cho ${key}:`, "Đã kiểm tra và xử lý xong");
    if (note === null) return;

    try {
        const payload = {patient_key: key, note: note || "Đã xử lý xong", doctor: doctor, shift: shift};
        const r = await fetch(`/api/handle-patient?patient_key=${encodeURIComponent(key)}`, {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify(payload),
            cache: "no-store"
        });

        let data = null;
        try {
            data = await r.json();
        } catch (jsonErr) {
            alert(`Không đọc được phản hồi từ server. HTTP ${r.status}`);
            return;
        }

        if (!r.ok || !data.ok) {
            alert("Không xử lý được bệnh nhân: " + prettyApiError(data));
            console.error("handle patient failed", data);
            return;
        }

        await refreshPatients();
        await refreshHandledHistory();
        alert("Đã đánh dấu xử lý xong cho " + key);
    } catch (e) {
        alert("Lỗi gửi yêu cầu đã xử lý: " + e);
    }
}

function renderHandlingList(arr) {
    const el = document.getElementById("handlingList");
    if (!el) return;

    const active = arr
        .filter(p => p.handling && p.handling.status === "active")
        .sort((a, b) => {
            const d = priorityScore(b) - priorityScore(a);
            if (d !== 0) return d;
            return (a.handling.needs_handling_since || 0) - (b.handling.needs_handling_since || 0);
        });

    if (active.length === 0) {
        el.innerHTML = `<div class="muted">Hiện chưa có bệnh nhân cần xử lý.</div>`;
        return;
    }

    el.innerHTML = active.map(p => {
        const alerts = displayAlerts(p);
        const pr = p.priority || {level: "Thấp", score: 0, reasons: []};
        return `
            <div class="handling-item">
                <div><b>${esc(p.key)}</b></div>
                <div class="${priorityClass(pr.level)}">${esc(pr.level)} - ${pr.score} điểm</div>
                <div class="muted">Bắt đầu: ${fmtTime(p.handling.needs_handling_since)}</div>
                <div class="alert">${esc(alertText(alerts))}</div>
                <button class="handle-btn" onclick="markHandled('${esc(p.key)}', event)">✓ Đã xử lý xong</button>
            </div>
        `;
    }).join("");
}

function renderHandledHistoryList(records) {
    const el = document.getElementById("handledHistoryList");
    if (!el) return;

    const top = (records || []).slice(-8).reverse();
    if (top.length === 0) {
        el.innerHTML = `<div class="muted">Chưa có ca nào được đánh dấu đã xử lý.</div>`;
        return;
    }

    el.innerHTML = top.map(r => `
        <div class="handled-item">
            <div><b>${esc(r.patient_key)}</b> <span class="handled-badge">Đã xử lý</span></div>
            <div class="muted">Thời gian: ${fmtTime(r.handled_at)} | Người xử lý: ${esc(r.handled_by || "-")} | Ca: ${esc(r.shift || "-")}</div>
            <div>Ghi chú: ${esc(r.note || "-")}</div>
            <div class="muted">Cảnh báo: ${esc(alertText(r.alerts || []))}</div>
        </div>
    `).join("");
}

async function refreshHandledHistory() {
    try {
        const r = await fetch("/api/handled-history?ts=" + Date.now(), {cache: "no-store"});
        const data = await r.json();
        renderHandledHistoryList(data);
    } catch (e) {
        renderHandledHistoryList([]);
    }
}

function cardClass(p) {
    const level = levelOf(p);
    if (level === "critical") return "card critical";
    if (level === "warning") return "card warning";
    return "card";
}

function fmtTime(ts) {
    if (!ts) return "-";
    return new Date(ts * 1000).toLocaleTimeString();
}

function updateHistory(p) {
    if (!history[p.key]) history[p.key] = [];

    const v = p.vital;
    const l = p.loadcell;
    const point = {
        t: Date.now(),
        hr: v ? Number(v.hr) : null,
        spo2: v ? Number(v.spo2) : null,
        temp: v ? Number(v.temp) : null,
        volume: l ? Number(l.volume_ml) : null,
        dpm: l ? Number(l.drops_per_min) : null,
        alertLevel: levelOf(p),
        fall: v && v.status_flags ? Number(!!v.status_flags.fall) : 0,
        sos: v && v.status_flags ? Number(!!v.status_flags.sos) : 0,
    };

    const arr = history[p.key];
    const last = arr[arr.length - 1];

    // Chỉ thêm điểm mới khi seq thay đổi hoặc sau 1 giây
    const changed =
        !last ||
        last.hr !== point.hr ||
        last.spo2 !== point.spo2 ||
        last.temp !== point.temp ||
        last.volume !== point.volume ||
        last.dpm !== point.dpm ||
        last.fall !== point.fall ||
        last.sos !== point.sos ||
        last.alertLevel !== point.alertLevel ||
        point.t - last.t >= 1000;

    if (changed) {
        arr.push(point);
        while (arr.length > MAX_HISTORY_POINTS) arr.shift();
    }
}

function alertLevelNumber(level) {
    if (level === "critical") return 2;
    if (level === "warning") return 1;
    return 0;
}

function drawLineChart(canvas, points, fields) {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    const w = canvas.width = canvas.clientWidth * window.devicePixelRatio;
    const h = canvas.height = canvas.clientHeight * window.devicePixelRatio;
    const dpr = window.devicePixelRatio;
    ctx.scale(dpr, dpr);

    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    ctx.clearRect(0, 0, width, height);

    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, width, height);

    const padL = 42, padR = 12, padT = 18, padB = 26;
    const plotW = width - padL - padR;
    const plotH = height - padT - padB;

    ctx.strokeStyle = "#e5e7eb";
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
        const y = padT + (plotH * i / 4);
        ctx.beginPath();
        ctx.moveTo(padL, y);
        ctx.lineTo(width - padR, y);
        ctx.stroke();
    }

    if (!points || points.length < 2) {
        ctx.fillStyle = "#6b7280";
        ctx.font = "14px Arial";
        ctx.fillText("Chưa đủ dữ liệu để vẽ biểu đồ", padL, height / 2);
        return;
    }

    let values = [];
    fields.forEach(f => {
        points.forEach(p => {
            if (p[f.key] !== null && p[f.key] !== undefined && !Number.isNaN(p[f.key])) {
                // Bỏ qua giá trị 0 cho HR/SpO2/temp nếu đó là chưa đo được, nhưng vẫn giữ volume/dpm
                if ((f.key === "hr" || f.key === "spo2" || f.key === "temp") && p[f.key] === 0) return;
                values.push(p[f.key]);
            }
        });
    });

    if (values.length === 0) {
        ctx.fillStyle = "#6b7280";
        ctx.font = "14px Arial";
        ctx.fillText("Chưa có giá trị hợp lệ", padL, height / 2);
        return;
    }

    let minV = Math.min(...values);
    let maxV = Math.max(...values);
    if (minV === maxV) {
        minV -= 1;
        maxV += 1;
    }
    const margin = (maxV - minV) * 0.15;
    minV -= margin;
    maxV += margin;

    const t0 = points[0].t;
    const t1 = points[points.length - 1].t || (t0 + 1);

    function xOf(t) {
        return padL + ((t - t0) / Math.max(1, t1 - t0)) * plotW;
    }

    function yOf(v) {
        return padT + (1 - ((v - minV) / (maxV - minV))) * plotH;
    }

    ctx.fillStyle = "#6b7280";
    ctx.font = "11px Arial";
    ctx.fillText(maxV.toFixed(0), 4, padT + 4);
    ctx.fillText(minV.toFixed(0), 4, padT + plotH);

    const palette = ["#2563eb", "#dc2626", "#16a34a", "#9333ea", "#ea580c"];
    fields.forEach((f, idx) => {
        ctx.strokeStyle = palette[idx % palette.length];
        ctx.lineWidth = 2;
        ctx.beginPath();

        let started = false;
        points.forEach(p => {
            let v = p[f.key];
            if (v === null || v === undefined || Number.isNaN(v)) return;
            if ((f.key === "hr" || f.key === "spo2" || f.key === "temp") && v === 0) return;

            const x = xOf(p.t);
            const y = yOf(v);

            if (!started) {
                ctx.moveTo(x, y);
                started = true;
            } else {
                ctx.lineTo(x, y);
            }
        });

        if (started) ctx.stroke();

        ctx.fillStyle = palette[idx % palette.length];
        ctx.font = "12px Arial";
        ctx.fillText(f.label, padL + idx * 92, 13);
    });

    ctx.fillStyle = "#6b7280";
    ctx.font = "11px Arial";
    ctx.fillText(new Date(t0).toLocaleTimeString(), padL, height - 6);
    ctx.fillText(new Date(t1).toLocaleTimeString(), width - 82, height - 6);
}

function drawStatusChart(canvas, points) {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    const w = canvas.width = canvas.clientWidth * window.devicePixelRatio;
    const h = canvas.height = canvas.clientHeight * window.devicePixelRatio;
    const dpr = window.devicePixelRatio;
    ctx.scale(dpr, dpr);

    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, width, height);

    const padL = 42, padR = 12, padT = 18, padB = 26;
    const plotW = width - padL - padR;
    const plotH = height - padT - padB;

    ctx.strokeStyle = "#e5e7eb";
    ctx.lineWidth = 1;
    for (let i = 0; i <= 2; i++) {
        const y = padT + plotH - (plotH * i / 2);
        ctx.beginPath();
        ctx.moveTo(padL, y);
        ctx.lineTo(width - padR, y);
        ctx.stroke();
        ctx.fillStyle = "#6b7280";
        ctx.font = "11px Arial";
        const label = i === 0 ? "OK" : (i === 1 ? "WARN" : "CRIT");
        ctx.fillText(label, 4, y + 4);
    }

    if (!points || points.length < 2) {
        ctx.fillStyle = "#6b7280";
        ctx.font = "14px Arial";
        ctx.fillText("Chưa đủ dữ liệu trạng thái", padL, height / 2);
        return;
    }

    const t0 = points[0].t;
    const t1 = points[points.length - 1].t || (t0 + 1);

    function xOf(t) {
        return padL + ((t - t0) / Math.max(1, t1 - t0)) * plotW;
    }

    function yOfLevel(level) {
        const n = alertLevelNumber(level);
        return padT + plotH - (n / 2) * plotH;
    }

    ctx.strokeStyle = "#111827";
    ctx.lineWidth = 2;
    ctx.beginPath();
    points.forEach((p, idx) => {
        const x = xOf(p.t);
        const y = yOfLevel(p.alertLevel);
        if (idx === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
    });
    ctx.stroke();

    points.forEach(p => {
        const x = xOf(p.t);
        const y = yOfLevel(p.alertLevel);
        ctx.fillStyle = p.alertLevel === "critical" ? "#ef4444" : (p.alertLevel === "warning" ? "#f59e0b" : "#22c55e");
        ctx.beginPath();
        ctx.arc(x, y, 3, 0, Math.PI * 2);
        ctx.fill();
    });

    ctx.fillStyle = "#6b7280";
    ctx.font = "11px Arial";
    ctx.fillText(new Date(t0).toLocaleTimeString(), padL, height - 6);
    ctx.fillText(new Date(t1).toLocaleTimeString(), width - 82, height - 6);
}

function renderCharts() {
    const select = document.getElementById("patientSelect");
    if (!select) return;

    const keys = Object.keys(patients).sort();
    const old = selectedKey;
    select.innerHTML = keys.map(k => `<option value="${k}">${k}</option>`).join("");

    if (old && keys.includes(old)) selectedKey = old;
    if (!selectedKey && keys.length > 0) selectedKey = keys[0];
    if (selectedKey) select.value = selectedKey;

    const points = selectedKey ? (history[selectedKey] || []) : [];
    drawLineChart(document.getElementById("vitalChart"), points, [
        {key: "hr", label: "HR"},
        {key: "spo2", label: "SpO2"},
        {key: "temp", label: "Temp"},
    ]);
    drawLineChart(document.getElementById("loadcellChart"), points, [
        {key: "volume", label: "Volume"},
        {key: "dpm", label: "DPM"},
    ]);
    drawStatusChart(document.getElementById("statusChart"), points);
}

function render() {
    const grid = document.getElementById("grid");
    const q = document.getElementById("search").value.toLowerCase();
    const filterEl = document.getElementById("filterLevel");
    let filter = filterEl.value;

    if (filter === "chart") {
        showCharts();
        filter = "all";
        filterEl.value = "all";
    }

    let arr = Object.values(patients);

    let total = arr.length;
    let ok = arr.filter(p => levelOf(p) === "ok").length;
    let warning = arr.filter(p => levelOf(p) === "warning").length;
    let critical = arr.filter(p => levelOf(p) === "critical").length;
    let urgent = arr.filter(p => priorityScore(p) >= 80).length;
    let needHandling = arr.filter(p => p.handling && p.handling.status === "active").length;

    document.getElementById("total").textContent = total;
    document.getElementById("ok").textContent = ok;
    document.getElementById("warning").textContent = warning;
    document.getElementById("critical").textContent = critical;
    document.getElementById("urgent").textContent = urgent;
    document.getElementById("needHandling").textContent = needHandling;

    renderHandlingList(arr);
    renderPriorityList(arr);

    let statusEl = document.getElementById("connStatus");
    if (!statusEl) {
        statusEl = document.createElement("div");
        statusEl.id = "connStatus";
        statusEl.className = "muted";
        document.body.insertBefore(statusEl, document.getElementById("grid"));
    }
    statusEl.textContent = `Chế độ cập nhật: Polling mỗi 1 giây | Cập nhật cuối: ${lastUpdateText}`;

    arr = arr.filter(p => {
        const text = `${p.key} ${p.wing} ${p.room} ${p.bed} ${p.patient}`.toLowerCase();
        if (q && !text.includes(q)) return false;

        const level = levelOf(p);
        if (filter === "alert" && level === "ok") return false;
        if (filter === "critical" && level !== "critical") return false;
        if (filter === "warning" && level !== "warning") return false;

        return true;
    });

    arr.sort((a, b) => {
        const d = priorityScore(b) - priorityScore(a);
        if (d !== 0) return d;
        return a.key.localeCompare(b.key);
    });

    grid.innerHTML = "";
    arr.forEach(p => {
        const v = p.vital;
        const l = p.loadcell;
        const alerts = displayAlerts(p);
        const handling = p.handling || {status: "normal"};

        const div = document.createElement("div");
        div.className = cardClass(p);
        div.onclick = () => {
            selectedKey = p.key;
            renderCharts();
            window.scrollTo({top: 0, behavior: "smooth"});
        };

        const vf = v && v.status_flags ? v.status_flags : {};
        const lf = l && l.status_flags ? l.status_flags : {};
        const pr = p.priority || {level: "Thấp", score: 0, reasons: []};
        const smart = p.smart || {notes: [], actions: [], infusion_thresholds: {}};
        const th = l && l.dynamic_thresholds ? l.dynamic_thresholds : (smart.infusion_thresholds || {});
        const statusHtml = v ? `
            ${statusPill("Té ngã", !!vf.fall)}
            ${statusPill("SOS", !!vf.sos)}
            ${statusPill("HR bất thường", !!vf.hr_abnormal)}
            ${statusPill("SpO2 thấp", !!vf.spo2_low)}
        ` : "-";

        const smartNotes = (smart.notes || []).slice(0, 3).map(x => `<li>${esc(x)}</li>`).join("");
        const smartActions = (smart.actions || []).slice(0, 3).map(x => `<li>${esc(x)}</li>`).join("");

        div.innerHTML = `
            <div class="title">${esc(p.key)}</div>

            <div class="priority-row">
                <span class="${priorityClass(pr.level)}">Ưu tiên: ${esc(pr.level)} - ${pr.score} điểm</span>
                <span class="muted">${pr.reasons && pr.reasons.length ? esc(pr.reasons.join(", ")) : "Không có lý do ưu tiên"}</span>
            </div>

            <div class="row"><b>VITAL:</b> ${
                v ? `HR ${v.hr} | SpO2 ${v.spo2}% | T ${Number(v.temp).toFixed(1)}°C | Pin ${v.battery}% | Seq ${v.seq}` : "-"
            }</div>

            <div class="row"><b>Data mới:</b> ${statusHtml}</div>

            <div class="row"><b>LOADCELL:</b> ${
                l ? `${l.volume_ml} ml | ${l.drops_per_min} giọt/phút | ${l.infusion.flow_ml_per_min} ml/phút | ${l.infusion.remaining_text} | Seq ${l.seq}` : "-"
            }</div>

            <div class="row"><b>Ngưỡng dịch:</b> ${
                l ? `${th.mode === "dynamic" ? "Động theo bệnh nhân" : "Tạm dùng mặc định"} | thấp≤${th.low_warning ?? "-"} | cao≥${th.high_warning ?? "-"} | rất cao≥${th.high_critical ?? "-"} | baseline=${th.baseline_dpm ?? "-"} DPM` : "-"
            }</div>

            <div class="smart-box">
                <div><b>Nhận xét:</b><ul>${smartNotes || "<li>Chưa có nhận xét.</li>"}</ul></div>
                <div><b>Đề xuất:</b><ul>${smartActions || "<li>Tiếp tục theo dõi.</li>"}</ul></div>
            </div>

            <div class="row"><b>Trạng thái xử lý:</b> ${
                handling.status === "active"
                    ? `<span class="alert">Đang cần xử lý từ ${fmtTime(handling.needs_handling_since)}</span><br><button class="handle-btn" onclick="markHandled('${esc(p.key)}', event)">✓ Đã xử lý xong</button>`
                    : (handling.status === "handled"
                        ? `<span class="handled-badge">Đã xử lý xong ${handling.handled_at ? "lúc " + fmtTime(handling.handled_at) : ""}</span>`
                        : `<span class="ok">Bình thường</span>`)
            }</div>

            <div class="row"><b>Cảnh báo:</b> ${
                alerts.length === 0
                    ? `<span class="ok">OK</span>`
                    : alerts.map(a => `<div class="alert">${esc(a.level.toUpperCase())} - ${esc(a.message)}: ${esc(JSON.stringify(a.value))}</div>`).join("")
            }</div>
            <div class="muted">Last seen: ${fmtTime(p.last_seen)} | Bấm card để xem biểu đồ</div>
        `;

        grid.appendChild(div);
    });

    renderCharts();
}

async function refreshPatients() {
    try {
        const r = await fetch("/api/patients?ts=" + Date.now(), {cache: "no-store"});
        const data = await r.json();

        Object.keys(patients).forEach(k => delete patients[k]);

        data.forEach(p => {
            patients[p.key] = p;
            updateHistory(p);
        });

        lastUpdateText = new Date().toLocaleTimeString();
        render();
    } catch (e) {
        lastUpdateText = "Lỗi tải /api/patients";
        render();
    }
}

window.addEventListener("resize", renderCharts);

setInterval(refreshPatients, 1000);
setInterval(refreshHandledHistory, 5000);
refreshPatients();
refreshHandledHistory();
</script>
</body>
</html>
"""

    @app.get("/")
    async def index():
        return HTMLResponse(html)

    @app.get("/api/patients")
    async def api_patients():
        with patients_lock:
            data = [patient_to_dict(p) for p in patients.values()]
            data.sort(key=lambda x: (-x["priority"]["score"], x["key"]))
            return JSONResponse(data)

    @app.post("/api/handle-patient")
    async def api_handle_patient(
        body: Any = Body(default=None),
        patient_key: str = Query(default=""),
        key: str = Query(default=""),
        note: str = Query(default="Đã xử lý xong"),
        doctor: str = Query(default="Bác sĩ/nhân viên"),
        shift: str = Query(default="Ca trực chưa nhập"),
    ):
        """
        Endpoint xử lý bệnh nhân bản V4.
        Không dùng tham số request bắt buộc nữa nên sẽ không còn lỗi 422:
        {"loc":["query","request"],"msg":"Field required"}.

        Nhận được cả 2 kiểu:
        - JSON body: {"patient_key":"B-R001-B002-P003", "note":"..."}
        - Query: /api/handle-patient?patient_key=B-R001-B002-P003
        """
        body_data = {}

        if isinstance(body, dict):
            body_data = body
        elif isinstance(body, str):
            body_data = {"patient_key": body}
        elif body is not None:
            body_data = {"patient_key": str(body)}

        final_key = str(
            body_data.get("patient_key")
            or body_data.get("key")
            or patient_key
            or key
            or ""
        ).strip()

        final_note = str(body_data.get("note") or note or "Đã xử lý xong")
        final_doctor = str(body_data.get("doctor") or doctor or "Bác sĩ/nhân viên")
        final_shift = str(body_data.get("shift") or shift or "Ca trực chưa nhập")

        if final_key in ("", "[object Object]", "None", "null"):
            return JSONResponse({
                "ok": False,
                "error": "Thiếu mã bệnh nhân patient_key. Hãy bấm Ctrl+F5 và chạy đúng file patient_can_web_dashboard_handling_v5.py",
                "received_body": body_data,
                "received_query_patient_key": patient_key,
                "received_query_key": key,
            }, status_code=400)

        result = mark_patient_handled(final_key, note=final_note, doctor=final_doctor, shift=final_shift)
        status_code = 200 if result.get("ok") else 404
        return JSONResponse(result, status_code=status_code)

    @app.get("/api/handle-patient-test")
    async def api_handle_patient_test(patient_key: str = ""):
        # Test nhanh trên trình duyệt: /api/handle-patient-test?patient_key=B-R001-B002-P003
        result = mark_patient_handled(patient_key, note="Test xử lý từ trình duyệt", doctor="Test", shift="Test")
        return JSONResponse(result)

    @app.post("/api/patients/{patient_key}/handled")
    async def api_patient_handled(
        patient_key: str,
        body: Any = Body(default=None),
    ):
        body_data = body if isinstance(body, dict) else {}
        note = str(body_data.get("note", "Đã xử lý xong"))
        doctor = str(body_data.get("doctor", "Bác sĩ/nhân viên"))
        shift = str(body_data.get("shift", "Ca trực chưa nhập"))
        result = mark_patient_handled(patient_key, note=note, doctor=doctor, shift=shift)
        status_code = 200 if result.get("ok") else 404
        return JSONResponse(result, status_code=status_code)

    @app.get("/api/handling/active")
    async def api_handling_active():
        with patients_lock:
            data = [patient_to_dict(p) for p in patients.values() if p.handling_status == "active"]
            data.sort(key=lambda x: (-x["priority"]["score"], x["key"]))
            return JSONResponse(data)

    @app.get("/api/handled-history")
    async def api_handled_history():
        with patients_lock:
            return JSONResponse(handled_history[-MAX_HANDLED_HISTORY:])

    @app.post("/api/clear-handled-history")
    async def api_clear_handled_history(body: Optional[dict] = Body(default=None)):
        try:
            body_data = body if isinstance(body, dict) else {}
            doctor = str(body_data.get("doctor") or "Bác sĩ/nhân viên")
            shift = str(body_data.get("shift") or "Ca trực chưa nhập")
            reason = str(body_data.get("reason") or "Sang ngày mới / bàn giao ca trực")

            with patients_lock:
                result = clear_handled_history(doctor=doctor, shift=shift, reason=reason)

            return JSONResponse(result)
        except Exception as exc:
            return JSONResponse({"ok": False, "error": str(exc)}, status_code=500)

    @app.get("/api/export-handled-excel")
    async def api_export_handled_excel():
        try:
            path = export_handled_history_excel(HANDLED_EXCEL_FILE)
            filename = f"handled_patients_{time.strftime('%Y%m%d_%H%M%S')}.xlsx"
            return FileResponse(
                path=str(path),
                filename=filename,
                media_type="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            )
        except ImportError:
            return JSONResponse({
                "ok": False,
                "error": "Chưa cài openpyxl. Hãy chạy: pip install openpyxl"
            }, status_code=500)
        except Exception as exc:
            return JSONResponse({"ok": False, "error": str(exc)}, status_code=500)

    @app.get("/api/thresholds")
    async def api_thresholds():
        return JSONResponse({
            "HR_LOW_CRITICAL": HR_LOW_CRITICAL,
            "HR_LOW_WARNING": HR_LOW_WARNING,
            "HR_HIGH_WARNING": HR_HIGH_WARNING,
            "HR_HIGH_CRITICAL": HR_HIGH_CRITICAL,
            "SPO2_WARNING": SPO2_WARNING,
            "SPO2_CRITICAL": SPO2_CRITICAL,
            "TEMP_WARNING": TEMP_WARNING,
            "TEMP_CRITICAL": TEMP_CRITICAL,
            "BATTERY_WARNING": BATTERY_WARNING,
            "BATTERY_CRITICAL": BATTERY_CRITICAL,
            "INFUSION_EMPTY_WARNING_ML": INFUSION_EMPTY_WARNING_ML,
            "INFUSION_EMPTY_CRITICAL_ML": INFUSION_EMPTY_CRITICAL_ML,
            "DROPS_LOW_WARNING": DROPS_LOW_WARNING,
            "DROPS_HIGH_WARNING": DROPS_HIGH_WARNING,
            "DROPS_HIGH_CRITICAL": DROPS_HIGH_CRITICAL,
            "OFFLINE_WARNING_SEC": OFFLINE_WARNING_SEC,
            "OFFLINE_CRITICAL_SEC": OFFLINE_CRITICAL_SEC,
            "AUTO_REMOVE_PATIENT_SEC": AUTO_REMOVE_PATIENT_SEC,
            "INFUSION_STOP_CONFIRM_SEC": INFUSION_STOP_CONFIRM_SEC,
            "STATUS_FALL_BIT": STATUS_FALL,
            "STATUS_SOS_BIT": STATUS_SOS,
            "DPM_BASELINE_MIN_SAMPLES": DPM_BASELINE_MIN_SAMPLES,
            "DPM_DYNAMIC_LOW_RATIO": DPM_DYNAMIC_LOW_RATIO,
            "DPM_DYNAMIC_HIGH_RATIO": DPM_DYNAMIC_HIGH_RATIO,
            "DPM_DYNAMIC_CRITICAL_RATIO": DPM_DYNAMIC_CRITICAL_RATIO,
        })

    global web_loop
    web_loop = asyncio.new_event_loop()
    asyncio.set_event_loop(web_loop)

    config = uvicorn.Config(app, host=host, port=port, log_level="info", loop="asyncio")
    server = uvicorn.Server(config)
    web_loop.run_until_complete(server.serve())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="slcan", help="Windows CANable thường dùng slcan")
    parser.add_argument("--channel", default="COM27", help="Ví dụ COM27")
    parser.add_argument("--bitrate", type=int, default=125000)
    parser.add_argument("--print-raw", action="store_true")
    parser.add_argument("--web", action="store_true")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()

    load_handled_history()

    t = threading.Thread(
        target=can_reader_thread,
        args=(args.interface, args.channel, args.bitrate, args.print_raw),
        daemon=True,
    )
    t.start()

    if args.web:
        print(f"Web dashboard V6: http://{args.host}:{args.port}")
        run_web_server(args.host, args.port)
    else:
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("Exit")


if __name__ == "__main__":
    main()

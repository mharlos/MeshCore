"""Farm Radio Fleet CHIRP bridge.

Uses model-specific CHIRP drivers vendored from CHIRP (GPL). Only the exact
driver matching the operator-selected radio model is instantiated. Full EEPROM
is read before any memory update. Never transfer raw images across models.
"""
from __future__ import annotations

import base64
import hashlib
import importlib
import json
import math
from java import jclass

# Build workflow copies the GPL CHIRP package here from RadioDroid's vendored
# upstream CHIRP snapshot.  We call existing driver sync_in/sync_out methods.
for driver in ("uv5r", "baofeng_uv17Pro"):
    importlib.import_module("chirp.drivers." + driver)

from chirp import chirp_common, directory
RadioIo = jclass("com.mharlos.radiofleet.RadioIo")

MODELS = {
    "UV-5R": ("Baofeng", "UV-5R"),
    "UV-5RM": ("Baofeng", "5RM"),
}

class AndroidPipe:
    """Simple serial-like object implemented with synchronous Kotlin BLE I/O."""
    def __init__(self):
        self.timeout = 3.0
        self.baudrate = 9600
        self.parity = "N"
        self.rts = False
        self.dtr = False
        self.is_open = True

    def read(self, size=1):
        raw = RadioIo.read(int(size), max(1, int(float(self.timeout or 1) * 1000)))
        return bytes((int(item) & 255 for item in raw))

    def write(self, data):
        payload = bytes(data)
        RadioIo.write(payload)
        return len(payload)

    def flush(self):
        return None

    def flushInput(self):
        RadioIo.flushInput()

    def reset_input_buffer(self):
        RadioIo.flushInput()

    def setRTS(self, value):
        self.rts = value

    def setDTR(self, value):
        self.dtr = value

    def close(self):
        self.is_open = False

def _class(model):
    if model not in MODELS:
        raise ValueError("Unknown model: " + str(model))
    vendor, name = MODELS[model]
    for candidate in directory.DRV_TO_RADIO.values():
        if candidate.VENDOR == vendor and candidate.MODEL == name:
            if not issubclass(candidate, chirp_common.CloneModeRadio):
                raise ValueError("Expected a CHIRP clone-mode driver")
            return candidate
    raise ValueError("Bundled CHIRP driver missing: " + vendor + " " + name)

def _sync_in(model):
    cls = _class(model)
    radio = cls(None)
    radio.pipe = AndroidPipe()
    radio.pipe.timeout = 5.0
    try:
        radio.sync_in()
        return radio
    except BaseException:
        radio.pipe.close()
        raise

def _image(radio):
    mmap = radio.get_mmap()
    raw = mmap.get_byte_compatible().get_packed() if hasattr(mmap, "get_byte_compatible") else mmap.get_packed()
    return bytes(raw)

def _channels(radio):
    features = radio.get_features()
    lo, hi = features.memory_bounds
    result = []
    for slot in range(lo, hi + 1):
        try:
            memory = radio.get_memory(slot)
            if memory.empty:
                continue
            freq = int(memory.freq)
            duplex = memory.duplex or ""
            if duplex == "off":
                tx = None
            elif duplex == "split":
                tx = int(memory.offset)
            elif duplex == "+":
                tx = freq + int(memory.offset)
            elif duplex == "-":
                tx = freq - int(memory.offset)
            else:
                tx = freq
            tone = float(memory.rtone) if memory.tmode in ("Tone", "TSQL") else None
            receive_tone = float(memory.ctone) if memory.tmode == "TSQL" else None
            result.append({
                "slot": slot,
                "name": memory.name or "",
                "receive": round(freq / 1e6, 6),
                "transmit": None if tx is None else round(tx / 1e6, 6),
                "txCtcss": tone,
                "rxCtcss": receive_tone,
                "narrow": memory.mode == "NFM",
                "power": str(memory.power or "High"),
            })
        except Exception as error:
            raise RuntimeError("Failed reading memory slot %d: %s" % (slot, error)) from error
    return result

def read_radio(model):
    radio = _sync_in(model)
    try:
        raw = _image(radio)
        return json.dumps({
            "model": model,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "image_base64": base64.b64encode(raw).decode("ascii"),
            "channels": _channels(radio),
        })
    finally:
        radio.pipe.close()

def _apply(radio, item):
    slot = int(item["slot"])
    features = radio.get_features()
    lo, hi = features.memory_bounds
    if not (lo <= slot <= hi):
        raise ValueError("Memory %d not available on this model" % slot)
    memory = radio.get_memory(slot)
    memory.empty = False
    memory.number = slot
    receive = int(round(float(item["receive"]) * 1e6))
    transmit = item.get("transmit")
    if transmit is None:
        memory.duplex = "off"
        memory.offset = 0
    else:
        transmit = int(round(float(transmit) * 1e6))
        if transmit == receive:
            memory.duplex = ""
            memory.offset = 0
        elif transmit < receive:
            memory.duplex = "-"
            memory.offset = receive - transmit
        else:
            memory.duplex = "+"
            memory.offset = transmit - receive
    memory.freq = receive
    memory.name = str(item.get("name", ""))[:features.valid_name_length]
    memory.mode = "NFM" if item.get("narrow", False) else "FM"
    tx_tone = item.get("txCtcss")
    rx_tone = item.get("rxCtcss")
    if rx_tone is not None:
        memory.tmode = "TSQL" if tx_tone is None or abs(float(tx_tone) - float(rx_tone)) < 0.05 else "Cross"
        if memory.tmode == "Cross":
            memory.cross_mode = "Tone->Tone"
        memory.rtone = float(tx_tone or rx_tone)
        memory.ctone = float(rx_tone)
    elif tx_tone is not None:
        memory.tmode = "Tone"
        memory.rtone = float(tx_tone)
        memory.ctone = 88.5
    else:
        memory.tmode = ""
        memory.rtone = 88.5
        memory.ctone = 88.5
    levels = getattr(features, "valid_power_levels", None) or []
    if levels:
        requested = str(item.get("power", "High")).lower()
        memory.power = next((level for level in levels if requested in str(level).lower()), levels[0])
    issues = radio.validate_memory(memory)
    errors = [str(x) for x in issues if isinstance(x, chirp_common.ValidationError)]
    if errors:
        raise ValueError("Memory %d: %s" % (slot, "; ".join(errors)))
    radio.set_memory(memory)

def write_radio(model, expected_image_sha256, channel_json):
    """Re-read, check baseline, apply only requested channels, and write via driver.

    Android persists a backup from read_radio BEFORE exposing Write.
    The model and SHA guard prevent accidental use of a different model or image.
    The app must perform an additional fresh read-back after this returns.
    """
    profile = json.loads(channel_json)
    if not isinstance(profile, list) or not profile:
        raise ValueError("No memories selected")
    if len({int(x["slot"]) for x in profile}) != len(profile):
        raise ValueError("Duplicate memory locations")
    radio = _sync_in(model)
    try:
        original = _image(radio)
        baseline_sha = hashlib.sha256(original).hexdigest()
        if baseline_sha != expected_image_sha256:
            raise ValueError(
                "Radio memory changed since the backup. Read again before writing."
            )
        for item in profile:
            _apply(radio, item)
        radio.sync_out()
        return json.dumps({
            "written": len(profile),
            "model": model,
            "baseline_sha256": baseline_sha,
            "readback_required": True,
        })
    finally:
        radio.pipe.close()

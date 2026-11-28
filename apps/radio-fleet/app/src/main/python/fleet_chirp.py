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
import logging
import traceback
import time
from java import jclass

# Build workflow copies the GPL CHIRP package here from RadioDroid's vendored
# upstream CHIRP snapshot.  We call existing driver sync_in/sync_out methods.
for driver in ("uv5r", "baofeng_uv17Pro"):
    importlib.import_module("chirp.drivers." + driver)

from chirp import chirp_common, directory
RadioIo = jclass("com.mharlos.radiofleet.RadioIo")
LOG = logging.getLogger("radiofleet.chirp")

# UV-5RM clone driver has no explicit exit command after download.
# Allow the handset to settle before a new identification handshake.
SESSION_SETTLE_SECONDS = 3.0
IDENT_RETRY_SECONDS = 5.0
IDENT_FAILURE = "Radio did not respond as expected (A)"


def _settle_session(seconds):
    time.sleep(seconds)
    RadioIo.flushInput()


def _ident_failure(error):
    # Exact pinned driver's pre-block identification error only.
    return str(error) == IDENT_FAILURE


class _DriverErrors(logging.Handler):
    """Only collect errors and stack traces; never channel/EERPOM contents."""
    def __init__(self):
        super().__init__(level=logging.ERROR)
        self.messages = []
    def emit(self, record):
        try:
            message = record.getMessage()
            if record.exc_info:
                pieces = traceback.format_exception(*record.exc_info)
                # Include error type and last meaningful Python frame.
                message += " | " + "".join(pieces)[-1500:]
            self.messages.append(message[-1800:])
        except Exception:
            pass


MODELS = {
    "UV-5R": ("Baofeng", "UV-5R"),
    "UV-5RM": ("Baofeng", "5RM"),
}

class AndroidPipe:
    """Simple serial-like object implemented with synchronous Kotlin BLE I/O."""
    def __init__(self):
        self.timeout = 3.0
        self.baudrate = 9600
        self.read_calls = 0
        self.write_calls = 0
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.last_io = "initialized"
        self.recent_io = []
        self.ident_frames = ()
        self.ident_pending = b""
        self.ident_batches = 0
        self.parity = "N"
        self.rts = False
        self.dtr = False
        self.is_open = True

    def read(self, size=1):
        self._flush_ident()
        self.read_calls += 1
        requested = int(size)
        timeout_ms = max(1, int(float(self.timeout or 1) * 1000))
        raw = RadioIo.read(requested, timeout_ms)
        result = bytes((int(item) & 255 for item in raw))
        self.rx_bytes += len(result)
        self.last_io = "read %d/%d bytes, timeout %d ms" % (
            len(result), requested, timeout_ms)
        self._record(self.last_io)
        return result

    def _record(self, message):
        self.recent_io.append(message)
        del self.recent_io[:-12]

    def configure_uv5r_ident(self, radio):
        # Match only the pinned driver's known identification sequences.
        # Do not combine arbitrary ACKs, data frames or other radio models.
        driver = importlib.import_module("chirp.drivers.uv5r")
        self.ident_frames = tuple(bytes(x) for x in
                                  list(radio._idents) + list(driver.IDENT_BLACKLIST))

    def _flush_ident(self):
        if self.ident_pending:
            payload = self.ident_pending
            self.ident_pending = b""
            self._send(payload)

    def write(self, data):
        payload = bytes(data)
        if self.ident_frames and len(payload) == 1:
            candidate = self.ident_pending + payload
            if candidate in self.ident_frames:
                self.ident_pending = b""
                self._send(candidate)
                self.ident_batches += 1
                return len(payload)
            if any(frame.startswith(candidate) for frame in self.ident_frames):
                self.ident_pending = candidate
                return len(payload)
        self._flush_ident()
        if self.ident_frames and len(payload) == 1 and any(
                frame.startswith(payload) for frame in self.ident_frames):
            self.ident_pending = payload
            return len(payload)
        return self._send(payload)

    def _send(self, payload):
        self.write_calls += 1
        self.last_io = "writing %d bytes" % len(payload)
        RadioIo.write(payload)
        self.tx_bytes += len(payload)
        self.last_io = "wrote %d bytes" % len(payload)
        self._record(self.last_io)
        return len(payload)

    def log(self, message, *args):
        """CHIRP Baofeng driver calls pipe.log() when cloning blocks.

        Missing method was raising AttributeError, then masked by CHIRP as
        'Unexpected error communicating with the radio'.
        """
        LOG.debug("%s", message)

    def diagnostic(self):
        return ("baud=%d tx=%d bytes (%d calls) rx=%d bytes (%d calls); last=%s" %
                (self.baudrate, self.tx_bytes, self.write_calls,
                 self.rx_bytes, self.read_calls, self.last_io)) + " | UART baud unverified; ident batches=%d | recent: " % self.ident_batches + "; ".join(self.recent_io)

    def flush(self):
        self._flush_ident()

    def flushInput(self):
        RadioIo.flushInput()

    def reset_input_buffer(self):
        RadioIo.flushInput()

    def setRTS(self, value):
        self.rts = value

    def setDTR(self, value):
        self.dtr = value

    def close(self):
        self.ident_pending = b""
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

def _sync_in(model, ident_retry=False):
    try:
        return _sync_in_once(model)
    except RuntimeError as error:
        if not (ident_retry and model == "UV-5RM" and
                _ident_failure(error.__cause__)):
            raise
        _settle_session(IDENT_RETRY_SECONDS)
        return _sync_in_once(model)


def _sync_in_once(model):
    cls = _class(model)
    radio = cls(None)
    radio.pipe = AndroidPipe()
    # UV-5RM CHIRP driver requests 115200; UV-5R requests 9600.
    # This sets the expected UART speed in diagnostics. The TD-BL1 adapter
    # may also require a separate OEM baud-control command; do not assume
    # that changing a Python property switches the hardware serial clock.
    radio.pipe.baudrate = int(getattr(cls, "BAUD_RATE", 9600))
    radio.pipe.timeout = 5.0
    if model == "UV-5R":
        radio.pipe.configure_uv5r_ident(radio)
    capture = _DriverErrors()
    driver_logger = logging.getLogger("chirp.drivers.baofeng_uv17Pro")
    driver_logger.addHandler(capture)
    try:
        radio.sync_in()
        return radio
    except Exception as error:
        diag = radio.pipe.diagnostic()
        underlying = capture.messages[-1] if capture.messages else ""
        # Show the genuine exception that CHIRP masks during sync_in.
        # Redact potentially sensitive binary payloads by never logging bytes.
        underlying = underlying.replace("\\x", "0x")
        message = ("%s: %s | %s" % (
            type(error).__name__, str(error), diag))
        if underlying:
            message += " | driver: " + underlying[-1200:]
        radio.pipe.close()
        raise RuntimeError(message[:2000]) from error
    finally:
        driver_logger.removeHandler(capture)

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
            tx_spec, rx_spec = chirp_common.split_tone_encode(memory)
            result.append({
                "slot": slot,
                "name": memory.name or "",
                "receive": round(freq / 1e6, 6),
                "transmit": None if tx is None else round(tx / 1e6, 6),
                "txCtcss": tx_spec[1] if tx_spec[0] == "Tone" else None,
                "rxCtcss": rx_spec[1] if rx_spec[0] == "Tone" else None,
                "txDcs": tx_spec[1] if tx_spec[0] == "DTCS" else None,
                "rxDcs": rx_spec[1] if rx_spec[0] == "DTCS" else None,
                "dcsPolarity": (tx_spec[2] or "N") + (rx_spec[2] or "N"),
                "mode": memory.mode,
                "narrow": memory.mode == "NFM",
                "power": str(memory.power or "High"),
            })
        except Exception as error:
            raise RuntimeError("Failed reading memory slot %d: %s" % (slot, error)) from error
    return result

def read_radio(model, after_upload=False):
    if after_upload:
        _settle_session(SESSION_SETTLE_SECONDS)
    radio = _sync_in(model, ident_retry=bool(after_upload))
    try:
        raw = _image(radio)
        try:
            settings = _settings_snapshot(radio)
            settings_error = ""
        except Exception as error:
            settings = []
            settings_error = str(error)
        return json.dumps({
            "settings": settings,
            "settings_error": settings_error,
            "model": model,
            "sha256": hashlib.sha256(raw).hexdigest(),
            "image_base64": base64.b64encode(raw).decode("ascii"),
            "channels": _channels(radio),
        })
    finally:
        radio.pipe.close()

def _gmrs_pair(receive, transmit):
    simplex = (
        {462562500 + i * 25000 for i in range(7)} |
        {467562500 + i * 25000 for i in range(7)} |
        {462550000 + i * 25000 for i in range(8)}
    )
    return (receive == transmit and receive in simplex) or (
        receive in {462550000 + i * 25000 for i in range(8)} and
        transmit == receive + 5000000)


def _apply(radio, item, gmrs_authorized=False):
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
    memory.mode = item.get("mode", "NFM" if item.get("narrow", False) else "FM")
    polarity = item.get("dcsPolarity", "NN")
    if len(polarity) != 2 or any(x not in "NR" for x in polarity):
        raise ValueError("Invalid DCS polarity")
    def spec(ctcss, dcs, direction):
        if ctcss is not None and dcs is not None:
            raise ValueError("Choose CTCSS or DCS, not both, per direction")
        if dcs is not None:
            return ("DTCS", int(dcs), polarity[direction])
        if ctcss is not None:
            return ("Tone", float(ctcss), None)
        return ("", None, None)
    memory.tmode = ""
    chirp_common.split_tone_decode(memory,
        spec(item.get("txCtcss"), item.get("txDcs"), 0),
        spec(item.get("rxCtcss"), item.get("rxDcs"), 1))
    # Exact GMRS pairs are allowed only on an explicitly authorized target.
    if transmit is not None and not (144000000 <= transmit <= 148000000 or
                                      420000000 <= transmit <= 450000000):
        if not gmrs_authorized or not _gmrs_pair(receive, transmit):
            raise ValueError("Memory %d: TX not authorized for this service" % slot)
        if receive in {467562500 + i * 25000 for i in range(7)}:
            raise ValueError("GMRS 8-14 require verified 0.5 W ERP hardware")
        if receive in {462562500 + i * 25000 for i in range(7)} and str(item.get("power", "")).lower() != "low":
            raise ValueError("GMRS 1-7 require low power on this driver")
    if memory.mode == "AM" and transmit is not None:
        raise ValueError("AM memories must be receive only")
    levels = getattr(features, "valid_power_levels", None) or []
    if levels:
        requested = str(item.get("power", "High")).lower()
        memory.power = next((level for level in levels if requested in str(level).lower()), levels[0])
    issues = radio.validate_memory(memory)
    errors = [str(x) for x in issues if isinstance(x, chirp_common.ValidationError)]
    if errors:
        raise ValueError("Memory %d: %s" % (slot, "; ".join(errors)))
    radio.set_memory(memory)

def write_radio(model, expected_image_sha256, channel_json, gmrs_authorized=False):
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
    # The initial operator backup may have just finished cloning.
    _settle_session(SESSION_SETTLE_SECONDS)
    radio = _sync_in(model, ident_retry=True)
    try:
        original = _image(radio)
        baseline_sha = hashlib.sha256(original).hexdigest()
        if baseline_sha != expected_image_sha256:
            raise ValueError(
                "Radio memory changed since the backup. Read again before writing."
            )
        for item in profile:
            _apply(radio, item, gmrs_authorized)
        _settle_session(SESSION_SETTLE_SECONDS)
        _upload_with_recovery(radio, model)
        return json.dumps({
            "written": len(profile),
            "model": model,
            "baseline_sha256": baseline_sha,
            "readback_required": True,
        })
    finally:
        radio.pipe.close()


def _upload_with_recovery(radio, model):
    """Retry only the UV-5RM's exact pre-block identification failure.

    The pinned driver's first status callback follows successful identification
    and precedes any block write. Once reached, never replay the upload.
    """
    progress = {"started": False, "cur": 0, "max": 0}
    previous_status = radio.status_fn

    def status(update):
        progress["started"] = True
        progress["cur"] = update.cur
        progress["max"] = update.max
        previous_status(update)

    radio.status_fn = status
    try:
        for attempt in range(2):
            try:
                radio.sync_out()
                return
            except Exception as error:
                if (attempt == 0 and model == "UV-5RM" and
                        not progress["started"] and _ident_failure(error)):
                    _settle_session(IDENT_RETRY_SECONDS)
                    continue
                phase = ("UPLOAD BLOCKS" if progress["started"]
                         else "UPLOAD IDENTIFICATION")
                raise RuntimeError(
                    "%s failed, attempt %d, blocks %d/%d: %s | %s" %
                    (phase, attempt + 1, progress["cur"], progress["max"],
                     error, radio.pipe.diagnostic())) from error
    finally:
        radio.status_fn = previous_status

# Allowlist ordinary operator settings. Never expose firmware band limits,
# factory calibration, password, reset, work-mode frequencies or alarm controls.
_OPERATOR_SETTINGS = {
    "squelch", "save", "vox", "abr", "tdr", "tdrab", "beep", "timeout",
    "voice", "screv", "mdfa", "mdfb", "bcl", "autolk", "roger", "fmradio",
    "wtled", "rxled", "txled", "ste", "rpste", "rptrl", "keylock",
    "poweron_msg.line1", "poweron_msg.line2",
    "settings.squelch", "settings.tot", "settings.dualstandby",
    "settings.powerondistype", "settings.voice", "settings.voicesw",
    "settings.backlight", "settings.autolock", "settings.beep",
    "settings.roger", "settings.chadistype", "settings.chbdistype",
    "settings.savemode", "settings.scanmode", "settings.keylock",
    "settings.voxsw", "settings.vox", "settings.voxdlytime",
    "ani.dtmfon", "ani.dtmfoff", "ani.code", "ani.aniid",
}

def _operator_settings(radio):
    from chirp.settings import RadioSetting
    tree = radio.get_settings()
    def walk(group):
        for entry in group:
            if isinstance(entry, RadioSetting):
                name = entry.get_name()
                if name in _OPERATOR_SETTINGS or (
                        name.startswith("pttid/") and name.endswith(".code")):
                    yield entry
            else:
                yield from walk(entry)
    return tree, list(walk(tree))

def _settings_snapshot(radio):
    from chirp.settings import (RadioSettingValueBoolean, RadioSettingValueInteger,
                                RadioSettingValueList, RadioSettingValueString)
    _, entries = _operator_settings(radio)
    result = []
    for entry in entries:
        value = entry.value
        if not value.get_mutable() or not value.initialized:
            continue
        row = {"key": entry.get_name(), "label": entry.get_shortname()}
        if isinstance(value, RadioSettingValueBoolean):
            row.update(kind="choice", options=["False", "True"], value=str(bool(value)))
        elif isinstance(value, RadioSettingValueList):
            row.update(kind="choice", options=[str(x) for x in value.get_options()],
                       value=str(value))
        elif isinstance(value, RadioSettingValueInteger):
            row.update(kind="integer", min=value.get_min(), max=value.get_max(),
                       value=str(int(value)))
        elif isinstance(value, RadioSettingValueString):
            row.update(kind="text", max=value.maxlength, value=str(value).rstrip())
        else:
            continue
        result.append(row)
    return result

def _apply_settings(radio, changes):
    from chirp.settings import RadioSettingValueBoolean, RadioSettingValueInteger
    tree, entries = _operator_settings(radio)
    by_key = {x.get_name(): x for x in entries}
    if not isinstance(changes, dict) or not changes:
        raise ValueError("No setting changes")
    for key, text in changes.items():
        entry = by_key.get(key)
        if entry is None or not entry.value.get_mutable():
            raise ValueError("Unsupported setting: " + key)
        value = entry.value
        if isinstance(value, RadioSettingValueBoolean):
            if text not in ("True", "False"):
                raise ValueError("Invalid Boolean")
            value.set_value(text == "True")
        elif isinstance(value, RadioSettingValueInteger):
            value.set_value(int(text))
        else:
            value.set_value(text)
    radio.set_settings(tree)

def write_settings(model, expected_image_sha256, changes_json):
    changes = json.loads(changes_json)
    _settle_session(SESSION_SETTLE_SECONDS)
    radio = _sync_in(model, ident_retry=True)
    try:
        if hashlib.sha256(_image(radio)).hexdigest() != expected_image_sha256:
            raise ValueError("Radio changed since backup; read again")
        _apply_settings(radio, changes)
        _settle_session(SESSION_SETTLE_SECONDS)
        _upload_with_recovery(radio, model)
        return json.dumps({"written": len(changes)})
    finally:
        radio.pipe.close()

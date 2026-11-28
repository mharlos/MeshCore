"""Host-side regression tests for bounded clone-session recovery."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1] / "app/src/main/python/fleet_chirp.py"
modules = {name: types.ModuleType(name) for name in (
    "java", "chirp", "chirp.drivers", "chirp.chirp_common",
    "chirp.directory", "chirp.drivers.uv5r", "chirp.drivers.baofeng_uv17Pro",
)}
modules["java"].jclass = Mock(return_value=Mock())
modules["chirp"].__path__ = []
modules["chirp.drivers"].__path__ = []
modules["chirp"].chirp_common = modules["chirp.chirp_common"]
modules["chirp"].directory = modules["chirp.directory"]
spec = importlib.util.spec_from_file_location("fleet_chirp", SOURCE)
bridge = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, modules):
    spec.loader.exec_module(bridge)


class CloneRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.radio = Mock()
        self.radio.pipe.diagnostic.return_value = "transport diagnostics"
        self.original_status = self.radio.status_fn
        self.ident_error = RuntimeError(bridge.IDENT_FAILURE)

    def test_upload_retries_exact_ident_failure_once(self):
        self.radio.sync_out.side_effect = [self.ident_error, None]
        with patch.object(bridge, "_settle_session") as settle:
            bridge._upload_with_recovery(self.radio, "UV-5RM")
        self.assertEqual(self.radio.sync_out.call_count, 2)
        settle.assert_called_once_with(bridge.IDENT_RETRY_SECONDS)
        self.assertIs(self.radio.status_fn, self.original_status)

    def test_ident_retry_is_bounded(self):
        self.radio.sync_out.side_effect = self.ident_error
        with patch.object(bridge, "_settle_session") as settle:
            with self.assertRaisesRegex(RuntimeError, "UPLOAD IDENTIFICATION failed, attempt 2"):
                bridge._upload_with_recovery(self.radio, "UV-5RM")
        self.assertEqual(self.radio.sync_out.call_count, 2)
        settle.assert_called_once()
        self.assertIs(self.radio.status_fn, self.original_status)

    def test_no_retry_after_upload_begins(self):
        def fail():
            self.radio.status_fn(types.SimpleNamespace(cur=0, max=100))
            raise self.ident_error
        self.radio.sync_out.side_effect = fail
        with patch.object(bridge, "_settle_session") as settle:
            with self.assertRaisesRegex(RuntimeError, "UPLOAD BLOCKS failed"):
                bridge._upload_with_recovery(self.radio, "UV-5RM")
        self.radio.sync_out.assert_called_once()
        settle.assert_not_called()

    def test_no_retry_other_error_or_model(self):
        for model, error in (("UV-5RM", RuntimeError("Bad ack writing block")),
                             ("UV-5R", self.ident_error)):
            self.radio.sync_out.reset_mock()
            self.radio.sync_out.side_effect = error
            with patch.object(bridge, "_settle_session") as settle:
                with self.assertRaises(RuntimeError):
                    bridge._upload_with_recovery(self.radio, model)
            self.radio.sync_out.assert_called_once()
            settle.assert_not_called()

    def test_read_retry_only_for_ident_failure(self):
        wrapped = RuntimeError("read failed")
        wrapped.__cause__ = self.ident_error
        with patch.object(bridge, "_sync_in_once", side_effect=[wrapped, self.radio]) as read:
            with patch.object(bridge, "_settle_session") as settle:
                self.assertIs(bridge._sync_in("UV-5RM", ident_retry=True), self.radio)
        self.assertEqual(read.call_count, 2)
        settle.assert_called_once_with(bridge.IDENT_RETRY_SECONDS)

    def test_no_read_retry_without_authorization(self):
        wrapped = RuntimeError("read failed")
        wrapped.__cause__ = self.ident_error
        with patch.object(bridge, "_sync_in_once", side_effect=wrapped) as read:
            with patch.object(bridge, "_settle_session") as settle:
                with self.assertRaises(RuntimeError):
                    bridge._sync_in("UV-5RM")
        read.assert_called_once()
        settle.assert_not_called()

    def test_settle_waits_before_flushing(self):
        calls = []
        with patch.object(bridge.time, "sleep", side_effect=lambda n: calls.append(n)):
            with patch.object(bridge.RadioIo, "flushInput", side_effect=lambda: calls.append("flush")):
                bridge._settle_session(3.0)
        self.assertEqual(calls, [3.0, "flush"])



class IdentificationBatchTests(unittest.TestCase):
    def test_known_magic_is_sent_in_one_ble_write(self):
        pipe = bridge.AndroidPipe()
        magic = bytes.fromhex("50112233445566")
        pipe.ident_frames = (magic,)
        with patch.object(bridge.RadioIo, "write") as send:
            for byte in magic:
                self.assertEqual(pipe.write(bytes([byte])), 1)
            send.assert_called_once_with(magic)
        self.assertEqual(pipe.ident_batches, 1)
        self.assertEqual(pipe.write_calls, 1)

    def test_other_model_preserves_individual_writes(self):
        pipe = bridge.AndroidPipe()
        with patch.object(bridge.RadioIo, "write") as send:
            pipe.write(bytes([0x50]))
            pipe.write(bytes([0x06]))
            self.assertEqual(send.call_count, 2)

    def test_partial_prefix_flushes_before_read(self):
        pipe = bridge.AndroidPipe()
        pipe.ident_frames = (b"ABC",)
        with patch.object(bridge.RadioIo, "write") as send:
            with patch.object(bridge.RadioIo, "read", return_value=[6]):
                pipe.write(b"A")
                self.assertEqual(pipe.read(1), bytes([0x06]))
            send.assert_called_once_with(b"A")

    def test_mismatch_preserves_wire_order(self):
        pipe = bridge.AndroidPipe()
        pipe.ident_frames = (b"ABC",)
        with patch.object(bridge.RadioIo, "write") as send:
            pipe.write(b"A")
            pipe.write(b"Z")
            self.assertEqual([c.args[0] for c in send.call_args_list], [b"A", b"Z"])


if __name__ == "__main__":
    unittest.main()

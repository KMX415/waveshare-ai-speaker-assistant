import importlib.util
from pathlib import Path
import unittest
from unittest.mock import MagicMock, patch

spec = importlib.util.spec_from_file_location("provision_storage", Path(__file__).parents[1] / "scripts/provision_storage.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class ProvisionStorageTests(unittest.TestCase):
    def run_case(self, state, answer):
        identity = {"firmware": "home-voice-standalone-0.6.1"}
        board = MagicMock()
        query = MagicMock(side_effect=[identity, state])
        with patch.object(module, "open_board", return_value=board), patch.object(module, "exchange", query):
            result = module.provision("TEST-PORT", confirm=lambda _: answer)
        return result, query

    def test_cancel_never_sends_efuse_command(self):
        result, query = self.run_case({"active": False, "secure_storage": False}, "yes")
        self.assertFalse(result)
        self.assertEqual([call.args[1] for call in query.call_args_list], [b"I", b"N"])

    def test_already_provisioned_is_read_only(self):
        result, query = self.run_case({"active": False, "secure_storage": True}, module.CONFIRMATION)
        self.assertFalse(result)
        self.assertEqual(query.call_count, 2)

    def test_active_conversation_refuses_provisioning(self):
        with self.assertRaisesRegex(RuntimeError, "Stop"):
            self.run_case({"active": True, "secure_storage": False}, module.CONFIRMATION)

    def test_explicit_confirmation_and_reboot_verification(self):
        events = [{"firmware": "home-voice-standalone-0.6.1"},
                  {"active": False, "secure_storage": False},
                  {"type": "storage_provisioned"}, {"secure_storage": True}]
        with patch.object(module, "open_board", return_value=MagicMock()), patch.object(module, "exchange", side_effect=events) as query, patch.object(module.time, "sleep"):
            self.assertTrue(module.provision("TEST-PORT", confirm=lambda _: module.CONFIRMATION))
        self.assertEqual([call.args[1] for call in query.call_args_list],
                         [b"I", b"N", b"EENABLE-HARDWARE-KEY-STORAGE", b"N"])

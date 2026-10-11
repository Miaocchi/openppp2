import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening_brace = source.index("{", start)
    depth = 0
    for offset, character in enumerate(source[opening_brace:], opening_brace):
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[opening_brace + 1 : offset]
    raise AssertionError(f"unterminated function: {signature}")


class WindowsDnsSnapshotContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.source = (
            ROOT / "windows/ppp/win32/network/NetworkInterface.cpp"
        ).read_text(encoding="utf-8")

    def test_apply_is_limited_to_captured_authorization_set(self) -> None:
        apply_body = function_body(
            self.source,
            "int SetAllNicsDnsAddresses(ppp::vector<boost::asio::ip::address>& servers",
        )
        self.assertIn("for (const auto& [interface_index, ignored] : addresses)", apply_body)
        self.assertIn("SetDnsAddresses(interface_index, servers_string)", apply_body)
        self.assertNotIn("SetDnsAddresses(ni->InterfaceIndex, servers_string)", apply_body)
        self.assertNotIn("addresses.clear()", apply_body)

    def test_failed_restore_keeps_snapshot_for_retry(self) -> None:
        restore_body = function_body(
            self.source,
            "int SetAllNicsDnsAddresses(ppp::unordered_map<int, ppp::vector<boost::asio::ip::address>>& addresses)",
        )
        self.assertIn("it = addresses.erase(it)", restore_body)
        self.assertIn("Retain failed snapshots", restore_body)
        self.assertNotIn("addresses.clear()", restore_body)


if __name__ == "__main__":
    unittest.main()

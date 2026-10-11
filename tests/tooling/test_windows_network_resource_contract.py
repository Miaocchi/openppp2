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


class WindowsNetworkResourceContractTests(unittest.TestCase):
    def test_default_gateway_cleanup_owns_exact_default_routes(self) -> None:
        header = (ROOT / "windows/ppp/win32/network/NetworkInterface.h").read_text(
            encoding="utf-8"
        )
        source = (ROOT / "windows/ppp/win32/network/NetworkInterface.cpp").read_text(
            encoding="utf-8"
        )
        body = function_body(source, "bool DeleteAllDefaultGatewayRoutes(boost::asio::ip::address gw)")

        self.assertIn("bool                                                DeleteAllDefaultGatewayRoutes", header)
        self.assertIn("Router::GetIpForwardTable()", body)
        self.assertIn("Router::Delete(route)", body)
        self.assertIn("Router::Add(previous)", body)
        self.assertIn('inet_addr("128.0.0.0")', body)
        self.assertIn("route.dwForwardDest == ppp::net::IPEndPoint::AnyAddress", body)
        self.assertIn("route.dwForwardDest == half", body)
        self.assertNotIn("Win32Native::Echo", body)

    def test_hosted_route_apply_propagates_default_route_failure(self) -> None:
        source = (ROOT / "ppp/app/client/VEthernetNetworkSwitcher.cpp").read_text(
            encoding="utf-8"
        )
        body = function_body(
            source, "bool VEthernetNetworkSwitcher::TryApplyHostedNetworkRoutes(bool allow_pre_established)"
        )
        cleanup = body[body.index("DeleteAllDefaultGatewayRoutes") :]
        self.assertIn("rollback();", cleanup)
        self.assertIn("return false;", cleanup)

    def test_system_proxy_apply_checks_each_write_and_readback(self) -> None:
        source = (ROOT / "windows/ppp/net/proxies/HttpProxy.cpp").read_text(
            encoding="utf-8"
        )
        body = function_body(
            source,
            "bool HttpProxy::SetSystemProxy(const std::wstring& server",
        )
        self.assertIn("bool ok = true;", body)
        self.assertGreaterEqual(body.count("&& ok"), 6)
        self.assertIn("GetRegistryValueString", body)
        self.assertIn("GetRegistryValueDword", body)
        self.assertIn("HttpProxyApplyFailed", body)
        self.assertNotIn("std::regex_match", body)

    def test_proxy_setup_failure_is_not_reported_as_success(self) -> None:
        source = (ROOT / "ppp/app/client/VEthernetNetworkSwitcher.cpp").read_text(
            encoding="utf-8"
        )
        body = function_body(source, "bool VEthernetNetworkSwitcher::SetHttpProxyToSystemEnv()")
        failed = body[body.index("if (!bok)") :]
        self.assertIn("ClearHttpProxyToSystemEnv();", failed)
        self.assertIn("return false;", failed)


if __name__ == "__main__":
    unittest.main()

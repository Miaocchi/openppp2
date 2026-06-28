import XCTest
@testable import OpenPPP2Logic

final class AppGroupResolverTests: XCTestCase {
    func test_resolve_prefers_configured_group() {
        let value = AppGroupResolver.resolve(
            configured: " group.custom ",
            bundleIdentifier: "com.example.app"
        )
        XCTAssertEqual(value, "group.custom")
    }

    func test_resolve_falls_back_to_bundle_id() {
        let value = AppGroupResolver.resolve(
            configured: nil,
            bundleIdentifier: "com.example.openppp2"
        )
        XCTAssertEqual(value, "group.com.example.openppp2")
    }

    func test_resolve_uses_default_when_missing_inputs() {
        let value = AppGroupResolver.resolve(configured: "  ", bundleIdentifier: nil)
        XCTAssertEqual(value, "group.openppp2")
    }

    func test_resolve_rejects_empty_bundle_id() {
        let value = AppGroupResolver.resolve(configured: nil, bundleIdentifier: "")
        XCTAssertEqual(value, "group.openppp2")
    }
}

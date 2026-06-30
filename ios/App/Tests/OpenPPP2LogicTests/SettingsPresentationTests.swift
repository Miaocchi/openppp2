import XCTest
@testable import OpenPPP2Logic

final class SettingsPresentationTests: XCTestCase {
    // Aim: settings page keeps the planned grouped information architecture.
    func test_sections_and_rows_match_mobile_ui_plan() {
        let sections = SettingsPresentation.Section.allCases

        XCTAssertEqual(
            sections.map(\.title),
            ["应用状态", "诊断", "遥测上传", "系统", "危险操作"]
        )
        XCTAssertEqual(sections[0].rows, [.vpnStatus])
        XCTAssertEqual(sections[1].rows, [.debugPanel, .crashReports])
        XCTAssertEqual(sections[2].rows, [.telemetryUpload])
        XCTAssertEqual(sections[3].rows, [.openSystemSettings])
        XCTAssertEqual(sections[4].rows, [.resetProfiles])
    }

    // Aim: telemetry row remains discoverable as the OpenTelemetry upload entry.
    func test_row_titles_and_accessories() {
        XCTAssertEqual(SettingsPresentation.Row.telemetryUpload.title, "上传到 OpenTelemetry")
        XCTAssertEqual(SettingsPresentation.Row.resetProfiles.title, "清空服务器")
        XCTAssertFalse(SettingsPresentation.Row.debugPanel.usesDisclosure)
        XCTAssertTrue(SettingsPresentation.Row.telemetryUpload.usesDisclosure)
    }

    // Aim: disabled upload renders as off, regardless of stale endpoint values.
    func test_telemetry_summary_disabled() {
        let summary = SettingsPresentation.telemetrySummary(
            uploadEnabled: false,
            destinationName: "自定义",
            endpoint: " https://collector.example/v1/logs "
        )

        XCTAssertEqual(summary, "关闭")
    }

    // Aim: enabled upload includes trimmed endpoint when available.
    func test_telemetry_summary_enabled_with_endpoint() {
        let summary = SettingsPresentation.telemetrySummary(
            uploadEnabled: true,
            destinationName: "自定义",
            endpoint: " https://collector.example/v1/logs "
        )

        XCTAssertEqual(summary, "自定义 · https://collector.example/v1/logs")
    }

    // Aim: enabled upload without endpoint still names the selected destination.
    func test_telemetry_summary_enabled_without_endpoint() {
        let summary = SettingsPresentation.telemetrySummary(
            uploadEnabled: true,
            destinationName: "开发者默认",
            endpoint: "  "
        )

        XCTAssertEqual(summary, "开发者默认")
    }
}

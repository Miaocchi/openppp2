import XCTest
@testable import OpenPPP2Logic

final class RuntimeControlsTests: XCTestCase {
    func testIdleEnablesStartAndProfileEditing() {
        let controls = controlsFor(.idle)
        XCTAssertEqual(controls.action, .start)
        XCTAssertTrue(controls.buttonEnabled)
        XCTAssertEqual(controls.buttonTitleKey, "home.connect")
        XCTAssertTrue(controls.configEditable)
    }

    func testConnectedAndReconnectingEnableStop() {
        for phase in [RuntimePhase.connected, .reconnecting] {
            let controls = controlsFor(phase)
            XCTAssertEqual(controls.action, .stop)
            XCTAssertTrue(controls.buttonEnabled)
            XCTAssertEqual(controls.buttonTitleKey, "home.stop")
            XCTAssertFalse(controls.configEditable)
        }
    }

    func testStoppingDisablesConnectionActions() {
        let controls = controlsFor(.stopping)
        XCTAssertEqual(controls.action, .none)
        XCTAssertFalse(controls.buttonEnabled)
        XCTAssertEqual(controls.buttonTitleKey, "home.stop")
        XCTAssertFalse(controls.configEditable)
    }

    func testFailedEnablesRetryAndProfileEditing() {
        let controls = controlsFor(.failed)
        XCTAssertEqual(controls.action, .retry)
        XCTAssertTrue(controls.buttonEnabled)
        XCTAssertEqual(controls.buttonTitleKey, "home.retry")
        XCTAssertTrue(controls.configEditable)
    }

    func testUnknownExposesForceStop() {
        let controls = controlsFor(.unknown)
        XCTAssertEqual(controls.action, .forceStop)
        XCTAssertTrue(controls.buttonEnabled)
        XCTAssertEqual(controls.buttonTitleKey, "home.forceStop")
        XCTAssertFalse(controls.configEditable)
    }
}

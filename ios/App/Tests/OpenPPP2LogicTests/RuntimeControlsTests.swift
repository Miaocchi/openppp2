import XCTest
@testable import OpenPPP2Logic

final class RuntimeControlsTests: XCTestCase {
    func testPhaseMappingIsAuthoritativeForActions() {
        XCTAssertEqual(controlsFor(.idle).action, .start)
        XCTAssertEqual(controlsFor(.connected).action, .stop)
        XCTAssertEqual(controlsFor(.reconnecting).action, .stop)
        XCTAssertEqual(controlsFor(.stopping).action, .none)
        XCTAssertEqual(controlsFor(.failed).action, .retry)
        XCTAssertEqual(controlsFor(.unknown).action, .forceStop)
    }

    func testSystemSessionCannotSupplyRuntimeReadinessOrAllowAnotherStart() {
        for system in [SystemSessionStatus.connected, .transitioning] {
            for phase: RuntimePhase? in [nil, .idle, .failed] {
                let controls = homeControls(runtime: phase, system: system)
                XCTAssertEqual(controls.action, .forceStop)
                XCTAssertTrue(controls.buttonEnabled)
                XCTAssertFalse(controls.isConnected)
                XCTAssertFalse(controls.configEditable)
            }
        }
        XCTAssertEqual(homeControls(runtime: nil, system: .inactive).action, .start)
        XCTAssertEqual(homeControls(runtime: .failed, system: .inactive).action, .retry)
        XCTAssertEqual(homeControls(runtime: nil, system: .unavailable).action, .checkStatus)
    }

    func testMismatchPreservesRuntimeCommandsWithoutClaimingConnected() {
        for system in [SystemSessionStatus.inactive, .transitioning, .unavailable] {
            let controls = homeControls(runtime: .connected, system: system)
            XCTAssertEqual(controls.action, .stop)
            XCTAssertFalse(controls.isConnected)
            XCTAssertEqual(homeControls(runtime: .reconnecting, system: system).action, .stop)
            XCTAssertEqual(homeControls(runtime: .unknown, system: system).action, .forceStop)
            XCTAssertEqual(homeControls(runtime: .handshaking, system: system).action, .cancel)
            XCTAssertFalse(homeControls(runtime: .stopping, system: system).buttonEnabled)
        }
        XCTAssertTrue(homeControls(runtime: .connected, system: .connected).isConnected)
    }

    func testStaleObservationsPreserveRecoveryButNotReadiness() {
        let connected = homeControls(runtime: .connected, system: .connected, snapshotCurrent: false)
        XCTAssertEqual(connected.action, .stop)
        XCTAssertFalse(connected.isConnected)
        XCTAssertEqual(homeControls(runtime: .idle, system: .inactive, snapshotCurrent: false).action, .forceStop)
        XCTAssertEqual(homeControls(runtime: .unknown, system: .inactive, snapshotCurrent: false).action, .forceStop)
    }

    func testStopTimeoutChangesPresentationWithoutEnablingAction() {
        let controls = controlsFor(.stopping, stopTakingTooLong: true)
        XCTAssertEqual(controls.action, .none)
        XCTAssertFalse(controls.buttonEnabled)
        XCTAssertEqual(controls.detailKey, "home.stopTakingTooLong")
    }
}

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"


class CIWorkflowContractTests(unittest.TestCase):
    def read(self, name: str) -> str:
        return (WORKFLOWS / name).read_text(encoding="utf-8")

    def test_network_lifecycle_contracts_run_in_linux_unit_job(self) -> None:
        workflow = self.read("test.yml")
        required_commands = (
            "tests.tooling.test_windows_dns_snapshot_contract",
            "tests.tooling.test_windows_network_resource_contract",
            "tests.tooling.test_windows_ipv6_contract",
            "tests.tooling.test_lifecycle_wiring",
            "tests.tooling.test_ci_workflow_contract",
        )
        for command in required_commands:
            self.assertIn(command, workflow)

    def test_cpp_and_desktop_jobs_have_build_and_test_gates(self) -> None:
        workflow = self.read("test.yml")
        self.assertIn("cmake -S tests/cpp -B build/test", workflow)
        self.assertIn("ctest --test-dir build/test --output-on-failure", workflow)
        self.assertIn("npm test", workflow)
        self.assertIn("npm run build", workflow)
        self.assertIn("cargo test --locked --manifest-path src-tauri/Cargo.toml", workflow)
        self.assertIn("os: [ubuntu-latest, windows-2022]", workflow)

    def test_windows_x64_build_reports_missing_artifacts(self) -> None:
        workflow = self.read("build-windows-x64.yml")
        self.assertIn("pull_request:", workflow)
        self.assertIn("branches: [main, codex/runtime-ui-lifecycle-integration]", workflow)
        self.assertIn("if-no-files-found: error", workflow)

    def test_windows_arm64_is_explicitly_manual_or_main_only(self) -> None:
        workflow = self.read("build-windows-arm64.yml")
        self.assertIn("push:\n    branches: [main]", workflow)
        self.assertIn("workflow_dispatch:", workflow)
        self.assertNotIn("pull_request:", workflow)


if __name__ == "__main__":
    unittest.main()

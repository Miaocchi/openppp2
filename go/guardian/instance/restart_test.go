package instance

import (
	"os/exec"
	"testing"
	"time"
)

func TestCancelAutomaticRestartDuringBackoff(t *testing.T) {
	for _, action := range []string{"stop", "remove-and-replace", "shutdown"} {
		t.Run(action, func(t *testing.T) {
			m := NewManager()
			cfg := Config{Binary: "nonexistent-test-binary", AutoRestart: AutoRestartConfig{Enabled: true, RetryDelayMs: 60000}}
			if err := m.Add("worker", cfg); err != nil {
				t.Fatal(err)
			}
			inst := m.instances["worker"]
			// Wait on an unstarted command reports an exit error without spawning a child.
			cmd := exec.Command(cfg.Binary)
			inst.cmd, inst.running, inst.exited = cmd, true, make(chan struct{})
			events, unsubscribe := m.EventSubscribe()
			defer unsubscribe()
			finished := make(chan struct{})
			go func() {
				m.waitForExit(inst, cmd)
				close(finished)
			}()
			select {
			case e := <-events:
				if e.Type != "stopped" {
					t.Fatalf("expected stopped event, got %q", e.Type)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("exit notification not received")
			}
			inst.mu.RLock()
			token := inst.pendingRestart
			inst.mu.RUnlock()
			if token == nil {
				t.Fatal("expected a pending retry")
			}
			switch action {
			case "stop":
				if err := m.Stop("worker"); err != nil {
					t.Fatal(err)
				}
			case "remove-and-replace":
				if err := m.Remove("worker"); err != nil {
					t.Fatal(err)
				}
				if err := m.Add("worker", cfg); err != nil {
					t.Fatal(err)
				}
			case "shutdown":
				m.Shutdown()
			}
			select {
			case <-finished:
			case <-time.After(2 * time.Second):
				t.Fatal("backoff was not cancelled")
			}
			// Even if the timer fired concurrently, its old token cannot launch anything.
			if err := m.startInstance(inst, true, token); err != nil {
				t.Fatalf("stale retry attempted to spawn: %v", err)
			}
		})
	}
}

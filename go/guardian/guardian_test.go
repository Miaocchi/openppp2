package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"ppp/guardian/instance"
)

func TestGuardianSaveConfigWritesSecretConfigOwnerOnly(t *testing.T) {
	path := filepath.Join(t.TempDir(), "guardian.json")
	guardian := &Guardian{
		cfg: &GuardianConfig{
			Listen: "127.0.0.1:18080",
			Auth: AuthConfig{
				Enabled:   true,
				JWTSecret: "generated-secret",
			},
		},
		configPath: path,
		instances:  instance.NewManager(),
	}

	if err := guardian.SaveConfig(); err != nil {
		t.Fatalf("SaveConfig() error = %v", err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("Stat() error = %v", err)
	}
	if got := info.Mode().Perm(); got != 0o600 {
		t.Fatalf("mode = %o, want 600", got)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	var saved GuardianConfig
	if err := json.Unmarshal(data, &saved); err != nil {
		t.Fatalf("Unmarshal() error = %v", err)
	}
	if saved.Auth.JWTSecret != "generated-secret" {
		t.Fatalf("JWTSecret = %q, want generated-secret", saved.Auth.JWTSecret)
	}
}

func TestGuardianSaveConfigMergesInstances(t *testing.T) {
	disabled := InstanceConfig{
		Name: "offline", Binary: "offline-bin",
		Args: []string{"--offline"}, Env: map[string]string{"MODE": "paused"},
	}
	original := []InstanceConfig{
		{Name: "updated", Enabled: true, Binary: "old-bin"},
		disabled,
		{Name: "removed", Enabled: true, Binary: "removed-bin"},
		{Name: "reactivated", Binary: "disabled-bin"},
	}
	dir := t.TempDir()
	g := &Guardian{
		cfg:        &GuardianConfig{Instances: append([]InstanceConfig(nil), original...)},
		configPath: dir,
		instances:  instance.NewManager(),
	}
	// The manager holds only the current active entries, not the original list.
	for _, name := range []string{"new-z", "reactivated", "updated", "new-a"} {
		if err := g.instances.Add(name, instance.Config{Binary: name + "-bin"}); err != nil {
			t.Fatalf("Add(%q): %v", name, err)
		}
	}
	if err := g.SaveConfig(); err == nil {
		t.Fatal("SaveConfig to a directory succeeded")
	}
	if got := g.cfg.Instances; !reflect.DeepEqual(got, original) {
		t.Fatalf("failed save changed instances: got %+v, want %+v", got, original)
	}

	g.configPath = filepath.Join(dir, "guardian.json")
	wantNames := []string{"updated", "offline", "reactivated", "new-a", "new-z"}
	for save := 1; save <= 2; save++ {
		if err := g.SaveConfig(); err != nil {
			t.Fatalf("save %d: %v", save, err)
		}
		data, err := os.ReadFile(g.configPath)
		if err != nil {
			t.Fatal(err)
		}
		var saved GuardianConfig
		if err := json.Unmarshal(data, &saved); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(g.cfg.Instances, saved.Instances) {
			t.Fatalf("save %d: in-memory instances differ from file", save)
		}
		if len(saved.Instances) != len(wantNames) {
			t.Fatalf("save %d: got %d instances, want %d", save, len(saved.Instances), len(wantNames))
		}
		for i, name := range wantNames {
			got := saved.Instances[i]
			if name == disabled.Name {
				if !reflect.DeepEqual(got, disabled) {
					t.Fatalf("save %d: instance %d = %+v, want %+v", save, i, got, disabled)
				}
			} else if got.Name != name || !got.Enabled || got.Binary != name+"-bin" {
				t.Fatalf("save %d: instance %d = %+v, want active snapshot of %q", save, i, got, name)
			}
		}
	}
}

func TestGuardianUpdateAuthConfigPersistsPasswordOwnerOnly(t *testing.T) {
	path := filepath.Join(t.TempDir(), "guardian.json")
	guardian := &Guardian{
		cfg: &GuardianConfig{
			Listen: "127.0.0.1:18080",
			Auth: AuthConfig{
				Enabled:          true,
				JWTSecret:        "old-secret",
				TokenExpiryHours: 24,
			},
		},
		configPath: path,
		instances:  instance.NewManager(),
	}

	if err := guardian.UpdateAuthConfig(true, "new-secret"); err != nil {
		t.Fatalf("UpdateAuthConfig() error = %v", err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("Stat() error = %v", err)
	}
	if got := info.Mode().Perm(); got != 0o600 {
		t.Fatalf("mode = %o, want 600", got)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile() error = %v", err)
	}
	var saved GuardianConfig
	if err := json.Unmarshal(data, &saved); err != nil {
		t.Fatalf("Unmarshal() error = %v", err)
	}
	if !saved.Auth.Enabled || saved.Auth.JWTSecret != "new-secret" {
		t.Fatalf("saved auth = %+v, want enabled with new secret", saved.Auth)
	}
	if saved.Auth.TokenExpiryHours != 24 {
		t.Fatalf("TokenExpiryHours = %d, want 24", saved.Auth.TokenExpiryHours)
	}
}

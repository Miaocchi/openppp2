package profile

import "testing"

func TestValidateProfile_accepts_known_client_key(t *testing.T) {
	t.Parallel()
	if err := validateProfile([]byte(`{"client":{"tcp":"127.0.0.1:20000"}}`)); err != nil {
		t.Fatalf("expected valid profile, got %v", err)
	}
}

func TestValidateProfile_rejects_empty_object(t *testing.T) {
	t.Parallel()
	if err := validateProfile([]byte(`{}`)); err == nil {
		t.Fatal("expected error for profile without known keys")
	}
}

func TestValidateProfile_rejects_unknown_keys_only(t *testing.T) {
	t.Parallel()
	if err := validateProfile([]byte(`{"foo":1,"bar":2}`)); err == nil {
		t.Fatal("expected error for unknown keys only")
	}
}

func TestValidateProfile_rejects_invalid_json(t *testing.T) {
	t.Parallel()
	if err := validateProfile([]byte(`{not-json`)); err == nil {
		t.Fatal("expected JSON parse error")
	}
}

func TestValidateProfile_rejects_nested_unknown_without_known_key(t *testing.T) {
	t.Parallel()
	if err := validateProfile([]byte(`{"outer":{"inner":1}}`)); err == nil {
		t.Fatal("expected error when no known ppp key exists")
	}
}

package binary

import "testing"

func TestIsPPPBinaryName(t *testing.T) {
	tests := []struct {
		name string
		want bool
	}{
		{name: "ppp", want: true},
		{name: "PPP", want: true},
		{name: "ppp.exe", want: true},
		{name: "PPP.EXE", want: true},
		{name: "ppp-ci", want: true},
		{name: "ppp-debug", want: true},
		{name: "ppp-ci.exe", want: true},
		{name: "ppp-backdoor.sh", want: false},
		{name: "ppp-ci.bin", want: false},
		{name: "ppp.txt", want: false},
		{name: "not-ppp", want: false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := isPPPBinaryName(tt.name); got != tt.want {
				t.Fatalf("isPPPBinaryName(%q) = %v, want %v", tt.name, got, tt.want)
			}
		})
	}
}

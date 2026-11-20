package instance

import (
	"reflect"
	"testing"
)

func TestRecentLogsAfterWrap(t *testing.T) {
	r := newRingBuffer(4)
	for _, entry := range []LogEntry{
		{Stream: "out", Text: "evicted"}, {Stream: "err", Text: "b"},
		{Stream: "out", Text: "c"}, {Stream: "err", Text: "d"},
		{Stream: "out", Text: "e"},
	} {
		r.add(entry)
	}
	for _, tc := range []struct {
		n      int
		stream string
		want   []string
	}{
		{2, "all", []string{"d", "e"}}, {0, "", []string{"b", "c", "d", "e"}},
		{1, "err", []string{"d"}}, {10, "out", []string{"c", "e"}}, {2, "none", nil},
	} {
		var got []string
		for _, entry := range r.list(tc.n, tc.stream) {
			got = append(got, entry.Text)
		}
		if !reflect.DeepEqual(got, tc.want) {
			t.Errorf("list(%d, %q) = %v, want %v", tc.n, tc.stream, got, tc.want)
		}
	}
}

package tui

import (
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// A form key applies what is pending, closes the form and is reported; the starting group and the
// footer follow the spec.
func TestFormKeysCloseAfterPendingApply(t *testing.T) {
	rows := []ui.FormRow{
		{ID: "a", Group: "One", Label: "A", Kind: ui.FormEnum, Value: "x", Choices: []string{"x", "y"}},
		{ID: "b", Group: "Two", Label: "B", Kind: ui.FormInt, Value: "5", Min: 0, Max: 9},
	}
	r := newRig(t, rows, 80, 24)
	r.f.Keys = []ui.FormKey{{Key: "r", Label: "restart"}}
	r.f.StartGroup = "Two"
	if g := r.f.ShownGroup(); g != "Two" {
		t.Fatalf("start group: %q", g)
	}
	if !strings.Contains(r.f.View(), "restart") {
		t.Error("the key is not in the footer")
	}
	r.press("right") // b: 6, pending
	r.press("r")
	if len(r.calls) != 1 || r.calls[0] != (applied{"b", "6"}) {
		t.Fatalf("pending change not applied first: %v", r.calls)
	}
	if !r.f.Done || r.f.Key != "r" || r.f.ShownGroup() != "Two" {
		t.Fatalf("done=%v key=%q group=%q", r.f.Done, r.f.Key, r.f.ShownGroup())
	}
}

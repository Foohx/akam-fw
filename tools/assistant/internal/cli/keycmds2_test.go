//go:build akamtest

package cli_test

import (
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// Nothing is probed or sent before the user said yes (the card probe may mount the card for a moment).
func TestKeyChangeSendsNothingBeforeTheYes(t *testing.T) {
	e := newEnv(t, func(o *testfake.Options) { o.CardPresent = true })
	e.needHelpers()
	e.useSharedKey()
	work := newWorkKey(t, e, "id_work")
	r := e.keyRun("n\n", "key", "use", work+".pub")
	expectRC(t, r, 0)
	notContains(t, r.out, "a card with keys")
	if n := authkeysCommands(e); n != 0 {
		t.Errorf("%d helper commands were sent before the yes", n)
	}
}

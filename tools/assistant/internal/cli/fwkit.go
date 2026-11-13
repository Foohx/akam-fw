package cli

import "os"

// kitTxn lets a kit be replaced tentatively. get-kit extracts the verified kit to <state>/kit and rewrites the
// kit keys of the state; if the firmware cannot be made ready after that (the image cannot be built here, a
// downgrade is refused, the build fails), rollback puts the previous kit folder and the previous state back,
// so a refused or impossible choice leaves the camera's progress exactly as it was.
type kitTxn struct {
	a      *App
	kv     map[string]string
	marked string // the time get-kit was marked done ("" : not done)
	dir    string
	keep   string
	moved  bool
}

const doneKey = "step_get_kit_done"

var kitKeys = []string{"mode", "kit_path", "kit_sha", "kit_format", "kit_unsigned_ok", "kit_version"}

func (a *App) kitBegin() *kitTxn {
	t := &kitTxn{a: a, kv: map[string]string{}, marked: a.St.Get(doneKey), dir: a.kitDir()}
	for _, k := range kitKeys {
		t.kv[k] = a.St.Get(k)
	}
	t.keep = t.dir + ".keep"
	_ = os.RemoveAll(t.keep)
	if _, err := os.Stat(t.dir); err == nil {
		if os.Rename(t.dir, t.keep) == nil {
			t.moved = true
		}
	}
	return t
}

// commit forgets the previous kit.
func (t *kitTxn) commit() {
	if t.moved {
		_ = os.RemoveAll(t.keep)
	}
}

// rollback restores the previous kit folder and kit state.
func (t *kitTxn) rollback() {
	_ = os.RemoveAll(t.dir)
	if t.moved {
		_ = os.Rename(t.keep, t.dir)
	}
	for _, k := range append(kitKeys[:len(kitKeys):len(kitKeys)], doneKey) {
		v := t.kv[k]
		if k == doneKey {
			v = t.marked
		}
		if v == "" {
			_ = t.a.St.Unset(k)
		} else {
			_ = t.a.St.Set(k, v)
		}
	}
}

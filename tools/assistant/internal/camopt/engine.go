package camopt

import (
	"fmt"
	"strings"
)

// Exchanger sends control-port lines (GET/SET) and returns the answers without the LOG lines. One call is
// one exchange: the engine never opens a second one for a group.
type Exchanger func(lines ...string) []string

// OptionState is what the camera said about one option.
type OptionState struct {
	Option
	Value       string // empty when unavailable
	Unavailable bool   // the camera did not answer, or answered something unsafe (older image)
}

// answer is the value of key among the answers (text after "key=" up to the first space).
func answer(key string, answers []string) (string, bool) {
	for _, l := range answers {
		if strings.HasPrefix(l, key+"=") {
			v := l[len(key)+1:]
			if i := strings.IndexByte(v, ' '); i >= 0 {
				v = v[:i]
			}
			return v, true
		}
	}
	return "", false
}

// Read reads the options with these IDs (all the readable ones when none is given) in ONE exchange.
// A value that is missing (an older image answers ERR), unsafe (SafeValue) or, for an Int option,
// not a plain whole number, gives an Unavailable state with an empty Value.
func Read(ex Exchanger, ids ...string) []OptionState {
	var opts []Option
	if len(ids) == 0 {
		for _, o := range Catalog() {
			if o.Key != "" {
				opts = append(opts, o)
			}
		}
	} else {
		for _, id := range ids {
			if o, ok := Find(id); ok && o.Key != "" {
				opts = append(opts, o)
			}
		}
	}
	lines := make([]string, len(opts))
	for i, o := range opts {
		lines[i] = "GET " + o.Key
	}
	var got []string
	if len(lines) > 0 {
		got = ex(lines...)
	}
	out := make([]OptionState, len(opts))
	for i, o := range opts {
		st := OptionState{Option: o}
		v, ok := answer(o.Key, got)
		if !ok || !SafeValue(v) || (o.Kind == Int && !SafeNumber(v)) {
			st.Unavailable = true
		} else {
			st.Value = v
		}
		out[i] = st
	}
	return out
}

// Change is a requested new value, as the user typed it.
type Change struct{ ID, Value string }

// Result is the camera's answer to one Change.
type Result struct {
	ID        string
	Value     string // normalised value that was sent
	Answered  bool
	OK        bool
	NotSaved  bool // applied but not saved: lost at the next reboot
	Restart   bool // "restart to apply": the camera runs the old value until the service restarts
	Err       string
	Line      string
	Persisted Persist
	Applies   Applies
}

// Apply validates every change first (nothing is sent when one is invalid), then sends all the SET
// lines in one exchange and reports each answer with the option's Persisted and Applies.
func Apply(ex Exchanger, changes []Change) ([]Result, error) {
	lines := make([]string, len(changes))
	res := make([]Result, len(changes))
	for i, c := range changes {
		v, err := Validate(c.ID, c.Value)
		if err != nil {
			return nil, err
		}
		o, _ := Find(c.ID)
		lines[i] = "SET " + o.Key + " " + v
		res[i] = Result{ID: c.ID, Value: v, Persisted: o.Persisted, Applies: o.Applies}
	}
	if len(lines) == 0 {
		return nil, nil
	}
	var answers []string
	for _, l := range ex(lines...) {
		if strings.HasPrefix(l, "OK ") || strings.HasPrefix(l, "ERR ") {
			answers = append(answers, l)
		}
	}
	for i := range res {
		if i >= len(answers) {
			break
		}
		l := answers[i]
		res[i].Answered, res[i].Line = true, l
		if strings.HasPrefix(l, "OK ") {
			res[i].OK = true
			res[i].NotSaved = strings.Contains(l, "not saved")
			res[i].Restart = strings.Contains(l, "restart to apply")
		} else {
			res[i].Err = strings.TrimPrefix(l, "ERR ")
		}
	}
	return res, nil
}

// Describe is the short "applies when / saved where" note of an option, for the result lines.
func Describe(o Option) string {
	var saved string
	switch o.Persisted {
	case PersistCamera:
		saved = "saved on the camera"
	case PersistRAM:
		saved = "not saved (back to the default at a reboot)"
	default:
		return "read-only"
	}
	when := map[Applies]string{Now: "applies now", VideoRestart: "applies at the next restart of the video service", NextPowerCycle: "applies at the next power cycle"}[o.Applies]
	return fmt.Sprintf("%s; %s", when, saved)
}

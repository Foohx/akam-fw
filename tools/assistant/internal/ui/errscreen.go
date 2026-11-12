package ui

// The error screen of the full-screen session (spec 12, 4.7). A flow still calls Fail(msg, hint); the
// session remembers it, and the home loop shows it as a screen once the flow has returned. Plain does
// not implement any of this: Fail keeps its "ERROR: " and "What to do: " lines there.

// ErrorSpec is what the error screen shows. Every text is plain words, not a log line.
type ErrorSpec struct {
	Context string   // the flow and the camera ("Update this camera - salon  192.0.2.41"); may be empty
	Title   string   // one sentence, in the error colour
	Body    []string // what happened, a sentence per line
	Todo    []string // the "What to do" list
	Tech    string   // one muted technical line ("camera answered: DESCRIBE 503"); may be empty
	Help    string   // where to read more; may be empty
	Retry   bool     // offer "r retry"
	Log     string   // the tail of the log, shown by "l"
}

// Keys an ErrorScreen reports.
const (
	ErrKeyBack   = ""       // Esc: back to the home
	ErrKeyRetry  = "retry"  // r
	ErrKeyCamera = "camera" // c
)

// ErrorScreener is implemented by the session UI: ErrorScreen shows the screen and returns the key the
// user chose (one of the ErrKey constants). The log pane ("l") stays inside the screen.
type ErrorScreener interface {
	ErrorScreen(spec ErrorSpec) (key string, err error)
}

// FailInfo is the last Fail of a session.
type FailInfo struct{ Msg, Hint, Tech string }

// FailReader is implemented by the session UI: it remembers the last Fail so the caller can show it as
// a screen. ClearFail forgets it (called when an action starts and once the screen was shown).
type FailReader interface {
	LastFail() (FailInfo, bool)
	ClearFail()
}

// TechFailer is Fail with a muted technical line for the error screen; the session UI implements it.
type TechFailer interface {
	FailTech(msg, hint, tech string) error
}

// OutputClearer is implemented by the session UI: ClearOutput empties the transcript and the steps of
// the previous action, so the next action starts on a clean panel.
type OutputClearer interface{ ClearOutput() }

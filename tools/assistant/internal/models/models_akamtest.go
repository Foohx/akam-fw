//go:build akamtest

package models

// AddForTests adds a model to the registry until the returned function is called (tests only: this file is
// compiled with the akamtest tag, never in a release build).
func AddForTests(m Model) (remove func()) {
	extra = append(extra, m)
	n := len(extra)
	return func() { extra = extra[:n-1] }
}

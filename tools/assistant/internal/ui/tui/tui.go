package tui

import (
	"fmt"
	"os"

	tea "github.com/charmbracelet/bubbletea"
	"golang.org/x/term"

	"github.com/charmbracelet/lipgloss"

	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// UI is the plain UI with bubbletea widgets for the questions and the full-screen menu. Messages
// (Say, Warn, ...) are the plain ones, so the transcript of a run reads the same in both modes.
// Without a terminal on stdin and stderr every widget falls back to the plain behaviour.
type UI struct {
	*ui.Plain
}

// New wraps a Plain. Use it only on a real terminal (the caller checks).
func New(p *ui.Plain) *UI {
	// Only 16-colour attributes are used, so there is no need to ask the terminal for its
	// background colour (that query waits for an answer some terminals never give).
	lipgloss.SetHasDarkBackground(true)
	return &UI{Plain: p}
}

func (u *UI) widgets() bool {
	return u.InFile != nil && u.ErrFile != nil && os.Getenv("AKAM_PLAIN") != "1" &&
		term.IsTerminal(int(u.InFile.Fd())) && term.IsTerminal(int(u.ErrFile.Fd()))
}

func (u *UI) run(m tea.Model, alt bool) error {
	opts := []tea.ProgramOption{tea.WithInput(u.InFile), tea.WithOutput(u.ErrFile)}
	if alt {
		opts = append(opts, tea.WithAltScreen())
	}
	_, err := tea.NewProgram(m, opts...).Run()
	return err
}

func (u *UI) AskYN(prompt string) (bool, error) {
	if !u.widgets() {
		return u.Plain.AskYN(prompt)
	}
	m := &YesNo{Prompt: prompt}
	if err := u.run(m, false); err != nil {
		return false, err
	}
	return m.Result, nil
}

func (u *UI) Danger(title, question string, lines ...string) (bool, error) {
	if u.DryRun || !u.widgets() {
		return u.Plain.Danger(title, question, lines...)
	}
	m := &YesNo{Prompt: question, Title: title, Box: lines, Danger: true}
	if err := u.run(m, false); err != nil {
		return false, err
	}
	return m.Result, nil
}

func (u *UI) Prompt(prompt, def string) (string, error) {
	if !u.widgets() || u.DryRun {
		return u.Plain.Prompt(prompt, def)
	}
	m := &Input{Prompt: prompt, Def: def}
	if err := u.run(m, false); err != nil {
		return "", err
	}
	if m.Cancelled {
		return "", ui.ErrNoInput
	}
	return m.Value(), nil
}

func (u *UI) Secret(prompt string) (string, error) {
	if !u.widgets() {
		return u.Plain.Secret(prompt)
	}
	m := &Input{Prompt: prompt, Secret: true}
	if err := u.run(m, false); err != nil {
		return "", err
	}
	if m.Cancelled {
		return "", ui.ErrNoInput
	}
	return m.Value(), nil // the caller registers it once it has validated it
}

func (u *UI) Pick(title string, items []string) (int, error) {
	if !u.widgets() || u.DryRun {
		return u.Plain.Pick(title, items)
	}
	m := &Picker{Title: title, Items: items}
	if err := u.run(m, false); err != nil {
		return -1, err
	}
	if m.Cancelled {
		return -1, ui.ErrNoInput
	}
	return m.Cursor, nil
}

// Menu is the full-screen main menu (the cli.Menuer interface); it returns -1 for quit.
func (u *UI) Menu(title string, items []ui.MenuItem) (int, error) {
	m := NewMenu(title, items)
	if err := u.run(m, true); err != nil {
		return -1, fmt.Errorf("menu: %w", err)
	}
	if m.Quit || !m.Done {
		return -1, nil
	}
	return m.Cursor, nil
}

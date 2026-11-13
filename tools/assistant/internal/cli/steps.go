package cli

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// Step metadata, copied from steps_host.sh so the status table is identical.

func stepTitle(s string) string {
	switch s {
	case "doctor":
		return "Check this computer"
	case "get-kit":
		return "Get the Akam kit"
	case "prepare-backup-sd":
		return "Prepare the backup microSD card"
	case "find-camera":
		return "Find the camera on your network"
	case "backup":
		return "Back up the camera (all 8 partitions)"
	case "image":
		return "Build the Akam image from your backup"
	case "prepare-flash-sd":
		return "Prepare the flash microSD card"
	case "flash":
		return "Flash the image to the camera"
	case "verify":
		return "Verify the result"
	}
	return ""
}

func stepWhy(s string) string {
	switch s {
	case "doctor":
		return "Why: tools must be present before anything else. Risk: none (read-only)."
	case "get-kit":
		return "Why: the prebuilt kit means no compiling. Risk: none (download + signature check)."
	case "prepare-backup-sd":
		return "Why: boots the camera into a rescue shell. Risk: ERASES the microSD card."
	case "find-camera":
		return "Why: later steps need the camera address. Risk: none."
	case "backup":
		return "Why: the only way back to the original firmware. Risk: none for the camera; keep TWO copies."
	case "image":
		return "Why: builds your Akam image from YOUR dump. Risk: none (files on this PC only)."
	case "prepare-flash-sd":
		return "Why: carries the image and your SSH key. Risk: ERASES the microSD card."
	case "flash":
		return "Why: installs Akam. Risk: WRITES the camera flash; a power cut can brick it."
	case "verify":
		return "Why: proves SSH and video work before you remove the card. Risk: none."
	}
	return ""
}

// stepWhyShort is stepWhy cut to one clause for the full-screen menu (the risk stays, the reason goes);
// the full text is shown when the step starts and stays in the plain interface.
func stepWhyShort(s string) string {
	switch s {
	case "doctor", "verify":
		return "read-only check"
	case "get-kit":
		return "download and signature check"
	case "prepare-backup-sd", "prepare-flash-sd":
		return "ERASES the microSD card"
	case "find-camera":
		return "read-only"
	case "backup":
		return "keep TWO copies"
	case "image":
		return "files on this PC only"
	case "flash":
		return "WRITES the flash: no power cut"
	}
	return ""
}

func stepDeps(s string) []string {
	switch s {
	case "get-kit", "prepare-backup-sd":
		return []string{"doctor"}
	case "find-camera":
		return []string{"prepare-backup-sd"}
	case "backup":
		return []string{"find-camera"}
	case "image":
		return []string{"get-kit", "backup"}
	case "prepare-flash-sd":
		return []string{"image"}
	case "flash":
		return []string{"prepare-flash-sd"}
	case "verify":
		return []string{"flash"}
	}
	return nil
}

func stepNum(s string) int {
	for i, x := range state.Steps {
		if x == s {
			return i + 1
		}
	}
	return 0
}

func stepValidName(s string) bool { return stepNum(s) > 0 }

func fileSHA(p string) string {
	f, err := os.Open(p)
	if err != nil {
		return ""
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil || st.IsDir() {
		return ""
	}
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return ""
	}
	return hex.EncodeToString(h.Sum(nil))
}

func (a *App) kitCurrentSHA() string {
	if a.staged != nil { // a verified kit waiting to be installed
		return a.staged.sha
	}
	p := a.St.Get("kit_path")
	if a.St.Get("mode") == "source" {
		return "source"
	}
	if st, err := os.Stat(p); err == nil && st.IsDir() {
		return fileSHA(filepath.Join(p, "MANIFEST.txt"))
	}
	return fileSHA(p)
}

func (a *App) backupCurrentSHA() string {
	return fileSHA(filepath.Join(a.St.Get("backup_dir"), "mtdblock7.bin"))
}

// stepValidate returns "" when the recorded result still matches the inputs on disk, else the
// reason (step_validate in steps_host.sh).
func (a *App) stepValidate(step string) string {
	dry := a.St.Dry
	switch step {
	case "get-kit":
		if a.St.Get("mode") == "source" {
			return ""
		}
		now := a.kitCurrentSHA()
		if now == "" {
			if dry {
				return ""
			}
			return "the kit file is gone: " + a.St.Get("kit_path")
		}
		if now != a.St.Get("kit_sha") {
			if dry && a.St.Get("kit_sha") == "dry-run" {
				return ""
			}
			return "the kit file changed since it was verified"
		}
	case "backup":
		now := a.backupCurrentSHA()
		if now == "" {
			if dry {
				return ""
			}
			return "the backup folder or its APP dump is gone: " + a.St.Get("backup_dir")
		}
		if now != a.St.Get("backup_sha") {
			return "the APP dump (mtdblock7.bin) changed since it was checked"
		}
	case "image":
		now := fileSHA(a.St.Get("image_path"))
		if now == "" {
			if dry {
				return ""
			}
			return "the image file is gone: " + a.St.Get("image_path")
		}
		if now != a.St.Get("image_sha") {
			return "the image file changed after it was built"
		}
		if a.kitCurrentSHA() != a.St.Get("image_kit_sha") {
			return "the kit changed since the image was built"
		}
		if a.backupCurrentSHA() != a.St.Get("image_backup_sha") {
			return "the backup changed since the image was built"
		}
	case "prepare-flash-sd":
		if a.St.Get("flash_sd_image_sha") != a.St.Get("image_sha") {
			return "the image changed after the flash card was prepared"
		}
		if r := a.stepValidate("image"); r != "" {
			return "the card was made from an image that is out of date (" + r + ")"
		}
	}
	return ""
}

// stepStatus is done | stale | blocked | todo.
func (a *App) stepStatus(step string) string {
	if step == "get-kit" && a.staged != nil {
		return "done" // the kit that is staged counts: the image of it may be checked or built before it is installed
	}
	if a.St.IsMarked(step) {
		if a.stepValidate(step) == "" {
			return "done"
		}
		return "stale"
	}
	for _, d := range stepDeps(step) {
		if a.stepStatus(d) != "done" {
			return "blocked"
		}
	}
	return "todo"
}

func (a *App) stepBlockers(step string) []string {
	var out []string
	for _, d := range stepDeps(step) {
		if a.stepStatus(d) != "done" {
			out = append(out, d)
		}
	}
	return out
}

// statusTable prints the numbered steps (status_table).
func (a *App) statusTable() {
	mode := "real"
	if a.UI.Dry() {
		mode = "rehearsal"
	}
	a.UI.Bold(fmt.Sprintf("Akam setup progress (%s; camera: %s; state: %s)", mode, a.Cams.Name, a.St.File))
	if fw := a.St.Get("fw_source"); fw != "" { // only after a firmware was chosen here and written
		line := "  Firmware source: " + ui.Sanitize(fw)
		if d := a.St.Get("fw_source_at"); d != "" {
			// an update by the bash wizard does not touch it: say when akam recorded it
			line += "  (recorded by akam on " + ui.Sanitize(d) + ")"
		}
		a.UI.Say(line)
	}
	if a.St.Get("ssh_pubkey") != "" { // only once a key was chosen: a fresh camera prints exactly what the bash wizard does
		line := "  SSH key: " + a.keyRowText()
		if fp := camssh.PubFingerprint(a.keyPub()); fp != "" {
			line += "  " + fp
		}
		a.UI.Say(line)
	}
	var b strings.Builder
	for i, s := range state.Steps {
		st := a.stepStatus(s)
		label := "todo   "
		switch st {
		case "done":
			label = "done   "
		case "blocked":
			label = "blocked"
		}
		fmt.Fprintf(&b, "  %d. [%s] %-18s %s\n", i+1, label, s, stepTitle(s))
		fmt.Fprintf(&b, "       %s\n", stepWhy(s))
		switch st {
		case "stale":
			fmt.Fprintf(&b, "       Inputs changed: %s. Run this step again.\n", a.stepValidate(s))
		case "blocked":
			fmt.Fprintf(&b, "       Waiting for: %s\n", strings.Join(a.stepBlockers(s), " "))
		}
	}
	fmt.Fprint(a.out(), b.String())
}

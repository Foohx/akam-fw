package cli

// Your own firmware builds: extra trusted signing keys (trust-key) and the GitHub release a download comes from
// (release-source, --repo). Both live in the assistant's user state, not in a camera's. Neither ever makes a
// download unsigned: a signature by the official key or by a key you trusted is still mandatory.

import (
	"fmt"
	"os"
	"strings"

	gossh "golang.org/x/crypto/ssh"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

func (a *App) userTrust() *kit.UserTrust { return kit.OpenUserTrust(a.Cams.Base) }

// userSigners is the content of the user trust file for a verification. A damaged file is reported and not used
// (only the official key then counts).
func (a *App) userSigners() []byte {
	b, err := a.userTrust().Bytes()
	if err != nil {
		a.UI.Warn("your trusted keys file cannot be used (" + ui.Sanitize(err.Error()) + "): only the official key is accepted. Check it with " + Prog + " trust-key list.")
		return nil
	}
	return b
}

// repo is the release source of this run: --repo, else the one you saved (release-source), else the official one.
func (a *App) repo() string {
	if a.Opt.Repo != "" {
		return a.Opt.Repo
	}
	if s, err := kit.ReadSource(a.Cams.Base); err == nil && s != "" {
		return s
	}
	return kitRepo
}

func (a *App) repoOfficial() bool { return a.repo() == kitRepo }

// repoText is how every download message names its source.
func (a *App) repoText() string {
	if a.repoOfficial() {
		return a.repo() + " (the official source)"
	}
	return a.repo() + " (NOT the official source: the kit must be signed by the official key or by a key you trust)"
}

// signerText is the line that follows a successful verification.
func (a *App) signerText() string {
	if a.lastSigner == "" {
		return ""
	}
	return "Signed by " + kit.WithThe(a.lastSigner) + "."
}

const trustWarning = "A key you trust can sign firmware that this program will accept and that is then written to your camera. " +
	"Only trust a key that is yours, or whose owner you know. If in doubt, answer No."

// cmdTrustKey is `trust-key [list | add FILE.pub [--name LABEL] | remove LABEL]`.
func (a *App) cmdTrustKey(args []string) error {
	word := "list"
	if len(args) > 0 {
		word = args[0]
	}
	tr := a.userTrust()
	switch word {
	case "list":
		if len(args) > 1 {
			return usageErr("akam: trust-key list takes no argument")
		}
		return a.showTrust()
	case "add":
		if len(args) != 2 {
			return usageErr("akam: trust-key add needs one public key file: " + Prog + " trust-key add FILE.pub [--name LABEL]")
		}
		return a.trustAdd(tr, args[1], a.Opt.Name)
	case "remove":
		if len(args) != 2 {
			return usageErr("akam: trust-key remove needs the label shown by " + Prog + " trust-key list")
		}
		if a.UI.Dry() {
			a.UI.Say("  WOULD stop trusting the key " + ui.Sanitize(args[1]) + ".")
			return nil
		}
		ok, err := tr.Remove(args[1])
		if err != nil {
			return a.UI.Fail("cannot update the trusted keys: "+ui.Sanitize(err.Error())+".", "")
		}
		if !ok {
			return a.UI.Fail("no trusted key is labelled "+ui.Sanitize(args[1])+".", "see "+Prog+" trust-key list.")
		}
		a.UI.Good("  Removed. Kits signed by that key are refused again.")
		return nil
	}
	return usageErr("akam: trust-key takes: list, add FILE.pub [--name LABEL], or remove LABEL")
}

func (a *App) showTrust() error {
	a.UI.Say("  Official key: always trusted (built into this program).")
	ks, err := a.userTrust().List()
	if err != nil {
		return a.UI.Fail("your trusted keys file is damaged: "+ui.Sanitize(err.Error())+".", "fix or delete "+a.userTrust().Path+".")
	}
	if len(ks) == 0 {
		a.UI.Say("  Your trusted keys: none. Add one with " + Prog + " trust-key add FILE.pub")
		return nil
	}
	a.UI.Say("  Your trusted keys:")
	for _, k := range ks {
		a.UI.Say(fmt.Sprintf("    %-20s %s", ui.Sanitize(k.Label), k.Fingerprint))
	}
	return nil
}

func (a *App) trustAdd(tr *kit.UserTrust, file, label string) error {
	st, err := os.Stat(file)
	if err != nil || !st.Mode().IsRegular() {
		return a.UI.Fail("cannot read the public key file "+ui.Sanitize(file)+".", "give the .pub file made by ssh-keygen -t ed25519.")
	}
	if st.Size() > kit.MaxPubKeyFile {
		return a.UI.Fail(ui.Sanitize(file)+" is too large to be a public key.", "")
	}
	b, err := os.ReadFile(file)
	if err != nil {
		return a.UI.Fail("cannot read "+ui.Sanitize(file)+": "+ui.Sanitize(err.Error())+".", "")
	}
	key, comment, err := kit.ParsePubKeyFile(b)
	if err != nil {
		return a.UI.Fail("not accepted: "+ui.Sanitize(err.Error())+".", "")
	}
	fp := gossh.FingerprintSHA256(key)
	if label == "" {
		label = comment
	}
	if label == "" {
		label = "key-" + strings.Map(func(r rune) rune {
			if r >= '0' && r <= '9' || r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' {
				return r
			}
			return -1
		}, strings.TrimPrefix(fp, "SHA256:"))[:8]
	}
	if !kit.ValidLabel(label) {
		return a.UI.Fail("the label "+ui.Sanitize(label)+" is not usable.", "use --name LABEL: 1 to 40 letters, digits, '.', '_' or '-' (not 'official').")
	}
	a.UI.Say("  Key to trust: " + fp + "  (label " + label + ")")
	if a.UI.Dry() {
		a.UI.Say("  WOULD trust this key (after a yes/no). Nothing is written in a rehearsal.")
		return nil
	}
	if !a.Opt.Yes {
		ok, derr := a.UI.Danger("Trust a signing key", "Trust this key to sign firmware?", trustWarning, "Fingerprint: "+fp, "Label: "+label)
		if derr != nil || !ok {
			return a.UI.Fail("the key was not added.", "")
		}
	} else {
		a.UI.Warn(trustWarning)
	}
	if _, err := tr.Add(label, key); err != nil {
		return a.UI.Fail("not added: "+ui.Sanitize(err.Error())+".", "")
	}
	a.UI.Good("  Trusted: " + label + " " + fp + ". Kits signed by it are now accepted, and named 'your key " + label + "'.")
	return nil
}

// cmdReleaseSource is `release-source [show | owner/name | default]`.
func (a *App) cmdReleaseSource(args []string) error {
	if len(args) > 1 {
		return usageErr("akam: release-source takes one word: show, default, or owner/name")
	}
	word := "show"
	if len(args) == 1 {
		word = args[0]
	}
	switch {
	case word == "show":
		a.showReleaseSource()
		return nil
	case word == "default":
		if a.UI.Dry() {
			a.UI.Say("  WOULD go back to the official source " + kitRepo + ".")
			return nil
		}
		if err := kit.WriteSource(a.Cams.Base, ""); err != nil {
			return a.UI.Fail("cannot update the release source: "+ui.Sanitize(err.Error())+".", "")
		}
	case !kit.ValidRepo(word):
		return a.UI.Fail(fmt.Sprintf("'%s' is not a GitHub repository name.", ui.Sanitize(word)), "use owner/name, for example "+kitRepo+".")
	default:
		if a.UI.Dry() {
			a.UI.Say("  WOULD download releases from " + word + " from now on.")
			return nil
		}
		v := word
		if word == kitRepo {
			v = ""
		}
		if err := kit.WriteSource(a.Cams.Base, v); err != nil {
			return a.UI.Fail("cannot update the release source: "+ui.Sanitize(err.Error())+".", "")
		}
	}
	a.showReleaseSource()
	return nil
}

func (a *App) showReleaseSource() {
	a.UI.Say("  Release source: " + ui.Sanitize(a.repoText()))
	if a.Opt.Repo == "" {
		a.UI.Say("  (" + Prog + " release-source owner/name changes it; " + Prog + " release-source default goes back to the official one; --repo owner/name changes one run)")
	}
}

// checkRepoOption refuses a bad --repo (and a stray --name) before any command runs.
func (a *App) checkRepoOption(cmd string) error {
	if a.Opt.Name != "" && cmd != "trust-key" {
		return usageErr("akam: --name is an option of trust-key add (see " + Prog + " help)")
	}
	if a.Opt.Repo == "" {
		return nil
	}
	switch cmd {
	case "get-kit", "update", "appfs-build", "releases", "prepare-2k-sd", "step":
	default:
		return usageErr("akam: --repo is for get-kit, update, appfs-build and releases (see " + Prog + " help)")
	}
	if !kit.ValidRepo(a.Opt.Repo) {
		return usageErr(fmt.Sprintf("akam: --repo must be a GitHub owner/name, for example %s", kitRepo))
	}
	return nil
}

// flowTrust is the settings entry for the keys you trust and the release source.
func (a *App) flowTrust() {
	for {
		a.UI.Blank()
		a.UI.Bold("Your own firmware builds")
		_ = a.showTrust()
		a.showReleaseSource()
		i, err := a.UI.Pick("What do you want to do?", []string{"Trust a public key (.pub file)", "Stop trusting a key", "Change the release source", "Back"})
		if err != nil || i == 3 {
			return
		}
		switch i {
		case 0:
			f, perr := a.UI.Prompt("  Public key file (.pub; empty: cancel):", "")
			if perr != nil || f == "" {
				continue
			}
			_ = a.trustAdd(a.userTrust(), f, "")
		case 1:
			l, perr := a.UI.Prompt("  Label of the key to stop trusting (empty: cancel):", "")
			if perr != nil || l == "" {
				continue
			}
			_ = a.cmdTrustKey([]string{"remove", l})
		case 2:
			r, perr := a.UI.Prompt("  GitHub owner/name (default: the official source; empty: cancel):", "")
			if perr != nil || r == "" {
				continue
			}
			_ = a.cmdReleaseSource([]string{r})
		}
	}
}

// repoShort is the source in a title: the name alone for the official one, the full warning for another.
func (a *App) repoShort() string {
	if a.repoOfficial() {
		return a.repo()
	}
	return a.repoText()
}

package sdcard

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/crypto/ssh"
)

func pubKeyLine(t *testing.T) []byte {
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	k, err := ssh.NewPublicKey(pub)
	if err != nil {
		t.Fatal(err)
	}
	return []byte(strings.TrimSpace(string(ssh.MarshalAuthorizedKey(k))) + " test\n")
}

// synthToolkit is a tiny stand-in for the toolkit's sd_card tree.
func synthToolkit() map[string][]byte {
	return map[string][]byte{
		"hack.sh":                      []byte("#!/bin/sh\nSCRIPTS_DIR=/tmp/sd/custom/scripts"), // no trailing newline, like the real one
		"custom/configs/hack.conf":     []byte("# c\nTELNET=0\nTELNET_PORT=23\nOFFLINE=0\nOTHER=1\n"),
		"custom/configs/wifi.conf":     []byte("MODE=ap\n"),
		"custom/scripts/entrypoint.sh": []byte("#!/bin/sh\necho start\n"),
		"custom/scripts/other.sh":      []byte("echo other\n"),
		"custom/state/shadow.done":     []byte{},
		"shadow":                       []byte("root::0:0\n"),
		"logs/.gitkeep":                {},
	}
}

func TestPayloadEdits(t *testing.T) {
	files, err := BuildPayload(PayloadInput{Mode: ModeFirstBoot, Toolkit: synthToolkit(), PubKey: pubKeyLine(t), SSID: "My Net", Pass: "it's secret"})
	if err != nil {
		t.Fatal(err)
	}
	m := map[string]string{}
	for _, f := range files {
		m[f.Path] = string(f.Data)
	}
	if m["custom/configs/hack.conf"] != "# c\nTELNET=1\nTELNET_PORT=24\nOFFLINE=1\nOTHER=1\n" {
		t.Errorf("hack.conf %q", m["custom/configs/hack.conf"])
	}
	if m["custom/configs/wifi.conf"] != "MODE=sta\nSSID='My Net'\nPASS='it'\\''s secret'\nSECURITY=3\nDHCP=1\n" {
		t.Errorf("wifi.conf %q", m["custom/configs/wifi.conf"])
	}
	if !strings.HasPrefix(m["custom/scripts/entrypoint.sh"], "#!/bin/sh\n# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0\necho start\n") {
		t.Errorf("entrypoint %q", m["custom/scripts/entrypoint.sh"])
	}
	if !strings.HasSuffix(m["hack.sh"], "SCRIPTS_DIR=/tmp/sd/custom/scripts\n# DECLOUD_WIZARD_HOOK\n/bin/sh \"${SCRIPTS_DIR}/entrypoint.sh\" >/dev/null 2>&1 &\n") {
		t.Errorf("hack.sh %q", m["hack.sh"])
	}
	if _, ok := m["custom/state/shadow.done"]; ok {
		t.Error("shadow.done kept")
	}
	if _, ok := m["custom/state/dump.done"]; !ok {
		t.Error("dump.done missing")
	}
	if _, ok := m["AKAM_MAINTENANCE"]; !ok {
		t.Error("AKAM_MAINTENANCE missing")
	}
	// a backup card has no AKAM_MAINTENANCE and no key
	files, _ = BuildPayload(PayloadInput{Mode: ModeBackup, Toolkit: synthToolkit(), SSID: "a", Pass: "b"})
	for _, f := range files {
		if f.Path == "AKAM_MAINTENANCE" || f.Path == "akam/authorized_keys" {
			t.Errorf("backup card holds %s", f.Path)
		}
	}
}

func TestPayloadRefusals(t *testing.T) {
	tk := synthToolkit()
	cases := map[string]PayloadInput{
		"empty ssid":      {Mode: ModeBackup, Toolkit: tk, Pass: "x"},
		"newline":         {Mode: ModeBackup, Toolkit: tk, SSID: "a\nb", Pass: "x"},
		"bad mode":        {Mode: "flash-files", Toolkit: tk, SSID: "a", Pass: "b"},
		"no key":          {Mode: ModeFirstBoot, Toolkit: tk, SSID: "a", Pass: "b"},
		"private key":     {Mode: ModeFirstBoot, Toolkit: tk, SSID: "a", Pass: "b", PubKey: []byte("-----BEGIN OPENSSH "+"PRIVATE KEY-----\n")},
		"rsa key":         {Mode: ModeFirstBoot, Toolkit: tk, SSID: "a", Pass: "b", PubKey: []byte("ssh-rsa AAAA x\n")},
		"flash on backup": {Mode: ModeBackup, Toolkit: tk, SSID: "a", Pass: "b", Flash: &FlashFiles{}},
	}
	for n, in := range cases {
		if _, err := BuildPayload(in); err == nil {
			t.Errorf("%s: accepted", n)
		}
	}
	bin := synthToolkit()
	bin["evil.BIN"] = []byte("x")
	if _, err := BuildPayload(PayloadInput{Mode: ModeBackup, Toolkit: bin, SSID: "a", Pass: "b"}); err == nil || !strings.Contains(err.Error(), "*.bin") {
		t.Errorf("a *.bin at the root: %v", err)
	}
	noconf := synthToolkit()
	delete(noconf, "custom/configs/hack.conf")
	if _, err := BuildPayload(PayloadInput{Mode: ModeBackup, Toolkit: noconf, SSID: "a", Pass: "b"}); err == nil {
		t.Error("no hack.conf accepted")
	}
}

func flashFiles() *FlashFiles {
	sc := map[string][]byte{}
	for _, n := range FlashScriptNames {
		sc[n] = []byte("#!/bin/sh\n# " + n + "\n")
	}
	return &FlashFiles{ImageName: "ak_rtsp_firmware_20251111_101010.squashfs", Image: rnd(5242880), FlashTool: rnd(1000), Scripts: sc, MTD7: rnd(MTD7Size)}
}

// A whole first-boot + flash card is written to a file, read back and verified; damage is detected.
func TestCardRoundTripAndVerify(t *testing.T) {
	tk := synthToolkit()
	files, err := BuildPayload(PayloadInput{Mode: ModeFirstBoot, Toolkit: tk, PubKey: pubKeyLine(t), SSID: "n", Pass: "p", Flash: flashFiles()})
	if err != nil {
		t.Fatal(err)
	}
	const size = 256 << 20
	f, _ := sparse(t, size)
	off, ps, err := FormatMBR(f, size)
	if err != nil {
		t.Fatal(err)
	}
	if err := WriteFAT32(f, off, ps, files, WriteOptions{}); err != nil {
		t.Fatal(err)
	}
	card, err := OpenCard(f)
	if err != nil {
		t.Fatal(err)
	}
	spec := VerifySpec{Mode: ModeFirstBoot, Flash: true, Toolkit: ToolkitHashes(tk)}
	lines, bad := VerifyTree(card, spec)
	if bad != 0 || lines[len(lines)-1] != "VERIFY: OK" {
		t.Fatalf("bad=%d\n%s", bad, strings.Join(lines, "\n"))
	}
	// a backup-card verification of a first-boot card must fail (AKAM_MAINTENANCE present)
	_, bad = VerifyTree(card, VerifySpec{Mode: ModeBackup})
	if bad == 0 {
		t.Error("first-boot card verified as a backup card")
	}
	// a changed toolkit file is detected
	spec.Toolkit["custom/scripts/other.sh"] = strings.Repeat("0", 64)
	if _, bad := VerifyTree(card, spec); bad == 0 {
		t.Error("a toolkit file mismatch went unnoticed")
	}
}

func TestToolkitTreeHashParityWithBash(t *testing.T) {
	home, _ := os.UserHomeDir()
	tk := filepath.Join(home, "akam-sdprep", "toolkit")
	if _, err := os.Stat(filepath.Join(tk, "sd_card")); err != nil {
		t.Skip("no toolkit checkout at " + tk)
	}
	files, err := ReadTree(filepath.Join(tk, "sd_card"))
	if err != nil {
		t.Fatal(err)
	}
	want := "3aa3f698fd6d6f228ee785726970f708035799aa5bbcb73ef369bc3757747c2f"
	if got := TreeHash(files); got != want {
		t.Skipf("local toolkit is not the pinned commit (%s)", got) // parity of the algorithm is tested below
	}
	if _, err := LoadToolkit(tk); err != nil {
		t.Error(err)
	}
}

// The same tree hash as `find | sort | xargs sha256sum | sha256sum`, on a synthetic tree.
func TestTreeHashAlgorithmMatchesShell(t *testing.T) {
	if _, err := exec.LookPath("sha256sum"); err != nil {
		t.Skip("no sha256sum")
	}
	d := t.TempDir()
	tree := map[string][]byte{"a.txt": []byte("a"), "B/c d.txt": []byte("c"), "b": []byte("bb"), "Z": []byte("z"), "a/x": []byte("x")}
	for p, b := range tree {
		os.MkdirAll(filepath.Dir(filepath.Join(d, p)), 0o755)
		os.WriteFile(filepath.Join(d, p), b, 0o644)
	}
	script := `cd "$1" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs -d '\n' sha256sum | sha256sum | cut -d' ' -f1`
	out, err := exec.Command("bash", "-c", script, "x", d).Output()
	if err != nil {
		t.Fatal(err)
	}
	if got := TreeHash(tree); got != strings.TrimSpace(string(out)) {
		t.Errorf("go %s, shell %s", got, out)
	}
}

// When the toolkit and the bash tool are here, the staged tree of sd_card.sh and ours are identical.
func TestPayloadParityWithBash(t *testing.T) {
	home, _ := os.UserHomeDir()
	tk := filepath.Join(home, "akam-sdprep", "toolkit")
	if _, err := os.Stat(filepath.Join(tk, "sd_card")); err != nil {
		t.Skip("no toolkit checkout")
	}
	files, err := ReadTree(filepath.Join(tk, "sd_card"))
	if err != nil {
		t.Fatal(err)
	}
	pin := TreeHash(files)
	repo, _ := filepath.Abs(filepath.Join("..", "..", "..", ".."))
	pub := filepath.Join(t.TempDir(), "k.pub")
	line := pubKeyLine(t)
	os.WriteFile(pub, line, 0o644)
	for _, mode := range []string{ModeBackup, ModeFirstBoot} {
		stage := filepath.Join(t.TempDir(), "stage")
		args := []string{filepath.Join(repo, "tools/sd/sd_card.sh"), "prepare", "--mode", mode, "--dry-run", "--stage-dir", stage, "--toolkit", tk}
		if mode == ModeFirstBoot {
			args = append(args, "--pubkey", pub)
		}
		cmd := exec.Command("bash", args...)
		cmd.Env = append(os.Environ(), "AKAM_WIFI_SSID=Home Net", "AKAM_WIFI_PASS=pa'ss word", "AKAM_TEST_DRYRUN_ONLY=1", "AKAM_TEST_PIN_TREE_SHA="+pin)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Skipf("bash stage not possible here: %v\n%s", err, out)
		}
		want, err := ReadTree(stage)
		if err != nil {
			t.Fatal(err)
		}
		in := PayloadInput{Mode: mode, Toolkit: files, SSID: "Home Net", Pass: "pa'ss word"}
		if mode == ModeFirstBoot {
			in.PubKey = line
		}
		got, err := BuildPayload(in)
		if err != nil {
			t.Fatal(err)
		}
		have := map[string][]byte{}
		for _, f := range got {
			if !f.Dir {
				have[f.Path] = f.Data
			}
		}
		for p, b := range want {
			if g, ok := have[p]; !ok || !bytes.Equal(g, b) {
				t.Errorf("%s: %s differs from the bash tool's staged file", mode, p)
			}
		}
		for p := range have {
			if _, ok := want[p]; !ok {
				t.Errorf("%s: %s is not in the bash tool's tree", mode, p)
			}
		}
	}
}

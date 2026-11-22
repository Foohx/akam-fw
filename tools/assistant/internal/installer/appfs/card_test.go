package appfs

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/nativeapp/sdcard"
)

const testPub = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGv3x6q5h1c0uQkq2m2k4n7yTq8o0VZr1d5mH3Qb9W2L test@pc\n"

func cardKit(t *testing.T) *Kit {
	t.Helper()
	f := kitFiles()
	f["card/yy_debug.ini"] = []byte("mode=debug\nstart_sh=/mnt/sdcard/akam_boot.sh")
	f["card/akam_boot.sh"] = []byte("#!/bin/sh\n# akam_boot\n")
	f["card/akam/enable_local.sh"] = []byte("#!/bin/sh\n# enable_local\n")
	f["card/akam/restore_param.sh"] = []byte("#!/bin/sh\n# restore_param\n")
	f["licenses/LICENSE.dropbear"] = []byte("MIT\n")
	useBusyBox(t, f) // the card builder checks the pin, like the image builder
	return mustKit(t, sealKit(f))
}

func TestWPAConfUsesTheStandardPSK(t *testing.T) {
	// the IEEE 802.11i test vector (passphrase "password", SSID "IEEE")
	b, err := WPAConf("IEEE", "password")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(b), "psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e\n") {
		t.Fatalf("psk wrong:\n%s", b)
	}
	if strings.Contains(string(b), "password") || !strings.Contains(string(b), "ssid=49454545\n") {
		t.Fatalf("the passphrase is on the card, or the ssid is not hex:\n%s", b)
	}
	for name, c := range map[string][2]string{
		"empty ssid": {"", "password1"}, "long ssid": {strings.Repeat("a", 33), "password1"}, "short pass": {"x", "short"},
		"long pass": {"x", strings.Repeat("p", 64)}, "newline": {"x", "pass\nword1"}, "control": {"x\x01", "password1"},
	} {
		if _, err := WPAConf(c[0], c[1]); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
}

func TestBuildCardTreeAndVerify(t *testing.T) {
	k := cardKit(t)
	c, err := BuildCard(k, CardInput{PubKey: []byte(testPub), SSID: "home", Pass: "correct horse", Decloud: true})
	if err != nil {
		t.Fatal(err)
	}
	have := map[string][]byte{}
	for _, f := range c.Files {
		have[f.Path] = f.Data
	}
	for _, p := range []string{"yy_debug.ini", "akam_boot.sh", "busybox", "akam/own_app", "akam/decloud", "akam/wpa.conf", "akam/authorized_keys",
		"akam/own_app_stub.sh", "akam/common.sh", "akam/akam_ntp.sh", "akam/akam_hostname.sh", "akam/dropbearmulti", "akam/bin/akam2k_rtsp",
		"akam/bin/akam2k_sntp.BUILDINFO", "akam/enable_local.sh", "akam/restore_param.sh", "akam/LICENSE.dropbear", "akam/no_telnet"} {
		if _, ok := have[p]; !ok {
			t.Errorf("card lacks %s", p)
		}
	}
	for _, p := range []string{"akam/akam_flash_boot.sh", "akam/enable_param_conf"} {
		if _, ok := have[p]; ok {
			t.Errorf("card has %s", p)
		}
	}
	for p := range have {
		low := strings.ToLower(p)
		if strings.HasSuffix(low, ".bin") || strings.HasSuffix(low, ".zip") || strings.Contains(low, "yy_upgrade") || strings.Contains(low, "fac_av130") {
			t.Errorf("a firmware-like file on the card: %s", p)
		}
	}
	if !bytes.Contains(have["akam/bin/akam2k_sntp.BUILDINFO"], []byte("tool=akam2k_sntp\nsha256="+shaHex(k.File(pathBin+"akam2k_sntp")))) {
		t.Error("sntp BUILDINFO")
	}
	// written to a directory and verified with the card verifier: ok, then a changed file and a trigger fail it
	dir := t.TempDir()
	for _, f := range c.Files {
		full := filepath.Join(dir, filepath.FromSlash(f.Path))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, f.Data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	lines, bad := sdcard.VerifyTree(sdcard.DirTree{Root: dir}, c.Verify)
	if bad != 0 {
		t.Fatalf("verify: %v", lines)
	}
	if err := os.WriteFile(filepath.Join(dir, "akam", "common.sh"), []byte("tampered"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, bad := sdcard.VerifyTree(sdcard.DirTree{Root: dir}, c.Verify); bad != 1 {
		t.Fatalf("a changed file: %d problems", bad)
	}
	_ = os.WriteFile(filepath.Join(dir, "akam", "common.sh"), have["akam/common.sh"], 0o644)
	if err := os.WriteFile(filepath.Join(dir, "fac_av130.zip"), []byte("z"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, bad := sdcard.VerifyTree(sdcard.DirTree{Root: dir}, c.Verify); bad == 0 {
		t.Fatal("a factory trigger passed the verification")
	}
}

func TestBuildCardRefusals(t *testing.T) {
	in := CardInput{PubKey: []byte(testPub), SSID: "home", Pass: "correct horse"}
	if _, err := BuildCard(mustKit(t, kitFiles()), in); err == nil || !strings.Contains(err.Error(), "no SD card files") {
		t.Errorf("kit without card files: %v", err)
	}
	k := cardKit(t)
	bad := in
	bad.PubKey = []byte("-----BEGIN OPENSSH " + "PRIVATE KEY-----\nx\n")
	if _, err := BuildCard(k, bad); err == nil {
		t.Error("a private key was accepted")
	}
	bad = in
	bad.Pass = "short"
	if _, err := BuildCard(k, bad); err == nil {
		t.Error("a short password was accepted")
	}
	f := kitFiles()
	f["card/yy_debug.ini"] = []byte("mode=factory\nstart_sh=/mnt/sdcard/akam_boot.sh")
	f["card/akam_boot.sh"], f["card/akam/enable_local.sh"], f["card/akam/restore_param.sh"] = []byte("x"), []byte("x"), []byte("x")
	useBusyBox(t, f)
	if _, err := BuildCard(mustKit(t, sealKit(f)), in); err == nil || !strings.Contains(err.Error(), "yy_debug.ini") {
		t.Errorf("factory mode: %v", err)
	}
	// a partial card set is refused by the kit itself
	g := kitFiles()
	g["card/yy_debug.ini"] = []byte("mode=debug\nstart_sh=/mnt/sdcard/akam_boot.sh")
	if _, err := NewKit(sealKit(g)); err == nil || !strings.Contains(err.Error(), "only some of the SD card files") {
		t.Errorf("partial card files: %v", err)
	}
}

// The card carries the flag akam/no_telnet by default (the SSH key is on the card, telnet is not needed); only a
// rescue card asked for with RescueTelnet keeps the passwordless telnet of the boot script.
func TestBuildCardTelnetFlag(t *testing.T) {
	k := cardKit(t)
	for _, rescue := range []bool{false, true} {
		c, err := BuildCard(k, CardInput{PubKey: []byte(testPub), SSID: "home", Pass: "correct horse", RescueTelnet: rescue})
		if err != nil {
			t.Fatal(err)
		}
		has := false
		for _, f := range c.Files {
			if f.Path == "akam/no_telnet" {
				has = true
			}
		}
		if has == rescue {
			t.Errorf("RescueTelnet=%v but akam/no_telnet present=%v", rescue, has)
		}
	}
}

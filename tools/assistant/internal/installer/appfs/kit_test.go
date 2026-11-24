package appfs

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestKitAcceptsTheFixture(t *testing.T) {
	k, err := NewKit(kitFiles())
	if err != nil {
		t.Fatal(err)
	}
	if k.Version() != "0.2.0" || k.Commit() != "abc123" {
		t.Errorf("%q %q", k.Version(), k.Commit())
	}
}

func TestKitRefusals(t *testing.T) {
	cases := map[string]struct {
		mutate func(map[string][]byte)
		seal   bool
		want   string
	}{
		"a required file is missing":      {func(f map[string][]byte) { delete(f, pathBin+"akam-sntp") }, false, "has no appfs/bin/akam-sntp"},
		"an unknown file":                 {func(f map[string][]byte) { f["appfs/overlay/evil.sh"] = []byte("x") }, false, "kit newer than the program"},
		"a vendor-looking file":           {func(f map[string][]byte) { f["appfs/bin/yy_cam"] = []byte("x") }, false, "does not install"},
		"a file changed after SHA256SUMS": {func(f map[string][]byte) { f[pathOverlay+"common.sh"] = []byte("#!/bin/sh\nrm -rf /\n") }, false, "does not match SHA256SUMS"},
		"SHA256SUMS lacks a file": {func(f map[string][]byte) {
			f[pathSums] = bytes.ReplaceAll(f[pathSums], []byte(pathBin+"busybox\n"), []byte("appfs/bin/gone\n"))
		}, false, "does not list appfs/bin/busybox"},
		"SHA256SUMS lists a file that is not there": {func(f map[string][]byte) {
			f[pathSums] = append(f[pathSums], []byte(strings.Repeat("a", 64)+"  appfs/bin/extra\n")...)
		}, false, "which is not in the kit"},
		"malformed SHA256SUMS": {func(f map[string][]byte) { f[pathSums] = []byte("garbage\n") }, false, "malformed"},
		"uppercase hash":       {func(f map[string][]byte) { f[pathSums] = []byte(strings.Repeat("A", 64) + "  MANIFEST.txt\n") }, false, "malformed"},
		"duplicate line": {func(f map[string][]byte) {
			f[pathSums] = append(f[pathSums], bytes.SplitN(f[pathSums], []byte("\n"), 2)[0]...)
			f[pathSums] = append(f[pathSums], '\n')
		}, false, "twice"},
		"wrong kind": {func(f map[string][]byte) {
			f[pathManifest] = bytes.Replace(f[pathManifest], []byte("kind=cardless-appfs"), []byte("kind=native-app"), 1)
		}, true, "not an appfs kit"},
		"no kind": {func(f map[string][]byte) { f[pathManifest] = []byte("time64_gate=pass\n") }, true, "not an appfs kit"},
		"time64 gate failed": {func(f map[string][]byte) {
			f[pathManifest] = bytes.Replace(f[pathManifest], []byte("time64_gate=pass"), []byte("time64_gate=fail"), 1)
		}, true, "time64_gate=pass"},
		"no time64 gate": {func(f map[string][]byte) {
			f[pathManifest] = bytes.Replace(f[pathManifest], []byte("time64_gate=pass\n"), nil, 1)
		}, true, "time64_gate=pass"},
		"two time64 gates": {func(f map[string][]byte) { f[pathManifest] = append(f[pathManifest], []byte("time64_gate=pass\n")...) }, true, "time64_gate=pass"},
	}
	for name, c := range cases {
		t.Run(name, func(t *testing.T) {
			f := kitFiles()
			c.mutate(f)
			if c.seal {
				sealKit(f)
			}
			_, err := NewKit(f)
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("got %v, want %q", err, c.want)
			}
		})
	}
}

func TestLoadKitFromADirectory(t *testing.T) {
	dir := t.TempDir()
	for p, b := range kitFiles() {
		full := filepath.Join(dir, filepath.FromSlash(p))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := LoadKit(dir); err != nil {
		t.Fatal(err)
	}
	// an extra file on disk is a kit newer than the program
	extra := filepath.Join(dir, "appfs", "bin", "new_tool")
	if err := os.WriteFile(extra, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadKit(dir); err == nil || !strings.Contains(err.Error(), "kit newer than the program") {
		t.Fatalf("got %v", err)
	}
	_ = os.Remove(extra)
	_ = os.Remove(filepath.Join(dir, "appfs", "bin", "busybox"))
	if _, err := LoadKit(dir); err == nil || !strings.Contains(err.Error(), "has no appfs/bin/busybox") {
		t.Fatalf("got %v", err)
	}
	if _, err := LoadKit(filepath.Join(dir, "nope")); err == nil {
		t.Fatal("a missing kit dir was accepted")
	}
}

func TestOutputsRoundTripAndNoOverwrite(t *testing.T) {
	f := kitFiles()
	useBusyBox(t, f)
	res, err := build(t, stockAppfs(t, stockInit), f)
	if err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(t.TempDir(), "out")
	if err := WriteOutputs(dir, res); err != nil {
		t.Fatal(err)
	}
	img, sha, sign, err := ReadImageDir(dir)
	if err != nil || !bytes.Equal(img, res.Image) || sha != res.SHA256 || sign != res.Sign {
		t.Fatalf("%v", err)
	}
	if err := WriteOutputs(dir, res); err == nil {
		t.Fatal("an existing image was overwritten")
	}
	if _, _, _, err := ReadImageDir(t.TempDir()); err == nil {
		t.Fatal("an empty directory was read")
	}
}

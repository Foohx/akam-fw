package kit_test

import (
	"archive/tar"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
)

func verify(t *testing.T, b kittest.Built, mod func(*kit.Options)) (*kit.Result, error, []string) {
	t.Helper()
	var said []string
	o := kit.Options{AllowedSigners: b.AllowedSigners, Say: func(s string) { said = append(said, s) }, Warn: func(s string) { said = append(said, "WARN "+s) }}
	if mod != nil {
		mod(&o)
	}
	r, err := kit.VerifyTarball(b.Tarball, o)
	return r, err, said
}

func wantFail(t *testing.T, err error, sub string) {
	t.Helper()
	var f *kit.Fail
	if !errors.As(err, &f) {
		t.Fatalf("want a verification failure containing %q, got %v", sub, err)
	}
	if !strings.Contains(f.Msg, sub) {
		t.Fatalf("failure %q does not contain %q", f.Msg, sub)
	}
}

func TestSignedKitVerifies(t *testing.T) {
	for _, alg := range []string{"sha256", "sha512"} {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{HashAlg: alg})
		r, err, said := verify(t, b, nil)
		if err != nil {
			t.Fatalf("%s: %v", alg, err)
		}
		defer r.Close()
		if !r.Signed || !strings.Contains(r.SignedBy, `Good "akam-release" signature for akam-release with ED25519 key SHA256:`) {
			t.Errorf("%s: signer text %q", alg, r.SignedBy)
		}
		joined := strings.Join(said, "\n")
		for _, w := range []string{"ok: akam-kit-1.0.0.tar.xz sha256 ", "ok: signature valid (", "ok: archive layout is safe", "ok: kit_format 1 is supported", "MANIFEST entries match (akam_version 1.0.0, source 0123456789ab)"} {
			if !strings.Contains(joined, w) {
				t.Errorf("missing %q in:\n%s", w, joined)
			}
		}
		if r.Manifest["akam_version"] != "1.0.0" {
			t.Errorf("manifest not returned: %v", r.Manifest)
		}
		if _, err := os.Stat(filepath.Join(r.Dir, "bin", "flash_tool")); err != nil {
			t.Error(err)
		}
	}
}

func TestExtractTo(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	to := filepath.Join(t.TempDir(), "kit")
	r, err, _ := verify(t, b, func(o *kit.Options) { o.ExtractTo = to })
	if err != nil {
		t.Fatal(err)
	}
	r.Close()
	if st, err := os.Stat(filepath.Join(to, "bin", "flash_tool")); err != nil || st.Mode().Perm() != 0o755 {
		t.Errorf("flash_tool not extracted with mode 755: %v %v", st, err)
	}
	if l, err := os.Readlink(filepath.Join(to, "overlay", "bin", "dropbear")); err != nil || l != "dropbearmulti" {
		t.Errorf("symlink: %q %v", l, err)
	}
	// a second run into the same place is refused
	_, err, _ = verify(t, b, func(o *kit.Options) { o.ExtractTo = to })
	wantFail(t, err, "already exists")
}

func TestUnsigned(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{Unsigned: true})
	_, err, said := verify(t, b, nil)
	if !errors.Is(err, kit.ErrUnsigned) {
		t.Fatalf("want ErrUnsigned, got %v", err)
	}
	if !strings.Contains(strings.Join(said, "\n"), "WARN no signature (") {
		t.Errorf("no warning: %v", said)
	}
	r, err, _ := verify(t, b, func(o *kit.Options) { o.AllowUnsigned = true })
	if err != nil || r.Signed {
		t.Fatalf("allow-unsigned: %v signed=%v", err, r != nil && r.Signed)
	}
	r.Close()
	// an explicitly named signature must exist
	_, err, _ = verify(t, b, func(o *kit.Options) { o.Sig = filepath.Join(b.Dir, "nope.sig"); o.SigGiven = true })
	wantFail(t, err, "an explicitly named signature must exist")
}

func TestSignatureRefusals(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	// the placeholder file of today (comments only): a signed kit is refused
	_, err, _ := verify(t, b, func(o *kit.Options) { o.AllowedSigners = []byte("# placeholder\n\n") })
	wantFail(t, err, "contains no public key (placeholder not replaced)")
	// a signature from another key
	other := kittest.NewSigner(t)
	_, err, _ = verify(t, b, func(o *kit.Options) { o.AllowedSigners = kittest.AllowedLine(other) })
	wantFail(t, err, "INVALID signature")
	// the right key but only for another namespace
	wrongNS := []byte(strings.Replace(string(b.AllowedSigners), `namespaces="akam-release"`, `namespaces="other"`, 1))
	_, err, _ = verify(t, b, func(o *kit.Options) { o.AllowedSigners = wrongNS })
	wantFail(t, err, "INVALID signature")
	// the right key under another principal
	wrongP := []byte(strings.Replace(string(b.AllowedSigners), "akam-release ", "someone ", 1))
	_, err, _ = verify(t, b, func(o *kit.Options) { o.AllowedSigners = wrongP })
	wantFail(t, err, "INVALID signature")
	// a tampered SHA256SUMS no longer matches its signature
	sums, _ := os.ReadFile(b.Sums)
	// keep the digest valid but change the layout of the file
	if err := os.WriteFile(b.Sums, append(sums, []byte("\n# x\n")...), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err, _ = verify(t, b, nil)
	wantFail(t, err, "INVALID signature")
	// garbage signature
	if err := os.WriteFile(b.Sums, sums, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(b.Sig, []byte("not a signature\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err, _ = verify(t, b, nil)
	wantFail(t, err, "INVALID signature")
}

func TestChecksumRefusals(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{BadSumsLine: true})
	_, err, _ := verify(t, b, nil)
	wantFail(t, err, "sha256 mismatch for akam-kit-1.0.0.tar.xz")

	b = kittest.Build(t, t.TempDir(), kittest.Spec{})
	if err := os.WriteFile(b.Sums, []byte(strings.Repeat("0", 64)+"  other.tar.xz\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err, _ = verify(t, b, nil)
	wantFail(t, err, "is not listed in")

	b = kittest.Build(t, t.TempDir(), kittest.Spec{})
	sums, _ := os.ReadFile(b.Sums)
	_ = os.WriteFile(b.Sums, append(sums, sums...), 0o644)
	_, err, _ = verify(t, b, func(o *kit.Options) { o.AllowUnsigned = true })
	wantFail(t, err, "is listed more than once")

	b = kittest.Build(t, t.TempDir(), kittest.Spec{})
	_ = os.Remove(b.Sums)
	_, err, _ = verify(t, b, nil)
	wantFail(t, err, "not found (download SHA256SUMS next to the kit)")
}

func TestExpectSHA256(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	r, err, _ := verify(t, b, func(o *kit.Options) { o.Expect = strings.ToUpper(b.TarSHA[:10]) })
	if err != nil {
		t.Fatal(err)
	}
	r.Close()
	_, err, _ = verify(t, b, func(o *kit.Options) { o.Expect = "deadbeefdead" })
	wantFail(t, err, "does not start with --expect-sha256")
	_, err, _ = verify(t, b, func(o *kit.Options) { o.Expect = "abc" })
	var u *kit.Usage
	if !errors.As(err, &u) || !strings.Contains(u.Msg, "at least 8 hex digits") {
		t.Fatalf("short expect: %v", err)
	}
	_, err, _ = verify(t, b, func(o *kit.Options) { o.Expect = "zzzzzzzzzz" })
	if !errors.As(err, &u) || !strings.Contains(u.Msg, "needs hex digits") {
		t.Fatalf("non-hex expect: %v", err)
	}
}

func TestArchiveRefusals(t *testing.T) {
	cases := []struct {
		name string
		mut  func(*[]kittest.Entry)
		want string
	}{
		{"absolute path", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "/etc/passwd", Type: tar.TypeReg, Mode: 0o644})
		}, "absolute or '..' paths"},
		{"dotdot", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/../x", Type: tar.TypeReg, Mode: 0o644})
		}, "absolute or '..' paths"},
		{"space in a name", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/a b", Type: tar.TypeReg, Mode: 0o644})
		}, "character outside"},
		{"entry outside top", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "other/x", Type: tar.TypeReg, Mode: 0o644})
		}, "entries outside akam-kit-1.0.0/"},
		{"duplicate", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/MANIFEST.txt", Type: tar.TypeReg, Mode: 0o644})
		}, "duplicate entries"},
		{"setuid file", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/x", Type: tar.TypeReg, Mode: 0o4755})
		}, "unexpected file mode"},
		{"world writable file", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/x", Type: tar.TypeReg, Mode: 0o666})
		}, "unexpected file mode"},
		{"dir mode", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/d/", Type: tar.TypeDir, Mode: 0o777})
		}, "unexpected directory mode"},
		{"hard link", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/h", Type: tar.TypeLink, Mode: 0o644, Linkname: "akam-kit-1.0.0/MANIFEST.txt"})
		}, "unexpected archive entry"},
		{"device node", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/dev", Type: tar.TypeChar, Mode: 0o644})
		}, "unexpected archive entry"},
		{"other symlink", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/evil", Type: tar.TypeSymlink, Mode: 0o777, Linkname: "/etc/passwd"})
		}, "unexpected archive entry"},
		{"dropbear link elsewhere", func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/overlay/bin/dropbearkey", Type: tar.TypeSymlink, Mode: 0o777, Linkname: "../../etc/shadow"})
		}, "unexpected archive entry"},
		{"wrong top dir", func(e *[]kittest.Entry) {
			for i := range *e {
				(*e)[i].Name = strings.Replace((*e)[i].Name, "akam-kit-", "nokit-", 1)
				(*e)[i].Linkname = strings.Replace((*e)[i].Linkname, "akam-kit-", "nokit-", 1)
			}
		}, "is not akam-kit-*"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			b := kittest.Build(t, t.TempDir(), kittest.Spec{TarMutate: c.mut})
			_, err, _ := verify(t, b, nil)
			wantFail(t, err, c.want)
		})
	}
}

func TestManifestRefusals(t *testing.T) {
	t.Run("tampered file", func(t *testing.T) {
		// the file content differs from MANIFEST and SHA256SUMS (made after the digests were taken)
		b := kittest.Build(t, t.TempDir(), kittest.Spec{TarMutate: func(e *[]kittest.Entry) {
			for i := range *e {
				if strings.HasSuffix((*e)[i].Name, "bin/ak_rtsp") {
					(*e)[i].Body = []byte("evil\n")
				}
			}
		}})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "SHA256SUMS check failed")
	})
	t.Run("extra file", func(t *testing.T) {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{TarMutate: func(e *[]kittest.Entry) {
			*e = append(*e, kittest.Entry{Name: "akam-kit-1.0.0/extra.txt", Type: tar.TypeReg, Mode: 0o644, Body: []byte("x")})
		}})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "files differ from SHA256SUMS")
	})
	t.Run("unsupported format", func(t *testing.T) {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{Format: "2"})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "kit_format 2 is not supported")
	})
	t.Run("test grade", func(t *testing.T) {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{Grade: "test"})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "not a release kit")
		r, err, said := verify(t, b, func(o *kit.Options) { o.AllowTest = true })
		if err != nil {
			t.Fatal(err)
		}
		r.Close()
		if !strings.Contains(strings.Join(said, "\n"), "WARN kit_grade=test") {
			t.Error("no test-kit warning")
		}
		b = kittest.Build(t, t.TempDir(), kittest.Spec{Grade: "weird"})
		_, err, _ = verify(t, b, func(o *kit.Options) { o.AllowTest = true })
		wantFail(t, err, "not a release kit")
	})
	t.Run("version differs from directory", func(t *testing.T) {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{ManifestExtra: "akam_version=9.9.9\n"})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "no (single) akam_version")
	})
	t.Run("missing payload", func(t *testing.T) {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{Files: map[string][]byte{"bin/flash_tool": nil}})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "required file bin/flash_tool is not in the kit")
	})
	t.Run("no validated hardware", func(t *testing.T) {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{Files: map[string][]byte{"supported_hardware.txt": []byte("untested|x\n")}})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "lists no validated target")
	})
	t.Run("time64 gate", func(t *testing.T) {
		b := kittest.Build(t, t.TempDir(), kittest.Spec{ManifestExtra: "time64_gate=fail\n"})
		_, err, _ := verify(t, b, nil)
		wantFail(t, err, "exactly one time64_gate line")
	})
}

func TestDirectoryMode(t *testing.T) {
	b := kittest.Build(t, t.TempDir(), kittest.Spec{})
	to := filepath.Join(t.TempDir(), "kit")
	r, err, _ := verify(t, b, func(o *kit.Options) { o.ExtractTo = to })
	if err != nil {
		t.Fatal(err)
	}
	r.Close()
	o := kit.Options{}
	if _, err := o.VerifyExtracted(to); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(to, "bin", "ak_rtsp"), []byte("changed"), 0o755); err != nil {
		t.Fatal(err)
	}
	_, err = o.VerifyExtracted(to)
	wantFail(t, err, "SHA256SUMS check failed")
}

// The signature scheme is OpenSSH's SSHSIG: it must interoperate with ssh-keygen -Y in both
// directions (skipped where ssh-keygen is not installed; the files live in a private temp dir).
func TestSSHSIGInteropWithSSHKeygen(t *testing.T) {
	sk, err := exec.LookPath("ssh-keygen")
	if err != nil {
		t.Skip("skip: ssh-keygen is not installed")
	}
	dir := t.TempDir()
	key := filepath.Join(dir, "k")
	if out, err := exec.Command(sk, "-q", "-t", "ed25519", "-N", "", "-C", "t", "-f", key).CombinedOutput(); err != nil {
		t.Skipf("skip: cannot generate a key: %v %s", err, out)
	}
	pub, _ := os.ReadFile(key + ".pub")
	allowed := []byte("akam-release namespaces=\"akam-release\" " + string(pub))
	msg := filepath.Join(dir, "SHA256SUMS")
	if err := os.WriteFile(msg, []byte("abc  file\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	// ssh-keygen signs, Go verifies
	if out, err := exec.Command(sk, "-Y", "sign", "-f", key, "-n", "akam-release", msg).CombinedOutput(); err != nil {
		t.Fatalf("sign: %v %s", err, out)
	}
	sigb, _ := os.ReadFile(msg + ".sig")
	sig, err := kit.ParseSignature(sigb)
	if err != nil {
		t.Fatal(err)
	}
	al, err := kit.ParseAllowedSigners(allowed)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := kit.Verify(al, "akam-release", "akam-release", sig, []byte("abc  file\n")); err != nil {
		t.Fatalf("Go refuses a ssh-keygen signature: %v", err)
	}
	if _, err := kit.Verify(al, "akam-release", "akam-release", sig, []byte("abc  filf\n")); err == nil {
		t.Fatal("Go accepts a modified message")
	}
	// Go signs, ssh-keygen verifies
	signer := kittest.NewSigner(t)
	gsig := kit.SignForTest(signer, "akam-release", "sha512", []byte("hello\n"))
	_ = os.WriteFile(filepath.Join(dir, "g.sig"), gsig, 0o644)
	_ = os.WriteFile(filepath.Join(dir, "g.allowed"), kittest.AllowedLine(signer), 0o644)
	cmd := exec.Command(sk, "-Y", "verify", "-f", filepath.Join(dir, "g.allowed"), "-I", "akam-release", "-n", "akam-release", "-s", filepath.Join(dir, "g.sig"))
	cmd.Stdin = strings.NewReader("hello\n")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("ssh-keygen refuses a Go signature: %v %s", err, out)
	} else if !strings.Contains(string(out), "Good \"akam-release\" signature for akam-release") {
		t.Fatalf("unexpected ssh-keygen text %q", out)
	}
	// the text we print equals ssh-keygen's for the same signature
	a2, _ := kit.ParseAllowedSigners(kittest.AllowedLine(signer))
	s2, _ := kit.ParseSignature(gsig)
	ours, err := kit.Verify(a2, "akam-release", "akam-release", s2, []byte("hello\n"))
	if err != nil {
		t.Fatal(err)
	}
	cmd = exec.Command(sk, "-Y", "verify", "-f", filepath.Join(dir, "g.allowed"), "-I", "akam-release", "-n", "akam-release", "-s", filepath.Join(dir, "g.sig"))
	cmd.Stdin = strings.NewReader("hello\n")
	theirs, _ := cmd.Output()
	if strings.TrimSpace(string(theirs)) != ours {
		t.Errorf("text differs:\n ours:   %s\n theirs: %s", ours, theirs)
	}
}

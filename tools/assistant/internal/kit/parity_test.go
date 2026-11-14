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

// Parity with the bash verifier: the same kits get the same verdict (exit status 0 verified, 1
// refused, 3 unsigned) from tools/release/verify_kit.sh and from this package. The script is only
// run here, on kits built in a private temporary directory; it needs bash, tar, xz, sha256sum and
// ssh-keygen and the test is skipped without them.
func TestParityWithVerifyKitSh(t *testing.T) {
	script := filepath.Join("..", "..", "..", "release", "verify_kit.sh")
	if _, err := os.Stat(script); err != nil {
		t.Skip("skip: not in a source checkout")
	}
	for _, tool := range []string{"bash", "tar", "xz", "sha256sum", "ssh-keygen", "find", "awk"} {
		if _, err := exec.LookPath(tool); err != nil {
			t.Skipf("skip: %s is not installed", tool)
		}
	}
	add := func(e kittest.Entry) func(*[]kittest.Entry) {
		return func(l *[]kittest.Entry) { *l = append(*l, e) }
	}
	type tc struct {
		name  string
		spec  kittest.Spec
		flags []string // for the script
		mod   func(*kit.Options)
	}
	allowUnsigned := func(o *kit.Options) { o.AllowUnsigned = true }
	allowTest := func(o *kit.Options) { o.AllowTest = true }
	cases := []tc{
		{"good signed", kittest.Spec{}, nil, nil},
		{"good sha512 signature", kittest.Spec{HashAlg: "sha512"}, nil, nil},
		{"unsigned refused", kittest.Spec{Unsigned: true}, nil, nil},
		{"unsigned allowed", kittest.Spec{Unsigned: true}, []string{"--allow-unsigned"}, allowUnsigned},
		{"bad sums digest", kittest.Spec{BadSumsLine: true}, nil, nil},
		{"unsupported format", kittest.Spec{Format: "2"}, nil, nil},
		{"test grade refused", kittest.Spec{Grade: "test"}, nil, nil},
		{"test grade allowed", kittest.Spec{Grade: "test"}, []string{"--allow-test-kit"}, allowTest},
		{"time64 gate not pass", kittest.Spec{ManifestExtra: "time64_gate=fail\n"}, nil, nil},
		{"missing payload", kittest.Spec{Files: map[string][]byte{"bin/flash_tool": nil}}, nil, nil},
		{"no validated hardware", kittest.Spec{Files: map[string][]byte{"supported_hardware.txt": []byte("model=lsc-indoor-1080p-ak3918\nuntested|x\n")}}, nil, nil},
		{"two model lines", kittest.Spec{ManifestExtra: "model=other-model\n"}, nil, nil},
		{"hardware list of another model", kittest.Spec{Files: map[string][]byte{"supported_hardware.txt": []byte("model=other-model\nvalidated|x\n")}}, nil, nil},
		{"another model, accepted", kittest.Spec{Model: "other-model"}, nil, nil},
		{"expected model mismatch", kittest.Spec{}, []string{"--expect-model", "other-model"}, func(o *kit.Options) { o.ExpectModel = "other-model" }},
		{"expected model match", kittest.Spec{}, []string{"--expect-model", "lsc-indoor-1080p-ak3918"}, func(o *kit.Options) { o.ExpectModel = "lsc-indoor-1080p-ak3918" }},
		{"absolute path", kittest.Spec{TarMutate: add(kittest.Entry{Name: "/etc/x", Type: tar.TypeReg, Mode: 0o644})}, nil, nil},
		{"dotdot path", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/../x", Type: tar.TypeReg, Mode: 0o644})}, nil, nil},
		{"space in name", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/a b", Type: tar.TypeReg, Mode: 0o644})}, nil, nil},
		{"duplicate", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/MANIFEST.txt", Type: tar.TypeReg, Mode: 0o644})}, nil, nil},
		{"setuid file", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/x", Type: tar.TypeReg, Mode: 0o4755})}, nil, nil},
		{"group writable file", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/x", Type: tar.TypeReg, Mode: 0o664})}, nil, nil},
		{"dir mode 700", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/d/", Type: tar.TypeDir, Mode: 0o700})}, nil, nil},
		{"hard link", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/h", Type: tar.TypeLink, Mode: 0o644, Linkname: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/MANIFEST.txt"})}, nil, nil},
		{"fifo", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/f", Type: tar.TypeFifo, Mode: 0o644})}, nil, nil},
		{"foreign symlink", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/s", Type: tar.TypeSymlink, Mode: 0o777, Linkname: "/etc/passwd"})}, nil, nil},
		{"extra file", kittest.Spec{TarMutate: add(kittest.Entry{Name: "akam-kit-lsc-indoor-1080p-ak3918-1.0.0/extra", Type: tar.TypeReg, Mode: 0o644, Body: []byte("x")})}, nil, nil},
		{"entry outside top", kittest.Spec{TarMutate: add(kittest.Entry{Name: "other/x", Type: tar.TypeReg, Mode: 0o644})}, nil, nil},
		{"tampered file", kittest.Spec{TarMutate: func(e *[]kittest.Entry) {
			for i := range *e {
				if strings.HasSuffix((*e)[i].Name, "bin/ak_rtsp") {
					(*e)[i].Body = []byte("evil\n")
				}
			}
		}}, nil, nil},
		{"expect sha256 mismatch", kittest.Spec{}, []string{"--expect-sha256", "deadbeefdead"}, func(o *kit.Options) { o.Expect = "deadbeefdead" }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			b := kittest.Build(t, t.TempDir(), c.spec)
			allowed := filepath.Join(t.TempDir(), "allowed_signers")
			_ = os.WriteFile(allowed, b.AllowedSigners, 0o600)
			args := append([]string{script, "--allowed-signers", allowed}, c.flags...)
			args = append(args, b.Tarball)
			out, err := exec.Command("bash", args...).CombinedOutput()
			bashRC := 0
			var ee *exec.ExitError
			if errors.As(err, &ee) {
				bashRC = ee.ExitCode()
			} else if err != nil {
				t.Fatal(err)
			}
			o := kit.Options{AllowedSigners: b.AllowedSigners}
			if c.mod != nil {
				c.mod(&o)
			}
			r, gerr := kit.VerifyTarball(b.Tarball, o)
			goRC := 0
			var f *kit.Fail
			var u *kit.Usage
			switch {
			case gerr == nil:
				r.Close()
			case errors.Is(gerr, kit.ErrUnsigned):
				goRC = 3
			case errors.As(gerr, &f):
				goRC = 1
			case errors.As(gerr, &u):
				goRC = 2
			default:
				t.Fatalf("unexpected error %v", gerr)
			}
			if goRC != bashRC {
				t.Fatalf("verdict differs: bash %d, Go %d (%v)\nbash output:\n%s", bashRC, goRC, gerr, out)
			}
		})
	}
}

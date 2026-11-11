package ssh_test

import (
	"errors"
	"os"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
)

// A key pinned by the bash wizard's ssh in ~/.ssh/known_hosts that no longer matches is as hard an
// error as a mismatch in Akam's own file.
func TestUserKnownHostsMismatchIsAHardError(t *testing.T) {
	f := newFixture(t)
	c, err := camssh.Dial(f.cfg(yes))
	if err != nil {
		t.Fatal(err)
	}
	c.Close()
	b, _ := os.ReadFile(f.known)
	if err := os.WriteFile(f.userKnow, b, 0o600); err != nil {
		t.Fatal(err)
	}
	os.Remove(f.known)
	f.cam.RotateHostKey()
	_, err = camssh.Dial(f.cfg(func(camssh.NewHostKey) (bool, error) {
		t.Error("must not be offered for acceptance")
		return true, nil
	}))
	var hk *camssh.HostKeyChangedError
	if !errors.As(err, &hk) || hk.KnownHosts != f.userKnow {
		t.Fatalf("err = %v", err)
	}
	if _, serr := os.Stat(f.known); serr == nil {
		t.Error("a pin was written despite the mismatch")
	}
}

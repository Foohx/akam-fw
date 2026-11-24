package testfake

import (
	"os"
	"path/filepath"
)

// SetStreamLogin gives the fake RTSP endpoint a user and password without the helper script (a 2K, which has no SSH helpers in a test).
func (c *Camera) SetStreamLogin(user, pass string) {
	c.t.Helper()
	p := filepath.Join(c.Root, "etc", "config", "akam", "credentials")
	if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
		c.t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte("user="+user+"\npass="+pass+"\n"), 0o600); err != nil {
		c.t.Fatal(err)
	}
}

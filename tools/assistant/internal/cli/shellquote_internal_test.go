package cli

import "testing"

func TestShellQuoteFor(t *testing.T) {
	for _, c := range []struct{ goos, in, want string }{
		{"linux", "/home/x/backups/cam/20261107", "/home/x/backups/cam/20261107"},
		{"linux", "/home/x/Akam backups/cam/20261107", "'/home/x/Akam backups/cam/20261107'"},
		{"linux", "/home/x/it's here", `'/home/x/it'\''s here'`},
		{"darwin", "/Users/x/a$b", "'/Users/x/a$b'"},
		{"linux", "", "''"},
		{"windows", `C:\Users\x\Akam backups\cam`, `"C:\Users\x\Akam backups\cam"`},
		{"windows", `C:\Users\x\backups`, `C:\Users\x\backups`},
	} {
		if got := shellQuoteFor(c.goos, c.in); got != c.want {
			t.Errorf("%s %q: got %s want %s", c.goos, c.in, got, c.want)
		}
	}
}

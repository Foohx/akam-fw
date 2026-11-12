package state

import "testing"

// A short secret only matches as a whole token, so it cannot block or garble ordinary values
// ("a" in a camera name, "1234" in an address); a long one matches anywhere.
func TestShortSecretsMatchWholeTokensOnly(t *testing.T) {
	sec := &Secrets{}
	sec.Add("abc")
	sec.Add("12")
	sec.Add("longsecret-value")
	for text, want := range map[string]bool{
		"abc":                          true,
		"user abc here":                true,
		"pass=abc;":                    true,
		"abcdef":                       false,
		"xabc":                         false,
		"cabinet":                      false,
		"198.51.100.12":                true, // '.' separates tokens, it is a whole token here
		"image 123456":                 false,
		"the longsecret-value, again":  true,
		"prefixlongsecret-valuesuffix": true,
		"plain":                        false,
	} {
		if got := sec.Has(text); got != want {
			t.Errorf("Has(%q) = %v, want %v", text, got, want)
		}
	}
	if got := sec.Redact("abcdef abc 12 123 x12"); got != "abcdef *** *** 123 x12" {
		t.Errorf("Redact = %q", got)
	}
	if got := sec.Redact("zz longsecret-value zz"); got != "zz *** zz" {
		t.Errorf("Redact long = %q", got)
	}
}

func TestStoreStillRefusesRealSecrets(t *testing.T) {
	sec := &Secrets{}
	s, err := Open(t.TempDir(), false, sec)
	if err != nil {
		t.Fatal(err)
	}
	sec.Add("hunter2")
	if err := s.Set("camera_ip", "hunter2"); err == nil {
		t.Error("a whole-token secret was stored")
	}
	if err := s.Set("camera_ip", "hunter23x"); err != nil {
		t.Errorf("a harmless value was refused: %v", err)
	}
	sec.Add("a-very-long-secret")
	if err := s.Set("image_path", "/tmp/a-very-long-secret/x"); err == nil {
		t.Error("a long secret inside a path was stored")
	}
}

package sdcard

import "testing"

func TestPubKeyCRLFIsAcceptedAndWrittenWithLF(t *testing.T) {
	const line = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPDkvGQ3z3X0r1mHk3lT4cU8Yw0qkq0tKXx3o2V1b9mA akam"
	crlf := []byte(line + "\r\n")
	if err := CheckPubKey(crlf); err != nil {
		t.Fatalf("CRLF key refused: %v", err)
	}
	if got := string(PubKeyLF(crlf)); got != line+"\n" {
		t.Errorf("LF form: %q", got)
	}
	if err := CheckPubKey([]byte(line + "\r\n" + line + "\r\n")); err == nil {
		t.Error("two key lines accepted")
	}
}

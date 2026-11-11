package cli

import "testing"

func TestPlainTextStripsControlCharacters(t *testing.T) {
	if got := plainText("ok\x1b[2Jx\ry\x07\u009b"); got != "ok?[2Jx?y??" {
		t.Errorf("plainText = %q", got)
	}
	if plainText("time sync: on, servers: pool.ntp.org") != "time sync: on, servers: pool.ntp.org" {
		t.Error("plain text altered")
	}
	if ctlOnOff("1\x1b[31m") != "1?[31m" {
		t.Error("ctlOnOff passes control characters")
	}
}

// The audited restart command is matched as a whole string: nothing may be added around it.
func TestRestartVideoGuardRejectsWhitespaceAndNewlineVariants(t *testing.T) {
	for _, v := range []string{
		restartVideoCmd + "\n", restartVideoCmd + "\nreboot", restartVideoCmd + " ", "\t" + restartVideoCmd,
		restartVideoCmd + " && reboot", restartVideoCmd + "&&reboot", restartVideoCmd + "\r", restartVideoCmd + "; reboot",
	} {
		if forbiddenRemote(v) == "" {
			t.Errorf("the guard lets %q through", v)
		}
	}
}

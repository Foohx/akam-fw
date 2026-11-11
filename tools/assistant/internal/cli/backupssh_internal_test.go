package cli

import (
	"strings"
	"testing"
)

// The one flash read that may be sent on its own is the anchored read-only dd; any of=, any other
// partition number and anything chained to it is refused, so there is no write path.
func TestGuardAllowsOnlyTheReadOnlyDd(t *testing.T) {
	for n := 0; n < 8; n++ {
		c := remoteReadMtd(n)
		if why := forbiddenRemote(c); why != "" {
			t.Errorf("%s: %s", c, why)
		}
		if strings.Contains(c, "of=") {
			t.Errorf("a read with of=: %s", c)
		}
	}
	for _, bad := range []string{
		"dd if=/dev/mtdblock7 bs=64k of=/dev/mtdblock7",
		"dd if=/dev/zero of=/dev/mtdblock7 bs=64k",
		"dd of=/dev/mtdblock3 if=/dev/mtdblock7 bs=64k",
		"dd if=/dev/mtdblock7 bs=64k of=/tmp/x",
		"dd if=/dev/mtdblock7 bs=64k > /dev/mtdblock3",
		"dd if=/dev/mtdblock7 bs=64k; reboot",
		"dd if=/dev/mtdblock7 bs=64k && kill 1",
		"dd if=/dev/mtdblock8 bs=64k of=/dev/mtd0",
		"dd if=/dev/zero of=/dev/mtd7",
		"dd if=/dev/mtd7 of=/dev/mtd7 bs=64k",
		"mtd_debug write /dev/mtd7 0 1 x",
		"flash_tool -w x",
		"killall watchdog",
		"reboot",
	} {
		if forbiddenRemote(bad) == "" {
			t.Errorf("the guard lets %q through", bad)
		}
	}
	// the read form is exact: another block size, number or spacing is not the allowed form
	for _, odd := range []string{"dd if=/dev/mtdblock8 bs=64k", "dd if=/dev/mtdblock1 bs=1M", "dd if=/dev/mtdblock1  bs=64k", "dd if=/dev/mtdblock1 bs=64k ", "dd if=/dev/mtdblock1 bs=64k\n"} {
		if remoteReadRe.MatchString(odd) {
			t.Errorf("%q matches the allowed read form", odd)
		}
	}
	defer func() {
		if recover() == nil {
			t.Error("remoteReadMtd(8) did not refuse")
		}
	}()
	remoteReadMtd(8)
}

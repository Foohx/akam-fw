package cli

import "testing"

// Bypass attempts: every write form, chaining after the allowed read, quoting and globbing.
func TestGuardBypassTable(t *testing.T) {
	for _, bad := range []string{
		"dd if=/dev/mtdblock7 bs=64k; ls",
		"dd if=/dev/mtdblock7 bs=64k && ls",
		"dd if=/dev/mtdblock7 bs=64k | cat",
		"dd if=/dev/mtdblock7 bs=64k |tee /tmp/x",
		"dd if=/dev/mtdblock7 bs=64k\nls",
		"dd if=/dev/mtdblock7 bs=64k 2>/dev/null",
		"dd if=/dev/mtdblock7 bs=64k $(reboot)",
		"dd if=/dev/mtdblock7 bs=64k seek=1",
		"dd if=/dev/mtdblock7 bs=64k conv=notrunc",
		"dd if=/dev/zero bs=64k oflag=direct",
		"dd if=/dev/zero bs=64k skip=1",
		"dd if=/dev/zero of =/dev/mtdblock3",
		"dd if=/dev/zero of=/dev/mtdblock3",
		"cat /tmp/x > /dev/mtdblock3",
		"cat /tmp/x >/dev/mtdblock3",
		"cat /tmp/x >> /dev/mtd3",
		"cat /tmp/x 1> /dev/mtdblock3",
		"cat /tmp/x 1>/dev/mtd3",
		"cat /tmp/x | tee /dev/mtdblock3",
		"cat /tmp/x | tee /dev/mtd3 >/dev/null",
		"cp /tmp/x /dev/mtdblock3",
		"cp /tmp/x /dev/mtd*",
		"cat /tmp/x > /dev/mtdblock?",
		"cat /tmp/x > /dev/block/mtdblock3",
		"cat /tmp/x > /dev/mem",
		"flash_tool -w x",
		"flash_eraseall /dev/mtd3",
		"flash_erase /dev/mtd3 0 0",
		"flashcp x /dev/mtd3",
		"nandwrite -p /dev/mtd3 x",
		"mtd_debug write /dev/mtd3 0 1 x",
		"kill 1", "pkill akam", "killall akam", "reboot", "/sbin/REBOOT", "busybox reboot", "poweroff", "echo b > /proc/sysrq-trigger",
		"watchdog -T 1 /dev/watchdog", "echo 1 > /dev/watchdog",
		"dd if=/dev/mtdblock8 bs=64k", "dd if=/dev/mtdblock10 bs=64k", "dd if=/dev/mtdblock0 bs=64K",
		" dd if=/dev/mtdblock0 bs=64k", "dd if=/dev/mtdblock0 bs=64k ", "dd if=/dev/mtd0 bs=64k",
		"DD if=/dev/mtdblock0 bs=64k",
	} {
		if forbiddenRemote(bad) == "" {
			t.Errorf("the guard lets %q through", bad)
		}
	}
	for _, ok := range []string{"cat /proc/mtd", "uname -r; uname -v", remoteRegister(), "dd if=/dev/mtdblock0 bs=64k", "dd if=/dev/mtdblock7 bs=64k"} {
		if why := forbiddenRemote(ok); why != "" {
			t.Errorf("%q refused: %s", ok, why)
		}
	}
}

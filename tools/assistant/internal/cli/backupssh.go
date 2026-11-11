package cli

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
	"github.com/Foohx/akam-fw/tools/assistant/internal/textsafe"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// Backup of a camera that already runs Akam, over SSH (spec 11, 3.7, T11.11). It only reads: every
// partition is streamed twice with `dd if=/dev/mtdblockN bs=64k` (fixed words, N a number 0..7 that
// this code produces), the two reads are compared (size and sha256), and the folder has the format of
// the telnet route. Nothing is written to the camera, and nothing is written to this PC until the
// reads are complete and checked.

var procMtdRe = regexp.MustCompile(`^mtd([0-7]): ([0-9a-f]{8}) ([0-9a-f]{8}) "([A-Z0-9]{1,16})"$`)

// bkSSHLayout reads /proc/mtd, validates it strictly against the supported layout and returns the size
// of each partition as the camera reports it (the cap of its read).
func (a *App) bkSSHLayout(c *camssh.Client) ([8]int64, string, error) {
	var caps [8]int64
	out, status, err := a.runCapped(c, "cat /proc/mtd", 4096)
	if err != nil || status != 0 {
		return caps, "", a.UI.Fail("cannot read /proc/mtd on the camera.", "check the SSH session and run the backup again.")
	}
	var keep []string
	seen := 0
	for _, l := range strings.Split(string(out), "\n") {
		l = strings.TrimRight(l, "\r")
		if strings.HasPrefix(l, "dev:") || strings.TrimSpace(l) == "" {
			continue
		}
		keep = append(keep, l)
		if m := procMtdRe.FindStringSubmatch(l); m != nil {
			n, _ := strconv.Atoi(m[1])
			sz, err := strconv.ParseInt(m[2], 16, 64)
			if err == nil && n == seen {
				caps[n] = sz
				seen++
			}
		}
	}
	layout := strings.Join(keep, "\n")
	ok := layout == camLayout && seen == 8
	for n := range caps {
		if caps[n] != camMtdSizes[n] {
			ok = false
		}
	}
	if !ok {
		a.UI.Err("the partition table of this camera is not the supported 8-partition layout:")
		for i, l := range keep {
			if i >= 16 {
				break
			}
			if len(l) > 100 {
				l = l[:100]
			}
			a.UI.Say("      " + textsafe.Clean(l))
		}
		a.UI.Hint("do NOT continue: this camera model or firmware layout is not supported. Nothing was changed.")
		return caps, "", ui.ErrFailed
	}
	a.UI.Say("  Partition table: LAYOUT_OK (8 partitions, APP 00500000).")
	return caps, layout, nil
}

// capBuf collects a stream and refuses more than max bytes.
type capBuf struct {
	b    bytes.Buffer
	max  int64
	over bool
}

var errOverCap = errors.New("the camera sent more bytes than the partition holds")

func (w *capBuf) Write(p []byte) (int, error) {
	if int64(w.b.Len())+int64(len(p)) > w.max {
		w.over = true
		return 0, errOverCap
	}
	return w.b.Write(p)
}

// runCapped runs a read-only command and keeps at most max bytes of its output: a camera that sends
// more is refused (never buffered without bound).
func (a *App) runCapped(c *camssh.Client, cmd string, max int64) ([]byte, int, error) {
	if why := forbiddenRemote(cmd); why != "" {
		return nil, 0, errors.New(why)
	}
	a.Log.Line("CMD ssh root@%s %s", c.Host, cmd)
	ctx, cancel := context.WithTimeout(a.Ctx, 60*time.Second)
	defer cancel()
	w := &capBuf{max: max}
	res, err := c.RunStream(ctx, cmd, w)
	if err != nil {
		return nil, 0, err
	}
	return w.b.Bytes(), res.Status, nil
}

// bkSSHRead streams partition n once and returns its bytes and sha256.
func (a *App) bkSSHRead(c *camssh.Client, n int, size int64) ([]byte, string, error) {
	cmd := remoteReadMtd(n)
	if why := forbiddenRemote(cmd); why != "" {
		return nil, "", errors.New(why)
	}
	a.Log.Line("CMD ssh root@%s %s", c.Host, cmd)
	ctx, cancel := context.WithTimeout(a.Ctx, 300*time.Second)
	defer cancel()
	w := &capBuf{max: size}
	w.b.Grow(int(size))
	res, err := c.RunStream(ctx, cmd, w)
	switch {
	case w.over:
		return nil, "", fmt.Errorf("mtdblock%d: the camera sent more than the %d bytes of /proc/mtd; refused", n, size)
	case err != nil:
		return nil, "", fmt.Errorf("mtdblock%d: the connection to the camera was lost: %s", n, textsafe.Clean(err.Error()))
	case res.Status != 0:
		return nil, "", fmt.Errorf("mtdblock%d: the camera's read failed (status %d)", n, res.Status)
	case int64(w.b.Len()) != size:
		return nil, "", fmt.Errorf("mtdblock%d is %d bytes, expected %d", n, w.b.Len(), size)
	}
	sum := sha256.Sum256(w.b.Bytes())
	return w.b.Bytes(), hex.EncodeToString(sum[:]), nil
}

// bkSSHReadAll returns the 8 verified partitions. The two reads of a partition must be identical;
// mtdblock6 (CONFIG, read/write while the camera runs) may differ: it is read again twice at most,
// then the last first-read is used, like the telnet route.
func (a *App) bkSSHReadAll(c *camssh.Client, caps [8]int64) (data [8][]byte, configDiffers bool, err error) {
	for n := 0; n < 8; n++ {
		redo := 0
		for {
			a.UI.Say(fmt.Sprintf("  Reading mtdblock%d (read 1 of 2) ...", n))
			d1, h1, err := a.bkSSHRead(c, n, caps[n])
			if err != nil {
				return data, false, a.UI.Fail(err.Error()+".", "nothing was changed on the camera and nothing was written here; try again.")
			}
			a.UI.Say(fmt.Sprintf("  Reading mtdblock%d (read 2 of 2) ...", n))
			_, h2, err := a.bkSSHRead(c, n, caps[n])
			if err != nil {
				return data, false, a.UI.Fail(err.Error()+".", "nothing was changed on the camera and nothing was written here; try again.")
			}
			data[n] = d1
			if h1 == h2 {
				break
			}
			if n != 6 {
				return data, false, a.UI.Fail(fmt.Sprintf("the two reads differ on mtdblock%d (not CONFIG only): the read is not reliable.", n), "do not use these dumps; try again.")
			}
			if redo >= 2 {
				a.UI.Warn("mtdblock6 (CONFIG) still differs between the two reads. It is a live read/write filesystem and Akam never uses it: continuing with the first read.")
				configDiffers = true
				break
			}
			redo++
			a.UI.Warn(fmt.Sprintf("only mtdblock6 (CONFIG, read/write while the camera runs) differs: reading it again (%d of 2).", redo))
		}
	}
	a.UI.Good("  READS_IDENTICAL: both reads match on all 8 partitions (CONFIG excepted if said above).")
	return data, configDiffers, nil
}

// bkSSHWrite writes the verified partitions and the three text files. Every file is created private
// (0600, O_EXCL) under a temporary name and renamed when complete; the caller removes the whole new
// folder on a failure, so no partial file is left.
func (a *App) bkSSHWrite(dir string, data [8][]byte, ip, kernel, layout string, configDiffers bool) error {
	put := func(name string, b []byte) error {
		tmp := filepath.Join(dir, "."+name+".part")
		if err := fsutil.WritePrivateFile(tmp, b); err != nil {
			return err
		}
		return os.Rename(tmp, filepath.Join(dir, name))
	}
	var sums strings.Builder
	for n := 0; n < 8; n++ {
		f := fmt.Sprintf("mtdblock%d.bin", n)
		sum := sha256.Sum256(data[n])
		fmt.Fprintf(&sums, "%s  %s\n", hex.EncodeToString(sum[:]), f)
		if err := put(f, data[n]); err != nil {
			return err
		}
	}
	readme := fmt.Sprintf("Akam backup of the camera at %s, made %s by %s backup (over SSH).\n"+
		"mtdblock0..7.bin are the 8 flash partitions as they were when saved: this camera runs Akam, so this is NOT the original firmware.\n"+
		"Keep a second copy on a different medium. NEVER publish these files: they hold proprietary firmware and your passwords.\n"+
		"Check them with: sha256sum -c SHA256SUMS\n", ip, time.Now().UTC().Format("2006-01-02T15:04:05Z"), Prog)
	diff := "0"
	if configDiffers {
		diff = "1"
	}
	if kernel == "" {
		kernel = "unknown"
	}
	info := fmt.Sprintf("camera: %s\nkind=akam\nkernel: %s\ncamera_hash_tool: sha256 computed on this computer\nconfig_partition_differs_between_reads: %s\n--- /proc/mtd\n%s\n", ip, kernel, diff, layout)
	for _, f := range []struct{ name, body string }{{"SHA256SUMS", sums.String()}, {"README.txt", readme}, {"camera_info.txt", info}} {
		if err := put(f.name, []byte(f.body)); err != nil {
			return err
		}
	}
	return nil
}

// backupSSH is the Akam camera route of `backup`. It does not touch the stock-backup state (backup_dir,
// backup_sha, the step mark): that is bound to the original firmware, which this camera no longer has.
func (a *App) backupSSH() error {
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	a.UI.Say("  This camera runs Akam: this saves what is on the camera now. It cannot recreate the original (stock) firmware.")
	if a.UI.Dry() {
		for _, l := range []string{
			"  WOULD check /proc/mtd is the 8-partition layout over SSH (refuse otherwise),",
			"  read each partition twice with 'dd if=/dev/mtdblockN bs=64k' (read only), compare size and sha256,",
			"  then write <Documents>/Akam backups/<name>/<date>/ (mtdblock0..7.bin, SHA256SUMS, README.txt, camera_info.txt with kind=akam)."} {
			a.UI.Say(l)
		}
		return nil
	}
	c, err := a.connect(ip, "cannot log in to the camera over SSH.", "check the camera address and the SSH key (run: "+Prog+" verify).")
	if err != nil {
		return err
	}
	defer c.Close()
	caps, layout, err := a.bkSSHLayout(c)
	if err != nil {
		return err
	}
	kernel := ""
	if out, status, err := a.runCapped(c, "uname -r; uname -v", 1024); err == nil && status == 0 {
		ls := strings.Split(strings.TrimRight(textsafe.Clean(string(out)), "\n"), "\n")
		if len(ls) >= 2 {
			kernel = strings.TrimSpace(ls[0]) + " " + strings.TrimSpace(ls[1])
			a.UI.Say("  Kernel: " + kernel)
		}
	}
	data, configDiffers, err := a.bkSSHReadAll(c, caps)
	if err != nil {
		return err
	}
	c.Close()
	pcdir, parents := a.bkPCDir()
	a.UI.Say("  Writing the verified dump to this PC: " + pcdir)
	if probs := a.backupValidate(pcdir); len(probs) > 0 {
		return a.UI.Fail("cannot use "+pcdir+": "+strings.Join(probs, "; ")+".", "free some space or choose another folder, then run the backup again.")
	}
	// the folders are private before any secret is written
	if len(parents) > 0 {
		if err := fsutil.PrivateMkdirChain(parents...); err != nil {
			return a.UI.Fail("cannot create "+filepath.Dir(pcdir)+": "+err.Error(), "")
		}
	}
	if err := fsutil.PrivateMkdirNew(pcdir); err != nil {
		if os.IsExist(err) {
			return a.UI.Fail(pcdir+" appeared while the backup was running.", "run the backup again.")
		}
		return a.UI.Fail("cannot create "+pcdir+": "+err.Error(), "")
	}
	if !a.backupSecure(pcdir, false) {
		a.UI.Say("  Private: no (this medium has no permissions)")
		if !a.confirm("  The backup holds your Wi-Fi password. Keep it on this medium anyway?") {
			_ = os.Remove(pcdir)
			return a.UI.Fail("the backup folder cannot be made private.", "choose a folder on a disk that has permissions.")
		}
	}
	if err := a.bkSSHWrite(pcdir, data, ip, kernel, layout, configDiffers); err != nil {
		_ = os.RemoveAll(pcdir) // a folder this run created: no partial file stays
		return a.UI.Fail("writing the backup failed: "+err.Error()+".", "nothing was kept; free some space and run the backup again.")
	}
	a.backupSecure(pcdir, true)
	a.UI.Good("  Backup saved and verified (both reads of every partition match). SHA256SUMS written: " + pcdir)
	a.UI.Warn("This backup holds your Wi-Fi password and the camera's secrets: keep it private, and copy it to a second medium.")
	return nil
}

package testfake

import (
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

// FlashOptions select the faults of the reflash emulation. The emulation stands in for
// tools/flash_scripts/akam_reflash_ssh.sh and flash_tool (their real behaviour is covered by
// tests/reflash_ssh_test.sh): the "flash" is a copy into a regular byte slice that plays /dev/mtd7.
// Nothing here touches a device, the host's /tmp or a process.
type FlashOptions struct {
	// PrecheckFails makes the camera pre-check refuse.
	PrecheckFails bool
	// CorruptUpload flips one byte of image.squashfs as it is stored (the sha256 check must catch it).
	CorruptUpload bool
	// DropUploadAfter closes the connection after that many bytes of image.squashfs (0: never).
	DropUploadAfter int
	// NoSecondDropbear: "prepare" succeeds but the RAM Dropbear never comes up.
	NoSecondDropbear bool
	// BadWrite: flash_tool "succeeds" but writes wrong data (every attempt fails the read-back).
	BadWrite bool
	// VerifyLies: the worker reports OK but the independent read-back differs.
	VerifyLies bool
	// DropPollOnce closes the connection on the first status poll (the client must reconnect).
	DropPollOnce bool
	// WorkerDies: the worker writes nothing and disappears without a RESULT line.
	WorkerDies bool
	// WorkerBusy: a worker already holds the lock when the scripts are uploaded.
	WorkerBusy bool
	// HwcheckFails / WifiVerdict shape the verify-side scripts ("" = healthy: PRESENT).
	HwcheckFails bool
	WifiVerdict  string
}

const (
	dropConn = -1000
	rdir     = "/tmp/akam_reflash"
)

type flashState struct {
	mu        sync.Mutex
	mtd7      []byte
	files     map[string][]byte
	prepared  bool
	alive     bool
	log       bytes.Buffer
	starts    int
	polled    int
	scripts   map[string][]byte // files sent to /tmp (hwcheck, preflight)
	flashedOK bool
}

// Mtd7Initial is the content of the fake APP partition before any flash.
func Mtd7Initial() []byte {
	b := make([]byte, 5242880)
	for i := range b {
		b[i] = byte(i*7 + 3)
	}
	return b
}

func (c *Camera) initFlash() {
	c.fl.mtd7 = Mtd7Initial()
	c.fl.files = map[string][]byte{}
	c.fl.scripts = map[string][]byte{}
	if c.opts.Flash.WorkerBusy {
		c.fl.alive = true
	}
}

func (c *Camera) secondUp() bool {
	c.fl.mu.Lock()
	defer c.fl.mu.Unlock()
	return c.fl.prepared && !c.opts.Flash.NoSecondDropbear
}

// Mtd7 returns a copy of the fake APP partition.
func (c *Camera) Mtd7() []byte {
	c.fl.mu.Lock()
	defer c.fl.mu.Unlock()
	return append([]byte(nil), c.fl.mtd7...)
}

// FlashStarts is how many times the detached worker was started.
func (c *Camera) FlashStarts() int {
	c.fl.mu.Lock()
	defer c.fl.mu.Unlock()
	return c.fl.starts
}

// RamFile returns a file the PC uploaded to the camera's RAM work directory.
func (c *Camera) RamFile(name string) []byte {
	c.fl.mu.Lock()
	defer c.fl.mu.Unlock()
	return append([]byte(nil), c.fl.files[name]...)
}

// WaitWorker waits until the detached worker has finished (tests).
func (c *Camera) WaitWorker(d time.Duration) bool {
	end := time.Now().Add(d)
	for time.Now().Before(end) {
		c.fl.mu.Lock()
		alive := c.fl.alive && !c.opts.Flash.WorkerBusy
		c.fl.mu.Unlock()
		if !alive {
			return true
		}
		time.Sleep(10 * time.Millisecond)
	}
	return false
}

func md5Sum(b []byte) string { h := md5.Sum(b); return hex.EncodeToString(h[:]) }

var (
	uploadRe = regexp.MustCompile(`^umask 077; cat > '` + rdir + `/([A-Za-z0-9_.]+)'$`)
	shaRe    = regexp.MustCompile(`^sha256sum '` + rdir + `/([A-Za-z0-9_.]+)'$`)
	camShRe  = regexp.MustCompile(`^sh '` + rdir + `/akam_reflash_ssh\.sh' (precheck|prepare --md5 ([0-9a-f]{32}))$`)
	startRe  = regexp.MustCompile(`^` + rdir + `/bin/busybox sh '` + rdir + `/akam_reflash_ssh\.sh' start --md5 '([0-9a-f]{32})' --i-understand-no-uart$`)
	verifyRe = regexp.MustCompile(`^` + rdir + `/bin/busybox sh '` + rdir + `/akam_reflash_ssh\.sh' verify --md5 '([0-9a-f]{32})'$`)
	pollRe   = regexp.MustCompile(`^` + rdir + `/bin/busybox tail -c \+([0-9]+) '` + rdir + `/flash\.log'; printf '\\n=== akam-end-of-log\\n'; ` + rdir + `/bin/busybox sh '` + rdir + `/akam_reflash_ssh\.sh' status \| ` + rdir + `/bin/busybox grep '\^worker='$`)
)

// flashExec emulates the camera commands of the reflash, hwcheck and wifi gate. ok is false for a
// command that is not one of them.
func (c *Camera) flashExec(cmd string, stdin io.Reader, stdout, stderr io.Writer, nc net.Conn) (int, bool) {
	fo := c.opts.Flash
	switch {
	case strings.HasPrefix(cmd, "busy=0\nfor f in "+rdir+"/worker.pid"):
		c.fl.mu.Lock()
		defer c.fl.mu.Unlock()
		if c.fl.alive {
			return 3, true
		}
		c.fl.files = map[string][]byte{}
		c.fl.log.Reset()
		c.fl.prepared = false
		return 0, true
	case uploadRe.MatchString(cmd):
		name := uploadRe.FindStringSubmatch(cmd)[1]
		var buf bytes.Buffer
		if name == "image.squashfs" && fo.DropUploadAfter > 0 {
			io.CopyN(&buf, stdin, int64(fo.DropUploadAfter))
			return dropConn, true
		}
		if _, err := io.Copy(&buf, stdin); err != nil {
			return 1, true
		}
		b := buf.Bytes()
		if name == "image.squashfs" && fo.CorruptUpload && len(b) > 100 {
			b[100] ^= 0xff
		}
		c.fl.mu.Lock()
		c.fl.files[name] = b
		c.fl.mu.Unlock()
		return 0, true
	case shaRe.MatchString(cmd):
		name := shaRe.FindStringSubmatch(cmd)[1]
		c.fl.mu.Lock()
		b, ok := c.fl.files[name]
		c.fl.mu.Unlock()
		if !ok {
			fmt.Fprintf(stderr, "sha256sum: %s/%s: No such file or directory\n", rdir, name)
			return 1, true
		}
		h := sha256.Sum256(b)
		fmt.Fprintf(stdout, "%s  %s/%s\n", hex.EncodeToString(h[:]), rdir, name)
		return 0, true
	case camShRe.MatchString(cmd):
		return c.camScript(camShRe.FindStringSubmatch(cmd), stdout, stderr), true
	case cmd == rdir+"/bin/busybox echo second-session-ok":
		fmt.Fprintln(stdout, "second-session-ok")
		return 0, true
	case startRe.MatchString(cmd):
		return c.startWorker(startRe.FindStringSubmatch(cmd)[1], stdout, stderr), true
	case pollRe.MatchString(cmd):
		n, _ := strconv.Atoi(pollRe.FindStringSubmatch(cmd)[1])
		c.fl.mu.Lock()
		c.fl.polled++
		first := c.fl.polled == 1
		log := c.fl.log.Bytes()
		alive := c.fl.alive
		var tail []byte
		if n-1 < len(log) {
			tail = append(tail, log[n-1:]...)
		}
		c.fl.mu.Unlock()
		if fo.DropPollOnce && first {
			return dropConn, true
		}
		stdout.Write(tail)
		fmt.Fprint(stdout, "\n=== akam-end-of-log\n")
		if alive {
			fmt.Fprintln(stdout, "worker=alive")
		} else {
			fmt.Fprintln(stdout, "worker=dead")
		}
		return 0, true
	case verifyRe.MatchString(cmd):
		want := verifyRe.FindStringSubmatch(cmd)[1]
		c.fl.mu.Lock()
		got := md5Sum(c.fl.mtd7)
		c.fl.mu.Unlock()
		if fo.VerifyLies {
			got = strings.Repeat("0", 32)
		}
		fmt.Fprintf(stdout, "READBACK_MD5=%s\n", got)
		if got == want {
			fmt.Fprintln(stdout, "VERIFY=MATCH")
			return 0, true
		}
		fmt.Fprintln(stdout, "VERIFY=MISMATCH")
		return 1, true

	// ---- verify: hwcheck, preflight (wifi), onvif flag
	case cmd == "cat > /tmp/akam_hwcheck.sh" || cmd == "cat > /tmp/akam_preflight.sh":
		b, _ := io.ReadAll(stdin)
		c.fl.mu.Lock()
		c.fl.scripts[strings.TrimPrefix(cmd, "cat > /tmp/")] = b
		c.fl.mu.Unlock()
		return 0, true
	case cmd == "sh /tmp/akam_hwcheck.sh":
		if fo.HwcheckFails {
			fmt.Fprintln(stdout, "PASS ssh key login\nFAIL ak_rtsp is not running")
			return 1, true
		}
		fmt.Fprintln(stdout, "PASS ssh key login\nPASS ak_rtsp running\nINFO RTSP auth: off")
		return 0, true
	case cmd == "sh /tmp/akam_preflight.sh --ensure-wifi":
		v := fo.WifiVerdict
		if v == "" {
			v = "PRESENT"
		}
		fmt.Fprintf(stdout, "WIFI=%s\n", v)
		return 0, true
	case cmd == "cat /etc/config/akam/onvif 2>/dev/null":
		c.mu.Lock()
		on, port := c.onvifOn, c.onvifPrt
		c.mu.Unlock()
		if on {
			fmt.Fprintf(stdout, "enabled=1\nport=%s\n", port)
		}
		return 0, true
	}
	return 0, false
}

func (c *Camera) camScript(m []string, stdout, stderr io.Writer) int {
	fo := c.opts.Flash
	if m[1] == "precheck" {
		if fo.PrecheckFails {
			fmt.Fprintln(stderr, "akam_reflash: ERROR: only 100 kB free in /tmp, need 10240 kB (image + tools in RAM)")
			return 1
		}
		fmt.Fprint(stdout, "akam_reflash: mtd7 is APP, 00500000 bytes\nakam_reflash: 21900 kB free in /tmp (need 10240)\nakam_reflash: precheck OK\n")
		return 0
	}
	md5 := m[2]
	c.fl.mu.Lock()
	defer c.fl.mu.Unlock()
	img, ok := c.fl.files["image.squashfs"]
	switch {
	case !ok || c.fl.files["flash_tool"] == nil:
		fmt.Fprintln(stderr, "akam_reflash: ERROR: "+rdir+"/image.squashfs missing")
		return 1
	case len(img) != 5242880:
		fmt.Fprintf(stderr, "akam_reflash: ERROR: image is %d bytes, expected exactly 5242880 (full partition)\n", len(img))
		return 1
	case md5Sum(img) != md5:
		fmt.Fprintf(stderr, "akam_reflash: ERROR: image md5 %s does not match the expected %s\n", md5Sum(img), md5)
		return 1
	}
	c.fl.prepared = true
	fmt.Fprintf(stdout, "akam_reflash: image: 5242880 bytes, md5 %s; flash_tool in RAM\nakam_reflash: prepare OK: reconnect on port 2222, then run: start --md5 %s --i-understand-no-uart\n", md5, md5)
	return 0
}

func (c *Camera) startWorker(md5 string, stdout, stderr io.Writer) int {
	fo := c.opts.Flash
	c.fl.mu.Lock()
	defer c.fl.mu.Unlock()
	img := c.fl.files["image.squashfs"]
	if !c.fl.prepared || md5Sum(img) != md5 {
		fmt.Fprintln(stderr, "akam_reflash: ERROR: run prepare first")
		return 1
	}
	if c.fl.alive {
		fmt.Fprintln(stderr, "akam_reflash: ERROR: a reflash worker is already running")
		return 1
	}
	c.fl.starts++
	c.fl.alive = true
	fmt.Fprintf(&c.fl.log, "[up 100.00] start requested (md5 %s)\n", md5)
	fmt.Fprintln(stdout, "akam_reflash: detached worker started (pid 4242); follow "+rdir+"/flash.log")
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		step := func(format string, a ...any) {
			time.Sleep(15 * time.Millisecond)
			c.fl.mu.Lock()
			fmt.Fprintf(&c.fl.log, format, a...)
			c.fl.mu.Unlock()
		}
		if fo.WorkerDies {
			step("[up 100.10] worker pid 4242 up, waiting to be detached from the launching shell\n")
			c.fl.mu.Lock()
			c.fl.alive = false
			c.fl.mu.Unlock()
			return
		}
		step("[up 100.10] guard OK (RAM-only, detached, session leader)\n[up 100.11] image in RAM md5 %s\n", md5)
		attempts := 3
		for n := 1; n <= attempts; n++ {
			step("[up 100.20] attempt %d/%d: %s/flash_tool %s/image.squashfs /dev/mtd7\n", n, attempts, rdir, rdir)
			step("Erase: [=====>    ]  50%%\rErase: [==========] 100%%\rWrite: [=====>    ]  50%%\rWrite: [==========] 100%%\rflash_tool: complete\n")
			data := append([]byte(nil), img...)
			if fo.BadWrite {
				data[0] ^= 0xff
			}
			c.fl.mu.Lock()
			c.fl.mtd7 = data
			rb := md5Sum(c.fl.mtd7)
			c.fl.mu.Unlock()
			step("[up 100.30] flash_tool exit status 0\n[up 100.31] read-back md5 of /dev/mtd7: %s (expected %s)\n", rb, md5)
			if rb == md5 {
				step("[up 100.40] RESULT: OK read-back md5 %s matches. Power cycle BY HAND (unplug and replug USB-C).\n", rb)
				c.fl.mu.Lock()
				c.fl.alive = false
				c.fl.flashedOK = true
				c.fl.mu.Unlock()
				return
			}
		}
		step("[up 100.90] RESULT: FAILED after %d attempts. DO NOT power cycle. Reconnect on port 2222 and run start again, or use UART.\n", attempts)
		c.fl.mu.Lock()
		c.fl.alive = false
		c.fl.mu.Unlock()
	}()
	return 0
}

// CamFile returns a script the PC copied to the camera's /tmp (akam_hwcheck.sh, akam_preflight.sh).
func (c *Camera) CamFile(name string) []byte {
	c.fl.mu.Lock()
	defer c.fl.mu.Unlock()
	return append([]byte(nil), c.fl.scripts[name]...)
}

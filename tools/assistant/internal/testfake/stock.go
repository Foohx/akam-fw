package testfake

import (
	"bufio"
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"hash"
	"io"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// StockOptions shape the fake stock camera.
type StockOptions struct {
	// Akam: /usr/local/akam exists (a camera that already runs Akam with the maintenance card).
	Akam bool
	// NoCard: no microSD card is mounted.
	NoCard bool
	// FlashCard: the card carries the flash files (image, flash_tool, scripts) given in Files.
	Files map[string][]byte // card files by path under /tmp/sd (flash card: custom_firmware/..., flash_tool, ...)
	// Layout replaces /proc/mtd (an unsupported table).
	Layout string
	// Kernel is the output of uname -r and -v (default: the validated stock kernel).
	KernelR, KernelV string
	// NoSha256: the camera only has md5sum.
	NoSha256 bool
	// FlashMode: "" or ok, fail, badwrite, noline.
	FlashMode string
	// WifiVerdict is the WIFI= code of preflight_check.sh --ensure-wifi (default PRESENT).
	WifiVerdict string
	// FreeKiB is what df reports (default 900000).
	FreeKiB int
	// Mtd7 is the initial APP content (default: 5242880 bytes of "fake APP content").
	Mtd7 []byte
}

// Stock is a fake stock camera: a telnet server on 127.0.0.1 that answers the closed set of shell
// commands the assistant sends (BusyBox-like: IAC negotiation, CRLF echo, "# " prompt, ESC[6n), over a
// private directory tree. The mtd "devices" are regular files; nothing outside the tree is touched.
type Stock struct {
	t    *testing.T
	ln   net.Listener
	Root string
	opt  StockOptions

	mu       sync.Mutex
	cmds     []string
	events   []string
	unknown  []string
	confirms []string
	bound    bool // /mnt is bound to /tmp/sd
	// fault injection (counts, decremented at each use)
	drop, closeAt, corruptRx, corruptTx, truncateTx int
	truncateTxAfter                                  int
	conns                                            []net.Conn
}

// Layout of the camera.
var (
	StockMtdSizes = []int{204800, 4096, 4096, 65536, 1572864, 1032192, 262144, 5242880}
	stockMtdNames = []string{"UBOOT", "ENV", "ENVBK", "DTB", "KERNEL", "ROOTFS", "CONFIG", "APP"}
	stockMtdHex   = []string{"00032000", "00001000", "00001000", "00010000", "00180000", "000fc000", "00040000", "00500000"}
)

// StockLayout is the /proc/mtd table of the supported camera.
func StockLayout() string {
	var b strings.Builder
	b.WriteString("dev:    size   erasesize  name\n")
	for i := range stockMtdNames {
		fmt.Fprintf(&b, "mtd%d: %s 00001000 %q\n", i, stockMtdHex[i], stockMtdNames[i])
	}
	return b.String()
}

// StockMtdContent is the deterministic content of partition i.
func StockMtdContent(i int) []byte {
	pat := []byte(fmt.Sprintf("fake mtd%d content\n", i))
	if i == 7 {
		pat = []byte("fake APP content\n")
	}
	out := make([]byte, 0, StockMtdSizes[i])
	for len(out) < StockMtdSizes[i] {
		out = append(out, pat...)
	}
	return out[:StockMtdSizes[i]]
}

// NewStock starts the fake on a free port of 127.0.0.1.
func NewStock(t *testing.T, o StockOptions) *Stock {
	t.Helper()
	s := &Stock{t: t, Root: t.TempDir(), opt: o}
	for _, d := range []string{"dev", "proc", "tmp", "mnt", "etc/config"} {
		must(os.MkdirAll(filepath.Join(s.Root, d), 0o755))
	}
	layout := o.Layout
	if layout == "" {
		layout = StockLayout()
	}
	must(os.WriteFile(filepath.Join(s.Root, "proc/mtd"), []byte(layout), 0o644))
	must(os.WriteFile(filepath.Join(s.Root, "proc/version"), []byte("Linux version 4.4.302-cip94 (fake@build) (gcc) #1 Fri Nov 14 19:06:55 CST 2025\n"), 0o644))
	for i := 0; i < 8; i++ {
		c := StockMtdContent(i)
		if i == 7 && o.Mtd7 != nil {
			c = o.Mtd7
		}
		must(os.WriteFile(filepath.Join(s.Root, fmt.Sprintf("dev/mtdblock%d", i)), c, 0o644))
	}
	// /dev/mtd7 and /dev/mtdblock7 are the same device: the flash writes one, the dumps read the other.
	if !o.NoCard {
		must(os.MkdirAll(filepath.Join(s.Root, "tmp/sd/logs"), 0o755))
		must(os.WriteFile(filepath.Join(s.Root, "tmp/sd/hack.sh"), []byte("toolkit card\n"), 0o644))
		for p, b := range o.Files {
			fp := filepath.Join(s.Root, "tmp/sd", p)
			must(os.MkdirAll(filepath.Dir(fp), 0o755))
			must(os.WriteFile(fp, b, 0o644))
		}
	}
	if o.Akam {
		must(os.MkdirAll(filepath.Join(s.Root, "usr/local/akam"), 0o755))
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	s.ln = ln
	go s.accept()
	t.Cleanup(s.Close)
	return s
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}

// Port is the telnet port.
func (s *Stock) Port() int { return s.ln.Addr().(*net.TCPAddr).Port }

// Close stops the server.
func (s *Stock) Close() {
	s.ln.Close()
	s.mu.Lock()
	for _, c := range s.conns {
		c.Close()
	}
	s.mu.Unlock()
}

// Commands are the raw lines received (the wrapped ones, as typed).
func (s *Stock) Commands() []string { s.mu.Lock(); defer s.mu.Unlock(); return append([]string(nil), s.cmds...) }

// Events are reboot, poweroff, halt, kill... calls (must stay empty).
func (s *Stock) Events() []string { s.mu.Lock(); defer s.mu.Unlock(); return append([]string(nil), s.events...) }

// Unknown are commands the fake does not emulate (a test asserts it is empty).
func (s *Stock) Unknown() []string { s.mu.Lock(); defer s.mu.Unlock(); return append([]string(nil), s.unknown...) }

// Confirms are the lines read at the flash script's "Type YES to continue" prompt.
func (s *Stock) Confirms() []string { s.mu.Lock(); defer s.mu.Unlock(); return append([]string(nil), s.confirms...) }

// Mtd returns the content of /dev/mtdblock<i>.
func (s *Stock) Mtd(i int) []byte {
	b, _ := os.ReadFile(filepath.Join(s.Root, fmt.Sprintf("dev/mtdblock%d", i)))
	return b
}

// Fault injection: the next n command lines are ignored, the connection closes at the next one, a
// received file gets one byte flipped, a served file is corrupted or cut after cutAfter bytes.
func (s *Stock) Drop(n int)                  { s.mu.Lock(); s.drop = n; s.mu.Unlock() }
func (s *Stock) CloseAtNext(n int)           { s.mu.Lock(); s.closeAt = n; s.mu.Unlock() }
func (s *Stock) CorruptRx(n int)             { s.mu.Lock(); s.corruptRx = n; s.mu.Unlock() }
func (s *Stock) CorruptTx(n int)             { s.mu.Lock(); s.corruptTx = n; s.mu.Unlock() }
func (s *Stock) TruncateTx(n, after int)     { s.mu.Lock(); s.truncateTx, s.truncateTxAfter = n, after; s.mu.Unlock() }
func (s *Stock) take(p *int) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if *p > 0 {
		*p--
		return true
	}
	return false
}

func (s *Stock) accept() {
	for {
		c, err := s.ln.Accept()
		if err != nil {
			return
		}
		s.mu.Lock()
		s.conns = append(s.conns, c)
		s.mu.Unlock()
		go s.session(c)
	}
}

// host path of a camera path.
func (s *Stock) path(p string) string {
	p = filepath.Clean("/" + p)
	switch {
	case p == "/dev/mtd7":
		p = "/dev/mtdblock7"
	case p == "/mnt" || strings.HasPrefix(p, "/mnt/"):
		if s.bound {
			p = "/tmp/sd" + strings.TrimPrefix(p, "/mnt")
		}
	}
	return filepath.Join(s.Root, p)
}

type sconn struct {
	c net.Conn
	r *bufio.Reader
}

func (c *sconn) write(s string) { _, _ = io.WriteString(c.c, strings.ReplaceAll(strings.ReplaceAll(s, "\r\n", "\n"), "\n", "\r\n")) }

// readLine reads one typed line, dropping the client's IAC replies.
func (c *sconn) readLine() (string, error) {
	var b []byte
	for {
		x, err := c.r.ReadByte()
		if err != nil {
			return "", err
		}
		switch x {
		case 255:
			v, err := c.r.ReadByte()
			if err != nil {
				return "", err
			}
			if v >= 251 && v <= 254 {
				if _, err := c.r.ReadByte(); err != nil {
					return "", err
				}
			}
		case '\n':
			return strings.TrimRight(string(b), "\r"), nil
		case 3:
			b = b[:0]
		default:
			b = append(b, x)
		}
	}
}

var wrapRe = regexp.MustCompile(`^echo "(__AKAM_BEG_[0-9]+_[0-9]+_)"x; (.*); echo "(__AKAM_DONE_[0-9]+_[0-9]+):"\$\?"__"$`)

func (s *Stock) session(nc net.Conn) {
	defer nc.Close()
	c := &sconn{c: nc, r: bufio.NewReader(nc)}
	_, _ = nc.Write([]byte{255, 253, 34, 255, 251, 1, 255, 251, 3, 255, 253, 31})
	c.write("# ")
	for {
		line, err := c.readLine()
		if err != nil {
			return
		}
		s.mu.Lock()
		s.cmds = append(s.cmds, line)
		s.mu.Unlock()
		if s.take(&s.drop) {
			continue
		}
		if s.take(&s.closeAt) {
			return
		}
		// the line editor echoes the typed line, wrapped at 80 columns, and queries the cursor
		echo := line
		var wrapped []string
		for len(echo) > 80 {
			wrapped = append(wrapped, echo[:80])
			echo = echo[80:]
		}
		wrapped = append(wrapped, echo)
		c.write("\x1b[6n" + strings.Join(wrapped, "\r\n") + "\r\n")
		m := wrapRe.FindStringSubmatch(line)
		if m == nil {
			c.write("sh: unsupported line\n# ")
			s.mu.Lock()
			s.unknown = append(s.unknown, line)
			s.mu.Unlock()
			continue
		}
		c.write(m[1] + "x\n")
		out, rc := s.run(c, m[2])
		if out != "" {
			c.write(out)
		}
		c.write(fmt.Sprintf("%s:%d__\n# ", m[3], rc))
	}
}

func (s *Stock) hashFile(tool, p string) (string, bool) {
	b, err := os.ReadFile(p)
	if err != nil {
		return "", false
	}
	var h hash.Hash = sha256.New()
	if tool == "md5sum" {
		h = md5.New()
	}
	h.Write(b)
	return hex.EncodeToString(h.Sum(nil)), true
}

func (s *Stock) cardMounted() bool {
	_, err := os.Stat(filepath.Join(s.Root, "tmp/sd"))
	return err == nil
}

func (s *Stock) hashTool() string {
	if s.opt.NoSha256 {
		return "md5sum"
	}
	return "sha256sum"
}

type handler struct {
	re *regexp.Regexp
	fn func(c *sconn, m []string) (string, int)
}

var forbiddenWords = regexp.MustCompile(`\b(reboot|poweroff|halt|kill|killall)\b|watchdog`)

func (s *Stock) run(c *sconn, cmd string) (string, int) {
	if forbiddenWords.MatchString(cmd) {
		s.mu.Lock()
		s.events = append(s.events, cmd)
		s.mu.Unlock()
		return "", 0
	}
	for _, h := range s.handlers() {
		if m := h.re.FindStringSubmatch(cmd); m != nil {
			return h.fn(c, m)
		}
	}
	s.mu.Lock()
	s.unknown = append(s.unknown, cmd)
	s.mu.Unlock()
	return "sh: command not found\n", 127
}

func rx(p string) *regexp.Regexp { return regexp.MustCompile(p) }

func (s *Stock) handlers() []handler {
	return []handler{
		{rx(`^cat /proc/version; \[ -d /usr/local/akam \] && echo AKAM_PRESENT \|\| echo AKAM_ABSENT$`), func(c *sconn, m []string) (string, int) {
			v, _ := os.ReadFile(s.path("/proc/version"))
			if s.opt.Akam {
				return string(v) + "AKAM_PRESENT\n", 0
			}
			return string(v) + "AKAM_ABSENT\n", 0
		}},
		{rx(`^cat /proc/mtd$`), func(c *sconn, m []string) (string, int) {
			v, _ := os.ReadFile(s.path("/proc/mtd"))
			return string(v), 0
		}},
		{rx(`^uname -r; uname -v$`), func(c *sconn, m []string) (string, int) {
			r, v := s.opt.KernelR, s.opt.KernelV
			if r == "" {
				r = "4.4.302-cip94"
			}
			if v == "" {
				v = "#1 Fri Nov 14 19:06:55 CST 2025"
			}
			return r + "\n" + v + "\n", 0
		}},
		{rx(`^mount \| grep mmcblk \|`), func(c *sconn, m []string) (string, int) {
			if s.cardMounted() {
				return "/tmp/sd\n", 0
			}
			return "", 1
		}},
		{rx(`^df -k (\S+)$`), func(c *sconn, m []string) (string, int) {
			free := s.opt.FreeKiB
			if free == 0 {
				free = 900000
			}
			return fmt.Sprintf("Filesystem           1K-blocks      Used Available Use%% Mounted on\n/dev/mmcblk0p1         1000000    100000    %d  10%% %s\n", free, m[1]), 0
		}},
		{rx(`^command -v sha256sum >/dev/null 2>&1 && echo sha256sum \|\| echo md5sum$`), func(c *sconn, m []string) (string, int) {
			return s.hashTool() + "\n", 0
		}},
		{rx(`^mount > (\S+); ls -la /etc/config > (\S+); ps > (\S+)$`), func(c *sconn, m []string) (string, int) {
			for _, p := range m[1:] {
				_ = os.WriteFile(s.path(p), []byte("x\n"), 0o644)
			}
			return "", 0
		}},
		{rx(`^mkdir -p (\S+)$`), func(c *sconn, m []string) (string, int) {
			if err := os.MkdirAll(s.path(m[1]), 0o755); err != nil {
				return "mkdir: error\n", 1
			}
			return "", 0
		}},
		{rx(`^dd if=/dev/mtdblock([0-7]) of=(\S+) bs=64k 2>/dev/null$`), func(c *sconn, m []string) (string, int) {
			b, err := os.ReadFile(s.path("/dev/mtdblock" + m[1]))
			if err != nil || os.WriteFile(s.path(m[2]), b, 0o644) != nil {
				return "", 1
			}
			return "", 0
		}},
		{rx(`^sync$`), func(c *sconn, m []string) (string, int) { return "", 0 }},
		{rx(`^for f in (\S+)/dump1/mtdblock\*\.bin (\S+)/dump2/mtdblock\*\.bin; do echo`), func(c *sconn, m []string) (string, int) {
			var b strings.Builder
			for _, r := range []string{"dump1", "dump2"} {
				ms, _ := filepath.Glob(filepath.Join(s.path(m[1]), r, "mtdblock*.bin"))
				for _, f := range ms {
					st, _ := os.Stat(f)
					fmt.Fprintf(&b, "%d %s/%s/%s\n", st.Size(), m[1], r, filepath.Base(f))
				}
			}
			return b.String(), 0
		}},
		{rx(`^cd (\S+) && (sha256sum|md5sum) mtdblock\*\.bin \| tee (\S+)$`), func(c *sconn, m []string) (string, int) {
			ms, _ := filepath.Glob(filepath.Join(s.path(m[1]), "mtdblock*.bin"))
			var b strings.Builder
			for _, f := range ms {
				h, _ := s.hashFile(m[2], f)
				fmt.Fprintf(&b, "%s  %s\n", h, filepath.Base(f))
			}
			_ = os.WriteFile(s.path(m[3]), []byte(b.String()), 0o644)
			return b.String(), 0
		}},
		{rx(`^stat -c%s '(.*)'; (sha256sum|md5sum) '(.*)'$`), func(c *sconn, m []string) (string, int) {
			st, err := os.Stat(s.path(m[1]))
			if err != nil {
				return "stat: can't stat: No such file or directory\n", 1
			}
			h, _ := s.hashFile(m[2], s.path(m[1]))
			return fmt.Sprintf("%d\n%s  %s\n", st.Size(), h, m[3]), 0
		}},
		{rx(`^nc -l -p ([0-9]+) > '(.*)' < /dev/null$`), func(c *sconn, m []string) (string, int) { return s.ncRecv(m[1], m[2]) }},
		{rx(`^nc -l -p ([0-9]+) < '(.*)'$`), func(c *sconn, m []string) (string, int) { return s.ncSend(m[1], m[2]) }},
		{rx(`^if \[ -d /mnt/custom_firmware \]; then echo CARD_ON_MNT; elif \[ -d /tmp/sd/custom_firmware \]; then echo CARD_ON_TMPSD; else echo CARD_NONE; fi$`), func(c *sconn, m []string) (string, int) {
			if _, err := os.Stat(s.path("/mnt/custom_firmware")); err == nil {
				return "CARD_ON_MNT\n", 0
			}
			if _, err := os.Stat(filepath.Join(s.Root, "tmp/sd/custom_firmware")); err == nil {
				return "CARD_ON_TMPSD\n", 0
			}
			return "CARD_NONE\n", 0
		}},
		{rx(`^mount --bind /tmp/sd /mnt$`), func(c *sconn, m []string) (string, int) { s.bound = true; return "", 0 }},
		{rx(`^md5sum '(.*)' 2>/dev/null$`), func(c *sconn, m []string) (string, int) {
			h, ok := s.hashFile("md5sum", s.path(m[1]))
			if !ok {
				return "", 1
			}
			return h + "  " + m[1] + "\n", 0
		}},
		{rx(`^for f in /mnt/custom_firmware/\*; do if \[ -e "\$f" \]; then echo "\$\{f##\*/\}"; fi; done$`), func(c *sconn, m []string) (string, int) {
			es, _ := os.ReadDir(s.path("/mnt/custom_firmware"))
			var b strings.Builder
			for _, e := range es {
				b.WriteString("\x1b[1;32m" + e.Name() + "\x1b[m\n") // BusyBox colours even here
			}
			return b.String(), 0
		}},
		{rx(`^rm -f /mnt/custom_firmware/ak_rtsp_firmware_\*\.squashfs$`), func(c *sconn, m []string) (string, int) {
			ms, _ := filepath.Glob(filepath.Join(s.path("/mnt/custom_firmware"), "ak_rtsp_firmware_*.squashfs"))
			for _, f := range ms {
				os.Remove(f)
			}
			return "", 0
		}},
		{rx(`^chmod \+x '(.*)'$`), func(c *sconn, m []string) (string, int) { return "", 0 }},
		{rx(`^sh /mnt/preflight_check.sh --ensure-wifi$`), func(c *sconn, m []string) (string, int) {
			v := s.opt.WifiVerdict
			if v == "" {
				v = "PRESENT"
			}
			return "WIFI=" + v + "\n", 0
		}},
		{rx(`^trap '' HUP; sh /mnt/install_with_flash_tool.sh$`), func(c *sconn, m []string) (string, int) { return s.flashScript(c) }},
		{rx(`^dd if=/dev/mtd7 bs=64k 2>/dev/null \| md5sum$`), func(c *sconn, m []string) (string, int) {
			h, _ := s.hashFile("md5sum", s.path("/dev/mtd7"))
			return h + "  -\n", 0
		}},
	}
}

func (s *Stock) ncRecv(port, dst string) (string, int) {
	ln, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		return "nc: bind: Address in use\n", 1
	}
	defer ln.Close()
	_ = ln.(*net.TCPListener).SetDeadline(time.Now().Add(20 * time.Second))
	cn, err := ln.Accept()
	if err != nil {
		return "", 1
	}
	defer cn.Close()
	var buf bytes.Buffer
	_, _ = io.Copy(&buf, cn)
	b := buf.Bytes()
	if s.take(&s.corruptRx) && len(b) > 0 {
		b[0] ^= 0xff
	}
	if err := os.WriteFile(s.path(dst), b, 0o644); err != nil {
		return "", 1
	}
	return "", 0
}

func (s *Stock) ncSend(port, src string) (string, int) {
	b, err := os.ReadFile(s.path(src))
	if err != nil {
		return "nc: can't open: No such file or directory\n", 1
	}
	ln, err := net.Listen("tcp", "127.0.0.1:"+port)
	if err != nil {
		return "nc: bind: Address in use\n", 1
	}
	defer ln.Close()
	_ = ln.(*net.TCPListener).SetDeadline(time.Now().Add(20 * time.Second))
	cn, err := ln.Accept()
	if err != nil {
		return "", 1
	}
	defer cn.Close()
	if s.take(&s.corruptTx) && len(b) > 0 {
		b = append([]byte(nil), b...)
		b[0] ^= 0xff
	}
	if s.take(&s.truncateTx) {
		if s.truncateTxAfter < len(b) {
			b = b[:s.truncateTxAfter]
		}
		if tc, ok := cn.(*net.TCPConn); ok { // an abrupt end: the connection is reset
			_, _ = tc.Write(b)
			_ = tc.SetLinger(0)
			return "", 1
		}
	}
	_, _ = cn.Write(b)
	return "", 0
}

// flashScript emulates tools/flash_scripts/install_with_flash_tool.sh: same prompts and messages.
func (s *Stock) flashScript(c *sconn) (string, int) {
	ms, _ := filepath.Glob(filepath.Join(s.path("/mnt/custom_firmware"), "ak_rtsp_firmware_*.squashfs"))
	if len(ms) < 1 {
		return "ERROR: no ak_rtsp_firmware_*.squashfs found in /mnt/custom_firmware/\n", 1
	}
	img, _ := os.ReadFile(ms[0])
	if len(img) != 5242880 {
		return fmt.Sprintf("ERROR: expected exactly 5242880 bytes (full partition), got %d - refusing to flash\n", len(img)), 1
	}
	sum := md5.Sum(img)
	c.write(fmt.Sprintf("md5sum of /mnt/custom_firmware/%s (record this before flashing):\n%x  /mnt/custom_firmware/%s\n", filepath.Base(ms[0]), sum, filepath.Base(ms[0])))
	c.write("About to overwrite /dev/mtd7 (APP partition) with the image using /mnt/flash_tool.\n")
	c.write("Type YES to continue: ")
	ans, err := c.readLine()
	if err != nil {
		return "", 1
	}
	s.mu.Lock()
	s.confirms = append(s.confirms, ans)
	s.mu.Unlock()
	c.write(ans + "\n") // the pty echoes what was typed
	if ans != "YES" {
		return "Aborted.\n", 1
	}
	if s.opt.FlashMode == "fail" {
		c.write("Writing: [====>      ]   5% (1/5242880)\rWriting: [====>      ]  50% (2/5242880)\r")
		return "ERROR: Flashing/Verification failed with code 1!\n", 1
	}
	c.write("Writing: [====>      ]   5% (1/5242880)\rWriting: [========>  ]  95% (2/5242880)\r\n")
	w := append([]byte(nil), img...)
	if s.opt.FlashMode == "badwrite" {
		w[0] ^= 0xff
	}
	_ = os.WriteFile(s.path("/dev/mtd7"), w, 0o644)
	out := "FAULT SUMMARY: 0 retries\n"
	if s.opt.FlashMode != "noline" {
		out += "Flashing and verification completed successfully!\n"
	}
	return out + "Disconnect and reconnect USB-C power to power cycle.\n", 0
}

var _ = strconv.Itoa

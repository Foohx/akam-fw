package telnet_test

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/telnet"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

type rep struct{ say, warn, good, err []string }

func (r *rep) Say(s string)  { r.say = append(r.say, s) }
func (r *rep) Warn(s string) { r.warn = append(r.warn, s) }
func (r *rep) Good(s string) { r.good = append(r.good, s) }
func (r *rep) Err(s string)  { r.err = append(r.err, s) }

type manual struct {
	shown []string
	out   string
	ok    bool
	err   error
}

func (m *manual) Show(h string, p int, c string) { m.shown = append(m.shown, c) }
func (m *manual) Collect() (string, bool, error) { return m.out, m.ok, m.err }
func (m *manual) Wait(string) error              { return nil }

func session(t *testing.T, cam *testfake.Stock, mod func(*telnet.Config)) (*telnet.Session, *rep) {
	t.Helper()
	r := &rep{}
	cfg := telnet.Config{Host: "127.0.0.1", Port: cam.Port(), Timeout: 2 * time.Second, Warn: r.Warn}
	if mod != nil {
		mod(&cfg)
	}
	s := telnet.New(cfg)
	if err := s.Open(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(s.Close)
	return s, r
}

func TestExecStripsEchoAndMarkers(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	s, _ := session(t, cam, nil)
	if err := s.Exec("cat /proc/version; [ -d /usr/local/akam ] && echo AKAM_PRESENT || echo AKAM_ABSENT", 0); err != nil {
		t.Fatal(err)
	}
	if s.RC != 0 || !strings.HasPrefix(s.Line(1), "Linux version") || s.Line(2) != "AKAM_ABSENT" {
		t.Fatalf("rc=%d out=%q", s.RC, s.Out)
	}
	if strings.Contains(s.Out, "__AKAM") || strings.Contains(s.Out, "\x1b") || strings.Contains(s.Out, "\r") {
		t.Errorf("marker or escape leaked: %q", s.Out)
	}
	// a second command on the same session; a failing command keeps its exit code
	if err := s.Exec("stat -c%s '/nope'; sha256sum '/nope'", 0); err != nil {
		t.Fatal(err)
	}
	if s.RC == 0 {
		t.Errorf("rc should be non zero: %q", s.Out)
	}
	if got := cam.Unknown(); len(got) != 0 {
		t.Errorf("unknown commands: %v", got)
	}
}

func TestStripsColourOfLs(t *testing.T) {
	if got := telnet.StripANSI("\x1b[1;32mak_rtsp.squashfs\x1b[m"); got != "ak_rtsp.squashfs" {
		t.Errorf("got %q", got)
	}
}

func TestCommandLimits(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	s, _ := session(t, cam, nil)
	if err := s.Start("a\nb"); !errors.Is(err, telnet.ErrBadCommand) {
		t.Errorf("newline: %v", err)
	}
	if err := s.Start(strings.Repeat("x", 351)); !errors.Is(err, telnet.ErrBadCommand) {
		t.Errorf("long: %v", err)
	}
	if len(cam.Commands()) != 0 {
		t.Error("a refused command was sent")
	}
}

func TestTimeoutThenManualAfterTwoFailures(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	m := &manual{out: "pasted output\n", ok: true}
	s, r := session(t, cam, func(c *telnet.Config) { c.Timeout = 300 * time.Millisecond; c.Manual = m })
	cam.Drop(100) // the camera never answers
	if err := s.Exec("cat /proc/mtd", 0); err != nil {
		t.Fatal(err)
	}
	if !s.IsManual() {
		t.Fatal("not in manual mode after 2 failures")
	}
	if len(r.warn) != 3 || !strings.Contains(r.warn[0], "failure 1 of 2") || !strings.Contains(r.warn[2], "MANUAL mode") {
		t.Errorf("warnings: %q", r.warn)
	}
	if len(m.shown) != 1 || m.shown[0] != "cat /proc/mtd" {
		t.Errorf("shown: %q", m.shown)
	}
	// the output is what the human pasted, never invented
	if s.Out != "pasted output\n" || s.RC != 0 {
		t.Errorf("out=%q rc=%d", s.Out, s.RC)
	}
}

func TestForcedManualNeverDials(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	m := &manual{out: "x\n", ok: false}
	s := telnet.New(telnet.Config{Host: "127.0.0.1", Port: cam.Port(), ForceManual: true, Manual: m})
	if err := s.Open(); err != nil {
		t.Fatal(err)
	}
	if err := s.Exec("uname -r", 0); err != nil {
		t.Fatal(err)
	}
	if s.RC != 1 || len(cam.Commands()) != 0 {
		t.Errorf("rc=%d cmds=%v", s.RC, cam.Commands())
	}
	if err := s.SendLine("YES"); err == nil {
		t.Error("SendLine worked without a connection")
	}
}

func TestClosedSession(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	s, _ := session(t, cam, func(c *telnet.Config) { c.Manual = &manual{} })
	cam.CloseAtNext(1)
	if err := s.Start("cat /proc/mtd"); err != nil {
		t.Fatal(err)
	}
	if err := s.Finish(time.Second); !errors.Is(err, telnet.ErrClosed) {
		t.Errorf("got %v", err)
	}
}

func TestNegotiationRefusedExceptEchoAndSGA(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	got := make(chan []byte, 1)
	go func() {
		c, _ := ln.Accept()
		defer c.Close()
		// DO ECHO, DO NAWS, WILL ECHO, WILL SGA, WILL TTYPE, then a subnegotiation and a line
		c.Write([]byte{255, 253, 1, 255, 253, 31, 255, 251, 1, 255, 251, 3, 255, 251, 24, 255, 250, 24, 1, 255, 240})
		var all []byte
		buf := make([]byte, 512)
		for {
			c.SetReadDeadline(time.Now().Add(600 * time.Millisecond))
			n, err := c.Read(buf)
			all = append(all, buf[:n]...)
			if err != nil {
				break
			}
		}
		got <- all
	}()
	s := telnet.New(telnet.Config{Host: "127.0.0.1", Port: ln.Addr().(*net.TCPAddr).Port, Timeout: 400 * time.Millisecond})
	if err := s.Open(); err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	_ = s.Start("true")
	_ = s.Finish(400 * time.Millisecond) // the fake never answers: only the replies matter
	b := <-got
	want := []byte{255, 252, 1, 255, 252, 31, 255, 253, 1, 255, 253, 3, 255, 254, 24}
	if !strings.Contains(string(b), string(want)) {
		t.Errorf("replies % x, want % x inside", b, want)
	}
}

// ---- transfers

func sha(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

func xfer(t *testing.T, cam *testfake.Stock) (*telnet.Xfer, *rep) {
	s, r := session(t, cam, func(c *telnet.Config) { c.Timeout = 3 * time.Second })
	return &telnet.Xfer{S: s, Rep: r, PortIn: freePort(t), PortOut: freePort(t), ConnectTries: 20, ConnectEvery: 50 * time.Millisecond}, r
}

func freePort(t *testing.T) int {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	return ln.Addr().(*net.TCPAddr).Port
}

func TestGetVerifiesAndRenames(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	x, _ := xfer(t, cam)
	dst := filepath.Join(t.TempDir(), "mtdblock4.bin")
	want := testfake.StockMtdContent(4)
	if err := x.Get("/dev/mtdblock4", dst, -1, ""); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(dst)
	if sha(b) != sha(want) {
		t.Error("content differs")
	}
	if _, err := os.Stat(dst + ".part"); err == nil {
		t.Error(".part left behind")
	}
}

func TestGetRetriesAfterCorruptionThenSucceeds(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	x, r := xfer(t, cam)
	cam.CorruptTx(2)
	dst := filepath.Join(t.TempDir(), "f.bin")
	if err := x.Get("/dev/mtdblock1", dst, -1, ""); err != nil {
		t.Fatal(err)
	}
	if len(r.warn) < 2 {
		t.Errorf("expected two mismatch warnings: %q", r.warn)
	}
}

func TestGetFailsAfterThreeTriesAndLeavesNothing(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	x, r := xfer(t, cam)
	cam.CorruptTx(10)
	dir := t.TempDir()
	dst := filepath.Join(dir, "f.bin")
	if err := x.Get("/dev/mtdblock1", dst, -1, ""); err == nil {
		t.Fatal("a corrupted transfer was accepted")
	}
	ents, _ := os.ReadDir(dir)
	if len(ents) != 0 {
		t.Errorf("files left: %v", ents)
	}
	if len(r.err) != 1 || !strings.Contains(r.err[0], "failed after 3 tries") {
		t.Errorf("err: %q", r.err)
	}
}

func TestGetInterruptedTransfer(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	x, _ := xfer(t, cam)
	cam.TruncateTx(10, 1000)
	dir := t.TempDir()
	if err := x.Get("/dev/mtdblock4", filepath.Join(dir, "f.bin"), -1, ""); err == nil {
		t.Fatal("a truncated transfer was accepted")
	}
	if ents, _ := os.ReadDir(dir); len(ents) != 0 {
		t.Errorf("partial file left: %v", ents)
	}
}

func TestPutVerifiesOnCameraAndDetectsCorruption(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	x, _ := xfer(t, cam)
	src := filepath.Join(t.TempDir(), "up.bin")
	data := []byte(strings.Repeat("payload-", 5000))
	os.WriteFile(src, data, 0o600)
	if err := x.Put(src, "/tmp/sd/up.bin"); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(filepath.Join(cam.Root, "tmp/sd/up.bin"))
	if sha(got) != sha(data) {
		t.Error("camera copy differs")
	}
	cam.CorruptRx(10)
	if err := x.Put(src, "/tmp/sd/up2.bin"); err == nil {
		t.Error("a corrupted upload was accepted")
	}
}

func TestMd5OnlyCamera(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{NoSha256: true})
	x, _ := xfer(t, cam)
	dst := filepath.Join(t.TempDir(), "f.bin")
	if err := x.Get("/dev/mtdblock2", dst, -1, ""); err != nil {
		t.Fatal(err)
	}
	if x.HT != "md5sum" {
		t.Errorf("tool %q", x.HT)
	}
}

func TestBadRemotePathRefused(t *testing.T) {
	cam := testfake.NewStock(t, testfake.StockOptions{})
	x, _ := xfer(t, cam)
	if err := x.Get("/tmp/a'b", filepath.Join(t.TempDir(), "x"), -1, ""); err == nil {
		t.Error("quote accepted")
	}
	if len(cam.Commands()) != 0 {
		t.Error("sent a command for a bad path")
	}
}

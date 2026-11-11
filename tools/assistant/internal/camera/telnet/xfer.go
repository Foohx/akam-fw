package telnet

import (
	"crypto/md5"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"hash"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Reporter is what a transfer says to the user (ui.UI satisfies it).
type Reporter interface {
	Say(string)
	Warn(string)
	Good(string)
	Err(string)
}

// Xfer moves files between the PC and the camera with the camera's BusyBox `nc -l`, like transfer.sh.
// The PC always dials the camera (its firewall blocks inbound connections); the camera listens. Every
// transfer is verified (size and hash computed on both sides) and retried.
//
//	Put: camera `nc -l -p PORT > REMOTE < /dev/null`, the PC dials and writes
//	Get: camera `nc -l -p PORT < REMOTE`, the PC dials and reads until the camera closes
//
// The camera has no `wc`: sizes come from `stat -c%s`. The hash is sha256sum when the camera has it,
// else md5sum.
type Xfer struct {
	S       *Session
	Rep     Reporter
	PortIn  int // camera listens, PC writes (default 8765)
	PortOut int // camera listens, PC reads (default 8766)
	Tries   int // default 3
	// ConnectTries is the number of connection attempts, one every ConnectEvery (default 100 x 200 ms).
	ConnectTries int
	ConnectEvery time.Duration

	HT         string // "sha256sum" or "md5sum", once known
	RemoteSize string
	RemoteHash string
}

func (x *Xfer) tries() int {
	if x.Tries > 0 {
		return x.Tries
	}
	return 3
}

func (x *Xfer) say(m string)  { x.Rep.Say(m) }
func (x *Xfer) warn(m string) { x.Rep.Warn(m) }

// quote single-quotes a camera path; a quote or line break inside is refused.
func quote(p string) (string, bool) {
	if strings.ContainsAny(p, "'\n\r") {
		return "", false
	}
	return "'" + p + "'", true
}

// HashTool sets HT to the hash program of the camera.
func (x *Xfer) HashTool() error {
	if x.HT != "" {
		return nil
	}
	if err := x.S.Exec("command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo md5sum", 0); err != nil {
		return err
	}
	switch x.S.Line(1) {
	case "sha256sum", "md5sum":
		x.HT = x.S.Line(1)
		return nil
	}
	x.Rep.Err("cannot tell which hash tool the camera has (got: " + x.S.Line(1) + ").")
	return errors.New("unknown hash tool")
}

func (x *Xfer) newHash() hash.Hash {
	if x.HT == "md5sum" {
		return md5.New()
	}
	return sha256.New()
}

// LocalHash is the hash of a file with the same algorithm as the camera.
func (x *Xfer) LocalHash(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := x.newHash()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// RemoteInfo reads the size and hash of a file on the camera (RemoteSize, RemoteHash).
func (x *Xfer) RemoteInfo(remote string) error {
	x.RemoteSize, x.RemoteHash = "", ""
	q, ok := quote(remote)
	if !ok {
		return errors.New("bad remote path")
	}
	if err := x.HashTool(); err != nil {
		return err
	}
	if err := x.S.Exec(fmt.Sprintf("stat -c%%s %s; %s %s", q, x.HT, q), 600*time.Second); err != nil {
		return err
	}
	if x.S.RC != 0 {
		return errors.New("stat/hash failed on the camera")
	}
	size := strings.TrimSpace(x.S.Line(1))
	f := strings.Fields(x.S.Line(2))
	if _, err := strconv.ParseUint(size, 10, 64); err != nil || len(f) == 0 {
		return errors.New("cannot parse the size and hash")
	}
	x.RemoteSize, x.RemoteHash = size, f[0]
	return nil
}

func (x *Xfer) dial(port int) (net.Conn, error) {
	n, every := x.ConnectTries, x.ConnectEvery
	if n <= 0 {
		n = 100
	}
	if every <= 0 {
		every = 200 * time.Millisecond
	}
	addr := net.JoinHostPort(x.S.Host(), strconv.Itoa(port))
	var err error
	for i := 0; i < n; i++ {
		var c net.Conn
		c, err = x.S.cfg.Dial("tcp", addr, 5*time.Second)
		if err == nil {
			return c, nil
		}
		time.Sleep(every)
	}
	return nil, err
}

func (x *Xfer) sessionOK() error {
	if x.S.IsManual() || x.S.IsOpen() {
		return nil
	}
	return x.S.Open()
}

// settle waits for the camera's nc to exit. It does as soon as the PC closes the connection (the
// receiving nc runs with "< /dev/null": with stdin left on the telnet session BusyBox 1.35 nc never
// exits). If a version still waits, Ctrl-C ends it: the data is already written.
func (x *Xfer) settle(d time.Duration) {
	err := x.S.Finish(d)
	if errors.Is(err, ErrTimeout) && !x.S.IsManual() {
		_ = x.S.Interrupt()
		_ = x.S.Finish(d)
	}
}

func (x *Xfer) putOnce(data []byte, dst, q string, port int, hash string) error {
	size := int64(len(data))
	if err := x.sessionOK(); err != nil {
		x.warn("no camera session.")
		return err
	}
	if err := x.HashTool(); err != nil {
		return err
	}
	if err := x.S.Start(fmt.Sprintf("nc -l -p %d > %s < /dev/null", port, q)); err != nil {
		return err
	}
	if x.S.IsManual() {
		if err := x.S.ManualWait("Run the command above in your telnet window; it waits for the file."); err != nil {
			return err
		}
	}
	if err := x.send(data, port); err != nil {
		x.warn(fmt.Sprintf("could not send the file to the camera on port %d.", port))
		x.S.Close()
		return err
	}
	x.settle(15 * time.Second)
	if err := x.RemoteInfo(dst); err != nil {
		return err
	}
	if x.RemoteSize != strconv.FormatInt(size, 10) || x.RemoteHash != hash {
		x.warn(fmt.Sprintf("the camera received a different file (size %s, %s %s; expected %d, %s).", x.RemoteSize, x.HT, x.RemoteHash, size, hash))
		return errors.New("camera copy differs")
	}
	return nil
}

func (x *Xfer) send(data []byte, port int) error {
	c, err := x.dial(port)
	if err != nil {
		return err
	}
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(30*time.Second + time.Duration(len(data)/100000)*time.Second))
	_, err = c.Write(data)
	return err
}

// Put sends the local file src to dst on the camera and verifies size and hash (3 tries by default).
func (x *Xfer) Put(src, dst string) error {
	st, err := os.Stat(src)
	if err != nil || !st.Mode().IsRegular() {
		x.Rep.Err("transfer: no such file: " + src)
		return errors.New("no such file")
	}
	data, err := os.ReadFile(src)
	if err != nil {
		x.Rep.Err("transfer: cannot read " + src)
		return err
	}
	return x.PutData(filepath.Base(src), data, dst)
}

// PutData sends bytes already in memory (the hash is computed over exactly these bytes).
func (x *Xfer) PutData(name string, data []byte, dst string) error {
	q, ok := quote(dst)
	if !ok {
		x.Rep.Err("transfer: bad remote path")
		return errors.New("bad remote path")
	}
	tries := x.tries()
	port := x.PortIn
	if port == 0 {
		port = 8765
	}
	for try := 1; try <= tries; try++ {
		_ = x.HashTool()
		h := x.newHash()
		h.Write(data)
		hash := hex.EncodeToString(h.Sum(nil))
		x.say(fmt.Sprintf("  Sending %s (%d bytes), try %d of %d ...", name, len(data), try, tries))
		if x.putOnce(data, dst, q, port, hash) == nil {
			x.Rep.Good(fmt.Sprintf("  Verified on the camera: %s (%d bytes, %s %s).", dst, len(data), x.HT, hash))
			return nil
		}
	}
	x.Rep.Err(fmt.Sprintf("transfer to the camera failed after %d tries: %s", tries, dst))
	return errors.New("transfer failed")
}

func (x *Xfer) getOnce(part, q string, port int, size int64, hash string) error {
	if err := x.sessionOK(); err != nil {
		x.warn("no camera session.")
		return err
	}
	if err := x.HashTool(); err != nil {
		return err
	}
	_ = os.Remove(part)
	if err := x.S.Start(fmt.Sprintf("nc -l -p %d < %s", port, q)); err != nil {
		return err
	}
	if x.S.IsManual() {
		if err := x.S.ManualWait("Run the command above in your telnet window; it waits for the PC."); err != nil {
			return err
		}
	}
	if err := x.recv(port, part, size); err != nil {
		x.warn(fmt.Sprintf("could not read the file from the camera on port %d.", port))
		x.S.Close()
		return err
	}
	x.settle(15 * time.Second)
	st, err := os.Stat(part)
	if err != nil {
		return err
	}
	got, _ := x.LocalHash(part)
	if st.Size() != size || got != hash {
		x.warn(fmt.Sprintf("the received file differs (size %d, expected %d, or hash mismatch).", st.Size(), size))
		return errors.New("received file differs")
	}
	return nil
}

// recv reads until the camera closes; more than max bytes is refused (a stray sender cannot fill the disk).
func (x *Xfer) recv(port int, out string, max int64) error {
	c, err := x.dial(port)
	if err != nil {
		return err
	}
	defer c.Close()
	_ = os.Remove(out) // a left-over file would keep its own mode; the new one is made 0600, exclusively
	f, err := os.OpenFile(out, os.O_CREATE|os.O_WRONLY|os.O_EXCL, 0o600)
	if err != nil {
		return err
	}
	defer f.Close()
	buf := make([]byte, 64*1024)
	var total int64
	for {
		_ = c.SetReadDeadline(time.Now().Add(60 * time.Second))
		n, err := c.Read(buf)
		if n > 0 {
			if total += int64(n); total > max {
				return errors.New("the camera sent more than the expected size")
			}
			if _, werr := f.Write(buf[:n]); werr != nil {
				return werr
			}
		}
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
	}
}

// Get fetches remote into local. size and hash (same algorithm as HT) are read on the camera when empty.
// local appears only when both match; a partial file is never left behind.
func (x *Xfer) Get(src, dst string, size int64, hash string) error {
	q, ok := quote(src)
	if !ok {
		x.Rep.Err("transfer: bad remote path")
		return errors.New("bad remote path")
	}
	port := x.PortOut
	if port == 0 {
		port = 8766
	}
	if size < 0 || hash == "" {
		if err := x.RemoteInfo(src); err != nil {
			x.Rep.Err("cannot read the size and hash of " + src + " on the camera.")
			return err
		}
		size, _ = strconv.ParseInt(x.RemoteSize, 10, 64)
		hash = x.RemoteHash
	}
	part := dst + ".part"
	tries := x.tries()
	for try := 1; try <= tries; try++ {
		x.say(fmt.Sprintf("  Fetching %s (%d bytes), try %d of %d ...", filepath.Base(dst), size, try, tries))
		if x.getOnce(part, q, port, size, hash) == nil {
			if err := os.Rename(part, dst); err != nil {
				return err
			}
			x.Rep.Good(fmt.Sprintf("  Received and verified: %s (%d bytes, %s %s).", filepath.Base(dst), size, x.HT, hash))
			return nil
		}
		_ = os.Remove(part)
	}
	_ = os.Remove(part)
	x.Rep.Err(fmt.Sprintf("transfer from the camera failed after %d tries: %s", tries, src))
	return errors.New("transfer failed")
}

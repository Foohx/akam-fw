package cli_test

import (
	"context"
	"errors"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/cli"
)

// fakeLAN maps the documentation addresses in addrs to the fake camera (loopback, same ports) and
// refuses every other address; the one interface is eth0 with 192.0.2.5/24.
func (e *env) fakeLAN(addrs ...string) {
	up := map[string]bool{}
	for _, a := range addrs {
		up[a] = true
	}
	e.disc = cli.DiscoverHooks{
		Dial: func(ctx context.Context, network, addr string) (net.Conn, error) {
			host, port, _ := net.SplitHostPort(addr)
			if !up[host] {
				return nil, errors.New("no route (test)")
			}
			var d net.Dialer
			return d.DialContext(ctx, network, net.JoinHostPort("127.0.0.1", port))
		},
		Interfaces: func() ([]net.Interface, error) {
			return []net.Interface{{Index: 1, Name: "eth0", Flags: net.FlagUp | net.FlagBroadcast}}, nil
		},
		Addrs: func(*net.Interface) ([]net.Addr, error) {
			return []net.Addr{&net.IPNet{IP: net.IPv4(192, 0, 2, 5), Mask: net.CIDRMask(24, 32)}}, nil
		},
		Lookup: func(context.Context, string) ([]string, error) { return nil, errors.New("no dns (test)") },
	}
}

// rememberCamera seeds the state of a camera called name at ip and pins the fake camera's host key
// under that address (as a first connection would have).
func (e *env) rememberCamera(name, ip string) {
	e.t.Helper()
	dir := filepath.Join(e.state, "cameras", name)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		e.t.Fatal(err)
	}
	st := "camera_ip=" + ip + "\ncamera_kind=akam\ncamera_fw=b30-old\ncamera_host=" + name + "\ncamera_seen=2026-11-01T10:00:00Z\n"
	if err := os.WriteFile(filepath.Join(dir, "state"), []byte(st), 0o600); err != nil {
		e.t.Fatal(err)
	}
	key, err := camssh.ProbeHostKey(context.Background(), "127.0.0.1", e.cam.SSHPort(), nil, 0)
	if err != nil {
		e.t.Fatal(err)
	}
	if err := camssh.PinHostKey(filepath.Join(e.state, "known_hosts"), ip, key); err != nil {
		e.t.Fatal(err)
	}
}

func (e *env) noIP(stdin string, args ...string) result {
	return e.runRaw(stdin, append([]string{"--no-color", "--state-dir", e.state, "--identity", e.key}, args...)...)
}

// A remembered camera that moved: the scan finds it at a new address with the same SSH host key; one
// yes/no (default No) pins it under the new address. The host key comes from the handshake only.
func TestChooserSameCameraNewAddress(t *testing.T) {
	for _, tc := range []struct {
		name, answer string
		pinned       bool
	}{{"yes", "y\n", true}, {"default no", "\n", false}} {
		t.Run(tc.name, func(t *testing.T) {
			e := newEnv(t)
			e.rememberCamera("salon", "192.0.2.21")
			e.fakeLAN("192.0.2.99")
			// home: 5 = find cameras; chooser: 1 row (the moved camera is found, so the scan row comes first), 3 = scan
			// the scan question is answered y, then the row 1 is chosen; the re-pin question follows; 9 quits the home
			r := e.noIP("5\n3\ny\n1\n"+tc.answer+"9\n", "--camera", "salon")
			expectRC(t, r, 0)
			contains(t, r.out, "Cameras on your network", "Name           Address           Kind            Firmware         Remembered",
				"salon", "192.0.2.99", "yes: salon (was 192.0.2.21)", "Camera: salon at 192.0.2.99.")
			contains(t, r.err, "Scan 192.0.2.0/24: 253 addresses, ports "+e.sortedPorts()+" only? [y/N]",
				"Same camera, new address: remember the host key for 192.0.2.99 too? [y/N]")
			got := camssh.PinnedFingerprint(filepath.Join(e.state, "known_hosts"), "192.0.2.99")
			if tc.pinned && got != e.cam.HostFingerprint() {
				t.Errorf("not pinned: %q", got)
			}
			if !tc.pinned && got != "" {
				t.Errorf("pinned without a yes: %q", got)
			}
			if e.readStateOf("salon", "camera_ip") != "192.0.2.99" {
				t.Errorf("the address was not recorded: %q", e.readStateOf("salon", "camera_ip"))
			}
		})
	}
}

// sortedPorts is the "a, b, c, d" of the scan question for the fake camera's ports.
func (e *env) sortedPorts() string {
	p := []int{e.cam.SSHPort(), e.cam.RTSPPort(), e.cam.CtlPort()}
	tel, _ := strconv.Atoi(os.Getenv("AKAM_PORT_TELNET"))
	p = append(p, tel)
	sort.Ints(p)
	var out []string
	for _, x := range p {
		out = append(out, strconv.Itoa(x))
	}
	return strings.Join(out, ", ")
}

func (e *env) readStateOf(name, key string) string {
	b, _ := os.ReadFile(filepath.Join(e.state, "cameras", name, "state"))
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, key+"="); ok {
			return v
		}
	}
	return ""
}

// The scan is a yes/no with default No: nothing is dialled before a yes.
func TestChooserScanDefaultsToNo(t *testing.T) {
	e := newEnv(t)
	e.rememberCamera("salon", "192.0.2.21")
	var dials atomic.Int32 // the dial hook runs on the probe goroutines
	e.fakeLAN("192.0.2.21")
	inner := e.disc.Dial
	e.disc.Dial = func(ctx context.Context, n, a string) (net.Conn, error) { dials.Add(1); return inner(ctx, n, a) }
	r := e.noIP("5\n2\n7\n9\n", "--camera", "salon") // check remembered (4 ports), then back
	expectRC(t, r, 0)
	if dials.Load() == 0 {
		t.Error("checking the remembered camera dialled nothing")
	}
	dials.Store(0)
	r = e.noIP("5\n3\n\n7\n9\n", "--camera", "salon") // scan, Enter = No
	expectRC(t, r, 0)
	if n := dials.Load(); n != 0 {
		t.Errorf("%d connections before a yes", n)
	}
	contains(t, r.out, "salon", "not answering")
}

// A camera that is not remembered is named (validated) and chosen; the text it sends is cleaned.
func TestChooserNewCameraIsNamed(t *testing.T) {
	e := newEnv(t)
	e.cam.SetCtl("sys.hostname", "evil\x1b[2Jcam")
	e.fakeLAN("192.0.2.57")
	// find (5) > type an address (4 with no rows: after the 4 fixed actions, index 4) ; bad name, then salon
	r := e.noIP("5\n4\n192.0.2.57\n1\nBad Name\nsalon\n9\n", "--camera", "first")
	expectRC(t, r, 0)
	contains(t, r.out, "(evilcam)", "'Bad Name' is not valid", "Camera: salon at 192.0.2.57.")
	notContains(t, r.out, "\x1b")
	if e.readStateOf("salon", "camera_kind") != "akam" || e.readStateOf("salon", "camera_ip") != "192.0.2.57" {
		t.Errorf("state: kind=%q ip=%q", e.readStateOf("salon", "camera_kind"), e.readStateOf("salon", "camera_ip"))
	}
	if cur, _ := os.ReadFile(filepath.Join(e.state, "current_camera")); strings.TrimSpace(string(cur)) != "salon" {
		t.Errorf("current camera: %q", cur)
	}
}

// Forgetting a camera drops its address and pin and keeps the rest; default No.
func TestChooserForget(t *testing.T) {
	e := newEnv(t)
	e.rememberCamera("salon", "192.0.2.21")
	r := e.noIP("5\n6\n1\n\n7\n9\n", "--camera", "salon") // forget > salon > Enter (No)
	expectRC(t, r, 0)
	if e.readStateOf("salon", "camera_ip") == "" {
		t.Error("forgotten without a yes")
	}
	r = e.noIP("5\n6\n1\ny\n7\n9\n", "--camera", "salon")
	expectRC(t, r, 0)
	if e.readStateOf("salon", "camera_ip") != "" || e.readStateOf("salon", "camera_kind") != "akam" {
		t.Errorf("ip=%q kind=%q", e.readStateOf("salon", "camera_ip"), e.readStateOf("salon", "camera_kind"))
	}
	if camssh.PinnedFingerprint(filepath.Join(e.state, "known_hosts"), "192.0.2.21") != "" {
		t.Error("the pin stayed")
	}
}

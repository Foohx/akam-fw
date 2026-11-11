package discover

import (
	"bufio"
	"context"
	"net"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestVirtualNameTables(t *testing.T) {
	cases := []struct {
		goos, name string
		virtual    bool
	}{
		{"linux", "eth0", false}, {"linux", "enp3s0", false}, {"linux", "wlan0", false}, {"linux", "wlp2s0", false},
		{"linux", "docker0", true}, {"linux", "veth1a2b", true}, {"linux", "br-1234", true}, {"linux", "tailscale0", true},
		{"linux", "virbr0", true}, {"linux", "wg0", true}, {"linux", "lo", true},
		{"windows", "Ethernet", false}, {"windows", "Wi-Fi", false}, {"windows", "Ethernet 2", false},
		{"windows", "vEthernet (WSL)", true}, {"windows", "vEthernet (Default Switch)", true},
		{"windows", "Loopback Pseudo-Interface 1", true}, {"windows", "VMware Network Adapter VMnet8", true},
		{"windows", "VirtualBox Host-Only Network", true}, {"windows", "Bluetooth Network Connection", true},
		{"windows", "isatap.{ABC}", true}, {"windows", "Teredo Tunneling Pseudo-Interface", true},
		{"windows", "Local Area Connection* 9 (Hyper-V)", true},
		{"darwin", "en0", false}, {"darwin", "en1", false},
		{"darwin", "lo0", true}, {"darwin", "utun3", true}, {"darwin", "awdl0", true}, {"darwin", "llw0", true},
		{"darwin", "bridge100", true}, {"darwin", "gif0", true}, {"darwin", "stf0", true}, {"darwin", "ap1", true},
		{"darwin", "anpi0", true}, {"darwin", "vmenet0", true},
	}
	for _, c := range cases {
		if got := VirtualName(c.goos, c.name); got != c.virtual {
			t.Errorf("%s %q: virtual=%v, want %v", c.goos, c.name, got, c.virtual)
		}
	}
}

func ipnet(s string) net.Addr {
	ip, n, _ := net.ParseCIDR(s)
	n.IP = ip
	return n
}

func TestSelectSubnets(t *testing.T) {
	up := net.FlagUp | net.FlagMulticast
	ifaces := []Iface{
		{Name: "eth0", Flags: up, Addrs: []net.Addr{ipnet("192.168.1.77/24"), ipnet("169.254.3.4/16"), ipnet("fe80::1/64")}},
		{Name: "eth1", Flags: up, Addrs: []net.Addr{ipnet("10.8.0.9/8")}},                   // too large: cut to /24
		{Name: "eth2", Flags: up, Addrs: []net.Addr{ipnet("198.51.100.5/23")}},              // /23 kept whole
		{Name: "eth3", Flags: up, Addrs: []net.Addr{ipnet("203.0.113.9/22")}},               // /22 kept
		{Name: "eth4", Flags: up, Addrs: []net.Addr{ipnet("169.254.7.7/16")}},               // link-local only
		{Name: "eth5", Flags: net.FlagMulticast, Addrs: []net.Addr{ipnet("192.0.2.5/24")}},  // down
		{Name: "docker0", Flags: up, Addrs: []net.Addr{ipnet("172.17.0.1/16")}},             // virtual
		{Name: "lo", Flags: up | net.FlagLoopback, Addrs: []net.Addr{ipnet("127.0.0.1/8")}}, // loopback
	}
	got := SelectSubnets("linux", ifaces)
	var s []string
	for _, x := range got {
		s = append(s, x.Iface+"="+x.CIDR.String())
	}
	want := "eth0=192.168.1.0/24 eth1=10.8.0.0/24 eth2=198.51.100.0/23 eth3=203.0.112.0/22"
	if strings.Join(s, " ") != want {
		t.Errorf("subnets = %v, want %s", s, want)
	}
	if n := len(got[2].Hosts()); n != 509 {
		t.Errorf("/23 hosts = %d", n)
	}
	q := got[2].Question([]int{22, 24, 554, 8091})
	if q != "Scan 198.51.100.0/23: 509 addresses, ports 22, 24, 554, 8091 only?" {
		t.Errorf("question = %q", q)
	}
}

// serveCtl runs a fake line-protocol server and records every first word it receives.
func serveCtl(t *testing.T, script func(c net.Conn, br *bufio.Reader, words *[]string, mu *sync.Mutex)) (int, func() []string) {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	var mu sync.Mutex
	var words []string
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				script(c, bufio.NewReader(c), &words, &mu)
			}()
		}
	}()
	return ln.Addr().(*net.TCPAddr).Port, func() []string { mu.Lock(); defer mu.Unlock(); return append([]string(nil), words...) }
}

func record(words *[]string, mu *sync.Mutex, line string) string {
	w, _, _ := strings.Cut(strings.TrimSpace(line), " ")
	mu.Lock()
	*words = append(*words, w)
	mu.Unlock()
	return w
}

func answering(vals map[string]string, push bool) func(net.Conn, *bufio.Reader, *[]string, *sync.Mutex) {
	return func(c net.Conn, br *bufio.Reader, words *[]string, mu *sync.Mutex) {
		if push {
			c.Write([]byte("LOG [x] hello\n"))
		}
		for {
			line, err := br.ReadString('\n')
			if err != nil {
				return
			}
			switch record(words, mu, line) {
			case "QUIT":
				c.Write([]byte("OK bye\n"))
				return
			case "GET":
				k := strings.Fields(line)[1]
				if v, ok := vals[k]; ok {
					c.Write([]byte(k + "=" + v + "\n"))
				} else {
					c.Write([]byte("ERR unknown param " + k + "\n"))
				}
			default:
				c.Write([]byte("ERR syntax\n"))
			}
		}
	}
}

func TestIdentifyReadsVersionAndHostAndSendsOnlyGetAndQuit(t *testing.T) {
	port, words := serveCtl(t, answering(map[string]string{"fw.version": "b35-2af81c5", "sys.hostname": "akam-01"}, true))
	id, err := Identify(context.Background(), "127.0.0.1", port, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !id.Akam || id.Version != "b35-2af81c5" || id.Hostname != "akam-01" {
		t.Errorf("identity = %+v", id)
	}
	time.Sleep(50 * time.Millisecond)
	for _, w := range words() {
		if w != "GET" && w != "QUIT" {
			t.Errorf("sent %q", w)
		}
	}
	if w := words(); len(w) != 3 || w[2] != "QUIT" {
		t.Errorf("words = %v", w)
	}
}

func TestIdentifyOlderImageStillAkam(t *testing.T) {
	port, _ := serveCtl(t, answering(nil, false))
	id, err := Identify(context.Background(), "127.0.0.1", port, nil)
	if err != nil || !id.Akam || id.Version != "" || id.Hostname != "" {
		t.Errorf("identity = %+v, %v", id, err)
	}
}

func TestIdentifyHostileAnswers(t *testing.T) {
	long := strings.Repeat("A", 100000)
	port, _ := serveCtl(t, func(c net.Conn, br *bufio.Reader, words *[]string, mu *sync.Mutex) {
		for {
			line, err := br.ReadString('\n')
			if err != nil {
				return
			}
			switch record(words, mu, line) {
			case "GET":
				if strings.Contains(line, "fw.version") {
					c.Write([]byte("fw.version=" + long + "\n"))
				} else {
					c.Write([]byte("sys.hostname=a\x1b[2J\x07b‮c\n"))
				}
			case "QUIT":
				return
			}
		}
	})
	id, err := Identify(context.Background(), "127.0.0.1", port, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(id.Version) > identifyLineMax || len(id.Version) < 200 {
		t.Errorf("version length %d", len(id.Version))
	}
	// the raw text is returned as is (control characters included): callers sanitise it
	if !strings.Contains(id.Hostname, "\x1b") {
		t.Errorf("hostname = %q", id.Hostname)
	}
}

func TestIdentifyFloodOfLogLinesIsBounded(t *testing.T) {
	port, _ := serveCtl(t, func(c net.Conn, br *bufio.Reader, words *[]string, mu *sync.Mutex) {
		for i := 0; i < 1000; i++ {
			if _, err := c.Write([]byte("LOG spam\n")); err != nil {
				return
			}
		}
		time.Sleep(300 * time.Millisecond)
	})
	start := time.Now()
	id, _ := Identify(context.Background(), "127.0.0.1", port, nil)
	if id.Akam || time.Since(start) > 2*time.Second {
		t.Errorf("identity %+v after %v", id, time.Since(start))
	}
}

func TestIdentifySilenceTimesOutAfterTwoSeconds(t *testing.T) {
	port, _ := serveCtl(t, func(c net.Conn, br *bufio.Reader, words *[]string, mu *sync.Mutex) {
		time.Sleep(4 * time.Second)
	})
	start := time.Now()
	id, err := Identify(context.Background(), "127.0.0.1", port, nil)
	el := time.Since(start)
	if err == nil || id.Akam || el < 1500*time.Millisecond || el > 3*time.Second {
		t.Errorf("id=%+v err=%v after %v", id, err, el)
	}
}

func TestIdentifyRefusedPort(t *testing.T) {
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()
	if _, err := Identify(context.Background(), "127.0.0.1", port, nil); err == nil {
		t.Error("no error for a closed port")
	}
	_ = strconv.Itoa
}

func TestMerge(t *testing.T) {
	now := time.Date(2026, 11, 11, 12, 0, 0, 0, time.UTC)
	known := []Known{
		{Name: "salon", IP: "192.0.2.21", HostKey: "SHA256:aaa"},
		{Name: "garage", IP: "192.0.2.30", HostKey: "SHA256:bbb"},
		{Name: "attic", IP: "192.0.2.40", HostKey: "SHA256:ccc", Seen: now.Add(-72 * time.Hour)},
	}
	found := []Camera{
		{IP: "192.0.2.64", Kind: Stock},
		{IP: "192.0.2.34", Kind: Akam, HostKey: "SHA256:bbb"}, // garage moved
		{IP: "192.0.2.21", Kind: Akam, FW: "b35-2af81c5"},     // salon, same address
		{IP: "192.0.2.57", Kind: Akam, HostKey: "SHA256:zzz"}, // new
	}
	rows := Merge(known, found, now)
	type r struct{ name, ip, old string }
	var got []r
	for _, x := range rows {
		ip := x.IP
		if !x.Found {
			ip = "last:" + x.LastIP
		}
		got = append(got, r{x.Name, ip, x.OldIP})
	}
	want := []r{{"salon", "192.0.2.21", ""}, {"garage", "192.0.2.34", "192.0.2.30"}, {"", "192.0.2.57", ""}, {"", "192.0.2.64", ""}, {"attic", "last:192.0.2.40", ""}}
	if len(got) != len(want) {
		t.Fatalf("rows = %+v", got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("row %d = %+v, want %+v", i, got[i], want[i])
		}
	}
	if a := Ago(rows[4].LastSeen, now); a != "3 days ago" {
		t.Errorf("ago = %q", a)
	}
	if Ago(time.Time{}, now) != "never" || Ago(now.Add(-5*time.Minute), now) != "5 min ago" || Ago(now.Add(-3*time.Hour), now) != "3 hours ago" {
		t.Error("Ago")
	}
}

func TestMergeSameKeyIsNotAMoveWhenAddressMatches(t *testing.T) {
	now := time.Now()
	rows := Merge([]Known{{Name: "a", IP: "192.0.2.1", HostKey: "k"}}, []Camera{{IP: "192.0.2.1", HostKey: "k"}, {IP: "192.0.2.2", HostKey: "k"}}, now)
	if rows[0].Name != "a" || rows[0].OldIP != "" || rows[1].Name != "" {
		t.Errorf("rows = %+v", rows)
	}
}

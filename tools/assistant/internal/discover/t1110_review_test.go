package discover

import (
	"context"
	"net"
	"strings"
	"testing"
	"time"
)

func TestSelectSubnetsCutIsShownDedupedAndPointToPointExcluded(t *testing.T) {
	up := net.FlagUp | net.FlagMulticast
	ifaces := []Iface{
		{Name: "eth0", Flags: up, Addrs: []net.Addr{ipnet("10.20.30.40/16")}},
		{Name: "wlan0", Flags: up, Addrs: []net.Addr{ipnet("10.20.30.41/24")}},
		{Name: "corp", Flags: up | net.FlagPointToPoint, Addrs: []net.Addr{ipnet("10.99.0.2/16")}},
		{Name: "eth9", Flags: up, Addrs: []net.Addr{ipnet("10.50.0.2/32"), ipnet("10.51.0.2/31")}},
	}
	got := SelectSubnets("linux", ifaces)
	if len(got) != 1 || got[0].CIDR.String() != "10.20.30.0/24" || got[0].Whole == nil || got[0].Whole.String() != "10.20.0.0/16" {
		t.Fatalf("subnets = %+v", got)
	}
	q := got[0].Question([]int{22, 24, 554, 8091})
	if !strings.Contains(q, "10.20.30.0/24") || !strings.Contains(q, "a part of 10.20.0.0/16") || !strings.Contains(got[0].Describe(), "10.20.0.0/16") {
		t.Errorf("the question must show the whole network: %q / %q", q, got[0].Describe())
	}
}

func TestVirtualNameVPNs(t *testing.T) {
	for _, c := range [][2]string{{"linux", "ppp0"}, {"linux", "ipsec0"}, {"linux", "nordlynx"}, {"windows", "WireGuard Tunnel"},
		{"windows", "OpenVPN Wintun"}, {"windows", "Tailscale"}, {"windows", "Local Area Connection* 3"}, {"darwin", "ppp0"}} {
		if !VirtualName(c[0], c[1]) {
			t.Errorf("%s %q must be excluded", c[0], c[1])
		}
	}
}

func TestMergeMovedCameraCannotStealAnAnswerOfAnotherOne(t *testing.T) {
	now := time.Now()
	known := []Known{{Name: "a", IP: "192.0.2.9", HostKey: "k1"}, {Name: "b", IP: "192.0.2.1", HostKey: "k2"}}
	rows := Merge(known, []Camera{{IP: "192.0.2.1", HostKey: "k1"}}, now)
	if rows[0].Name != "b" || rows[0].OldIP != "" || rows[1].Name != "a" || rows[1].Found {
		t.Errorf("rows = %+v", rows)
	}
}

func TestMergeAmbiguousKeyAndHostNameNeverDecideIdentity(t *testing.T) {
	now := time.Now()
	known := []Known{{Name: "a", IP: "192.0.2.9", Host: "akam-01", HostKey: "k1"}, {Name: "nokey", IP: "192.0.2.8", Host: "akam-02"}}
	found := []Camera{{IP: "192.0.2.2", HostKey: "k1"}, {IP: "192.0.2.3", HostKey: "k1"}, {IP: "192.0.2.4", Host: "akam-01"}, {IP: "192.0.2.5", Host: "akam-02"}}
	for _, r := range Merge(known, found, now) {
		if r.Name != "" && r.Found {
			t.Errorf("%s was re-pinned to %s", r.Name, r.IP)
		}
	}
}

func TestIdentifyEndlessLineIsBoundedInBytes(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go func() {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		defer c.Close()
		chunk := []byte(strings.Repeat("A", 4096))
		for n := 0; n < 4<<20; n += len(chunk) {
			_ = c.SetWriteDeadline(time.Now().Add(300 * time.Millisecond))
			if _, err := c.Write(chunk); err != nil {
				return
			}
		}
	}()
	port := ln.Addr().(*net.TCPAddr).Port
	start := time.Now()
	_, _ = Identify(context.Background(), "127.0.0.1", port, nil)
	if time.Since(start) > 3*time.Second {
		t.Errorf("took %v", time.Since(start))
	}
}

package discover

import (
	"context"
	"encoding/binary"
	"errors"
	"net"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestClassify(t *testing.T) {
	p := Ports{Telnet: 24, SSH: 22, RTSP: 554, CTL: 8091}
	cases := []struct {
		open []int
		want Kind
	}{
		{[]int{24, 22}, Stock}, {[]int{24}, Stock}, {[]int{22, 554, 8091}, Akam}, {[]int{22, 554}, Partial},
		{[]int{22}, Partial}, {[]int{554, 8091}, Unknown}, {nil, Unknown}, {[]int{24, 22, 554, 8091}, Stock},
	}
	for _, c := range cases {
		if got := Classify(c.open, p); got != c.want {
			t.Errorf("%v: %s, want %s", c.open, got, c.want)
		}
	}
}

func TestOpenPortsKeepsTheOrderAndNeverLeavesTheDialer(t *testing.T) {
	var calls atomic.Int32
	dial := func(ctx context.Context, network, addr string) (net.Conn, error) {
		calls.Add(1)
		if !strings.HasPrefix(addr, "192.0.2.7:") {
			t.Errorf("dialled %s", addr)
		}
		if strings.HasSuffix(addr, ":22") || strings.HasSuffix(addr, ":8091") {
			a, b := net.Pipe()
			b.Close()
			return a, nil
		}
		return nil, errors.New("refused")
	}
	got := OpenPorts(context.Background(), "192.0.2.7", []int{8091, 24, 22, 554}, time.Second, dial)
	if len(got) != 2 || got[0] != 8091 || got[1] != 22 || calls.Load() != 4 {
		t.Errorf("%v after %d dials", got, calls.Load())
	}
}

func TestSubnets(t *testing.T) {
	mk := func(name string, flags net.Flags) net.Interface { return net.Interface{Name: name, Flags: flags} }
	up := net.FlagUp
	ifs := []net.Interface{mk("lo", up|net.FlagLoopback), mk("eth0", up), mk("docker0", up), mk("wlan0", up), mk("down0", 0), mk("br-123", up), mk("veth9", up)}
	addrs := map[string][]net.Addr{
		"eth0":    {&net.IPNet{IP: net.ParseIP("192.0.2.30"), Mask: net.CIDRMask(24, 32)}, &net.IPNet{IP: net.ParseIP("fe80::1"), Mask: net.CIDRMask(64, 128)}},
		"wlan0":   {&net.IPNet{IP: net.ParseIP("10.1.2.3"), Mask: net.CIDRMask(16, 32)}}, // a /16 is limited to its /24
		"docker0": {&net.IPNet{IP: net.ParseIP("172.17.0.1"), Mask: net.CIDRMask(16, 32)}},
	}
	subs, err := Subnets(func() ([]net.Interface, error) { return ifs, nil }, func(i *net.Interface) ([]net.Addr, error) { return addrs[i.Name], nil })
	if err != nil || len(subs) != 2 {
		t.Fatalf("%v %v", subs, err)
	}
	if subs[0].CIDR.String() != "192.0.2.0/24" || subs[1].CIDR.String() != "10.1.2.0/24" {
		t.Errorf("%v %v", subs[0].CIDR, subs[1].CIDR)
	}
	h := subs[0].Hosts()
	if len(h) != 253 || h[0].String() != "192.0.2.1" || h[len(h)-1].String() != "192.0.2.254" {
		t.Errorf("%d hosts, %v..%v", len(h), h[0], h[len(h)-1]) // 254 minus the PC itself
	}
	for _, x := range h {
		if x.String() == "192.0.2.30" {
			t.Error("scanned itself")
		}
	}
}

func TestScanFindsHostsAndRespectsTheConcurrencyLimit(t *testing.T) {
	_, cidr, _ := net.ParseCIDR("192.0.2.0/24")
	sn := Subnet{Iface: "eth0", CIDR: cidr}
	var cur, peak atomic.Int32
	dial := func(ctx context.Context, network, addr string) (net.Conn, error) {
		n := cur.Add(1)
		for {
			p := peak.Load()
			if n <= p || peak.CompareAndSwap(p, n) {
				break
			}
		}
		time.Sleep(time.Millisecond)
		cur.Add(-1)
		host, port, _ := net.SplitHostPort(addr)
		open := map[string][]string{"192.0.2.10": {"22", "554", "8091"}, "192.0.2.200": {"24"}, "192.0.2.77": {"22"}}
		for _, p := range open[host] {
			if p == port {
				a, b := net.Pipe()
				b.Close()
				return a, nil
			}
		}
		return nil, errors.New("closed")
	}
	got := Scan(context.Background(), sn, Ports{Telnet: 24, SSH: 22, RTSP: 554, CTL: 8091}, 64, time.Second, dial)
	if len(got) != 3 || got[0].IP != "192.0.2.10" || got[0].Kind != Akam || got[1].IP != "192.0.2.77" || got[1].Kind != Partial || got[2].IP != "192.0.2.200" || got[2].Kind != Stock {
		t.Fatalf("%+v", got)
	}
	if peak.Load() > 64 {
		t.Errorf("%d dials at once, the limit is 64", peak.Load())
	}
	if ScanCount(sn) != 254*4 {
		t.Error(ScanCount(sn))
	}
}

const probeMatch = `<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><d:ProbeMatches xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"><d:ProbeMatch>` +
	`<d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>onvif://www.onvif.org/name/akam-1a2b onvif://www.onvif.org/hardware/lsc-indoor</d:Scopes>` +
	`<d:XAddrs>http://192.0.2.50:80/onvif/device_service</d:XAddrs></d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>`

func TestWSDiscoveryOneProbeAndItsAnswer(t *testing.T) {
	srv, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	got := make(chan string, 2)
	go func() {
		buf := make([]byte, 4096)
		for {
			n, from, err := srv.ReadFrom(buf)
			if err != nil {
				return
			}
			got <- string(buf[:n])
			srv.WriteTo([]byte("not soap"), from)
			srv.WriteTo([]byte(probeMatch), from)
			srv.WriteTo([]byte(probeMatch), from) // a duplicate answer
		}
	}()
	found, err := WSDiscover(context.Background(), srv.LocalAddr().String(), 400*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	probe := <-got
	for _, want := range []string{"discovery/Probe", "NetworkVideoTransmitter", "uuid:"} {
		if !strings.Contains(probe, want) {
			t.Errorf("probe lacks %s", want)
		}
	}
	if len(found) != 1 || found[0].IP != "127.0.0.1" || found[0].Name != "akam-1a2b" || found[0].Hard != "lsc-indoor" ||
		len(found[0].XAddrs) != 1 || found[0].XAddrs[0] != "http://192.0.2.50:80/onvif/device_service" {
		t.Errorf("%+v", found)
	}
	if len(got) != 0 {
		t.Error("more than one packet was sent")
	}
}

func TestMDNSLookup(t *testing.T) {
	srv, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	go func() {
		buf := make([]byte, 1500)
		n, from, err := srv.ReadFrom(buf)
		if err != nil {
			return
		}
		q := buf[:n]
		if binary.BigEndian.Uint16(q[4:]) != 1 || !strings.Contains(string(q), "\x05salon\x05local\x00") {
			return
		}
		// answer: header, the question again, one A record
		resp := append([]byte{}, q[:2]...)
		resp = append(resp, 0x84, 0, 0, 1, 0, 1, 0, 0, 0, 0)
		resp = append(resp, q[12:]...)
		resp = append(resp, 0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 120, 0, 4, 192, 0, 2, 77)
		srv.WriteTo(resp, from)
	}()
	ip, err := MDNSLookup(context.Background(), "salon.local", srv.LocalAddr().String(), time.Second)
	if err != nil || ip.String() != "192.0.2.77" {
		t.Fatalf("%v %v", ip, err)
	}
	if _, err := MDNSLookup(context.Background(), "nobody.local", "127.0.0.1:9", 100*time.Millisecond); err == nil {
		t.Error("an unanswered query succeeded")
	}
}

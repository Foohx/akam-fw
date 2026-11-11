package discover

import (
	"context"
	"encoding/binary"
	"net"
	"testing"
	"time"
)

func mdnsAnswer(q []byte, owner string, ip [4]byte) []byte {
	resp := append([]byte{}, q[:2]...)
	resp = append(resp, 0x84, 0, 0, 1, 0, 1, 0, 0, 0, 0)
	resp = append(resp, q[12:]...)
	for _, l := range splitDots(owner) {
		resp = append(resp, byte(len(l)))
		resp = append(resp, l...)
	}
	resp = append(resp, 0, 0, 1, 0, 1, 0, 0, 0, 120, 0, 4)
	return append(resp, ip[:]...)
}

func splitDots(s string) []string {
	var out []string
	cur := ""
	for _, r := range s {
		if r == '.' {
			out = append(out, cur)
			cur = ""
			continue
		}
		cur += string(r)
	}
	return append(out, cur)
}

func TestParseAMatchesOwnerName(t *testing.T) {
	q := dnsQuestion("salon.local")
	if got := parseA(mdnsAnswer(q, "other.local", [4]byte{6, 6, 6, 6}), "salon.local"); len(got) != 0 {
		t.Errorf("a record for another name accepted: %v", got)
	}
	if got := parseA(mdnsAnswer(q, "SALON.local", [4]byte{192, 168, 1, 9}), "salon.local."); len(got) != 1 || got[0].String() != "192.168.1.9" {
		t.Errorf("matching name refused (case-insensitive): %v", got)
	}
}

// A responder that answers with another host's A record is ignored; the right one is used.
func TestMDNSLookupIgnoresOtherNames(t *testing.T) {
	srv, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer srv.Close()
	go func() {
		buf := make([]byte, 1500)
		n, from, err := srv.ReadFrom(buf)
		if err != nil || binary.BigEndian.Uint16(buf[4:]) != 1 {
			return
		}
		q := buf[:n]
		srv.WriteTo(mdnsAnswer(q, "evil.local", [4]byte{6, 6, 6, 6}), from)
		srv.WriteTo(mdnsAnswer(q, "salon.local", [4]byte{192, 168, 1, 77}), from)
	}()
	ip, err := MDNSLookup(context.Background(), "salon.local", srv.LocalAddr().String(), time.Second)
	if err != nil || ip.String() != "192.168.1.77" {
		t.Fatalf("%v %v", ip, err)
	}
}

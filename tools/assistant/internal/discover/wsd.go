package discover

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/guard"
)

// WSDTarget is the WS-Discovery multicast address.
const WSDTarget = "239.255.255.250:3702"

// Found is one ONVIF device that answered a WS-Discovery Probe.
type Found struct {
	IP     string
	XAddrs []string
	Name   string // from the onvif://www.onvif.org/name/ scope, when present
	Hard   string // hardware scope
}

func newUUID() string {
	var b [16]byte
	_, _ = rand.Read(b[:])
	b[6] = b[6]&0x0f | 0x40
	b[8] = b[8]&0x3f | 0x80
	h := hex.EncodeToString(b[:])
	return h[0:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20] + "-" + h[20:]
}

func wsdProbe() []byte {
	return []byte(`<?xml version="1.0" encoding="UTF-8"?>` +
		`<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" ` +
		`xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" xmlns:dn="http://www.onvif.org/ver10/network/wsdl">` +
		`<e:Header><w:MessageID>uuid:` + newUUID() + `</w:MessageID>` +
		`<w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>` +
		`<w:Action e:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header>` +
		`<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body></e:Envelope>`)
}

var (
	xaddrsRe = regexp.MustCompile(`(?s)<[A-Za-z0-9]*:?XAddrs>(.*?)</[A-Za-z0-9]*:?XAddrs>`)
	scopesRe = regexp.MustCompile(`(?s)<[A-Za-z0-9]*:?Scopes[^>]*>(.*?)</[A-Za-z0-9]*:?Scopes>`)
)

// ParseProbeMatch reads one ProbeMatches datagram.
func ParseProbeMatch(from net.IP, data []byte) (Found, bool) {
	s := string(data)
	if !strings.Contains(s, "ProbeMatch") {
		return Found{}, false
	}
	f := Found{IP: from.String()}
	if m := xaddrsRe.FindStringSubmatch(s); m != nil {
		f.XAddrs = strings.Fields(m[1])
	}
	if m := scopesRe.FindStringSubmatch(s); m != nil {
		for _, sc := range strings.Fields(m[1]) {
			if i := strings.Index(sc, "/name/"); i >= 0 {
				f.Name = sc[i+6:]
			}
			if i := strings.Index(sc, "/hardware/"); i >= 0 {
				f.Hard = sc[i+10:]
			}
		}
	}
	return f, true
}

// WSDiscover sends one Probe to target (WSDTarget, or a unicast address in tests) and collects
// the answers for wait. One UDP packet out, nothing else.
func WSDiscover(ctx context.Context, target string, wait time.Duration) ([]Found, error) {
	return wsdOn(ctx, net.ListenConfig{}, target, wait)
}

// WSDiscoverEach sends the Probe once per eligible interface, from a socket whose outgoing
// multicast interface is that one (the OS default interface is often a VPN or a bridge), and merges
// the answers. Interfaces are probed in parallel, so the wait is not multiplied.
func WSDiscoverEach(ctx context.Context, goos string, ifaces []Iface, target string, wait time.Duration) ([]Found, error) {
	var mu sync.Mutex
	var out []Found
	seen := map[string]bool{}
	var firstErr error
	var wg guard.Group
	sent := 0
	for _, ifc := range ifaces {
		if !ifc.Eligible(goos) || ifc.Flags&net.FlagMulticast == 0 {
			continue
		}
		nets := ifc.IPv4s()
		if len(nets) == 0 {
			continue
		}
		sent++
		lc := multicastConfig(nets[0].IP.To4())
		wg.Go(func() {
			got, err := wsdOn(ctx, lc, target, wait)
			mu.Lock()
			defer mu.Unlock()
			if err != nil && firstErr == nil {
				firstErr = err
			}
			for _, f := range got {
				if !seen[f.IP] {
					seen[f.IP] = true
					out = append(out, f)
				}
			}
		})
	}
	wg.Wait()
	if len(out) > 0 || sent == 0 {
		return out, nil
	}
	return out, firstErr
}

// wsdMaxAnswers bounds the answers kept from one probe (a hostile LAN can answer from many addresses).
const wsdMaxAnswers = 64

func wsdOn(ctx context.Context, lc net.ListenConfig, target string, wait time.Duration) ([]Found, error) {
	pc, err := lc.ListenPacket(ctx, "udp4", ":0")
	if err != nil {
		return nil, err
	}
	defer pc.Close()
	dst, err := net.ResolveUDPAddr("udp4", target)
	if err != nil {
		return nil, err
	}
	if _, err := pc.WriteTo(wsdProbe(), dst); err != nil {
		return nil, err
	}
	var out []Found
	seen := map[string]bool{}
	deadline := time.Now().Add(wait)
	buf := make([]byte, 65535)
	for {
		if ctx.Err() != nil {
			return out, ctx.Err()
		}
		_ = pc.SetReadDeadline(deadline)
		n, from, err := pc.ReadFrom(buf)
		if err != nil {
			break
		}
		ua, ok := from.(*net.UDPAddr)
		if !ok {
			continue
		}
		f, ok := ParseProbeMatch(ua.IP, buf[:n])
		if !ok || seen[f.IP] {
			continue
		}
		if len(out) >= wsdMaxAnswers {
			break
		}
		seen[f.IP] = true
		out = append(out, f)
	}
	return out, nil
}

// ---- mDNS ----

// MDNSTarget is the mDNS multicast address.
const MDNSTarget = "224.0.0.251:5353"

func dnsQuestion(name string) []byte {
	var b []byte
	b = binary.BigEndian.AppendUint16(b, 0) // id
	b = binary.BigEndian.AppendUint16(b, 0) // flags
	b = binary.BigEndian.AppendUint16(b, 1) // qdcount
	b = append(b, 0, 0, 0, 0, 0, 0)
	for _, l := range strings.Split(strings.TrimSuffix(name, "."), ".") {
		b = append(b, byte(len(l)))
		b = append(b, l...)
	}
	b = append(b, 0)
	b = binary.BigEndian.AppendUint16(b, 1)        // A
	b = binary.BigEndian.AppendUint16(b, 1|0x8000) // IN, unicast response wanted
	return b
}

func skipName(b []byte, o int) (int, error) {
	for hops := 0; o < len(b) && hops < 128; hops++ {
		l := int(b[o])
		switch {
		case l == 0:
			return o + 1, nil
		case l&0xC0 == 0xC0:
			return o + 2, nil
		default:
			o += 1 + l
		}
	}
	return 0, errors.New("bad name")
}

// readName decodes the (possibly compressed) name at o, lower-cased and without the final dot.
func readName(b []byte, o int) (string, error) {
	var parts []string
	for hops := 0; hops < 128 && o < len(b); hops++ {
		l := int(b[o])
		switch {
		case l == 0:
			return strings.ToLower(strings.Join(parts, ".")), nil
		case l&0xC0 == 0xC0:
			if o+1 >= len(b) {
				return "", errors.New("bad name")
			}
			o = (l&0x3F)<<8 | int(b[o+1])
		case l&0xC0 != 0 || o+1+l > len(b):
			return "", errors.New("bad name")
		default:
			parts = append(parts, string(b[o+1:o+1+l]))
			o += 1 + l
		}
	}
	return "", errors.New("bad name")
}

// parseA returns the A records of a DNS response whose owner name is want (any owner when want is "").
func parseA(b []byte, want string) []net.IP {
	want = strings.ToLower(strings.TrimSuffix(want, "."))
	if len(b) < 12 {
		return nil
	}
	qd, an := int(binary.BigEndian.Uint16(b[4:])), int(binary.BigEndian.Uint16(b[6:]))
	o := 12
	var err error
	for i := 0; i < qd; i++ {
		if o, err = skipName(b, o); err != nil {
			return nil
		}
		o += 4
	}
	var out []net.IP
	for i := 0; i < an; i++ {
		owner, nerr := readName(b, o)
		if o, err = skipName(b, o); err != nil || o+10 > len(b) {
			return out
		}
		typ, rdlen := binary.BigEndian.Uint16(b[o:]), int(binary.BigEndian.Uint16(b[o+8:]))
		o += 10
		if o+rdlen > len(b) {
			return out
		}
		if typ == 1 && rdlen == 4 && nerr == nil && (want == "" || owner == want) {
			out = append(out, net.IP(append([]byte(nil), b[o:o+4]...)))
		}
		o += rdlen
	}
	return out
}

// MDNSLookup resolves name (for example salon.local) with one mDNS query to target.
func MDNSLookup(ctx context.Context, name, target string, wait time.Duration) (net.IP, error) {
	pc, err := net.ListenPacket("udp4", ":0")
	if err != nil {
		return nil, err
	}
	defer pc.Close()
	dst, err := net.ResolveUDPAddr("udp4", target)
	if err != nil {
		return nil, err
	}
	if _, err := pc.WriteTo(dnsQuestion(name), dst); err != nil {
		return nil, err
	}
	deadline := time.Now().Add(wait)
	buf := make([]byte, 9000)
	for ctx.Err() == nil {
		_ = pc.SetReadDeadline(deadline)
		n, _, err := pc.ReadFrom(buf)
		if err != nil {
			break
		}
		if ips := parseA(buf[:n], name); len(ips) > 0 {
			return ips[0], nil
		}
	}
	return nil, fmt.Errorf("%s did not answer", name)
}

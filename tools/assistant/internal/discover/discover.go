// Package discover finds a camera: a parallel port probe of one address, name resolution (DNS and
// mDNS), ONVIF WS-Discovery and a scan of the local /24. Nothing here sends a packet unless the
// caller was told by the user to (the commands ask first), and every function takes the network as
// a parameter so the tests never leave the loopback.
package discover

import (
	"context"
	"fmt"
	"net"
	"runtime"
	"sort"
	"strconv"
	"sync"
	"time"
)

// DialFunc is net.Dialer.DialContext.
type DialFunc func(ctx context.Context, network, addr string) (net.Conn, error)

func realDial(timeout time.Duration) DialFunc {
	return func(ctx context.Context, network, addr string) (net.Conn, error) {
		d := net.Dialer{Timeout: timeout}
		return d.DialContext(ctx, network, addr)
	}
}

// OpenPorts dials every port of host in parallel and returns the open ones in the order given.
func OpenPorts(ctx context.Context, host string, ports []int, timeout time.Duration, dial DialFunc) []int {
	if dial == nil {
		dial = realDial(timeout)
	}
	open := make([]bool, len(ports))
	var wg sync.WaitGroup
	for i, p := range ports {
		wg.Add(1)
		go func() {
			defer wg.Done()
			c, cancel := context.WithTimeout(ctx, timeout)
			defer cancel()
			conn, err := dial(c, "tcp", net.JoinHostPort(host, strconv.Itoa(p)))
			if err == nil {
				conn.Close()
				open[i] = true
			}
		}()
	}
	wg.Wait()
	var out []int
	for i, p := range ports {
		if open[i] {
			out = append(out, p)
		}
	}
	return out
}

// Kind of a camera, from its open ports.
type Kind string

const (
	Stock   Kind = "stock"   // maintenance telnet open: stock firmware with the toolkit card
	Akam    Kind = "akam"    // SSH, video and control ports open, telnet closed
	Partial Kind = "partial" // only SSH answers (Akam still starting)
	Unknown Kind = "unknown"
)

// Ports are the four camera ports.
type Ports struct{ Telnet, SSH, RTSP, CTL int }

func has(open []int, p int) bool {
	for _, o := range open {
		if o == p {
			return true
		}
	}
	return false
}

// Classify is the port-based decision of find-camera (steps_camera.sh).
func Classify(open []int, p Ports) Kind {
	switch {
	case has(open, p.Telnet):
		return Stock
	case has(open, p.SSH) && has(open, p.RTSP) && has(open, p.CTL):
		return Akam
	case has(open, p.SSH):
		return Partial
	}
	return Unknown
}

// Host is one scan result.
type Host struct {
	IP   string
	Open []int
	Kind Kind
}

// Subnet is a range to scan.
type Subnet struct {
	Iface string
	CIDR  *net.IPNet
	Self  net.IP
	Whole *net.IPNet // the real network of the interface when CIDR is only the part of it scanned, else nil
}

func (s Subnet) String() string { return s.CIDR.String() }

// Hosts lists the addresses of the subnet (without network and broadcast, without Self).
func (s Subnet) Hosts() []net.IP {
	ip := s.CIDR.IP.To4()
	if ip == nil {
		return nil
	}
	ones, bits := s.CIDR.Mask.Size()
	if bits != 32 || ones < 16 {
		return nil
	}
	n := 1 << (32 - ones)
	base := uint32(ip[0])<<24 | uint32(ip[1])<<16 | uint32(ip[2])<<8 | uint32(ip[3])
	var out []net.IP
	for i := 1; i < n-1; i++ {
		v := base + uint32(i)
		a := net.IPv4(byte(v>>24), byte(v>>16), byte(v>>8), byte(v))
		if s.Self != nil && a.Equal(s.Self) {
			continue
		}
		out = append(out, a)
	}
	return out
}

// Subnets returns the scan ranges (see SelectSubnets) of the system's interfaces.
func Subnets(ifaces func() ([]net.Interface, error), addrs func(*net.Interface) ([]net.Addr, error)) ([]Subnet, error) {
	if ifaces == nil {
		ifaces = net.Interfaces
	}
	if addrs == nil {
		addrs = func(i *net.Interface) ([]net.Addr, error) { return i.Addrs() }
	}
	list, err := ifaces()
	if err != nil {
		return nil, err
	}
	return SelectSubnets(runtime.GOOS, Ifaces(list, addrs)), nil
}

// Scan probes the four camera ports of every host of the subnet: at most `conc` dials at once,
// `timeout` each. Hosts with at least one open port are returned, sorted by address.
func Scan(ctx context.Context, sn Subnet, ports Ports, conc int, timeout time.Duration, dial DialFunc) []Host {
	if dial == nil {
		dial = realDial(timeout)
	}
	if conc <= 0 {
		conc = 64
	}
	plist := []int{ports.Telnet, ports.SSH, ports.RTSP, ports.CTL}
	sem := make(chan struct{}, conc)
	var mu sync.Mutex
	var found []Host
	var wg sync.WaitGroup
	for _, ip := range sn.Hosts() {
		ipS := ip.String()
		for _, p := range plist {
			wg.Add(1)
			sem <- struct{}{}
			go func() {
				defer wg.Done()
				defer func() { <-sem }()
				if ctx.Err() != nil {
					return
				}
				c, cancel := context.WithTimeout(ctx, timeout)
				defer cancel()
				conn, err := dial(c, "tcp", net.JoinHostPort(ipS, strconv.Itoa(p)))
				if err != nil {
					return
				}
				conn.Close()
				mu.Lock()
				defer mu.Unlock()
				for i := range found {
					if found[i].IP == ipS {
						found[i].Open = append(found[i].Open, p)
						return
					}
				}
				found = append(found, Host{IP: ipS, Open: []int{p}})
			}()
		}
	}
	wg.Wait()
	for i := range found {
		sort.Slice(found[i].Open, func(a, b int) bool { return rank(plist, found[i].Open[a]) < rank(plist, found[i].Open[b]) })
		found[i].Kind = Classify(found[i].Open, ports)
	}
	sort.Slice(found, func(a, b int) bool { return ipLess(found[a].IP, found[b].IP) })
	return found
}

func rank(l []int, p int) int {
	for i, x := range l {
		if x == p {
			return i
		}
	}
	return len(l)
}

func ipLess(a, b string) bool {
	x, y := net.ParseIP(a).To4(), net.ParseIP(b).To4()
	if x == nil || y == nil {
		return a < b
	}
	return string(x) < string(y)
}

// ScanCount is the number of dials a Scan of the subnet makes.
func ScanCount(sn Subnet) int { return len(sn.Hosts()) * 4 }

// Describe is "192.168.1.0/24 (eth0, 254 addresses, 1016 connections)".
func (s Subnet) Describe() string {
	return fmt.Sprintf("%s%s (%s, %d addresses)", s.CIDR, s.cutNote(), s.Iface, len(s.Hosts()))
}

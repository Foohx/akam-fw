package discover

import (
	"net"
	"strconv"
	"strings"
)

// Iface is the small view of a network interface the discovery rules work on, so the filter is a
// pure function that can be tested with the name lists of every operating system.
type Iface struct {
	Name  string
	Flags net.Flags
	HW    net.HardwareAddr
	Addrs []net.Addr
	Index int
}

var (
	linuxVirtual = []string{"docker", "veth", "br-", "virbr", "vmnet", "vboxnet", "tun", "tap", "wg", "tailscale", "zt", "lo", "podman", "cni", "flannel", "kube",
		"ppp", "ipsec", "vpn", "nordlynx", "gre", "sit", "dummy", "macvtap", "ifb", "ham"}
	windowsVirtual = []string{"vethernet", "loopback pseudo-interface", "vmware network adapter", "virtualbox host-only", "hyper-v",
		"bluetooth network connection", "isatap", "teredo", "wintun", "wireguard", "openvpn", "tailscale", "zerotier", "ppp",
		"vpn", "local area connection*", "wan miniport", "cisco anyconnect", "fortinet", "globalprotect", "juniper"}
	darwinVirtual = []string{"lo", "utun", "awdl", "llw", "bridge", "gif", "stf", "ap", "anpi", "vmenet", "vmnet", "vboxnet", "tap", "tun", "ppp", "ipsec"}
)

// VirtualName reports whether an interface name is one of the virtual, tunnel or loopback names of
// goos (a runtime.GOOS value; anything but windows and darwin follows the Linux rules).
func VirtualName(goos, name string) bool {
	switch goos {
	case "windows":
		l := strings.ToLower(name)
		for _, p := range windowsVirtual {
			if strings.HasPrefix(l, p) || strings.Contains(l, "("+p) {
				return true
			}
		}
		return false
	case "darwin":
		return hasAnyPrefix(name, darwinVirtual)
	}
	return hasAnyPrefix(name, linuxVirtual)
}

func hasAnyPrefix(name string, list []string) bool {
	for _, p := range list {
		if strings.HasPrefix(name, p) {
			return true
		}
	}
	return false
}

// Eligible reports whether discovery may use the interface: up, not loopback, not point-to-point
// (VPNs and tunnels), not virtual for goos.
func (i Iface) Eligible(goos string) bool {
	return i.Flags&net.FlagUp != 0 && i.Flags&(net.FlagLoopback|net.FlagPointToPoint) == 0 && !VirtualName(goos, i.Name)
}

// IPv4s returns the usable (not link-local 169.254/16) IPv4 networks of the interface.
func (i Iface) IPv4s() []*net.IPNet {
	var out []*net.IPNet
	for _, a := range i.Addrs {
		ipn, ok := a.(*net.IPNet)
		if !ok || ipn.IP.To4() == nil || ipn.IP.IsLinkLocalUnicast() {
			continue
		}
		if ones, bits := ipn.Mask.Size(); bits != 32 || ones == 0 {
			continue
		}
		out = append(out, ipn)
	}
	return out
}

// MaxScanBits is the shortest prefix scanned without the user typing the range (/22).
const MaxScanBits = 22

// SelectSubnets returns the scan ranges of the eligible interfaces. A network larger than /22 is
// cut to the /24 around the interface address; a /23 or /22 is kept whole.
func SelectSubnets(goos string, ifaces []Iface) []Subnet {
	var out []Subnet
	have := map[string]bool{}
	for _, ifc := range ifaces {
		if !ifc.Eligible(goos) {
			continue
		}
		for _, ipn := range ifc.IPv4s() {
			ones, _ := ipn.Mask.Size()
			if ones < MaxScanBits {
				ones = 24
			}
			mask := net.CIDRMask(ones, 32)
			sn := Subnet{Iface: ifc.Name, CIDR: &net.IPNet{IP: ipn.IP.To4().Mask(mask), Mask: mask}, Self: ipn.IP.To4()}
			if o, _ := ipn.Mask.Size(); o < ones {
				sn.Whole = &net.IPNet{IP: ipn.IP.To4().Mask(ipn.Mask), Mask: ipn.Mask}
			}
			// a /31 or /32 has no host to scan; two interfaces on one LAN are scanned once
			if len(sn.Hosts()) == 0 || have[sn.CIDR.String()] {
				continue
			}
			have[sn.CIDR.String()] = true
			out = append(out, sn)
		}
	}
	return out
}

// Question is the opt-in sentence of the scan: the range, its size and the ports.
func (s Subnet) Question(ports []int) string {
	p := make([]string, len(ports))
	for i, x := range ports {
		p[i] = strconv.Itoa(x)
	}
	return "Scan " + s.CIDR.String() + s.cutNote() + ": " + strconv.Itoa(len(s.Hosts())) + " addresses, ports " + strings.Join(p, ", ") + " only?"
}

// Ifaces converts system interfaces; addrs reads the addresses of one.
func Ifaces(list []net.Interface, addrs func(*net.Interface) ([]net.Addr, error)) []Iface {
	out := make([]Iface, 0, len(list))
	for i := range list {
		as, err := addrs(&list[i])
		if err != nil {
			continue
		}
		out = append(out, Iface{Name: list[i].Name, Flags: list[i].Flags, HW: list[i].HardwareAddr, Addrs: as, Index: list[i].Index})
	}
	return out
}

// cutNote tells that the scanned range is only a part of the network of the interface.
func (s Subnet) cutNote() string {
	if s.Whole == nil {
		return ""
	}
	return " (a part of " + s.Whole.String() + ": the range around this computer)"
}

//go:build windows

package discover

import (
	"net"
	"syscall"
)

// multicastConfig returns a ListenConfig whose sockets send multicast out of the interface that
// owns ip (IP_MULTICAST_IF), instead of the OS default one. On Windows the descriptor is a Handle.
func multicastConfig(ip net.IP) net.ListenConfig {
	var a [4]byte
	copy(a[:], ip.To4())
	return net.ListenConfig{Control: func(network, address string, c syscall.RawConn) error {
		var serr error
		if err := c.Control(func(fd uintptr) {
			serr = syscall.SetsockoptInet4Addr(syscall.Handle(fd), syscall.IPPROTO_IP, syscall.IP_MULTICAST_IF, a)
		}); err != nil {
			return err
		}
		return serr
	}}
}

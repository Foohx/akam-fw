package ssh

import (
	"bytes"
	"context"
	"encoding/base64"
	"errors"
	"net"
	"os"
	"strconv"
	"time"

	gossh "golang.org/x/crypto/ssh"
)

// DialFunc opens a connection (net.Dialer.DialContext in production, a fake in tests).
type DialFunc func(ctx context.Context, network, addr string) (net.Conn, error)

var errProbeDone = errors.New("host key read")

// ProbeHostKey reads the ssh-ed25519 host key an address presents: the key exchange of the SSH
// handshake and nothing else (no login is attempted, no command is run, the connection is dropped as
// soon as the key is known). The key is the only identity of a camera that may be trusted for
// "same camera, new address": text the camera supplies (host name, version) is never used for it.
func ProbeHostKey(ctx context.Context, host string, port int, dial DialFunc, timeout time.Duration) (gossh.PublicKey, error) {
	if timeout <= 0 {
		timeout = 3 * time.Second
	}
	if dial == nil {
		d := net.Dialer{Timeout: timeout}
		dial = d.DialContext
	}
	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	addr := net.JoinHostPort(host, strconv.Itoa(port))
	raw, err := dial(ctx, "tcp", addr)
	if err != nil {
		return nil, err
	}
	defer raw.Close()
	_ = raw.SetDeadline(time.Now().Add(timeout))
	var got gossh.PublicKey
	cfg := &gossh.ClientConfig{
		User:              "root",
		HostKeyAlgorithms: []string{gossh.KeyAlgoED25519},
		HostKeyCallback: func(_ string, _ net.Addr, key gossh.PublicKey) error {
			got = key
			return errProbeDone // stop here: nothing is authenticated
		},
		ClientVersion: "SSH-2.0-akam",
		Timeout:       timeout,
	}
	_, _, _, err = gossh.NewClientConn(raw, addr, cfg)
	if got == nil {
		if err == nil {
			err = errors.New("no host key offered")
		}
		return nil, err
	}
	if got.Type() != gossh.KeyAlgoED25519 {
		return nil, errors.New("the address offered a " + got.Type() + " host key; Akam cameras use ed25519 only")
	}
	return got, nil
}

// Fingerprint is the SHA256:... fingerprint of a key.
func Fingerprint(key gossh.PublicKey) string { return gossh.FingerprintSHA256(key) }

// PinnedFingerprint is the fingerprint of the key pinned in file for alias ("" when none).
func PinnedFingerprint(file, alias string) string {
	b, err := os.ReadFile(file)
	if err != nil {
		return ""
	}
	for _, l := range bytes.Split(b, []byte("\n")) {
		f := bytes.Fields(l)
		if len(f) < 3 || bytes.HasPrefix(f[0], []byte("#")) || !hostsContain(string(f[0]), alias) || string(f[1]) != gossh.KeyAlgoED25519 {
			continue
		}
		raw, err := base64.StdEncoding.DecodeString(string(f[2]))
		if err != nil {
			continue
		}
		if k, err := gossh.ParsePublicKey(raw); err == nil {
			return gossh.FingerprintSHA256(k)
		}
	}
	return ""
}

// PinHostKey pins key for alias in file (the user said yes to "same camera, new address").
func PinHostKey(file, alias string, key gossh.PublicKey) error { return appendPin(file, alias, key) }

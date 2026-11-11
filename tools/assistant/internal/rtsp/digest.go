package rtsp

import (
	"crypto/md5" //nolint:gosec // RTSP Digest (RFC 2617) is defined on MD5; nothing else uses it
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"strings"
)

// Digest is the client side of RFC 2617 (MD5, qop=auth or the RFC 2069 form) with nonce counting.
type Digest struct {
	User, Pass string
	Realm      string
	Nonce      string
	QopAuth    bool
	nc         uint32
	// CNonce overrides the random client nonce (tests).
	CNonce func() string
}

func md5hex(parts ...string) string {
	h := md5.New() //nolint:gosec
	for _, p := range parts {
		h.Write([]byte(p))
	}
	return hex.EncodeToString(h.Sum(nil))
}

// ParseChallenge reads a WWW-Authenticate value ("Digest realm=..., nonce=..., qop=...").
// It reports false when it is not a Digest challenge.
func (d *Digest) ParseChallenge(h string) bool {
	if len(h) < 6 || !strings.EqualFold(h[:6], "Digest") {
		return false
	}
	d.Realm = param(h, "realm")
	d.Nonce = param(h, "nonce")
	q := param(h, "qop")
	d.QopAuth = strings.Contains(q, "auth")
	d.nc = 0
	return d.Nonce != ""
}

// param extracts key=value or key="value" (case-insensitive key, whole word).
func param(h, key string) string {
	low := strings.ToLower(h)
	k := strings.ToLower(key)
	for i := 0; ; {
		j := strings.Index(low[i:], k)
		if j < 0 {
			return ""
		}
		j += i
		i = j + len(k)
		if j > 0 {
			c := h[j-1]
			if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' {
				continue
			}
		}
		if i >= len(h) || h[i] != '=' {
			continue
		}
		v := h[i+1:]
		if strings.HasPrefix(v, `"`) {
			v = v[1:]
			if e := strings.IndexByte(v, '"'); e >= 0 {
				return v[:e]
			}
			return v
		}
		if e := strings.IndexAny(v, ", \t\r\n"); e >= 0 {
			return v[:e]
		}
		return v
	}
}

func (d *Digest) cnonce() string {
	if d.CNonce != nil {
		return d.CNonce()
	}
	var b [8]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

// Header returns the Authorization header line (without CRLF), or "" before a challenge.
func (d *Digest) Header(method, uri string) string {
	if d.Nonce == "" {
		return ""
	}
	ha1 := md5hex(d.User, ":", d.Realm, ":", d.Pass)
	ha2 := md5hex(method, ":", uri)
	if d.QopAuth {
		d.nc++
		nc := fmt.Sprintf("%08x", d.nc)
		cn := d.cnonce()
		resp := md5hex(ha1, ":", d.Nonce, ":", nc, ":", cn, ":", "auth", ":", ha2)
		return fmt.Sprintf(`Authorization: Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s", qop=auth, nc=%s, cnonce="%s"`,
			d.User, d.Realm, d.Nonce, uri, resp, nc, cn)
	}
	resp := md5hex(ha1, ":", d.Nonce, ":", ha2)
	return fmt.Sprintf(`Authorization: Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s"`,
		d.User, d.Realm, d.Nonce, uri, resp)
}

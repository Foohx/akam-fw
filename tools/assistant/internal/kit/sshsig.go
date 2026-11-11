// Package kit verifies an Akam release kit: SHA256SUMS, the OpenSSH signature (SSHSIG, the format
// `ssh-keygen -Y sign` writes and `ssh-keygen -Y verify` reads), the tarball layout and the kit's
// own MANIFEST. It is the Go port of tools/release/verify_kit.sh. Nothing from a kit is ever executed.
package kit

import (
	"bytes"
	"crypto/sha256"
	"crypto/sha512"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	gossh "golang.org/x/crypto/ssh"
)

const (
	// SigIdentity and SigNamespace are fixed: the maintainer signs with `-n akam-release` and the
	// allowed_signers principal is akam-release.
	SigIdentity  = "akam-release"
	SigNamespace = "akam-release"

	armorBegin = "-----BEGIN SSH SIGNATURE-----"
	armorEnd   = "-----END SSH SIGNATURE-----"
)

// Signature is a parsed SSHSIG.
type Signature struct {
	PublicKey gossh.PublicKey
	Namespace string
	HashAlg   string
	Sig       *gossh.Signature
}

type sshsigFields struct {
	Version   uint32
	PubKey    []byte
	Namespace string
	Reserved  string
	HashAlg   string
	Signature []byte
}

// ParseSignature reads an armored SSH signature.
func ParseSignature(armored []byte) (*Signature, error) {
	s := strings.TrimSpace(string(armored))
	lines := strings.Split(strings.ReplaceAll(s, "\r\n", "\n"), "\n")
	if len(lines) < 3 || strings.TrimSpace(lines[0]) != armorBegin || strings.TrimSpace(lines[len(lines)-1]) != armorEnd {
		return nil, errors.New("not an armored SSH signature")
	}
	raw, err := base64.StdEncoding.DecodeString(strings.Join(lines[1:len(lines)-1], ""))
	if err != nil {
		return nil, errors.New("signature is not valid base64")
	}
	if len(raw) < 6 || string(raw[:6]) != "SSHSIG" {
		return nil, errors.New("bad signature magic")
	}
	var f sshsigFields
	if err := gossh.Unmarshal(raw[6:], &f); err != nil {
		return nil, errors.New("malformed signature")
	}
	if f.Version != 1 {
		return nil, fmt.Errorf("unsupported signature version %d", f.Version)
	}
	pub, err := gossh.ParsePublicKey(f.PubKey)
	if err != nil {
		return nil, fmt.Errorf("bad public key in the signature: %v", err)
	}
	var sig gossh.Signature
	if err := gossh.Unmarshal(f.Signature, &sig); err != nil {
		return nil, errors.New("bad signature blob")
	}
	return &Signature{PublicKey: pub, Namespace: f.Namespace, HashAlg: f.HashAlg, Sig: &sig}, nil
}

func (s *Signature) signedData(message []byte) ([]byte, error) {
	var h []byte
	switch s.HashAlg {
	case "sha256":
		x := sha256.Sum256(message)
		h = x[:]
	case "sha512":
		x := sha512.Sum512(message)
		h = x[:]
	default:
		return nil, fmt.Errorf("unsupported hash algorithm %q", s.HashAlg)
	}
	str := func(b []byte) []byte { return gossh.Marshal(struct{ S []byte }{b}) }
	var buf bytes.Buffer
	buf.WriteString("SSHSIG")
	buf.Write(str([]byte(s.Namespace)))
	buf.Write(str(nil))
	buf.Write(str([]byte(s.HashAlg)))
	buf.Write(str(h))
	return buf.Bytes(), nil
}

// Allowed is one line of an allowed_signers file.
type Allowed struct {
	Principals []string
	Namespaces []string // empty: any
	Key        gossh.PublicKey
}

// ParseAllowedSigners reads the ssh-keygen allowed_signers format. Comment and blank lines are
// skipped; a line that cannot be parsed is an error (never silently ignored).
func ParseAllowedSigners(b []byte) ([]Allowed, error) {
	var out []Allowed
	for n, line := range strings.Split(string(b), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := splitQuoted(line)
		if len(fields) < 3 {
			return nil, fmt.Errorf("allowed_signers line %d: too few fields", n+1)
		}
		a := Allowed{Principals: strings.Split(fields[0], ",")}
		i := 1
		if !isKeyType(fields[i]) {
			ns, err := parseSignerOptions(fields[i])
			if err != nil {
				return nil, fmt.Errorf("allowed_signers line %d: %v", n+1, err)
			}
			a.Namespaces = ns
			i++
		}
		if i+1 >= len(fields) {
			return nil, fmt.Errorf("allowed_signers line %d: no key", n+1)
		}
		key, _, _, _, err := gossh.ParseAuthorizedKey([]byte(fields[i] + " " + fields[i+1]))
		if err != nil {
			return nil, fmt.Errorf("allowed_signers line %d: %v", n+1, err)
		}
		a.Key = key
		out = append(out, a)
	}
	return out, nil
}

// parseSignerOptions reads the options field. Only namespaces="a,b" is understood; any other
// option (cert-authority, valid-after, valid-before, ...) is an error, never silently dropped: a
// restriction that is ignored would widen what a key may sign.
func parseSignerOptions(f string) ([]string, error) {
	rest := f
	var ns []string
	for rest != "" {
		v, ok := strings.CutPrefix(rest, `namespaces="`)
		if !ok {
			return nil, fmt.Errorf("unsupported option %q (only namespaces=\"...\" is understood)", rest)
		}
		end := strings.IndexByte(v, '"')
		if end < 0 {
			return nil, errors.New("unterminated namespaces option")
		}
		ns = append(ns, strings.Split(v[:end], ",")...)
		rest = strings.TrimPrefix(v[end+1:], ",")
	}
	return ns, nil
}

func isKeyType(s string) bool {
	return strings.HasPrefix(s, "ssh-") || strings.HasPrefix(s, "ecdsa-") || strings.HasPrefix(s, "sk-")
}

// splitQuoted splits on blanks, keeping double-quoted parts together.
func splitQuoted(s string) []string {
	var out []string
	var cur strings.Builder
	q := false
	for _, r := range s {
		switch {
		case r == '"':
			q = !q
			cur.WriteRune(r)
		case (r == ' ' || r == '\t') && !q:
			if cur.Len() > 0 {
				out = append(out, cur.String())
				cur.Reset()
			}
		default:
			cur.WriteRune(r)
		}
	}
	if cur.Len() > 0 {
		out = append(out, cur.String())
	}
	return out
}

// HasKeys reports whether the file holds at least one non-comment line (the bash test:
// grep -qvE '^[[:space:]]*(#|$)').
func HasKeys(b []byte) bool {
	for _, l := range strings.Split(string(b), "\n") {
		l = strings.TrimSpace(l)
		if l != "" && !strings.HasPrefix(l, "#") {
			return true
		}
	}
	return false
}

// Verify checks sig over message for principal and namespace against the allowed signers. On
// success it returns the text `ssh-keygen -Y verify` prints.
func Verify(allowed []Allowed, principal, namespace string, sig *Signature, message []byte) (string, error) {
	if sig.Namespace != namespace {
		return "", fmt.Errorf("signature namespace %q does not match %q", sig.Namespace, namespace)
	}
	data, err := sig.signedData(message)
	if err != nil {
		return "", err
	}
	matched := false
	for _, a := range allowed {
		if !principalMatch(a.Principals, principal) {
			continue
		}
		if len(a.Namespaces) > 0 && !contains(a.Namespaces, namespace) {
			continue
		}
		if bytes.Equal(a.Key.Marshal(), sig.PublicKey.Marshal()) {
			matched = true
		}
	}
	if !matched {
		return "", errors.New("no principal matched (the signing key is not in allowed_signers for this identity and namespace)")
	}
	if sig.Sig.Format == gossh.KeyAlgoRSA {
		return "", errors.New("RSA signatures with SHA-1 (ssh-rsa) are refused: use rsa-sha2-256 or -512, or ed25519")
	}
	if err := sig.PublicKey.Verify(data, sig.Sig); err != nil {
		return "", fmt.Errorf("signature verification failed: %v", err)
	}
	return fmt.Sprintf("Good \"%s\" signature for %s with %s key %s", namespace, principal, keyKindName(sig.PublicKey), gossh.FingerprintSHA256(sig.PublicKey)), nil
}

func keyKindName(k gossh.PublicKey) string {
	switch k.Type() {
	case gossh.KeyAlgoED25519:
		return "ED25519"
	case gossh.KeyAlgoRSA:
		return "RSA"
	}
	return strings.ToUpper(strings.TrimPrefix(k.Type(), "ssh-"))
}

func principalMatch(list []string, p string) bool {
	for _, x := range list {
		if x == p || x == "*" {
			return true
		}
	}
	return false
}

func contains(l []string, s string) bool {
	for _, x := range l {
		if x == s {
			return true
		}
	}
	return false
}

// SignForTest builds an armored SSHSIG as `ssh-keygen -Y sign -n namespace` would. It exists for
// the tests only: the program never signs (signing stays on the maintainer's side).
func SignForTest(signer gossh.Signer, namespace, hashAlg string, message []byte) []byte {
	s := &Signature{PublicKey: signer.PublicKey(), Namespace: namespace, HashAlg: hashAlg}
	data, err := s.signedData(message)
	if err != nil {
		panic(err)
	}
	sig, err := signer.Sign(nil, data)
	if err != nil {
		panic(err)
	}
	blob := []byte("SSHSIG")
	blob = append(blob, gossh.Marshal(sshsigFields{1, signer.PublicKey().Marshal(), namespace, "", hashAlg, gossh.Marshal(sig)})...)
	enc := base64.StdEncoding.EncodeToString(blob)
	var sb strings.Builder
	sb.WriteString(armorBegin + "\n")
	for len(enc) > 70 {
		sb.WriteString(enc[:70] + "\n")
		enc = enc[70:]
	}
	sb.WriteString(enc + "\n" + armorEnd + "\n")
	return []byte(sb.String())
}

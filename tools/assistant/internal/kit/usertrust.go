package kit

// Extra signers the user trusts (akam trust-key): a file in the assistant's user state, in the ssh-keygen
// allowed_signers format, one ed25519 key per line, principal and namespace exactly those of
// tools/release/sign_kit.sh and verify_kit.sh. It only ever ADDS to the embedded official key; it never
// replaces it and never makes a download unsigned.

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	gossh "golang.org/x/crypto/ssh"

	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
)

// TrustFile is the name of the user trust file inside the state folder.
const TrustFile = "trusted_signers"

// MaxPubKeyFile caps a public key file (a real one is under 200 bytes).
const MaxPubKeyFile = 4096

// OfficialLabel names the embedded key in messages.
const OfficialLabel = "official Akam release key"

var labelRe = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,39}$`)

// ValidLabel reports whether s can name a trusted key.
func ValidLabel(s string) bool { return labelRe.MatchString(s) && strings.ToLower(s) != "official" }

// TrustedKey is one line of the trust file.
type TrustedKey struct {
	Label       string
	Fingerprint string // SHA256:...
	line        string
}

// ParsePubKeyFile validates the content of a .pub file: exactly one non-comment line holding an ed25519 key
// (no options, no other key type, no private key), a comment that is only used as a default label. It returns the
// key and that comment.
func ParsePubKeyFile(b []byte) (gossh.PublicKey, string, error) {
	if len(b) > MaxPubKeyFile {
		return nil, "", errors.New("the file is too large to be a public key")
	}
	if strings.Contains(string(b), "PRIVATE KEY") {
		return nil, "", errors.New("this is a PRIVATE key: give the public key (.pub) and keep the private one secret")
	}
	var lines []string
	for _, l := range strings.Split(string(b), "\n") {
		l = strings.TrimSpace(l)
		if l != "" && !strings.HasPrefix(l, "#") {
			lines = append(lines, l)
		}
	}
	switch {
	case len(lines) == 0:
		return nil, "", errors.New("no public key found in the file")
	case len(lines) > 1:
		return nil, "", errors.New("the file holds more than one key: give one public key per file")
	}
	for _, r := range lines[0] {
		if r < 0x20 || r == 0x7f {
			return nil, "", errors.New("the key line holds a control character")
		}
	}
	f := strings.Fields(lines[0])
	if len(f) < 2 || f[0] != gossh.KeyAlgoED25519 {
		return nil, "", errors.New("only an ed25519 public key is accepted (a line 'ssh-ed25519 AAAA... comment', as ssh-keygen -t ed25519 writes)")
	}
	key, _, _, _, err := gossh.ParseAuthorizedKey([]byte(f[0] + " " + f[1]))
	if err != nil || key.Type() != gossh.KeyAlgoED25519 {
		return nil, "", errors.New("this is not a valid ed25519 public key")
	}
	comment := ""
	if len(f) > 2 {
		comment = f[2]
	}
	return key, comment, nil
}

func trustLine(label string, key gossh.PublicKey) string {
	return fmt.Sprintf(`%s namespaces="%s" %s %s %s`, SigIdentity, SigNamespace, key.Type(),
		strings.Fields(string(gossh.MarshalAuthorizedKey(key)))[1], label)
}

// UserTrust is the trust file of a state folder.
type UserTrust struct{ Path string }

// OpenUserTrust is the trust file inside the state folder base.
func OpenUserTrust(base string) *UserTrust { return &UserTrust{Path: filepath.Join(base, TrustFile)} }

// List reads the keys. A line that does not parse is an error (the file is edited by hand at the owner's risk;
// nothing is silently dropped or trusted).
func (u *UserTrust) List() ([]TrustedKey, error) {
	b, err := os.ReadFile(u.Path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []TrustedKey
	for n, l := range strings.Split(string(b), "\n") {
		l = strings.TrimSpace(l)
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		al, err := ParseAllowedSigners([]byte(l))
		if err != nil || len(al) != 1 || al[0].Key.Type() != gossh.KeyAlgoED25519 {
			return nil, fmt.Errorf("%s line %d is not a valid trusted key", TrustFile, n+1)
		}
		f := splitQuoted(l)
		label := ""
		if len(f) > 4 {
			label = f[len(f)-1]
		}
		out = append(out, TrustedKey{Label: label, Fingerprint: gossh.FingerprintSHA256(al[0].Key), line: l})
	}
	return out, nil
}

// Bytes is the allowed_signers content to verify with (nil when there is no key); an unreadable or damaged
// file is an error, so that a verification never silently drops a key the user relies on.
func (u *UserTrust) Bytes() ([]byte, error) {
	ks, err := u.List()
	if err != nil || len(ks) == 0 {
		return nil, err
	}
	var sb strings.Builder
	for _, k := range ks {
		sb.WriteString(k.line + "\n")
	}
	return []byte(sb.String()), nil
}

func (u *UserTrust) write(ks []TrustedKey) error {
	if err := fsutil.PrivateMkdir(filepath.Dir(u.Path)); err != nil {
		return err
	}
	var sb strings.Builder
	sb.WriteString("# Keys you trust to sign Akam kits (akam trust-key). allowed_signers format; edit with the command.\n")
	for _, k := range ks {
		sb.WriteString(k.line + "\n")
	}
	if err := fsutil.AtomicWrite(u.Path, []byte(sb.String()), 0o600); err != nil {
		return err
	}
	fsutil.PrivateFile(u.Path)
	return nil
}

// Add stores key under label. Refused: a bad label, a label already used, a key already trusted.
func (u *UserTrust) Add(label string, key gossh.PublicKey) (TrustedKey, error) {
	if !ValidLabel(label) {
		return TrustedKey{}, errors.New("a label is 1 to 40 letters, digits, '.', '_' or '-' (not 'official')")
	}
	ks, err := u.List()
	if err != nil {
		return TrustedKey{}, err
	}
	fp := gossh.FingerprintSHA256(key)
	for _, k := range ks {
		switch {
		case k.Label == label:
			return TrustedKey{}, fmt.Errorf("a key is already trusted under the label %q (remove it first)", label)
		case k.Fingerprint == fp:
			return TrustedKey{}, fmt.Errorf("this key is already trusted (label %q)", k.Label)
		}
	}
	nk := TrustedKey{Label: label, Fingerprint: fp, line: trustLine(label, key)}
	return nk, u.write(append(ks, nk))
}

// Remove deletes the key with that label; it reports whether there was one.
func (u *UserTrust) Remove(label string) (bool, error) {
	ks, err := u.List()
	if err != nil {
		return false, err
	}
	var keep []TrustedKey
	found := false
	for _, k := range ks {
		if k.Label == label {
			found = true
			continue
		}
		keep = append(keep, k)
	}
	if !found {
		return false, nil
	}
	return true, u.write(keep)
}

// ---------------------------------------------------------------- the release source

// SourceFile is the name of the file that remembers the chosen release source.
const SourceFile = "release_source"

var (
	ownerRe = regexp.MustCompile(`^[A-Za-z0-9](?:[A-Za-z0-9]|-[A-Za-z0-9]){0,38}$`)
	repoRe  = regexp.MustCompile(`^[A-Za-z0-9._-]{1,100}$`)
)

// ValidRepo reports whether s is a GitHub "owner/name": nothing else can reach the download address (no scheme,
// host, port, user, query, fragment, backslash, percent-encoding, "." or "..").
func ValidRepo(s string) bool {
	o, n, ok := strings.Cut(s, "/")
	if !ok || !ownerRe.MatchString(o) || !repoRe.MatchString(n) {
		return false
	}
	return n != "." && n != ".." && !strings.Contains(n, "..")
}

// ReadSource returns the remembered release source ("" for the official one).
func ReadSource(base string) (string, error) {
	b, err := os.ReadFile(filepath.Join(base, SourceFile))
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	s := strings.TrimSpace(string(b))
	if s != "" && !ValidRepo(s) {
		return "", fmt.Errorf("%s does not hold a valid owner/name", SourceFile)
	}
	return s, nil
}

// WriteSource remembers repo; "" (or the official one, the caller decides) removes the file.
func WriteSource(base, repo string) error {
	p := filepath.Join(base, SourceFile)
	if repo == "" {
		if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
			return err
		}
		return nil
	}
	if !ValidRepo(repo) {
		return errors.New("not a valid owner/name")
	}
	if err := fsutil.PrivateMkdir(base); err != nil {
		return err
	}
	return fsutil.AtomicWrite(p, []byte(repo+"\n"), 0o600)
}

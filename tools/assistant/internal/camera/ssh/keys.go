package ssh

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/pem"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
	gossh "golang.org/x/crypto/ssh"
	"golang.org/x/crypto/ssh/agent"
)

// DefaultKeyPath is ~/.ssh/akam, the dedicated key the bash wizard creates with
// ssh-keygen -t ed25519 -C akam and puts (its .pub) on the flash card.
func DefaultKeyPath() string {
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".ssh", "akam")
}

// KeyPathFor mirrors cam_key in steps_camera.sh: --identity wins; else the private key beside
// the public key (--pubkey, the state key ssh_pubkey, or ~/.ssh/akam.pub) without ".pub".
func KeyPathFor(identity, pubkeyOpt, statePub string) string {
	if identity != "" {
		return identity
	}
	pub := pubkeyOpt
	if pub == "" {
		pub = statePub
	}
	if pub == "" {
		pub = DefaultKeyPath() + ".pub"
	}
	return strings.TrimSuffix(pub, ".pub")
}

// GenerateKey creates an ed25519 key pair in OpenSSH format: path (0600) and path.pub (0644),
// comment "akam" by default. It never overwrites an existing file. Both files are readable by
// ssh -i and ssh-keygen.
func GenerateKey(path, comment string, passphrase []byte) error {
	if comment == "" {
		comment = "akam"
	}
	for _, p := range []string{path, path + ".pub"} {
		if _, err := os.Stat(p); err == nil {
			return fmt.Errorf("%s already exists: not overwriting it", p)
		}
	}
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return err
	}
	var block *pem.Block
	if len(passphrase) > 0 {
		block, err = gossh.MarshalPrivateKeyWithPassphrase(priv, comment, passphrase)
	} else {
		block, err = gossh.MarshalPrivateKey(priv, comment)
	}
	if err != nil {
		return err
	}
	pemBytes := pem.EncodeToMemory(block)
	if err := fsutil.PrivateMkdir(filepath.Dir(path)); err != nil {
		return err
	}
	if err := os.WriteFile(path, pemBytes, 0o600); err != nil {
		return err
	}
	// Windows: OpenSSH refuses a key other accounts can read; a key we cannot protect is not kept.
	if err := fsutil.PrivateFileStrict(path); err != nil {
		os.Remove(path)
		return err
	}
	sp, err := gossh.NewPublicKey(pub)
	if err != nil {
		return err
	}
	line := strings.TrimRight(string(gossh.MarshalAuthorizedKey(sp)), "\n") + " " + comment + "\n"
	return os.WriteFile(path+".pub", []byte(line), 0o644)
}

// LoadSigner reads a private key. A key with a passphrase is signed through the ssh-agent
// (SSH_AUTH_SOCK, the agent key matching the .pub beside the file) or, failing that, with the
// passphrase returned by ask (no echo). It returns an error rather than prompting when ask is nil.
func LoadSigner(path string, ask func() (string, error)) (gossh.Signer, func(), error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, fmt.Errorf("cannot read the SSH key %s: %w", path, err)
	}
	if s, err := gossh.ParsePrivateKey(b); err == nil {
		return s, func() {}, nil
	} else {
		var pm *gossh.PassphraseMissingError
		if !errors.As(err, &pm) {
			return nil, nil, fmt.Errorf("%s is not a usable private key: %w", path, err)
		}
	}
	if s, closeFn := agentSigner(path); s != nil {
		return s, closeFn, nil
	}
	if ask == nil {
		return nil, nil, fmt.Errorf("the key %s has a passphrase and no ssh-agent holds it", path)
	}
	pw, err := ask()
	if err != nil {
		return nil, nil, err
	}
	s, err := gossh.ParsePrivateKeyWithPassphrase(b, []byte(pw))
	if err != nil {
		return nil, nil, fmt.Errorf("the passphrase for %s was refused", path)
	}
	return s, func() {}, nil
}

func agentSigner(keyPath string) (gossh.Signer, func()) {
	pubBytes, err := os.ReadFile(keyPath + ".pub")
	if err != nil {
		return nil, nil
	}
	want, _, _, _, err := gossh.ParseAuthorizedKey(pubBytes)
	if err != nil {
		return nil, nil
	}
	conn, err := dialAgent(os.Getenv("SSH_AUTH_SOCK"))
	if err != nil {
		return nil, nil
	}
	signers, err := agent.NewClient(conn).Signers()
	if err != nil {
		conn.Close()
		return nil, nil
	}
	for _, s := range signers {
		if string(s.PublicKey().Marshal()) == string(want.Marshal()) {
			return s, func() { conn.Close() }
		}
	}
	conn.Close()
	return nil, nil
}

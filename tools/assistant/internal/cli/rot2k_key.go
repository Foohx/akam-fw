package cli

import (
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// key-2k: create the assistant's API key on the card-less 2K. The camera makes the key itself (its own random pool,
// `akam_api_key.sh new`, the mechanism of docs/porting/lsc-rotatable-2k/control-api.md section 1) and prints the secret
// ONCE; this program reads it from the SSH answer, writes it to the camera's state folder (api_key, 0600) and never
// prints it. The API key is what lets `update` read the model and version and check them after the write.

const keyName2K = "assistant"

// keyToolCmd2K is the script of the camera (installed in the appfs image, run with the shell).
const keyToolCmd2K = "sh /appfs/akam/akam_api_key.sh new --scope admin --name " + keyName2K

var (
	keyIDLine  = regexp.MustCompile(`(?m)^key_id:\s+([0-9a-f]{8})\s*$`)
	keySecLine = regexp.MustCompile(`(?m)^secret:\s+([0-9a-f]{64})\s*$`)
)

// parseNewKeyOutput reads the id and the secret from what `akam_api_key.sh new` printed.
func parseNewKeyOutput(out []byte) (akamapi.Key, error) {
	id, sec := keyIDLine.FindSubmatch(out), keySecLine.FindSubmatch(out)
	if id == nil || sec == nil {
		return akamapi.Key{}, errors.New("the camera's answer has no key id and secret (an older firmware, or the app is not running)")
	}
	return akamapi.ParseKey(string(id[1]) + ":" + string(sec[1]))
}

// storeKey2K writes the key file (0600) atomically.
func storeKey2K(path string, k akamapi.Key) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	tmp := path + ".tmp"
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return err
	}
	_, werr := f.WriteString(k.ID + ":" + hex.EncodeToString(k.Secret) + "\n")
	if werr == nil {
		werr = f.Sync()
	}
	if cerr := f.Close(); werr == nil {
		werr = cerr
	}
	if werr != nil {
		_ = os.Remove(tmp)
		return werr
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// makeKey2K asks the camera for the key, keeps it in keyPath and returns it. The secret is never printed.
func (a *App) makeKey2K(conn appfs.Conn, keyPath string) (akamapi.Key, error) {
	r, err := conn.Run(a.Ctx, keyToolCmd2K, nil)
	if err != nil || r.Status != 0 {
		msg := strings.TrimSpace(string(r.Stderr))
		if strings.Contains(msg, "a key named") {
			return akamapi.Key{}, a.UI.Fail("the camera already has a key named '"+keyName2K+"', but this PC has not kept it.",
				"revoke it on the camera (ssh to it: sh /appfs/akam/akam_api_key.sh revoke "+keyName2K+") and run this command again.")
		}
		return akamapi.Key{}, a.UI.Fail("the camera did not create the key: "+ui.Sanitize(msg)+".", "nothing secret was shown. Check that Akam runs on the camera (card-less boot done, app started).")
	}
	k, err := parseNewKeyOutput(r.Stdout)
	a.Secrets.Add(hex.EncodeToString(k.Secret))
	if err != nil {
		return akamapi.Key{}, a.UI.Fail(ui.Sanitize(err.Error())+".", "")
	}
	if err := storeKey2K(keyPath, k); err != nil {
		// the key exists on the camera and was not kept: said plainly, without the secret
		return akamapi.Key{}, a.UI.Fail("the camera made key "+k.ID+" but it could not be saved on this PC ("+ui.Sanitize(err.Error())+").",
			"revoke it on the camera (ssh to it: sh /appfs/akam/akam_api_key.sh revoke "+keyName2K+") and run this command again.")
	}
	return k, nil
}

func (a *App) cmdKey2K() error {
	if err := a.appfsGate("key-2k"); err != nil {
		return err
	}
	a.UI.Banner("Create the API key of the card-less 2K")
	keyPath := filepath.Join(a.St.Dir, "api_key")
	if a.UI.Dry() {
		a.UI.Say("  WOULD ask the camera (over SSH) to create an admin key named '" + keyName2K + "' (it writes /config/akam/api_keys: a user action),")
		a.UI.Say("  read the secret from its answer without printing it, and store it in " + keyPath + " (0600).")
		return nil
	}
	if _, err := os.Stat(keyPath); err == nil {
		return a.UI.Fail("this PC already has an API key for this camera ("+keyPath+").",
			"it is used by "+Prog+" update. To replace it, revoke the old one on the camera (ssh to it: sh /appfs/akam/akam_api_key.sh revoke "+keyName2K+") and delete that file.")
	}
	if err := a.UI.RequireTTY("creating the API key"); err != nil {
		return ui.ErrFailed
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	ok, err := a.UI.AskYN("Create an admin API key named '" + keyName2K + "' on the camera at " + ip + " (it opens the control port 8092 on the local network)?")
	if err != nil || !ok {
		return a.UI.Fail("not confirmed.", "nothing was changed.")
	}
	conn, err := a.flashDial(ip)(0)
	if err != nil {
		return a.UI.Fail("cannot connect to the camera: "+ui.Sanitize(err.Error())+".", "nothing was changed.")
	}
	defer conn.Close()
	k, err := a.makeKey2K(conn, keyPath)
	if err != nil {
		return err
	}
	a.UI.Good(fmt.Sprintf("  API key %s created and saved in %s (the secret is not shown).", k.ID, keyPath))
	a.UI.Say("  " + Prog + " update will now read the model and version of the camera, and check the version after an update.")
	if note := keyFileNote(runtime.GOOS); note != "" {
		a.UI.Say("  " + note)
	}
	return nil
}

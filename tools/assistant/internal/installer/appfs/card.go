package appfs

import (
	"crypto/hmac"
	"crypto/sha1" //nolint:gosec // WPA-PSK is defined with HMAC-SHA1 (PBKDF2), it is not a security choice of ours
	"encoding/hex"
	"errors"
	"fmt"
	"sort"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/nativeapp/sdcard"
)

// The first-install SD card of the card-less 2K: the Go twin of the 2K model's firmware/.../build_card.sh
// (overlay card, --own-app, --wpa-conf), built from the kit instead of a source tree, so it needs no bash and runs on
// Linux, macOS and Windows. The card never touches the camera flash: it boots the camera in `mode=debug` from
// /mnt/sdcard/akam_boot.sh, and removing it returns to the flashed image (stock, or the card-less Akam).
//
// Like build_card.sh it never creates an upgrade or factory trigger: the card has no *.bin, no zip, no yy_upgrade file,
// and yy_debug.ini is checked to be exactly `mode=debug` + the Akam start script.

// CardInput is what the user chooses.
type CardInput struct {
	PubKey  []byte // one ssh-ed25519 line (the .pub file): copied to akam/authorized_keys
	SSID    string
	Pass    string // WPA-PSK passphrase, 8 to 63 characters
	Decloud bool   // the flag akam/decloud: block the vendor cloud at boot
	// ParamConf is the flag akam/enable_param_conf (opt-in: the card may edit /config/param.conf). Off by default.
	ParamConf bool
}

// WPAConf is the wpa_supplicant configuration of the card. The SSID is written in hex and the key as the 256-bit PSK
// (PBKDF2-HMAC-SHA1, 4096 rounds, as wpa_passphrase), so the card never holds the passphrase itself and no character
// of it needs quoting. scan_ssid=1 finds a hidden network too.
func WPAConf(ssid, pass string) ([]byte, error) {
	if ssid == "" || len(ssid) > 32 {
		return nil, errors.New("the Wi-Fi name must be 1 to 32 bytes")
	}
	if len(pass) < 8 || len(pass) > 63 {
		return nil, errors.New("the Wi-Fi password must be 8 to 63 characters (WPA2-PSK); an open network is not supported by this card")
	}
	for _, r := range ssid + pass {
		if r < 0x20 || r == 0x7f {
			return nil, errors.New("the Wi-Fi name and password must not contain control characters or newlines")
		}
	}
	psk := pbkdf2SHA1([]byte(pass), []byte(ssid), 4096, 32)
	return []byte("ctrl_interface=/tmp/wpa_supplicant\nupdate_config=0\nnetwork={\n\tssid=" + hex.EncodeToString([]byte(ssid)) +
		"\n\tscan_ssid=1\n\tkey_mgmt=WPA-PSK\n\tpsk=" + hex.EncodeToString(psk) + "\n}\n"), nil
}

func pbkdf2SHA1(pass, salt []byte, iter, n int) []byte {
	var out []byte
	for block := uint32(1); len(out) < n; block++ {
		h := hmac.New(sha1.New, pass) //nolint:gosec
		h.Write(salt)
		h.Write([]byte{byte(block >> 24), byte(block >> 16), byte(block >> 8), byte(block)})
		u := h.Sum(nil)
		t := append([]byte(nil), u...)
		for i := 1; i < iter; i++ {
			h = hmac.New(sha1.New, pass) //nolint:gosec
			h.Write(u)
			u = h.Sum(nil)
			for j := range t {
				t[j] ^= u[j]
			}
		}
		out = append(out, t...)
	}
	return out[:n]
}

// Card is the tree to write and what to verify after the write.
type Card struct {
	Files  []sdcard.File
	Verify sdcard.VerifySpec
}

// BuildCard returns the tree of the first-install card of the kit. The kit must carry the card files (a kit built by
// tools/release/make_kit_2k.sh does).
func BuildCard(k *Kit, in CardInput) (*Card, error) {
	if !k.HasCard() {
		return nil, errors.New("this kit has no SD card files (kit older than the card installer): get the current release")
	}
	// the same pin as Build: the card boots this busybox, so a kit folder with another one makes no card
	if got := sum(k.File(pathBin + "busybox")); got != busyBoxPin {
		return nil, fmt.Errorf("busybox in the kit is not the pinned %s (sha256 %s): no card is made from it", busyBoxPin, got)
	}
	if err := sdcard.CheckPubKey(in.PubKey); err != nil {
		return nil, err
	}
	wpa, err := WPAConf(in.SSID, in.Pass)
	if err != nil {
		return nil, err
	}
	ini := strings.TrimRight(string(k.File("card/yy_debug.ini")), "\r\n")
	if ini != "mode=debug\nstart_sh=/mnt/sdcard/akam_boot.sh" || strings.Contains(ini, "factory") {
		return nil, errors.New("the kit's yy_debug.ini is not the expected mode=debug file: refused")
	}
	files := map[string][]byte{
		"yy_debug.ini":                   k.File("card/yy_debug.ini"),
		"akam_boot.sh":                   k.File("card/akam_boot.sh"),
		"busybox":                        k.File(pathBin + "busybox"),
		"akam/enable_local.sh":           k.File("card/akam/enable_local.sh"),
		"akam/restore_param.sh":          k.File("card/akam/restore_param.sh"),
		"akam/dropbearmulti":             k.File(pathBin + "dropbearmulti"),
		"akam/bin/akam2k_rtsp":           k.File(pathBin + "akam2k_rtsp"),
		"akam/bin/akam2k_rtsp.BUILDINFO": k.File(pathBin + "akam2k_rtsp.BUILDINFO"),
		"akam/bin/akam2k_sntp":           k.File(pathBin + "akam2k_sntp"),
		"akam/bin/akam2k_sntp.BUILDINFO": []byte("tool=akam2k_sntp\nsha256=" + buildinfo(k.File(pathBin+"akam2k_rtsp.BUILDINFO"), "sntp_sha256") + "\n"),
		"akam/authorized_keys":           sdcard.PubKeyLF(in.PubKey),
		"akam/wpa.conf":                  wpa,
		"akam/own_app":                   {}, // arms the stub: the card boots Akam
	}
	for _, f := range overlayScripts {
		if f == "akam_flash_boot.sh" { // the flash-boot launcher is for the flashed image, not the card
			continue
		}
		files["akam/"+f] = k.File(pathOverlay + f)
	}
	if b := k.File("licenses/LICENSE.dropbear"); b != nil {
		files["akam/LICENSE.dropbear"] = b
	}
	if in.Decloud {
		files["akam/decloud"] = []byte{}
	}
	if in.ParamConf {
		files["akam/enable_param_conf"] = []byte{}
	}
	var names []string
	for p, b := range files {
		if b == nil {
			return nil, fmt.Errorf("the kit lacks the file for %s", p)
		}
		names = append(names, p)
	}
	sort.Strings(names)
	c := &Card{Verify: sdcard.VerifySpec{Mode: "rot2k", Files: map[string]string{}, Absent: []string{"fac_av130.zip", "yy_upgrade.bin"}}}
	for _, p := range names {
		if strings.EqualFold(p[strings.LastIndex(p, "/")+1:], "fac_av130.zip") || strings.HasPrefix(strings.ToLower(p), "yy_upgrade") {
			return nil, errors.New("internal: an upgrade trigger in the card tree")
		}
		c.Files = append(c.Files, sdcard.File{Path: p, Data: files[p]})
		c.Verify.Files[p] = sum(files[p])
	}
	return c, nil
}

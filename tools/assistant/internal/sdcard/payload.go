package sdcard

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// Card modes (tools/sd/sd_card.sh).
const (
	ModeBackup    = "backup"
	ModeFirstBoot = "first-boot"
)

// MTD7Size is the APP partition.
const MTD7Size = 5242880

var imageRe = regexp.MustCompile(`^ak_rtsp_firmware_[0-9]{8}_[0-9]{6}\.squashfs$`)

// FlashFiles are added to a first-boot card (sd_card.sh "flash-files"): the image, flash_tool, the
// camera-side scripts and the stock APP backup.
type FlashFiles struct {
	ImageName string // ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs
	Image     []byte
	FlashTool []byte
	Scripts   map[string][]byte // install_with_flash_tool.sh flash_guard.sh preflight_check.sh restore.sh akam_hwcheck.sh
	MTD7      []byte
}

// FlashScriptNames are the scripts of a flash card, at its root.
var FlashScriptNames = []string{"install_with_flash_tool.sh", "flash_guard.sh", "preflight_check.sh", "restore.sh", "akam_hwcheck.sh"}

// PayloadInput is everything that goes on a card.
type PayloadInput struct {
	Mode    string
	Toolkit map[string][]byte // the files of the toolkit's sd_card tree (relative paths), already pin-checked
	PubKey  []byte            // first-boot: one ssh-ed25519 line
	SSID    string
	Pass    string
	Flash   *FlashFiles // first-boot only
}

func shq(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }

// CheckPubKey is check_pubkey: one ssh-ed25519 line with a 68-character blob.
func CheckPubKey(b []byte) error {
	lines := strings.Split(strings.TrimRight(string(b), "\n"), "\n")
	if len(lines) != 1 || len(b) == 0 {
		return errors.New("the public key file must hold exactly one key line")
	}
	f := strings.Fields(lines[0])
	if len(f) < 2 || f[0] != "ssh-ed25519" {
		return errors.New("not an ssh-ed25519 public key")
	}
	if strings.Contains(string(b), "PRIVATE KEY") {
		return errors.New("this is a PRIVATE key: never put it on the card")
	}
	if !regexp.MustCompile(`^[A-Za-z0-9+/]+$`).MatchString(f[1]) || !strings.HasPrefix(f[1], "AAAAC3NzaC1lZDI1NTE5") || len(f[1]) != 68 {
		return errors.New("the key blob does not look like an ed25519 public key")
	}
	return nil
}

var (
	reTelnet  = regexp.MustCompile(`(?m)^TELNET=.*$`)
	reTelPort = regexp.MustCompile(`(?m)^TELNET_PORT=.*$`)
	reOffline = regexp.MustCompile(`(?m)^OFFLINE=.*$`)
)

// BuildPayload is build_payload (+ build_flash_overlay): the whole tree of the card, in memory.
func BuildPayload(in PayloadInput) ([]File, error) {
	if in.Mode != ModeBackup && in.Mode != ModeFirstBoot {
		return nil, fmt.Errorf("mode must be %s or %s", ModeBackup, ModeFirstBoot)
	}
	if in.SSID == "" || in.Pass == "" {
		return nil, errors.New("SSID and password must not be empty")
	}
	if strings.ContainsAny(in.SSID+in.Pass, "\r\n") {
		return nil, errors.New("SSID/password must not contain newlines")
	}
	files := map[string][]byte{}
	for p, b := range in.Toolkit {
		files[p] = b
	}
	delete(files, "custom/state/shadow.done")
	files["custom/state/dump.done"] = []byte{}
	conf, ok := files["custom/configs/hack.conf"]
	if !ok {
		return nil, errors.New("hack.conf missing in the toolkit tree")
	}
	conf = reTelnet.ReplaceAll(conf, []byte("TELNET=1"))
	conf = reTelPort.ReplaceAll(conf, []byte("TELNET_PORT=24"))
	conf = reOffline.ReplaceAll(conf, []byte("OFFLINE=1"))
	if n := len(regexp.MustCompile(`(?m)^(TELNET=1|TELNET_PORT=24|OFFLINE=1)$`).FindAll(conf, -1)); n != 3 {
		return nil, errors.New("hack.conf edits did not apply")
	}
	files["custom/configs/hack.conf"] = conf
	files["custom/configs/wifi.conf"] = []byte(fmt.Sprintf("MODE=sta\nSSID=%s\nPASS=%s\nSECURITY=3\nDHCP=1\n", shq(in.SSID), shq(in.Pass)))
	ep, ok := files["custom/scripts/entrypoint.sh"]
	if !ok {
		return nil, errors.New("entrypoint.sh missing in the toolkit tree")
	}
	// sed '2i\...': two lines before line 2
	lines := bytes.SplitAfter(ep, []byte("\n"))
	var out bytes.Buffer
	for i, l := range lines {
		if i == 1 {
			out.WriteString("# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0\n")
		}
		out.Write(l)
	}
	if len(lines) < 2 {
		out.WriteString("\n# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0\n")
	}
	files["custom/scripts/entrypoint.sh"] = out.Bytes()
	hs, ok := files["hack.sh"]
	if !ok {
		return nil, errors.New("hack.sh missing in the toolkit tree")
	}
	hs = append([]byte(nil), hs...)
	if len(hs) > 0 && hs[len(hs)-1] != '\n' {
		hs = append(hs, '\n') // the marker stays on its own line
	}
	hs = append(hs, "# DECLOUD_WIZARD_HOOK\n/bin/sh \"${SCRIPTS_DIR}/entrypoint.sh\" >/dev/null 2>&1 &\n"...)
	files["hack.sh"] = hs
	if in.Mode == ModeFirstBoot {
		if err := CheckPubKey(in.PubKey); err != nil {
			return nil, err
		}
		files["AKAM_MAINTENANCE"] = []byte{}
		files["akam/authorized_keys"] = in.PubKey
	}
	if f := in.Flash; f != nil {
		if in.Mode != ModeFirstBoot {
			return nil, errors.New("flash files go on a first-boot card")
		}
		if !imageRe.MatchString(f.ImageName) {
			return nil, fmt.Errorf("image name must look like ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs (got %s)", f.ImageName)
		}
		if len(f.Image) == 0 || len(f.FlashTool) == 0 {
			return nil, errors.New("image or flash_tool is empty")
		}
		if len(f.MTD7) != MTD7Size {
			return nil, fmt.Errorf("the stock APP backup is not %d bytes", MTD7Size)
		}
		files["custom_firmware/"+f.ImageName] = f.Image
		files["flash_tool"] = f.FlashTool
		for _, n := range FlashScriptNames {
			b, ok := f.Scripts[n]
			if !ok {
				return nil, fmt.Errorf("script %s missing", n)
			}
			files[n] = b
		}
		files["original_firmware/mtdblock7.bin"] = f.MTD7
	}
	for p := range files {
		if !strings.Contains(p, "/") && strings.EqualFold(path.Ext(p), ".bin") {
			return nil, errors.New("a *.bin sits at the card root: refusing (stock tf_update.sh would run system_upgrade on it)")
		}
	}
	names := make([]string, 0, len(files))
	for p := range files {
		names = append(names, p)
	}
	sort.Strings(names)
	var res []File
	for _, p := range names {
		res = append(res, File{Path: p, Data: files[p]})
	}
	res = append(res, File{Path: "logs", Dir: true}, File{Path: "custom/state", Dir: true})
	return res, nil
}

// ---------------------------------------------------------------- toolkit

// DirTree is a Tree over a staged directory.
type DirTree struct{ Root string }

func (d DirTree) ReadFile(rel string) ([]byte, error) {
	if !filepath.IsLocal(rel) {
		return nil, errors.New("bad path")
	}
	return os.ReadFile(filepath.Join(d.Root, rel))
}

func (d DirTree) List(rel string) ([]string, error) {
	es, err := os.ReadDir(filepath.Join(d.Root, rel))
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range es {
		out = append(out, e.Name())
	}
	return out, nil
}

func (d DirTree) Exists(rel string) bool {
	_, err := os.Stat(filepath.Join(d.Root, rel))
	return err == nil
}

// ReadTree reads every regular file under dir (relative slash paths). Anything else (links, devices)
// is refused: a toolkit tree must be plain files.
func ReadTree(dir string) (map[string][]byte, error) {
	out := map[string][]byte{}
	var walk func(rel string) error
	walk = func(rel string) error {
		es, err := os.ReadDir(filepath.Join(dir, rel))
		if err != nil {
			return err
		}
		for _, e := range es {
			p := path.Join(rel, e.Name())
			switch {
			case e.IsDir():
				if err := walk(p); err != nil {
					return err
				}
			case e.Type().IsRegular():
				b, err := os.ReadFile(filepath.Join(dir, p))
				if err != nil {
					return err
				}
				out[p] = b
			default:
				return fmt.Errorf("%s is not a plain file", p)
			}
		}
		return nil
	}
	return out, walk("")
}

// TreeHash is tree_hash of sd_card.sh: sha256 over "sha256sum" lines of the files in C-locale order
// ("<hash>  ./<path>\n").
func TreeHash(files map[string][]byte) string {
	names := make([]string, 0, len(files))
	for p := range files {
		names = append(names, "./"+p)
	}
	sort.Strings(names) // byte order = LC_ALL=C
	h := sha256.New()
	for _, n := range names {
		sum := sha256.Sum256(files[n[2:]])
		fmt.Fprintf(h, "%s  %s\n", hex.EncodeToString(sum[:]), n)
	}
	return hex.EncodeToString(h.Sum(nil))
}

// Pins of the toolkit (tasarren/lsc-tuya-toolkit), the same as tools/sd/sd_card.sh.
var (
	PinCommit  = "396db8a835d9787b2997de038b48f645c837e754"
	PinTreeSHA = "3aa3f698fd6d6f228ee785726970f708035799aa5bbcb73ef369bc3757747c2f"
)

// LoadToolkit reads <dir>/sd_card and checks its tree hash against the pin: a mismatch is refused.
func LoadToolkit(dir string) (map[string][]byte, error) {
	files, err := ReadTree(filepath.Join(dir, "sd_card"))
	if err != nil {
		return nil, fmt.Errorf("toolkit: %w", err)
	}
	if got := TreeHash(files); got != PinTreeSHA {
		return nil, fmt.Errorf("sd_card tree hash mismatch (got %s): refusing", got)
	}
	return files, nil
}

// SkipOnVerify are the files the preparation changes (not compared with the toolkit).
var SkipOnVerify = map[string]bool{
	"custom/configs/hack.conf": true, "custom/configs/wifi.conf": true, "custom/scripts/entrypoint.sh": true,
	"hack.sh": true, "custom/state/shadow.done": true,
}

// ToolkitHashes are the sha256 of the toolkit files the card must carry unchanged.
func ToolkitHashes(files map[string][]byte) map[string]string {
	out := map[string]string{}
	for p, b := range files {
		if SkipOnVerify[p] {
			continue
		}
		s := sha256.Sum256(b)
		out[p] = hex.EncodeToString(s[:])
	}
	return out
}

// ---------------------------------------------------------------- verify

// VerifySpec describes what a card must hold.
type VerifySpec struct {
	Mode    string            `json:"mode"`
	Flash   bool              `json:"flash"`
	Toolkit map[string]string `json:"toolkit,omitempty"` // path -> sha256 (nil: skipped)
}

// VerifyTree is verify_dir: "ok:" and "FAIL:" lines and the number of problems.
func VerifyTree(t Tree, spec VerifySpec) (lines []string, bad int) {
	chk := func(desc string, ok bool) {
		if ok {
			lines = append(lines, "ok: "+desc)
		} else {
			lines = append(lines, "FAIL: "+desc)
			bad++
		}
	}
	has := func(rel string, re string) bool {
		b, err := t.ReadFile(rel)
		return err == nil && regexp.MustCompile(re).Match(b)
	}
	root, _ := t.List("")
	noBin := true
	for _, n := range root {
		if strings.EqualFold(path.Ext(n), ".bin") && !isDir(t, n) {
			noBin = false
		}
	}
	chk("no *.bin at the card root", noBin)
	if spec.Flash {
		names, _ := t.List("custom_firmware")
		chk("custom_firmware holds exactly one file", len(names) == 1)
		img := ""
		for _, n := range names {
			if strings.HasPrefix(n, "ak_rtsp_firmware_") && strings.HasSuffix(n, ".squashfs") {
				img = n
			}
		}
		chk("image is ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs", imageRe.MatchString(img))
		if img != "" {
			b, err := t.ReadFile("custom_firmware/" + img)
			chk("image is not empty", err == nil && len(b) > 0)
		}
		b, err := t.ReadFile("flash_tool")
		chk("flash_tool present", err == nil && len(b) > 0)
		for _, n := range FlashScriptNames {
			b, err := t.ReadFile(n)
			chk(n+" present", err == nil && len(b) > 0)
		}
		b, err = t.ReadFile("original_firmware/mtdblock7.bin")
		chk(fmt.Sprintf("original_firmware/mtdblock7.bin is %d bytes", MTD7Size), err == nil && len(b) == MTD7Size)
	}
	if spec.Mode == ModeBackup || spec.Mode == ModeFirstBoot {
		conf := "custom/configs/hack.conf"
		chk("hack.conf: TELNET=1, TELNET_PORT=24, OFFLINE=1", has(conf, `(?m)^TELNET=1$`) && has(conf, `(?m)^TELNET_PORT=24$`) && has(conf, `(?m)^OFFLINE=1$`))
		chk("dump.done present", t.Exists("custom/state/dump.done"))
		chk("shadow.done absent", !t.Exists("custom/state/shadow.done"))
		chk("entrypoint.sh lock marker", has("custom/scripts/entrypoint.sh", `(?m)^# DECLOUD_ENTRYPOINT_LOCK$`))
		chk("hack.sh hook marker on its own line", has("hack.sh", `(?m)^# DECLOUD_WIZARD_HOOK$`))
		chk("hack.sh hook command follows the marker", has("hack.sh", `(?m)^# DECLOUD_WIZARD_HOOK\n.*entrypoint\.sh`))
		w := "custom/configs/wifi.conf"
		chk("wifi.conf has MODE=sta, SSID, PASS, SECURITY=3, DHCP=1",
			has(w, `(?m)^MODE=sta$`) && has(w, `(?m)^SSID=.`) && has(w, `(?m)^PASS=.`) && has(w, `(?m)^SECURITY=3$`) && has(w, `(?m)^DHCP=1$`))
		if spec.Mode == ModeFirstBoot {
			chk("AKAM_MAINTENANCE present", t.Exists("AKAM_MAINTENANCE"))
			k, err := t.ReadFile("akam/authorized_keys")
			chk("akam/authorized_keys is one ed25519 key", err == nil && CheckPubKey(k) == nil)
		} else {
			chk("AKAM_MAINTENANCE absent (backup card)", !t.Exists("AKAM_MAINTENANCE"))
		}
		if spec.Toolkit != nil {
			diff := 0
			var rels []string
			for r := range spec.Toolkit {
				rels = append(rels, r)
			}
			sort.Strings(rels)
			for _, r := range rels {
				b, err := t.ReadFile(r)
				s := sha256.Sum256(b)
				if err != nil || hex.EncodeToString(s[:]) != spec.Toolkit[r] {
					lines = append(lines, "FAIL: differs from the toolkit: "+r)
					diff++
				}
			}
			if diff == 0 {
				lines = append(lines, "ok: toolkit files unchanged (sha256)")
			}
			bad += diff
		} else {
			lines = append(lines, "skip: toolkit file hashes (no toolkit given)")
		}
	}
	if bad == 0 {
		lines = append(lines, "VERIFY: OK")
	} else {
		lines = append(lines, fmt.Sprintf("VERIFY: FAILED (%d problem(s))", bad))
	}
	return lines, bad
}

func isDir(t Tree, rel string) bool { _, err := t.List(rel); return err == nil }

package cli

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/camera/akamapi"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The install and update flows of the card-less 2K (kind cardless-appfs), on Linux, macOS and Windows, in Go only:
//
//	prepare-2k-sd   the first-install SD card from the signed kit (no flash is written)
//	update          one flow: detect (signed API), kit, image, what changes, rehearsal, confirmation, write, wait,
//	                verify, previous image kept; --rollback flashes the previous image with the same gates
//
// The write itself is appfs.Flash (the port of tools/flash_appfs.sh): this file only orders the screens and asks
// the questions. Nothing here writes flash without the recovery acknowledgement and the typed confirmation (a --yes
// flag answers the typed question and the rehearsal question only, and --recovery-at-hand is a second, separate
// flag for the recovery question; both need a terminal).

// is2K reports whether the current camera (or --model) is served by the appfs installer.
func (a *App) is2K() bool {
	id := strings.TrimSpace(a.Opt.Model)
	if id == "" {
		id = a.cameraType()
	}
	m, ok := models.ByID(id)
	if !ok {
		return false
	}
	f, err := installer.For(m)
	return err == nil && f == installer.Appfs
}

// modelID2K is the registry id of the camera being served.
func (a *App) modelID2K() string {
	if id := strings.TrimSpace(a.Opt.Model); id != "" {
		return id
	}
	return a.cameraType()
}

// kit2K returns the verified 2K kit: --kit FILE (signed tarball) or DIR (needs --allow-unsigned: a folder cannot be
// authenticated), else the newest firmware release that has a kit for the model, downloaded over HTTPS and verified.
func (a *App) kit2K() (*appfs.Kit, error) {
	model := a.modelID2K()
	path := a.Opt.Kit
	if path == "" {
		var err error
		if path, err = a.download2KKit(model); err != nil {
			return nil, err
		}
	}
	st, err := os.Stat(path)
	if err != nil {
		return nil, a.UI.Fail("kit not found: "+path, "check the path given to --kit.")
	}
	if st.IsDir() {
		if !a.Opt.AllowUnsigned {
			return nil, a.UI.Fail("the kit is a folder, which cannot be authenticated (no signature to check).",
				"pass the signed .tar.xz, or --allow-unsigned if you trust where this folder came from.")
		}
		// a local folder the operator trusts (--allow-unsigned): the only place a development (test grade) kit is accepted
		k, err := appfs.LoadKit(path, appfs.AllowTestGrade())
		if err != nil {
			return nil, a.UI.Fail("the kit is not usable: "+ui.Sanitize(err.Error()), "get the kit of the 2K from the release.")
		}
		if k.IsTestGrade() {
			a.UI.Warn("this kit is a DEVELOPMENT kit (kit_grade=" + ui.Sanitize(k.Manifest["kit_grade"]) + "), not a release, and a folder carries no signature: you are trusting it entirely.")
		}
		return k, a.kitModelCheck(k, model)
	}
	if a.Opt.Kit == "" { // downloaded: the signature file must be there (download2KKit fetched it); never an unsigned prompt
		if _, e := os.Stat(filepath.Join(filepath.Dir(path), "SHA256SUMS.sig")); e != nil {
			return nil, a.UI.Fail("the downloaded kit has no signature: refused.", "use a signed release, or a kit you already trust: --kit FILE --allow-unsigned.")
		}
	}
	tmp, err := os.MkdirTemp(a.St.Dir, "kit2k-")
	if err != nil {
		return nil, a.UI.Fail("cannot create a work folder: "+err.Error(), "")
	}
	defer os.RemoveAll(tmp)
	ext := filepath.Join(tmp, "kit")
	// a downloaded kit is never accepted unsigned; --allow-unsigned only covers a local --kit FILE
	allowUnsigned := a.Opt.AllowUnsigned && a.Opt.Kit != ""
	unsigned := allowUnsigned
	opts := kit.Options{
		AllowedSigners: a.allowedSigners(), AllowUnsigned: allowUnsigned, Expect: a.Opt.ExpectSHA256,
		ExtractTo: ext, Layout: appfs.KitLayout(), ExpectModel: model,
		Say:  func(s string) { a.UI.Say("  " + s) },
		Warn: func(s string) { fmt.Fprintln(a.Streams.Err, "WARNING: "+s) },
	}
	if _, err := a.verifyKitFile(opts, path, &unsigned); err != nil {
		return nil, err
	}
	k, err := appfs.LoadKit(ext)
	if err != nil {
		return nil, a.UI.Fail("the kit is not usable: "+ui.Sanitize(err.Error()), "get the kit of the 2K from the release.")
	}
	return k, a.kitModelCheck(k, model)
}

func (a *App) kitModelCheck(k *appfs.Kit, model string) error {
	if got := k.Manifest["model"]; got != model {
		return a.UI.Fail(fmt.Sprintf("this kit is for %s, not %s.", models.Label(got), models.Label(model)), "use the kit of this camera's model.")
	}
	a.UI.Good(fmt.Sprintf("  Kit verified (version %s, source %s).", k.Version(), shortCommit(k.Commit())))
	return nil
}

func shortCommit(c string) string {
	if len(c) > 12 {
		return c[:12]
	}
	return c
}

// download2KKit downloads the tarball, SHA256SUMS and signature of the wanted (or newest) release into the downloads
// folder and returns the tarball path. Nothing is run; verification is the caller's.
func (a *App) download2KKit(model string) (string, error) {
	ver := strings.TrimPrefix(a.Opt.Version, "v")
	if ver == "" {
		rels, err := a.releaseList()
		if err != nil || len(rels) == 0 {
			return "", a.UI.Fail("cannot tell which firmware release to download for "+models.Label(model)+".",
				fmt.Sprintf("pass --version X.Y.Z (see https://github.com/%s/releases) or --kit FILE.", kitRepo))
		}
		ver = rels[0].Version
		a.UI.Say("  Newest firmware release for " + models.Label(model) + ": " + ver + ".")
	}
	if !validKitVersion(ver) {
		return "", a.UI.Fail(fmt.Sprintf("'%s' is not a valid version.", ver), "use a number such as 0.1.0.")
	}
	base := fmt.Sprintf("https://github.com/%s/releases/download/%s", kitRepo, kit.ReleaseTag(ver))
	if o := testhook.Env("AKAM_KIT_URL_BASE"); o != "" { // test hook; still HTTPS only
		base = strings.TrimRight(o, "/") + "/" + kit.ReleaseTag(ver)
	}
	dir := filepath.Join(a.Cams.Base, "downloads", ver+"-"+model)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", a.UI.Fail("cannot create "+dir+": "+err.Error(), "")
	}
	file := kit.KitFile(model, ver)
	// The signature is mandatory for a download: a release without SHA256SUMS.sig is refused, whatever --version or
	// --allow-unsigned say (an unsigned kit can only be a local file the operator chose: --kit FILE --allow-unsigned).
	for _, f := range []struct {
		name string
		max  int64
	}{{file, int64(kit.MaxKitDownload)}, {"SHA256SUMS", kit.MaxSmallDownload}, {"SHA256SUMS.sig", kit.MaxSmallDownload}} {
		_ = os.Remove(filepath.Join(dir, f.name))
		if err := a.download(base+"/"+f.name, filepath.Join(dir, f.name), f.max); err != nil {
			var he *kit.HTTPError
			if f.name == "SHA256SUMS.sig" && errors.As(err, &he) && (he.Code == 404 || he.Code == 410) {
				return "", a.UI.Fail(fmt.Sprintf("firmware release %s has no signature (SHA256SUMS.sig): its kit cannot be authenticated, so it is refused.", ver),
					"use another release (without --version the newest signed one is taken), or a kit you already trust: --kit FILE --allow-unsigned.")
			}
			if errors.As(err, &he) && (he.Code == 404 || he.Code == 410) {
				return "", a.UI.Fail(fmt.Sprintf("firmware release %s was not found, or it has no kit for %s (%s).", ver, models.Label(model), f.name),
					fmt.Sprintf("see https://github.com/%s/releases, or use a kit you already have: --kit FILE.", kitRepo))
			}
			return "", a.UI.Fail(fmt.Sprintf("download of %s failed (%v).", f.name, err), "check your network and retry, or use --kit FILE.")
		}
	}
	return filepath.Join(dir, file), nil
}

// ---------------------------------------------------------------- prepare-2k-sd

func (a *App) cmdPrepare2KSD() error {
	if err := a.appfsGate("prepare-2k-sd"); err != nil {
		return err
	}
	a.UI.Banner("Prepare the first-install SD card of the 2K")
	a.UI.Say("  This card boots the camera into Akam with no flash write: remove it and the camera is back to what is in its flash.")
	a.UI.Say("  It holds your Wi-Fi key (as a 256-bit PSK, not the password) and your SSH public key. Treat it as private.")
	if a.UI.Dry() {
		a.UI.Say("  WOULD verify the 2K kit, ask for the SSH key and the Wi-Fi, ask the red-box yes/no, then write an MBR + FAT32 card and verify every file by sha256.")
		return nil
	}
	if err := a.UI.RequireTTY("preparing the card"); err != nil {
		return ui.ErrFailed
	}
	k, err := a.kit2K()
	if err != nil {
		return err
	}
	if !k.HasCard() {
		return a.UI.Fail("this kit has no SD card files.", "get the current release (a kit built before the card installer).")
	}
	pub, err := a.sshChoosePubkey()
	if err != nil {
		return err
	}
	if err := pubkeyCheck(a, pub); err != nil {
		return err
	}
	pk, err := os.ReadFile(pub)
	if err != nil {
		return a.UI.Fail("cannot read the public key: "+err.Error(), "")
	}
	c, info, err := a.sdChooseDevice()
	if err != nil {
		return err
	}
	ssid, pass, err := a.sdGetWifi()
	if err != nil {
		return err
	}
	decloud := false
	if ok, e := a.UI.AskYN("Block the vendor cloud at boot (flag akam/decloud)?"); e == nil {
		decloud = ok
	}
	card, err := appfs.BuildCard(k, appfs.CardInput{PubKey: pk, SSID: ssid, Pass: pass, Decloud: decloud})
	if err != nil {
		return a.UI.Fail("preparing the card failed: "+err.Error(), "the camera was not touched; fix the problem above and retry.")
	}
	if err := a.sdConfirmErase(c, info, "erase"); err != nil {
		return err
	}
	lines, err := a.sd().Prepare(c, card.Files, card.Verify)
	a.showVerify(lines)
	if err != nil {
		a.UI.Err(err.Error())
		return a.UI.Fail("preparing the card failed.", "the camera was not touched; fix the problem above and retry.")
	}
	a.UI.Good(fmt.Sprintf("  Card ready (%d files, every one checked by sha256 after the write).", len(card.Files)))
	a.UI.Say("  Next: power the camera off, insert the card, power it on, wait about 3 minutes, then run: " + Prog + " find-camera")
	return nil
}

// ---------------------------------------------------------------- update

func (a *App) imageStore() appfs.ImageStore {
	return appfs.ImageStore{Dir: filepath.Join(a.St.Dir, "appfs-images")}
}

// apiKey2K is the camera's API key: --api-key-file, the camera's state folder (api_key, 0600) or AKAM_KEY_ID and
// AKAM_KEY_SECRET. ok is false when none is available.
func (a *App) apiKey2K() (akamapi.Key, bool, error) {
	paths := []string{a.Opt.APIKeyFile, filepath.Join(a.St.Dir, "api_key")}
	for i, p := range paths {
		if p == "" {
			continue
		}
		k, err := akamapi.LoadKey(p)
		if err == nil {
			// a key read from a file is redacted from every screen and log, like one given by the environment
			a.Secrets.Add(hex.EncodeToString(k.Secret))
			if note := keyFileNote(runtime.GOOS); note != "" && !a.keyNoted {
				a.keyNoted = true
				a.UI.Say("  " + note)
			}
			return k, true, nil
		}
		if i == 0 || !errors.Is(err, os.ErrNotExist) { // a named file must work; a stale default is reported
			return akamapi.Key{}, false, err
		}
	}
	if id, sec := os.Getenv("AKAM_KEY_ID"), a.envSecret("AKAM_KEY_SECRET"); id != "" && sec != "" {
		a.Secrets.Add(sec)
		k, err := akamapi.ParseKey(id + ":" + sec)
		return k, err == nil, err
	}
	return akamapi.Key{}, false, nil
}

// keyFileNote is said when a key file is read on a system where the program cannot check who may read the file.
func keyFileNote(goos string) string {
	if goos != "windows" {
		return ""
	}
	return "Windows: the permissions of the API key file are not checked by this program. Keep it in your own user profile folder, not in a shared one."
}

// detect2K asks the signed API for the model and version. A missing key or a closed API is not an error: the
// update still runs, its flash gates prove the image on the camera byte for byte.
func (a *App) detect2K(ip string) appfs.Camera {
	k, ok, err := a.apiKey2K()
	if err != nil {
		a.UI.Warn("the API key cannot be used: " + ui.Sanitize(err.Error()))
		return appfs.Camera{}
	}
	if !ok {
		a.UI.Say("  No API key for this camera: its model and version are not read (the write is still checked byte for byte against the recorded image).")
		return appfs.Camera{}
	}
	cam, err := a.camAPI(ip, k).Info(a.Ctx)
	if err != nil {
		switch {
		case errors.Is(err, akamapi.ErrAuth):
			a.UI.Warn("the camera refused the API key (revoked or wrong camera): model and version not read.")
		default:
			a.UI.Say("  The camera did not answer the control API (" + ui.Sanitize(err.Error()) + "): model and version not read.")
		}
		return appfs.Camera{}
	}
	a.UI.Say(fmt.Sprintf("  The camera says: model %s, Akam %s.", ui.Sanitize(cam.Model), ui.Sanitize(cam.App)))
	return appfs.Camera{Model: cam.Model, Version: cam.App}
}

func (a *App) camAPI(ip string, k akamapi.Key) *akamapi.Client {
	if a.Hooks.CamAPI != nil {
		return a.Hooks.CamAPI(ip, k)
	}
	return akamapi.New(ip, k)
}

// backupDir2K is the verified full backup: --backup-dir, the recorded one, or asked.
func (a *App) backupDir2K() (string, error) {
	d := a.Opt.BackupDir
	if d == "" {
		d = a.St.Get("rot2k_backup")
	}
	if d == "" && a.UI.Interactive() {
		var err error
		if d, err = a.UI.Prompt("  Folder of your full backup of the 2K (mtd0..mtd5.bin and SHA256SUMS):", ""); err != nil {
			return "", ui.ErrFailed
		}
	}
	if d == "" {
		return "", a.UI.Fail("no backup folder.", "the update is built from YOUR backup of the camera (mtd3.bin) and compared with it: pass --backup-dir DIR.")
	}
	if _, err := os.Stat(filepath.Join(d, "mtd3.bin")); err != nil {
		return "", a.UI.Fail("no mtd3.bin in "+d+".", "pass the folder of the full backup (mtd0..mtd5.bin and SHA256SUMS).")
	}
	return d, nil
}

func (a *App) cmdUpdate2K() error {
	if err := a.appfsGate("update"); err != nil {
		return err
	}
	if a.Opt.Stock {
		return a.UI.Fail("--stock is the 1080p way back to the stock firmware.", "on the 2K use --rollback (previous image) or the SD card recovery; see docs/porting/lsc-rotatable-2k/cardless-appfs.md.")
	}
	title := "Update the card-less 2K"
	if a.Opt.Rollback {
		title = "Roll the card-less 2K back to its previous image"
	}
	a.UI.Banner(title)
	if a.UI.Dry() {
		for _, l := range []string{
			"1. read the model and the Akam version from the camera over the signed API (read only)",
			"2. verify the kit (signature, checksums, model) and build the image from your backup, or take the previous image",
			"3. show what changes: versions, image hashes, the files that differ",
			"4. rehearsal on the camera: stops the app, moves the helpers to RAM, restores everything, writes no flash (asks first)",
			"5. the explicit confirmation of the write: recovery means at hand, then the image hash and the partition typed by you",
			"6. the write, read back and compared before any reboot; the camera is never rebooted by this program",
			"7. you power-cycle; the program waits, compares mtd3 and the version, then records the previous image for a rollback",
		} {
			a.UI.Say("  WOULD " + l)
		}
		return nil
	}
	if err := a.UI.RequireTTY("updating the camera"); err != nil {
		return ui.ErrFailed
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	bdir, err := a.backupDir2K()
	if err != nil {
		return err
	}
	in := appfs.UpdateInput{ModelID: a.modelID2K(), BackupDir: bdir, Store: a.imageStore(), Hot: !a.Opt.Card, Force: a.Opt.Force,
		Rollback: a.Opt.Rollback, Target: "root@" + ip, Say: func(s string) { a.UI.Say("  " + s) }}
	in.WorkDir = filepath.Join(a.St.Dir, "appfs-flash-"+time.Now().UTC().Format("20060102T150405Z"))
	in.Camera = a.detect2K(ip)
	if a.Opt.CurrentImage != "" {
		if in.CurrentOverride, err = os.ReadFile(a.Opt.CurrentImage); err != nil {
			return a.UI.Fail("cannot read --current-image: "+err.Error(), "")
		}
	}
	if !in.Rollback {
		if in.Kit, err = a.kit2K(); err != nil {
			return err
		}
	}
	// an earlier write that was never recorded is settled first, from a read-only dump of mtd3
	also := ""
	if in.CurrentOverride != nil {
		also = fmt.Sprintf("%x", sha256.Sum256(in.CurrentOverride))
	}
	if _, err := in.Store.Reconcile(a.Ctx, a.flashDial(ip), also, func(s string) { a.UI.Say("  " + s) }); err != nil {
		return a.UI.Fail(ui.Sanitize(err.Error()), "")
	}
	p, err := appfs.PrepareUpdate(in)
	switch {
	case errors.Is(err, appfs.ErrUpToDate):
		a.UI.Good("  The camera already runs this image: nothing to update.")
		return nil
	case err != nil:
		return a.UI.Fail(ui.Sanitize(err.Error())+".", "nothing was sent to the camera.")
	}
	a.UI.Blank()
	a.UI.Bold("What will change")
	for _, l := range p.Summary() {
		a.UI.Say("  " + l)
	}
	a.UI.Blank()
	_ = a.St.Set("rot2k_backup", bdir)
	res, err := appfs.RunUpdate(p, a.runDeps2K(ip))
	if err != nil {
		return a.UI.Fail(ui.Sanitize(strings.TrimPrefix(err.Error(), "flash_appfs: ERROR: "))+".",
			"see the lines above. Never retry a failed write by hand without reading the recovery instructions in them; the Akam SD card inserted at power-up overrides the flashed image.")
	}
	_ = a.St.SetMany([][2]string{{"rot2k_image_sha", p.SHA}, {"rot2k_version", p.ToVersion}})
	a.UI.Good(fmt.Sprintf("  Updated and verified: mtd3 is the new image (sha256 %s...).", p.SHA[:16]))
	if res.Note != "" {
		a.UI.Warn(res.Note)
	}
	a.UI.Say("  The previous image is kept: " + Prog + " update --rollback puts it back with the same checks.")
	return nil
}

// flashDial opens the camera's ssh (port 0) or the temporary hot one.
func (a *App) flashDial(ip string) func(int) (appfs.Conn, error) {
	return func(port int) (appfs.Conn, error) {
		c, e := a.dialPort(ip, portOr(port, a.Env.PortSSH))
		if e != nil {
			return nil, e
		}
		return c, nil
	}
}

// runDeps2K wires RunUpdate to the camera, the terminal and the API.
func (a *App) runDeps2K(ip string) appfs.RunDeps {
	deps := appfs.Deps{Out: func(l string) { a.UI.Say("  " + l) }, Ctx: a.Ctx, Dial: a.flashDial(ip)}
	yes := a.Opt.Yes
	rd := appfs.RunDeps{
		FlashDeps: deps,
		Say:       func(s string) { a.UI.Bold(s) },
		Asks: appfs.Asks{
			AckRecovery: func() (bool, error) {
				if yes && a.Opt.RecoveryAtHand {
					a.UI.Say("  --recovery-at-hand: you stated that a UART or a CH341A programmer and a verified backup are at hand.")
					return true, nil
				}
				return a.UI.Danger("Card-less appfs flash: RECOVERY MEANS", "Do you have a UART or a CH341A programmer at hand and a verified backup?",
					"Camera: root@"+ip, "The Akam SD card brings back an appfs that MOUNTS. A power cut or crash during the 1-2 minute write leaves mtd3 half written:",
					"then only a UART (U-Boot sf write) or a CH341A programmer brings it back.", "Without a recovery means, do not continue.")
			},
			ConfirmRehearsal: func() (bool, error) {
				if yes {
					return true, nil
				}
				return a.UI.AskYN("Run the rehearsal now (it stops the app for a minute and restores it; no flash is written)?")
			},
			ConfirmWrite: func(sha string) (bool, error) {
				if yes {
					return true, nil
				}
				a.UI.Warn("This WRITES the camera flash (mtd3).")
				want := fmt.Sprintf("%s mtd3", sha[:12])
				ans, e := a.UI.Prompt(fmt.Sprintf("Type exactly '%s' (first 12 digits of the image sha256, then the partition) to write it", want), "")
				return e == nil && strings.TrimSpace(ans) == want, e
			},
		},
		PowerCycle: func() error {
			a.UI.Say("  Power the camera off and on now (unplug and replug it). This program never reboots it.")
			_, e := a.UI.Prompt("  Press Enter when the camera is powered again", "")
			return e
		},
		Detect: func(ctx context.Context) (appfs.Camera, error) {
			k, ok, err := a.apiKey2K()
			if err != nil || !ok {
				return appfs.Camera{}, errors.New("no API key")
			}
			in, err := a.camAPI(ip, k).Info(ctx)
			return appfs.Camera{Model: in.Model, Version: in.App}, err
		},
	}
	return rd
}

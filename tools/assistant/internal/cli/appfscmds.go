package cli

import (
	"crypto/sha256"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/installer"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The appfs commands: the installer of the models whose firmware kind is cardless-appfs (the LSC Rotatable 2K).
//
//	appfs-build  build the card-less appfs image from YOUR mtd3 backup and the Akam kit (host only, no camera, any OS)
//	appfs-flash  rehearse (default) or write it to mtd3, with the gates of tools/flash_appfs.sh
//
// The installer is chosen by the kind of the camera's model, never by its id. A camera of another kind is refused
// here, exactly as a 2K is refused by every 1080p flow (modelGate).

// appfsGate refuses a camera that is not served by the appfs installer (the default type, a 1080p, is one of them).
func (a *App) appfsGate(cmd string) error {
	id := strings.TrimSpace(a.Opt.Model)
	if id == "" {
		id = a.cameraType()
	}
	m, ok := models.ByID(id)
	if !ok {
		return a.UI.Fail(fmt.Sprintf("'%s': the camera type '%s' is not in this program's registry.", cmd, id),
			"nothing was changed. Pass --model with the id of the card-less 2K (see compatible-cameras.md).")
	}
	if f, err := installer.For(m); err != nil || f != installer.Appfs {
		return a.UI.Fail(fmt.Sprintf("'%s' is the card-less appfs installer: %s is not served by it.", cmd, models.Label(m.ID)),
			"nothing was changed. This command is for the models of firmware kind cardless-appfs (the LSC Rotatable 2K); pass --model for it.")
	}
	return nil
}

// cmdBackup2K reads the six partitions of the 2K over SSH (read only: nothing is written on the camera), each twice,
// into a new folder of the camera's state folder, verifies it, and records it as the backup `update` uses.
func (a *App) cmdBackup2K() error {
	if err := a.appfsGate("backup-2k"); err != nil {
		return err
	}
	a.UI.Banner("Back up the card-less 2K (six partitions, read only)")
	dir := a.Opt.Out
	if dir == "" {
		dir = filepath.Join(a.St.Dir, "backups", time.Now().UTC().Format("20060102T150405Z"))
	}
	if a.UI.Dry() {
		a.UI.Say("  WOULD read /proc/mtd (refused unless it is the 2K layout), then read the six partitions over SSH, each twice, compare the two reads")
		a.UI.Say("  and the sizes, write mtd0.bin..mtd5.bin and SHA256SUMS in " + dir + " and verify them from the files. Nothing is written on the camera.")
		return nil
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	res, err := appfs.Backup(a.Ctx, a.flashDial(ip), dir, func(s string) { a.UI.Say("  " + s) })
	if err != nil {
		return a.UI.Fail(ui.Sanitize(err.Error())+".", "nothing was written on the camera.")
	}
	_ = a.St.Set("rot2k_backup", res.Dir)
	a.UI.Good("  Backup verified: " + res.Dir)
	a.UI.Say("  It holds the camera's own firmware and your Wi-Fi settings: keep it private, never share it, and keep a second copy on another disk.")
	a.UI.Say("  " + Prog + " update will use it (" + Prog + " update --backup-dir DIR to use another).")
	return nil
}

func (a *App) cmdAppfsBuild() error {
	if err := a.appfsGate("appfs-build"); err != nil {
		return err
	}
	o := a.Opt
	if o.BackupDir == "" {
		return usageErr("akam: appfs-build needs --backup-dir DIR (your backup, with mtd3.bin); the kit is --kit FILE (the signed .tar.xz of the release) or --kit DIR (an unpacked kit), and is downloaded and verified when --kit is not given; optional --version X.Y.Z, --out DIR")
	}
	out := o.Out
	if out == "" {
		out = "akam-appfs-image"
	}
	a.UI.Banner("Build the card-less appfs image")
	if a.UI.Dry() {
		if o.Kit == "" {
			a.UI.Say("  WOULD download the signed 2K kit of the newest release (or --version) over HTTPS, verify its signature and checksums, then read it,")
		} else {
			a.UI.Say("  WOULD read the kit " + o.Kit + " (signature of a .tar.xz, SHA256SUMS, kind, time64 gate), your mtd3.bin from " + o.BackupDir + ",")
		}
		a.UI.Say("  patch the one init.sh branch, add the Akam payload under /appfs/akam, replace bin/yy_cam by a launcher,")
		a.UI.Say("  write the squashfs, check it against the stock superblock and re-read it, then write " + out + "/" + appfs.ImageName)
		return nil
	}
	var kit *appfs.Kit
	var err error
	if st, serr := os.Stat(o.Kit); o.Kit != "" && serr == nil && st.IsDir() {
		// an already unpacked kit folder (what a developer builds, or an earlier extraction): as before
		var kopts []appfs.KitOption
		if o.AllowUnsigned { // an explicit choice of a local development kit folder
			kopts = append(kopts, appfs.AllowTestGrade())
		}
		kit, err = appfs.LoadKit(o.Kit, kopts...)
		if err != nil {
			return a.UI.Fail("the appfs kit is not usable: "+err.Error(), "get the kit of the 2K from the release (the kit holds only Akam code).")
		}
	} else if kit, err = a.kit2K(); err != nil {
		// the signed kit of the release (--kit FILE, or downloaded): signature, checksums, layout and model are checked
		return err
	}
	stock, err := os.ReadFile(filepath.Join(o.BackupDir, "mtd3.bin"))
	if err != nil {
		return a.UI.Fail("cannot read mtd3.bin in the backup: "+err.Error(), "the card-less image is built from YOUR backup of the camera's appfs partition (mtd3).")
	}
	res, err := appfs.Build(appfs.Inputs{Backup: stock, Kit: kit, Say: func(s string) { a.UI.Say("  " + s) }})
	if err != nil {
		return a.UI.Fail("the image was not built: "+err.Error(), "nothing was changed on the camera.")
	}
	if err := appfs.WriteOutputs(out, res); err != nil {
		return a.UI.Fail("cannot write the image: "+err.Error(), "")
	}
	a.UI.Good(fmt.Sprintf("Image written: %s (sha256 %s, appfs_sign %s)", filepath.Join(out, appfs.ImageName), res.SHA256, res.Sign))
	a.UI.Hint(fmt.Sprintf("rehearse the flash: %s appfs-flash --model <2K> --ip ADDRESS --backup-dir %s --image-dir %s", Prog, o.BackupDir, out))
	return nil
}

func (a *App) cmdAppfsFlash() error {
	if err := a.appfsGate("appfs-flash"); err != nil {
		return err
	}
	o := a.Opt
	if o.ImageDir == "" || o.BackupDir == "" {
		return usageErr("akam: appfs-flash needs --image-dir DIR (output of appfs-build) and --backup-dir DIR (your verified full backup); optional --current-image FILE (update), --hot, --hot-preflight, --work-dir DIR, --nvram, --execute")
	}
	if (o.Hot || o.HotPreflight) && o.CurrentImage == "" {
		return usageErr("akam: --hot and --hot-preflight need --current-image FILE (the image believed to be on mtd3 now)")
	}
	img, sha, sign, err := appfs.ReadImageDir(o.ImageDir)
	if err != nil {
		return a.UI.Fail(err.Error(), "run appfs-build first.")
	}
	plan := appfs.Plan{Image: img, ImageSHA256: sha, Sign: sign, BackupDir: o.BackupDir, WorkDir: o.WorkDir,
		Hot: o.Hot, HotPreflight: o.HotPreflight, NVRAMWrite: o.NVRAMSet}
	if o.CurrentImage != "" {
		if plan.Current, err = os.ReadFile(o.CurrentImage); err != nil {
			return a.UI.Fail("cannot read --current-image: "+err.Error(), "")
		}
		plan.Update = true
	}
	ip, err := a.camNeedIP()
	if err != nil {
		return err
	}
	plan.Target = "root@" + ip
	if plan.WorkDir == "" {
		plan.WorkDir = filepath.Join(a.St.Dir, "appfs-flash-"+time.Now().UTC().Format("20060102T150405Z"))
	}
	a.UI.Banner("Card-less appfs flash")
	// A rehearsal of the program (--dry-run) and a run without --execute are both the script's dry run: local checks
	// and the printed plan, no connection, no confirmation, nothing sent.
	live := o.Execute && !a.UI.Dry()
	deps := appfs.Deps{Out: func(l string) { a.UI.Say("  " + l) }, Ctx: a.Ctx}
	var conf appfs.Confirmation
	if live {
		if err := a.UI.RequireTTY("flash the camera"); err != nil {
			return err
		}
		a.UI.Warn("This WRITES the camera flash (mtd3). The Akam SD card recovers an appfs that MOUNTS; a power cut or crash during the")
		a.UI.Warn("1-2 minute write leaves mtd3 half written, and then only a UART (U-Boot sf write) or a CH341A programmer brings it back.")
		want := fmt.Sprintf("%s mtd3", sha[:12])
		conf, err = appfs.NewConfirmation(sha,
			func() (bool, error) {
				return a.UI.Danger("Card-less appfs flash: RECOVERY MEANS", "Do you have a UART or a CH341A programmer at hand and a verified backup?",
					"Camera: "+plan.Target, "Backup: "+o.BackupDir, "Without a recovery means, do not continue.")
			},
			func() (bool, error) {
				ans, e := a.UI.Prompt(fmt.Sprintf("Type exactly '%s' (first 12 digits of the image sha256, then the partition) to write it", want), "")
				return e == nil && strings.TrimSpace(ans) == want, e
			})
		if err != nil {
			return a.UI.Fail("not confirmed.", "nothing was written.")
		}
		plan.Execute = true
		deps.Dial = func(port int) (appfs.Conn, error) {
			c, e := a.dialPort(ip, portOr(port, a.Env.PortSSH))
			if e != nil {
				return nil, e
			}
			return c, nil
		}
	}
	res, err := appfs.Flash(plan, conf, deps)
	if live {
		appfs.CleanWorkDir(plan.WorkDir, false) // the Wi-Fi config dump is never kept; the others stay for a failure
	}
	if err != nil {
		return a.UI.Fail(strings.TrimPrefix(err.Error(), "flash_appfs: ERROR: "), "see the lines above: the recovery instructions are in the message. Never retry a failed write by hand without reading them.")
	}
	if live && res != nil && res.Written {
		// the write is verified by its read-back: record it, so that the first `update` has a current image
		meta := appfs.ImageMeta{SHA256: sha, Sign: sign, Time: time.Now().UTC().Format(time.RFC3339)}
		var before appfs.ImageMeta
		if plan.Current != nil {
			before = appfs.ImageMeta{SHA256: fmt.Sprintf("%x", sha256.Sum256(plan.Current))}
		}
		if e := a.imageStore().RecordFlashed(plan.Current, before, img, meta); e != nil {
			a.UI.Warn("the image is written and verified, but it could not be recorded on this PC (" + ui.Sanitize(e.Error()) + "): the first update will ask for --current-image " + filepath.Join(o.ImageDir, appfs.ImageName) + ".")
		} else {
			a.UI.Say("  Recorded on this PC as the image on the camera: the next " + Prog + " update starts from it.")
		}
		a.UI.Good("mtd3 holds the new image (read back and compared). Now do the staged reboot printed above.")
	} else if !live {
		a.UI.Hint("this was a dry run. To write: add --execute (needs a terminal, the recovery acknowledgement and the typed confirmation).")
	}
	return nil
}

func portOr(port, def int) int {
	if port == 0 {
		return def
	}
	return port
}

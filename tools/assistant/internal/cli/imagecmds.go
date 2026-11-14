package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/fsutil"
	"github.com/Foohx/akam-fw/tools/assistant/internal/image"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// The `image` command and the image step (T11.16): three ways to get the image of a camera.
//
//	tools   the bash builder of a source checkout (src/ak_rtsp/build_firmware.sh): Linux only
//	import  an image built elsewhere, given with --image FILE
//	native  the in-process builder (internal/image): opt-in with --builder native, EXPERIMENTAL
//
// The default is a policy, not a capability: tools on Linux and import elsewhere until the human gate of
// specs/installer.md (a real camera flashed with the native image) is recorded in docs/hardware-test-checklists.md.
// Flipping the default after the gate is the one-line change of defaultBuilder.

// defaultBuilder is the builder used without --builder.
func defaultBuilder(goos string) string {
	if goos == "linux" {
		return "tools"
	}
	return "import"
}

// imageBuilder is the builder of this run: --image FILE imports, an explicit --builder wins, else the default.
func (a *App) imageBuilder() string {
	if a.Opt.Builder != "" {
		return a.Opt.Builder
	}
	if a.Opt.Image != "" {
		return "import"
	}
	return defaultBuilder(runtime.GOOS)
}

// cmdImage is the `image` command.
func (a *App) cmdImage() error {
	if a.Opt.Builder != "" && a.Opt.Image != "" {
		return usageErr("akam: --builder and --image both given: choose one (the image is built, or imported from a file)")
	}
	return a.installImage()
}

// nativeExperimental is what the user must read before the native builder runs.
const nativeExperimental = "the native image builder is EXPERIMENTAL: it has not been validated on real hardware yet. " +
	"Prefer the image built by the bash wizard (Linux) until the maintainer records the hardware test."

// buildNativeImage is the --builder native route of the image step: confirms, then builds from the verified kit
// and the stock backup, in memory.
func (a *App) buildNativeImage() error {
	a.UI.Warn(nativeExperimental)
	if a.UI.Dry() {
		a.UI.Say("  WOULD build the image in memory from your backup and the verified kit (--builder native), write it with its report, and record it.")
		_ = a.St.Set("image_path", "<image>")
		_ = a.St.Set("image_sha", "dry-run")
		_ = a.St.MarkDone("image")
		return nil
	}
	ok, err := a.UI.AskYN("  Build the image with the EXPERIMENTAL native builder (not yet validated on hardware)?")
	if err != nil || !ok {
		return a.UI.Fail("not building: the native builder was not confirmed.", fmt.Sprintf("use the default builder, or answer yes; to import an image built elsewhere: %s --image FILE image", Prog))
	}
	kitDir := a.kitDir()
	if st, err := os.Stat(filepath.Join(kitDir, "MANIFEST.txt")); err != nil || st.IsDir() {
		return a.UI.Fail("no verified kit found at "+kitDir+".", fmt.Sprintf("run: %s step get-kit", Prog))
	}
	bk := a.Opt.BackupDir
	if bk == "" {
		bk = a.St.Get("backup_dir")
	}
	if bk == "" {
		return a.UI.Fail("the backup folder is not known.", fmt.Sprintf("run: %s backup (or pass --backup-dir DIR)", Prog))
	}
	return a.runNativeImage(kitDir, bk)
}

// kitAuthText says how the kit was authenticated, as the report of the bash builder does.
func (a *App) kitAuthText() (auth, info string) {
	p, unsignedKit := a.kitInUse()
	if st, err := os.Stat(p); err == nil && st.IsDir() {
		return "directory (consistency checked, not authenticated; --allow-unsigned)", ""
	}
	if unsignedKit {
		return "UNSIGNED (checksum-only trust, --allow-unsigned)", "tarball sha256 " + a.kitCurrentSHA()
	}
	return "signature verified", "tarball sha256 " + a.kitCurrentSHA()
}

// imageEpoch is the time of the image: SOURCE_DATE_EPOCH, else the build time of the kit (reproducible), else now.
func imageEpoch(manifest map[string]string) (uint32, error) {
	if v := os.Getenv("SOURCE_DATE_EPOCH"); v != "" {
		n, err := strconv.ParseUint(v, 10, 32)
		if err != nil {
			return 0, fmt.Errorf("SOURCE_DATE_EPOCH must be an integer (seconds, below 2^32), got %q", v)
		}
		return uint32(n), nil
	}
	if t, err := time.Parse("2006-01-02T15:04:05Z", manifest["built_utc"]); err == nil && t.Unix() > 0 && t.Unix() < 1<<32 {
		return uint32(t.Unix()), nil
	}
	return uint32(time.Now().Unix()), nil
}

// verifiedKit re-verifies the kit NOW and returns the folder to load it from, plus a cleanup. A kit that
// came as a tarball is verified again from the tarball itself (SHA256SUMS, SSHSIG signature, safe layout,
// MANIFEST) into a private temporary folder, so a folder edited after get-kit is never used; a kit that is
// only a folder is checked against its own MANIFEST and SHA256SUMS (time64_gate=pass, release grade, every
// sha256 and mode). Nothing from the kit is run.
func (a *App) verifiedKit(kitDir string) (string, func(), error) {
	kp, unsignedKit := a.kitInUse()
	opts := kit.Options{AllowedSigners: a.allowedSigners(), AllowUnsigned: unsignedKit}
	if p := kp; p != "" {
		if st, err := os.Stat(p); err == nil && st.Mode().IsRegular() {
			res, err := kit.VerifyTarball(p, opts)
			if err != nil {
				return "", nil, err
			}
			return res.Dir, res.Close, nil
		}
	}
	if _, err := opts.VerifyExtracted(kitDir); err != nil {
		return "", nil, err
	}
	return kitDir, func() {}, nil
}

// confirmOverrides asks, default No, before an environment override that weakens a safety check of the
// native builder is honoured. It returns the overrides that were confirmed.
func (a *App) confirmOverrides() (tight, unpatched bool, err error) {
	tight = os.Getenv("AKAM_ALLOW_TIGHT") == "1"
	unpatched = os.Getenv("AKAM_ALLOW_UNPATCHED_SERVICE") == "1"
	if unpatched {
		ok, derr := a.UI.Danger("Unpatched service.sh", "Allow an image whose boot script is NOT locked down?",
			"AKAM_ALLOW_UNPATCHED_SERVICE=1 is set: if sbin/service.sh cannot be patched, the build continues.",
			"Telnet and FTP may then still be started by the stock boot script of the camera.",
			"If in doubt, answer No.")
		if derr != nil || !ok {
			return false, false, fmt.Errorf("AKAM_ALLOW_UNPATCHED_SERVICE=1 was not confirmed")
		}
	}
	if tight {
		ok, derr := a.UI.Danger("Image nearly full", "Allow an image within one block of the partition size?",
			"AKAM_ALLOW_TIGHT=1 is set: an image that leaves less than one block free in the APP partition is accepted.",
			"If in doubt, answer No.")
		if derr != nil || !ok {
			return false, false, fmt.Errorf("AKAM_ALLOW_TIGHT=1 was not confirmed")
		}
	}
	return tight, unpatched, nil
}

// outDir is where the image and its report go: --out, else a private folder of this camera.
func (a *App) imageOutDir() string {
	if a.Opt.Out != "" {
		return a.Opt.Out
	}
	return filepath.Join(a.Cams.DirOf(a.Cams.Name), "image")
}

// runNativeImage builds, writes the four files and records the image in the state.
func (a *App) runNativeImage(kitDir, backupDir string) error {
	// The kit is loaded only after it verified (again, now); the backup only after its SHA256SUMS
	// re-hashed clean, and never when it is a backup of an Akam camera (kind=akam): the image is built
	// on the ORIGINAL APP.
	kdir, done, err := a.verifiedKit(kitDir)
	if err != nil {
		fmt.Fprintln(a.Streams.Err, err.Error())
		return a.UI.Fail("the kit failed verification (see the message above).", fmt.Sprintf("do not use it. Run: %s step get-kit", Prog))
	}
	defer done()
	if p := stockBackupProblem(backupDir); p != "" {
		return a.UI.Fail("the backup does not verify: "+p+".", "the image is built from the backup of the ORIGINAL firmware (made with the card): use it or your second copy. Nothing was written.")
	}
	k, err := image.LoadKit(kdir)
	if err != nil {
		return a.UI.Fail("cannot use the kit: "+err.Error()+".", fmt.Sprintf("run: %s step get-kit", Prog))
	}
	stock, err := image.LoadBackup(backupDir)
	if err != nil {
		return a.UI.Fail(err.Error(), "the image is built from your own backup of this camera.")
	}
	epoch, err := imageEpoch(k.Manifest)
	if err != nil {
		return a.UI.Fail(err.Error()+".", "")
	}
	tight, unpatched, err := a.confirmOverrides()
	if err != nil {
		return a.UI.Fail(err.Error()+": nothing was built.", "")
	}
	auth, info := a.kitAuthText()
	say := func(s string) { a.UI.Say("  " + s) }
	res, err := image.Build(image.Options{
		Kit: k, Stock: stock, KitAuth: auth, KitInfo: info, Epoch: epoch,
		UntestedOK: a.Opt.UntestedOK, AllowTight: tight, AllowUnpatchedService: unpatched,
		Say: say, Note: func(s string) { fmt.Fprintln(a.Streams.Err, s) }, Warn: func(s string) { a.UI.Warn(s) },
	})
	if err != nil {
		return a.UI.Fail("native image build failed: "+err.Error()+".", "nothing was written; the stock backup is untouched.")
	}
	dir := a.imageOutDir()
	if err := fsutil.PrivateMkdir(dir); err != nil {
		return a.UI.Fail("cannot create "+dir+": "+err.Error(), "")
	}
	out := filepath.Join(dir, res.Name)
	// the image is checked in memory before any file is written (a file with the final name is never a bad one)
	if len(res.Image) != appPartitionBytes {
		return a.UI.Fail(fmt.Sprintf("the built image is %d bytes, expected exactly %d.", len(res.Image), appPartitionBytes), "nothing was written.")
	}
	if err := a.imageSquashBytes(res.Image); err != nil {
		return err
	}
	files := []struct {
		name string
		data []byte
	}{
		{res.Name, res.Image},
		{res.Name + ".sha256", []byte(res.SHA256 + "  " + res.Name + "\n")},
		{"build_report.txt", []byte(res.Report)},
		{"service_sh.diff", []byte(res.ServiceDiff)},
	}
	for _, f := range files {
		if err := fsutil.AtomicWrite(filepath.Join(dir, f.name), f.data, 0o600); err != nil {
			return a.UI.Fail("cannot write "+filepath.Join(dir, f.name)+": "+err.Error(), "")
		}
	}
	a.UI.Say("")
	for _, l := range strings.Split(strings.TrimRight(res.Report, "\n"), "\n") {
		a.UI.Say(l)
	}
	a.UI.Say("(the free-space figure is only meaningful for the real APP dump)")
	if got := fileSHA(out); got != res.SHA256 {
		_ = os.Remove(out)
		return a.UI.Fail("the image written to "+out+" does not match the one that was built (it was removed).", "do not use it; check the disk and run the command again.")
	}
	md5, err := a.imageGateChecked(out)
	if err != nil {
		_ = os.Remove(out)
		return err
	}
	_ = a.St.Set("image_path", out)
	_ = a.St.Set("image_sha", res.SHA256)
	_ = a.St.Set("image_md5", md5)
	_ = a.St.Set("image_kit_sha", a.kitCurrentSHA())
	_ = a.St.Set("image_backup_sha", a.backupCurrentSHA())
	_ = a.St.MarkDone("image")
	a.UI.Warn("this image comes from the EXPERIMENTAL native builder: it was re-read and compared with its plan, but it has not been validated on a camera.")
	a.UI.Good("  Image written: " + out)
	return nil
}

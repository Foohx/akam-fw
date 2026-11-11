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
)

// The `image` command and the image step (T11.16): three ways to get the image of a camera.
//
//	tools   the bash builder of a source checkout (src/ak_rtsp/build_firmware.sh): Linux only
//	import  an image built elsewhere, given with --image FILE
//	native  the in-process builder (internal/image): opt-in with --builder native, EXPERIMENTAL
//
// The default is a policy, not a capability: tools on Linux and import elsewhere until the human gate of
// spec 11 (4.8 step 4, a real camera flashed with the native image) is recorded in docs/akam_v1_test_plan.md.
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
	p := a.St.Get("kit_path")
	if st, err := os.Stat(p); err == nil && st.IsDir() {
		return "directory (consistency checked, not authenticated; --allow-unsigned)", ""
	}
	if a.St.Get("kit_unsigned_ok") == "1" {
		return "UNSIGNED (checksum-only trust, --allow-unsigned)", "tarball sha256 " + a.St.Get("kit_sha")
	}
	return "signature verified", "tarball sha256 " + a.St.Get("kit_sha")
}

// imageEpoch is the time of the image: SOURCE_DATE_EPOCH, else the build time of the kit (reproducible), else now.
func imageEpoch(manifest map[string]string) uint32 {
	if v := os.Getenv("SOURCE_DATE_EPOCH"); v != "" {
		if n, err := strconv.ParseUint(v, 10, 32); err == nil {
			return uint32(n)
		}
	}
	if t, err := time.Parse("2006-01-02T15:04:05Z", manifest["built_utc"]); err == nil && t.Unix() > 0 && t.Unix() < 1<<32 {
		return uint32(t.Unix())
	}
	return uint32(time.Now().Unix())
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
	kit, err := image.LoadKit(kitDir)
	if err != nil {
		return a.UI.Fail("cannot use the kit: "+err.Error()+".", fmt.Sprintf("run: %s step get-kit", Prog))
	}
	stock, err := image.LoadBackup(backupDir)
	if err != nil {
		return a.UI.Fail(err.Error(), "the image is built from your own backup of this camera.")
	}
	auth, info := a.kitAuthText()
	say := func(s string) { a.UI.Say("  " + s) }
	res, err := image.Build(image.Options{
		Kit: kit, Stock: stock, KitAuth: auth, KitInfo: info, Epoch: imageEpoch(kit.Manifest),
		UntestedOK: a.Opt.UntestedOK, AllowTight: os.Getenv("AKAM_ALLOW_TIGHT") == "1",
		AllowUnpatchedService: os.Getenv("AKAM_ALLOW_UNPATCHED_SERVICE") == "1",
		Say:                   say, Note: func(s string) { fmt.Fprintln(a.Streams.Err, s) }, Warn: func(s string) { a.UI.Warn(s) },
	})
	if err != nil {
		return a.UI.Fail("native image build failed: "+err.Error()+".", "nothing was written; the stock backup is untouched.")
	}
	dir := a.imageOutDir()
	if err := fsutil.PrivateMkdir(dir); err != nil {
		return a.UI.Fail("cannot create "+dir+": "+err.Error(), "")
	}
	out := filepath.Join(dir, res.Name)
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
	md5, err := a.imageGateChecked(out)
	if err != nil {
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

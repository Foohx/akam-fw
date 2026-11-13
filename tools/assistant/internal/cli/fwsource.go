package cli

import (
	"context"
	"crypto/md5"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// "Which firmware?" (Install and Update): the Akam version to put on the camera, a kit or an image the user
// already has, or the stock firmware from the backup. The list of releases is advice only: whatever is
// chosen is downloaded and verified exactly as `get-kit` does, and an image file or the stock firmware goes
// through the same gates as before (size, squashfs structure, md5 in the red box, verified backup).

// githubAPI is the address of the release list (the test hook AKAM_API_URL_BASE replaces it; HTTPS only).
const githubAPI = "https://api.github.com"

// Kinds of firmware source.
const (
	fwRelease = "release" // an Akam version published on GitHub
	fwKit     = "kit"     // a kit file or folder on this computer
	fwImage   = "image"   // an image (.squashfs, the APP partition) on this computer
	fwStock   = "stock"   // the original firmware, from the backup
)

// fwSource is one choice of firmware.
type fwSource struct {
	Kind    string
	Version string // release: X.Y.Z
	Path    string // kit and image: the file or folder
	// Checked: the downgrade check was made by the chooser (the question is not asked twice).
	Checked bool
}

// record is the text kept in the camera's state: release:X.Y.Z, kit:PATH, image:PATH or stock.
func (s fwSource) record() string {
	switch s.Kind {
	case fwRelease:
		return "release:" + s.Version
	case fwKit, fwImage:
		return s.Kind + ":" + stateLine(s.Path)
	case fwStock:
		return "stock"
	}
	return ""
}

func stateLine(s string) string {
	return strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return ' '
		}
		return r
	}, s)
}

// fwFromOptions is the firmware named on the command line (zero when none): --stock, else --image, else
// --kit, else --version. --kit with --image keeps the image as the firmware (the kit then only supplies the
// flash tools).
func fwFromOptions(o *Options) fwSource {
	switch {
	case o.Stock:
		return fwSource{Kind: fwStock}
	case o.Image != "":
		return fwSource{Kind: fwImage, Path: o.Image}
	case o.Kit != "":
		return fwSource{Kind: fwKit, Path: o.Kit}
	case o.Version != "":
		return fwSource{Kind: fwRelease, Version: strings.TrimPrefix(o.Version, "v")}
	}
	return fwSource{}
}

// fwRecord records what the camera runs after img was written and read back. It states facts, not what
// was chosen: an image built here from a kit is "release:X.Y.Z" (that kit is the download of release X.Y.Z)
// or "kit:PATH"; any other image, including one imported with --image or chosen from a release row, is
// "image:PATH" with no version.
func (a *App) fwRecord(img string) {
	src, ver := "image:"+stateLine(img), ""
	if sha := fileSHA(img); sha != "" && sha == a.St.Get("image_sha") && a.stepStatus("image") == "done" &&
		a.St.Get("image_origin") != "imported:"+sha && a.St.Get("mode") == "kit" {
		if kv, p := a.St.Get("kit_version"), a.St.Get("kit_path"); kv != "" {
			ver = kv
			if p == a.downloadedKitPath(kv) && a.St.Get("kit_unsigned_ok") != "1" {
				src = "release:" + kv
			} else {
				src = "kit:" + stateLine(p)
			}
		}
	}
	a.setState("fw_source", src)
	a.setState("fw_source_at", time.Now().UTC().Format("2006-01-02"))
	a.setState("fw_version", ver)
	a.setState("fw_build", "") // learned at the verify that follows this write, once the camera runs the new image
	a.setState("fw_pin", "1")
}

// fwRecordStock records the stock firmware after a verified restore.
func (a *App) fwRecordStock() {
	a.setState("fw_source", "stock")
	a.setState("fw_source_at", time.Now().UTC().Format("2006-01-02"))
	a.setState("fw_version", "")
	a.setState("fw_build", "")
	a.setState("fw_pin", "")
}

// fwNoteBuild remembers the build id the camera reports once it runs the written image (after verify). The
// recorded fw_version is believed only while the camera still reports that build: another build means
// something else was installed since (see currentFirmware).
func (a *App) fwNoteBuild(ip string) {
	if a.St.Get("fw_pin") != "1" || a.UI.Dry() { // only the first verify after a write recorded here
		return
	}
	if id, err := discover.Identify(a.Ctx, ip, a.ports().CTL, a.Hooks.Discover.Dial); err == nil && id.Version != "" {
		a.setState("fw_build", ui.Sanitize(id.Version))
		a.setState("fw_pin", "")
	}
}

// ---------------------------------------------------------------- the list of releases

// relCache is the release list of this session (asked once; a failure is remembered too).
type relCache struct {
	pre  bool
	list []kit.Release
	err  error
}

// apiBase is where the release list is asked. A test build may redirect it; with nothing set, a test
// build never reaches the network (releases then "cannot be reached").
func (a *App) apiBase() (string, bool) {
	if o := testhook.Env("AKAM_API_URL_BASE"); o != "" {
		return strings.TrimRight(o, "/"), true
	}
	if testhook.Compiled {
		return "", false
	}
	return githubAPI, true
}

// releaseList is the usable releases, newest first, read from GitHub at most once per session (and per
// --include-prereleases setting). Nothing it says is trusted: see kit/releases.go.
func (a *App) releaseList() ([]kit.Release, error) {
	if a.rel != nil && a.rel.pre == a.Opt.IncludePre {
		return a.rel.list, a.rel.err
	}
	rc := &relCache{pre: a.Opt.IncludePre}
	base, ok := a.apiBase()
	if !ok {
		rc.err = errors.New("no network in a test build")
	} else {
		a.UI.Say("  Asking GitHub for the releases (read only) ...")
		ctx, cancel := context.WithTimeout(a.Ctx, 20*time.Second)
		a.Log.Line("CMD list releases %s/repos/%s/releases", base, kitRepo)
		rc.list, rc.err = kit.ListReleases(ctx, a.Hooks.HTTP, base, kitRepo, a.Opt.IncludePre)
		cancel()
		if rc.err != nil && a.Ctx.Err() != nil {
			rc.err = a.Ctx.Err()
		}
	}
	a.rel = rc
	return rc.list, rc.err
}

// releaseListReport is releaseList with what the user needs to read: a line before the wait, and the reason
// when the list is missing or empty.
func (a *App) releaseListReport() ([]kit.Release, error) {
	rels, err := a.releaseList()
	if err != nil {
		if a.Ctx.Err() == nil {
			a.UI.Warn("could not read the list of releases: " + releaseProblem(err) + ". Kits and images on this computer can still be chosen.")
		}
	} else if len(rels) == 0 {
		a.UI.Say("  No Akam release is published on GitHub yet (only releases with a signed kit are listed).")
	}
	return rels, err
}

// releaseProblem is the sentence that explains why the list is missing (never raw network text).
func releaseProblem(err error) string {
	var he *kit.HTTPError
	switch {
	case errors.As(err, &he) && (he.Code == 403 || he.Code == 429):
		return "GitHub refused the request (HTTP " + fmt.Sprint(he.Code) + ": too many requests from this address, try again in a while)"
	case errors.As(err, &he):
		return fmt.Sprintf("GitHub answered HTTP %d", he.Code)
	case errors.Is(err, kit.ErrTooLarge), errors.Is(err, kit.ErrBadReleaseList):
		return "GitHub's answer was not a list of releases"
	}
	return "this computer cannot reach GitHub"
}

// downloadedKits lists the kits kept in the downloads folder of the assistant, newest first (by version).
// Only the file name and the folder name are read here; the kit is verified when it is used.
func (a *App) downloadedKits() []string {
	ents, err := os.ReadDir(filepath.Join(a.Cams.Base, "downloads"))
	if err != nil {
		return nil
	}
	var out []string
	for _, e := range ents {
		v := e.Name()
		if !e.IsDir() || !validKitVersion(v) {
			continue
		}
		if st, err := os.Stat(filepath.Join(a.Cams.Base, "downloads", v, "akam-kit-"+v+".tar.xz")); err == nil && st.Mode().IsRegular() {
			out = append(out, v)
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		c, ok := kit.CompareVersions(out[i], out[j])
		if !ok {
			return out[i] > out[j]
		}
		return c > 0
	})
	return out
}

func (a *App) downloadedKitPath(v string) string {
	return filepath.Join(a.Cams.Base, "downloads", v, "akam-kit-"+v+".tar.xz")
}

// cmdReleases is `akam releases`: the versions published on GitHub, newest first.
func (a *App) cmdReleases() error {
	a.banner("Akam releases published on GitHub (" + kitRepo + ")")
	if a.UI.Dry() {
		a.UI.Say("  WOULD ask GitHub (HTTPS, read only) for the newest releases of " + kitRepo + ". Nothing is downloaded.")
		return nil
	}
	list, err := a.releaseList()
	if err != nil {
		if a.Ctx.Err() != nil {
			return a.Ctx.Err()
		}
		return a.UI.Fail("cannot list the releases: "+releaseProblem(err)+".", fmt.Sprintf("check your network and retry; kits you already downloaded: %s update --kit FILE.", Prog))
	}
	if len(list) == 0 {
		a.UI.Say("  No release is published yet (only releases with a signed kit are listed).")
		return nil
	}
	cur := ""
	if ip := a.knownIP(); ip != "" {
		_, cur = a.currentFirmware(ip, false)
	}
	for i, r := range list {
		tag := ""
		if i == 0 {
			tag = "  latest"
		}
		if r.Prerelease {
			tag += "  prerelease"
		}
		if r.Version == cur {
			tag += "  (on the chosen camera)"
		}
		d := r.Published
		if d == "" {
			d = "          "
		}
		a.UI.Say(fmt.Sprintf("  %-12s %s%s", r.Version, d, tag))
	}
	a.UI.Say(fmt.Sprintf("  Use one: %s update --version X.Y.Z   (the kit is verified by its signature, as get-kit does).", Prog))
	if !a.Opt.IncludePre {
		a.UI.Say("  Prereleases are hidden; --include-prereleases shows them.")
	}
	return nil
}

// knownIP is the chosen camera's address, or "".
func (a *App) knownIP() string {
	if a.Opt.IP != "" {
		return a.Opt.IP
	}
	return a.St.Get("camera_ip")
}

// currentFirmware is what the camera runs: the text its control port reports (read only; sanitised) and
// the X.Y.Z of it. The camera's own text is a build id today ("b35-2af81c5"), so when it holds no X.Y.Z
// the version this assistant last installed (fw_version) is used, and said to be so in the text. With live
// false, or when the camera does not answer, the last value seen is used.
func (a *App) currentFirmware(ip string, live bool) (text, ver string) {
	raw := ui.Sanitize(a.St.Get("camera_fw"))
	fromCamera := false // raw was just read from the camera, not remembered from an earlier look
	if live && !a.UI.Dry() && ip != "" {
		if id, err := discover.Identify(a.Ctx, ip, a.ports().CTL, a.Hooks.Discover.Dial); err == nil && id.Version != "" {
			raw, fromCamera = ui.Sanitize(id.Version), true
		}
	}
	if v, ok := kit.ExtractVersion(raw); ok {
		return raw, v
	}
	join := func(s string) string {
		if raw == "" {
			return s
		}
		return raw + "; " + s
	}
	// the image carries the manifest of its kit: that is the camera's own word, read over SSH (read only)
	if live && !a.UI.Dry() && ip != "" {
		if v := a.cameraAkamVersion(ip); v != "" {
			return join("Akam " + v), v
		}
	}
	// else what this assistant installed, believed only while the camera still reports the build seen after
	// the write (another build means something else was installed since)
	if v := a.St.Get("fw_version"); v != "" {
		if b := a.St.Get("fw_build"); b != "" && fromCamera && raw != b {
			return raw, ""
		}
		if _, ok := kit.ParseVersion(v); ok {
			return join("Akam " + v + " installed here"), v
		}
	}
	return raw, ""
}

// cameraAkamVersion reads akam_version from the manifest the image carries (/usr/local/akam/MANIFEST.txt, the
// kit's own MANIFEST.txt) over SSH. "" when the camera cannot be reached, the file is absent (an image built
// before it was read this way, or by hand) or the version is not a plain X.Y.Z (a source build).
func (a *App) cameraAkamVersion(ip string) string {
	c, err := a.dial(ip)
	if err != nil {
		return ""
	}
	defer c.Close()
	r, err := a.run(c, remoteManifest, nil, "")
	if err != nil || r.Status != 0 {
		return ""
	}
	for _, l := range strings.Split(string(r.Stdout), "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(l), "akam_version="); ok {
			if _, ok := kit.ParseVersion(v); ok {
				return strings.TrimPrefix(v, "v")
			}
			return ""
		}
	}
	return ""
}

// ---------------------------------------------------------------- the chooser

type fwChoice struct {
	label string
	src   fwSource
	act   string // "" (the source), "kitfile", "imagefile", "back"
}

// fwMode is where the chooser is shown.
type fwMode int

const (
	fwInstall fwMode = iota // first install: no stock option
	fwUpdate                // Akam camera: the stock firmware is offered when a stock backup verifies
	fwKitOnly               // the kit only (the image file chosen needs the flash tools of a kit)
)

const maxOlderShown = 12

// fwList is what the chooser shows: Cur is the X.Y.Z the camera runs ("" when unknown), Downloaded the kits
// kept in the downloads folder, Built the image recorded by the image step ("" when it is not valid),
// BuiltFirst puts that image first (it is then the default: this computer cannot build the image of a new
// release), StockOK that a stock backup verifies, Retry that the release list failed (adds "Try again").
type fwList struct {
	Mode       fwMode
	Cur        string
	Rels       []kit.Release
	Downloaded []string
	Built      string
	BuiltFirst bool
	// BuiltImported: the image on record was imported, not built from a kit; BuiltOnCamera: it is the image the
	// camera was last written with (same md5)
	BuiltImported bool
	BuiltOnCamera bool
	StockOK       bool
	Retry         bool
}

// fwChoices builds the rows of the chooser.
func fwChoices(l fwList) []fwChoice {
	mode, cur, rels, downloaded, built, stockOK := l.Mode, l.Cur, l.Rels, l.Downloaded, l.Built, l.StockOK
	var out []fwChoice
	builtLabel := "The image already built from your backup (" + filepath.Base(built) + ")"
	if l.BuiltImported {
		builtLabel = "The image you imported earlier (" + filepath.Base(built) + ")"
	}
	if l.BuiltOnCamera {
		builtLabel += "  - already on the camera"
	}
	builtRow := fwChoice{label: builtLabel, src: fwSource{Kind: fwImage, Path: built}, act: "built"}
	if mode == fwUpdate && built != "" && l.BuiltFirst {
		out = append(out, builtRow)
	}
	older := 0
	for i, r := range rels {
		l := "Akam " + r.Version
		switch {
		case i == 0:
			l += " (latest)"
		default:
			l += "  - older"
		}
		if r.Prerelease {
			l += "  - prerelease"
		}
		if r.Version == cur {
			l += "  - on the camera now"
		}
		if i > 0 {
			if older++; older > maxOlderShown {
				continue
			}
		}
		out = append(out, fwChoice{label: l, src: fwSource{Kind: fwRelease, Version: r.Version}})
	}
	if older > maxOlderShown {
		out = append(out, fwChoice{label: fmt.Sprintf("(%d older releases not shown: %s update --version X.Y.Z; list: %s releases)", older-maxOlderShown, Prog, Prog), act: "none"})
	}
	for _, v := range downloaded {
		out = append(out, fwChoice{label: "Kit already downloaded: " + v, src: fwSource{Kind: fwKit, Version: v}})
	}
	if mode == fwUpdate && built != "" && !l.BuiltFirst {
		out = append(out, builtRow)
	}
	out = append(out, fwChoice{label: "A kit file or folder...", act: "kitfile"})
	if mode != fwKitOnly {
		out = append(out, fwChoice{label: "An image file you built (.squashfs)...", act: "imagefile"})
	}
	if mode == fwUpdate && stockOK {
		out = append(out, fwChoice{label: "Stock firmware (from your backup)  - never tested on a real camera", src: fwSource{Kind: fwStock}})
	}
	if l.Retry {
		out = append(out, fwChoice{label: "Try again to read the releases", act: "retry"})
	}
	return append(out, fwChoice{label: "Back", act: "back"})
}

// stockBackupDir is the recorded backup of the stock firmware when it verifies now, else "".
func (a *App) stockBackupDir() string {
	bk := a.Opt.BackupDir
	if bk == "" {
		bk = a.St.Get("backup_dir")
	}
	if bk == "" || stockBackupProblem(bk) != "" {
		return ""
	}
	return bk
}

// pickFirmware shows "Which firmware?" until a source is chosen (ok) or the user goes back. cur is the
// X.Y.Z the camera runs; curText the text it reports.
func (a *App) pickFirmware(mode fwMode, curText, cur string) (fwSource, bool) {
	rels, relErr := a.releaseListReport()
	if relErr != nil && a.Ctx.Err() != nil {
		return fwSource{}, false
	}
	stockOK := mode == fwUpdate && a.stockBackupDir() != ""
	title := "Which firmware?"
	switch {
	case mode == fwKitOnly:
		title = "Which kit? (it supplies the flash tools)"
	case curText != "":
		title = "Which firmware?  (the camera runs: " + curText + ")"
	}
	downloaded := a.downloadedKits()
	built := ""
	if p := a.St.Get("image_path"); p != "" && a.stepStatus("image") == "done" {
		built = p
	}
	// the image the camera already runs is no default: writing it again changes nothing
	onCamera := built != "" && a.St.Get("image_md5") != "" && a.St.Get("image_md5") == a.St.Get("flash_md5")
	for {
		items := fwChoices(fwList{Mode: mode, Cur: cur, Rels: rels, Downloaded: downloaded, Built: built,
			BuiltFirst: built != "" && !a.canBuildImage() && !onCamera, BuiltImported: a.importedImageOnRecord(), BuiltOnCamera: onCamera,
			StockOK: stockOK, Retry: relErr != nil})
		labels := make([]string, len(items))
		for i, it := range items {
			labels[i] = it.label
		}
		i, err := a.UI.Pick(title, labels)
		if err != nil || i < 0 || i >= len(items) {
			return fwSource{}, false
		}
		c := items[i]
		switch c.act {
		case "back":
			return fwSource{}, false
		case "none":
			continue
		case "retry":
			a.rel = nil
			if rels, relErr = a.releaseListReport(); relErr != nil && a.Ctx.Err() != nil {
				return fwSource{}, false
			}
			continue
		case "kitfile":
			p, ok := a.askKitPath()
			if !ok {
				continue
			}
			return fwSource{Kind: fwKit, Path: p}, true
		case "imagefile":
			p, ok := a.askImagePath()
			if !ok {
				continue
			}
			return fwSource{Kind: fwImage, Path: p}, true
		}
		if c.src.Kind == fwKit && c.src.Path == "" { // a kit of the downloads folder
			c.src.Path = a.downloadedKitPath(c.src.Version)
		}
		if c.src.Kind == fwStock {
			a.UI.Warn("the stock firmware restore has never been tried on a real camera. It writes the APP partition from your verified backup, through the same red box as Restore.")
		}
		if (c.src.Kind == fwRelease || c.src.Kind == fwKit) && c.src.Version != "" && mode != fwKitOnly {
			// the version is known from the list or the folder name: ask before anything is downloaded or replaced
			// (a kit that only supplies the flash tools is never a downgrade)
			if !a.downgradeOK(c.src.Version, cur) {
				continue
			}
			c.src.Checked = true
		}
		return c.src, true
	}
}

// askKitPath asks for a kit (a .tar.xz with its SHA256SUMS next to it, or a folder). It only checks that the
// path exists: the kit is verified (SHA256SUMS, signature, layout) when it is used.
func (a *App) askKitPath() (string, bool) {
	p, err := a.UI.Prompt("  Path of the kit (.tar.xz file or folder; empty: go back)", "")
	p = strings.TrimSpace(p)
	if err != nil || p == "" {
		return "", false
	}
	abs, aerr := filepath.Abs(p)
	if aerr == nil {
		p = abs
	}
	if _, err := os.Stat(p); err != nil {
		a.UI.Warn("no such file or folder: " + ui.Sanitize(p))
		return "", false
	}
	return p, true
}

// askImagePath asks for an image built elsewhere. It must be the exact size of the APP partition; its md5 is
// shown, and one yes/no (default No) says the user takes the responsibility: this program did not build it.
// The squashfs structure is checked later, with the same code as every image.
func (a *App) askImagePath() (string, bool) {
	p, err := a.UI.Prompt("  Path of the image (.squashfs, exactly 5242880 bytes; empty: go back)", "")
	p = strings.TrimSpace(p)
	if err != nil || p == "" {
		return "", false
	}
	if abs, aerr := filepath.Abs(p); aerr == nil {
		p = abs
	}
	st, serr := os.Stat(p)
	switch {
	case serr != nil || !st.Mode().IsRegular():
		a.UI.Warn("no such file: " + ui.Sanitize(p))
		return "", false
	case st.Size() != appPartitionBytes:
		a.UI.Warn(fmt.Sprintf("that file is %d bytes; an image is exactly %d. It is refused.", st.Size(), appPartitionBytes))
		return "", false
	}
	b, rerr := os.ReadFile(p)
	if rerr != nil {
		a.UI.Warn("cannot read the image: " + ui.Sanitize(rerr.Error()))
		return "", false
	}
	a.UI.Say(fmt.Sprintf("  Image %s: %d bytes, md5 %x.", ui.Sanitize(p), len(b), md5.Sum(b)))
	a.UI.Warn("this image was not built by this program: you take responsibility for what it contains. It must come from YOUR backup of THIS camera.")
	ok, qerr := a.UI.AskYN("  Use this image?")
	if qerr != nil || !ok {
		a.UI.Say("  Not used.")
		return "", false
	}
	return p, true
}

// downgradeOK is true when target is not older than the version the camera runs; an older one asks one
// yes/no (default No). Unknown versions (either side) cannot be compared and pass.
func (a *App) downgradeOK(target, cur string) bool {
	c, ok := kit.CompareVersions(target, cur)
	if !ok || c >= 0 {
		return true
	}
	a.UI.Warn(fmt.Sprintf("Akam %s is OLDER than the %s on this camera: this is a downgrade. Settings files saved by the newer version may not be understood by the older one. Your backup stays valid.", target, cur))
	yes, err := a.UI.AskYN(fmt.Sprintf("  Go back to Akam %s?", target))
	return err == nil && yes
}

// ---------------------------------------------------------------- applying the choice

// runGetKit runs get-kit (stage and install) for a version or a kit path without leaving them in the options.
func (a *App) runGetKit(version, kitPath string) error {
	sv, sk := a.Opt.Version, a.Opt.Kit
	a.Opt.Version, a.Opt.Kit = version, kitPath
	defer func() { a.Opt.Version, a.Opt.Kit = sv, sk }()
	return a.cmdGetKit()
}

// stageKit downloads or opens and verifies a kit without installing it (see fwkit.go).
func (a *App) stageKit(version, kitPath string) (*stagedKit, error) {
	sv, sk := a.Opt.Version, a.Opt.Kit
	a.Opt.Version, a.Opt.Kit = version, kitPath
	defer func() { a.Opt.Version, a.Opt.Kit = sv, sk }()
	return a.getKitStage()
}

// prepareFirmware makes the kit and the image of src ready and returns the image to write. A release or a
// kit is downloaded or opened and verified into a staging folder, the downgrade question is asked, and the
// image is checked or built for it; nothing recorded changes. The staged kit stays in a.staged and is installed
// (commitStaged) by the caller once the camera was written and read back, so a refusal, a failure, a "No" at
// the red box or an interruption leave the previous kit, kit state and image exactly as they were. An image
// file only needs a kit for the flash tools. cur is the X.Y.Z the camera runs. Nothing is written to the camera.
func (a *App) prepareFirmware(src fwSource, cur string, ask bool) (string, error) {
	if a.UI.Dry() { // a rehearsal says what would happen; no step is run, nothing is downloaded or asked
		switch src.Kind {
		case fwRelease:
			a.UI.Say(fmt.Sprintf("  WOULD download the kit of Akam %s (HTTPS only), verify its signature, and build the image from it and your backup.", src.Version))
		case fwKit:
			a.UI.Say("  WOULD verify the kit " + ui.Sanitize(src.Path) + " (SHA256SUMS, signature, layout) and build the image from it and your backup.")
		case fwImage:
			a.UI.Say("  WOULD use the image " + ui.Sanitize(src.Path) + " (exactly 5242880 bytes, structure checked, md5 shown in the red box).")
			return src.Path, nil
		}
		return "", nil
	}
	switch src.Kind {
	case fwRelease, fwKit:
		return a.prepareKit(src, cur)
	case fwImage:
		// the image is the firmware; the kit only supplies the flash tools
		var version, path string
		switch {
		case a.Opt.Kit != "" || a.Opt.Version != "":
			version, path = a.Opt.Version, a.Opt.Kit
		case a.stepStatus("get-kit") != "done" && ask:
			a.UI.Say("  The image is written with the flash tools of a kit, and no kit is ready yet.")
			ks, ok := a.pickFirmware(fwKitOnly, "", "")
			if !ok {
				return "", a.UI.Fail("no kit chosen.", "nothing was changed.")
			}
			version, path = ks.Version, ks.Path
		default:
			return src.Path, nil
		}
		s, err := a.stageKit(version, path)
		if err != nil {
			a.UI.Say("  The kit was not used: nothing was changed, the previous kit and image stay as they were.")
			return "", err
		}
		a.staged = s
		return src.Path, nil
	}
	return "", a.UI.Fail("no firmware to prepare.", "")
}

// canBuildImage tells whether this program can build the image itself (--builder native, experimental). By
// default the image is built by the bash wizard from a source checkout, or elsewhere and imported.
func (a *App) canBuildImage() bool { return a.Opt.Builder == "native" }

// importedImageOnRecord is true when the image on record was imported (--image or a chosen file), not built
// from a kit by this program or the bash wizard.
func (a *App) importedImageOnRecord() bool {
	sha := a.St.Get("image_sha")
	return sha != "" && a.St.Get("image_origin") == "imported:"+sha
}

// prepareKit stages the kit of a release or a kit file and makes the image for it ready, all or nothing: the
// kit is verified again every time (a recorded kit of the same version is never trusted), the downgrade
// question is asked about the version the kit itself states, and when the image cannot be made (no builder
// here, a refusal, a failure) nothing recorded has changed. On success the staged kit waits in a.staged.
func (a *App) prepareKit(src fwSource, cur string) (img string, err error) {
	if src.Kind == fwRelease && !src.Checked && !a.downgradeOK(src.Version, cur) {
		return "", a.UI.Fail("downgrade not confirmed.", "nothing was changed.")
	}
	var s *stagedKit
	if src.Kind == fwRelease {
		s, err = a.stageKit(src.Version, "")
	} else {
		s, err = a.stageKit("", src.Path)
	}
	if err != nil {
		a.UI.Say("  The kit was not used: nothing was changed, the previous kit and image stay as they were.")
		return "", err
	}
	a.staged = s
	defer func() {
		if err != nil {
			a.discardKit(s)
		}
	}()
	kv := s.version
	switch {
	case src.Kind == fwRelease && kv != src.Version:
		return "", a.UI.Fail(fmt.Sprintf("the kit of release v%s says it is version %s: it is not used.", src.Version, ui.Sanitize(kv)),
			"nothing was changed. Download it again, or report it if it persists.")
	case src.Kind == fwKit && (!src.Checked || (src.Version != "" && src.Version != kv)) && !a.downgradeOK(kv, cur):
		// asked about the version the kit states, also when it differs from its folder name
		return "", a.UI.Fail("downgrade not confirmed.", "nothing was changed.")
	}
	if a.stepStatus("image") != "done" {
		if !a.canBuildImage() {
			return "", a.imageNeededFailure(kv)
		}
		sv := a.Opt.Image
		a.Opt.Image = ""
		err = a.installImage()
		a.Opt.Image = sv
		if err != nil {
			return "", err
		}
	} else if a.importedImageOnRecord() {
		a.UI.Warn(fmt.Sprintf("the image on record was imported, not built from Akam %s: choosing this version does not change what would be written.", ui.Sanitize(kv)))
		ok, qerr := a.UI.AskYN("  Write that imported image anyway?")
		if qerr != nil || !ok {
			return "", a.UI.Fail("the imported image on record is not used.", "choose 'The image you imported earlier' to write it, or build the image of this version from your backup.")
		}
	}
	img = a.St.Get("image_path")
	if img == "" {
		return "", a.UI.Fail("no image to write.", "build it from your backup, or pass --image FILE.")
	}
	return img, nil
}

// imageNeededFailure explains, for the update, that the kit is fine but its image cannot be built here.
func (a *App) imageNeededFailure(kv string) error {
	v := ui.Sanitize(kv)
	a.UI.Say(fmt.Sprintf("  Akam %s is verified, but this program cannot build its image on this computer: the image is built from YOUR stock backup,", v))
	a.UI.Say("  by the bash wizard from a source checkout on Linux or WSL, or with --builder native (experimental).")
	return a.UI.Fail("the image for this kit cannot be built here: nothing was changed (the previous kit and image stay as they were).",
		fmt.Sprintf("on Linux or WSL run %s get-kit --version %s, then %s step image, then update again and choose 'The image already built from your backup'; or pass --image FILE.", Prog, v, Prog))
}

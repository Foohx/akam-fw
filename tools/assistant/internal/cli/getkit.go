package cli

import (
	"errors"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"os"
	"path/filepath"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
)

// kitRepo is KIT_REPO of steps_host.sh.
const kitRepo = "Foohx/akam-fw"

func validKitVersion(v string) bool {
	if v == "" || strings.Contains(v, "..") {
		return false
	}
	for _, r := range v {
		if !(r >= '0' && r <= '9' || r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r == '.' || r == '_' || r == '-') {
			return false
		}
	}
	return true
}

// allowedSigners is the trust list: AKAM_ALLOWED_SIGNERS (a file) or the copy built into the program.
func (a *App) allowedSigners() []byte {
	if f := testhook.Env("AKAM_ALLOWED_SIGNERS"); f != "" {
		if b, err := os.ReadFile(f); err == nil {
			return b
		}
		return nil // named but unreadable: "signature present but allowed_signers not found"
	}
	return kit.DefaultAllowedSigners
}

// cmdGetKit is cmd_get_kit: download (HTTPS only) and verify the prebuilt kit; nothing from it is run.
func (a *App) cmdGetKit() error {
	if err := a.stepNeed("get-kit"); err != nil {
		return err
	}
	a.stepBanner("get-kit")
	dry := a.UI.Dry()
	kitPath := a.Opt.Kit
	op := a.op()
	defer op.end()
	verifyItem := 0 // index of "Verify and unpack" in the operation
	if kitPath != "" {
		op.plan("Verify and unpack the kit")
	}
	if kitPath == "" {
		ver := strings.TrimPrefix(a.Opt.Version, "v")
		if ver == "" {
			if strings.Contains(a.Version, "dev") {
				if dry {
					ver = "X.Y.Z"
					a.UI.Say("  (development checkout: using the placeholder version X.Y.Z)")
				} else {
					return a.UI.Fail("this development checkout has no release to download.",
						fmt.Sprintf("pass --kit FILE for a kit you already have, or --version X.Y.Z to download that release, or run %s build.", Prog))
				}
			} else {
				ver = a.Version
			}
		}
		if !validKitVersion(ver) {
			return a.UI.Fail(fmt.Sprintf("'%s' is not a valid version.", ver), fmt.Sprintf("use a number such as 1.0.0 (see https://github.com/%s/releases).", kitRepo))
		}
		base := fmt.Sprintf("https://github.com/%s/releases/download/v%s", kitRepo, ver)
		if o := testhook.Env("AKAM_KIT_URL_BASE"); o != "" { // test hook; still HTTPS only
			base = strings.TrimRight(o, "/") + "/v" + ver
		}
		dl := filepath.Join(a.Cams.Base, "downloads", ver)
		kitPath = filepath.Join(dl, "akam-kit-"+ver+".tar.xz")
		a.UI.Say(fmt.Sprintf("  Downloading kit %s from %s (HTTPS only; nothing from it is run before it is verified).", ver, base))
		if !dry {
			if err := os.MkdirAll(dl, 0o700); err != nil {
				return a.UI.Fail("cannot create "+dl+": "+err.Error(), "")
			}
		}
		op.plan("Download the kit", "Download the checksums (SHA256SUMS)", "Download the signature", "Verify and unpack the kit")
		verifyItem = 3
		for i, f := range []string{"akam-kit-" + ver + ".tar.xz", "SHA256SUMS"} {
			op.start(i)
			op.status("HTTPS GET " + base + "/" + f + "   (nothing from it is run)")
			lim := int64(kit.MaxKitDownload)
			if f == "SHA256SUMS" {
				lim = kit.MaxSmallDownload
			}
			if err := a.download(base+"/"+f, filepath.Join(dl, f), lim); err != nil {
				_ = os.Remove(filepath.Join(dl, f))
				var he *kit.HTTPError
				if errors.As(err, &he) {
					return a.UI.Fail(fmt.Sprintf("release v%s was not found (%s). Akam may not have published a release yet.", ver, f),
						fmt.Sprintf("see https://github.com/%s/releases for the versions that exist, or use a kit you already have: %s get-kit --kit FILE (or build from source: %s build).", kitRepo, Prog, Prog))
				}
				return a.UI.Fail(fmt.Sprintf("download of %s failed (%v).", f, err),
					fmt.Sprintf("check your network and retry, or use a kit you already have: %s get-kit --kit FILE.", Prog))
			}
		}
		op.start(2)
		sig := filepath.Join(dl, "SHA256SUMS.sig")
		if err := a.download(base+"/SHA256SUMS.sig", sig, kit.MaxSmallDownload); err != nil {
			_ = os.Remove(sig)
			var he *kit.HTTPError
			if errors.As(err, &he) {
				a.UI.Warn("no signature file in this release: the kit would be checked by checksum only.")
			} else {
				return a.UI.Fail(fmt.Sprintf("download of SHA256SUMS.sig failed (%v).", err), "check your network and retry.")
			}
		}
	}
	if abs, err := filepath.Abs(kitPath); err == nil {
		if _, err := os.Stat(abs); err == nil {
			kitPath = abs
		}
	}
	st, statErr := os.Stat(kitPath)
	if statErr != nil && !dry {
		return a.UI.Fail("kit not found: "+kitPath, "check the path given to --kit.")
	}

	ext := filepath.Join(a.Cams.Base, "kit")
	if dry {
		ext = filepath.Join(a.Cams.Base, "kit.dry")
	}
	if statErr != nil { // rehearsal, nothing downloaded
		a.UI.Say(fmt.Sprintf("  WOULD verify %s (SHA256SUMS, signature, safe archive layout, MANIFEST) and extract it to %s   (kit not downloaded in a rehearsal)", kitPath, ext))
		_ = a.St.Set("mode", "kit")
		_ = a.St.Set("kit_path", kitPath)
		_ = a.St.Set("kit_sha", "dry-run")
		_ = a.St.Set("kit_format", "1")
		_ = a.St.MarkDone("get-kit")
		return nil
	}

	op.start(verifyItem)
	op.status("checking SHA256SUMS, the signature and the archive layout (read only; nothing from the kit is run)")
	unsigned := a.Opt.AllowUnsigned
	_ = os.RemoveAll(ext)
	a.UI.Say(fmt.Sprintf("  Verifying %s (read-only; nothing from the kit is run).", kitPath))
	opts := kit.Options{
		AllowedSigners: a.allowedSigners(),
		AllowUnsigned:  a.Opt.AllowUnsigned,
		Expect:         a.Opt.ExpectSHA256,
		ExtractTo:      ext,
		Say:            func(s string) { a.UI.Say("  " + s) },
		Warn:           func(s string) { fmt.Fprintln(a.Streams.Err, "WARNING: "+s) },
	}
	var man map[string]string
	var err error
	if st.IsDir() {
		// Directory mode cannot check a signature at all: it is only ever accepted as an unsigned kit.
		if !unsigned {
			return a.UI.Fail("the kit is a folder, which cannot be authenticated (no signature to check).",
				"pass the signed .tar.xz, or --allow-unsigned if you trust where this folder came from.")
		}
		man, err = a.verifyKitDir(opts, kitPath, ext)
	} else {
		man, err = a.verifyKitFile(opts, kitPath, &unsigned)
	}
	if err != nil {
		return err
	}
	fmtv := man["kit_format"]
	if !containsStr(kit.SupportedKitFormats, fmtv) {
		if fmtv == "" {
			fmtv = "?"
		}
		return a.UI.Fail(fmt.Sprintf("this kit has format '%s', which this program does not support.", fmtv),
			fmt.Sprintf("use the release that ships with this kit, or build from source (%s build).", Prog))
	}
	var sha string
	if st.IsDir() {
		sha = fileSHA(filepath.Join(kitPath, "MANIFEST.txt"))
	} else {
		sha = fileSHA(kitPath)
	}
	_ = a.St.Set("mode", "kit")
	_ = a.St.Set("kit_path", kitPath)
	_ = a.St.Set("kit_sha", sha)
	_ = a.St.Set("kit_format", fmtv)
	u := "0"
	if unsigned {
		u = "1"
	}
	_ = a.St.Set("kit_unsigned_ok", u)
	_ = a.St.Set("kit_version", man["akam_version"])
	_ = a.St.MarkDone("get-kit")
	op.done(verifyItem, "format "+fmtv)
	a.UI.Good(fmt.Sprintf("  Kit verified (format %s, sha256 %s...).", fmtv, sha[:12]))
	if a.Opt.ExpectSHA256 == "" {
		a.UI.Say("  Tip: compare that sha256 with the one in the release notes, or pass --expect-sha256 HEX.")
	}
	return nil
}

func containsStr(l []string, s string) bool {
	for _, x := range l {
		if x == s {
			return true
		}
	}
	return false
}

func (a *App) download(url, out string, max int64) error {
	if a.UI.Dry() {
		a.UI.Say("  WOULD download " + url)
		return nil
	}
	a.Log.Line("CMD download %s", url)
	a.UI.Say("  Downloading " + url)
	return kit.FetchLimit(a.Ctx, a.Hooks.HTTP, url, out, max)
}

// verifyKitFile runs the tarball checks; an unsigned kit needs the user's explicit consent.
func (a *App) verifyKitFile(opts kit.Options, file string, unsigned *bool) (map[string]string, error) {
	res, err := kit.VerifyTarball(file, opts)
	if errors.Is(err, kit.ErrUnsigned) {
		a.UI.Warn("this kit is NOT signed: only its checksum can be verified, so you must trust where you got it.")
		ok, derr := a.UI.Danger("Unsigned kit", "Use this kit without a signature?",
			"Only the checksum of this kit can be verified: nothing proves who made it.",
			"Kit: "+file, "Use it only if you got it from a source you trust. If in doubt, answer No.")
		if derr != nil || !ok {
			return nil, a.UI.Fail("unsigned kit refused.", "get the signed release, or pass --allow-unsigned if you really mean it.")
		}
		*unsigned = true
		_ = os.RemoveAll(opts.ExtractTo)
		opts.AllowUnsigned = true
		res, err = kit.VerifyTarball(file, opts)
	}
	if err != nil {
		fmt.Fprintln(a.Streams.Err, err.Error())
		return nil, a.UI.Fail("the kit failed verification (see the message above).",
			"do not use it. Download it again or get it from the official release page.")
	}
	defer res.Close()
	return res.Manifest, nil
}

func (a *App) verifyKitDir(opts kit.Options, dir, ext string) (map[string]string, error) {
	man, err := opts.VerifyExtracted(dir)
	if err != nil {
		fmt.Fprintln(a.Streams.Err, err.Error())
		return nil, a.UI.Fail("the kit failed verification (see the message above).",
			"do not use it. Download it again or get it from the official release page.")
	}
	if err := kit.CopyTree(dir, ext); err != nil {
		return nil, a.UI.Fail("cannot copy the kit folder: "+err.Error(), "")
	}
	return man, nil
}

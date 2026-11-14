package cli

import (
	"errors"
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"os"
	"path/filepath"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
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

// cmdGetKit is cmd_get_kit: download (HTTPS only) and verify the prebuilt kit, then install it; nothing from it
// is run.
func (a *App) cmdGetKit() error {
	s, err := a.getKitStage()
	if err != nil || s == nil {
		return err
	}
	if a.UI.Dry() {
		// A rehearsal verified a kit that exists; it is remembered in the rehearsal's own state, nothing is
		// installed: neither the kit folder, nor the downloads, nor the real state.
		_ = a.St.SetMany([][2]string{{"mode", "kit"}, {"kit_path", s.path}, {"kit_sha", s.sha}, {"kit_format", s.format},
			{"kit_unsigned_ok", map[bool]string{true: "1", false: "0"}[s.unsigned]}, {"kit_version", s.version}, {"kit_model", s.model}})
		_ = a.St.MarkDone("get-kit")
		return nil
	}
	if err := a.commitKit(s); err != nil {
		return a.UI.Fail(ui.Sanitize(err.Error())+".", "nothing was changed: the previous kit stays in use.")
	}
	return nil
}

// getKitStage is the staging half of get-kit (see getKitStageRaw); a failure removes what it unpacked.
func (a *App) getKitStage() (*stagedKit, error) {
	s, err := a.getKitStageRaw()
	if err != nil && !a.UI.Dry() {
		_ = os.RemoveAll(a.kitNewDir())
	}
	return s, err
}

// getKitStageRaw downloads (to a staging folder) and verifies the kit and unpacks it to kit.new. It changes
// nothing that is recorded: not the downloads folder, not the kit folder, not the state. commitKit installs
// the result, discardKit throws it away; until then a failure, a refusal or an interruption leaves the previous
// kit, its downloaded files and its state exactly as they were. nil, nil is a rehearsal.
func (a *App) getKitStageRaw() (*stagedKit, error) {
	if err := a.stepNeed("get-kit"); err != nil {
		return nil, err
	}
	a.stepBanner("get-kit")
	dry := a.UI.Dry()
	kitPath := a.Opt.Kit
	downloadModel := "" // the model whose kit is downloaded ("" for a kit given by --kit)
	var dl, finalTar, staging, finalDL string
	kept := false
	keepStaging := &kept // set once the staged files are handed to the caller
	op := a.op()
	defer op.end()
	verifyItem := 0 // index of "Verify and unpack" in the operation
	if kitPath != "" {
		op.plan("Verify and unpack the kit")
	}
	if kitPath == "" {
		model, err := a.needModel()
		if err != nil {
			return nil, err
		}
		downloadModel = model
		ver := strings.TrimPrefix(a.Opt.Version, "v")
		if ver == "" {
			// The firmware has its own version, unrelated to this program's: the newest firmware release that has
			// a kit for the camera's model is used.
			if dry {
				ver = "X.Y.Z"
				a.UI.Say("  (rehearsal: using the placeholder version X.Y.Z)")
			} else {
				rels, rerr := a.releaseList()
				if rerr != nil || len(rels) == 0 {
					return nil, a.UI.Fail("cannot tell which firmware release to download for "+models.Label(model)+".",
						fmt.Sprintf("pass --version X.Y.Z (see https://github.com/%s/releases), or --kit FILE for a kit you already have, or run %s build.", kitRepo, Prog))
				}
				ver = rels[0].Version
				a.UI.Say("  Newest firmware release for " + models.Label(model) + ": " + ver + ".")
			}
		}
		if !validKitVersion(ver) {
			return nil, a.UI.Fail(fmt.Sprintf("'%s' is not a valid version.", ver), fmt.Sprintf("use a number such as 0.1.0 (see https://github.com/%s/releases).", kitRepo))
		}
		tag := kit.ReleaseTag(ver)
		kitFile := kit.KitFile(model, ver)
		base := fmt.Sprintf("https://github.com/%s/releases/download/%s", kitRepo, tag)
		if o := testhook.Env("AKAM_KIT_URL_BASE"); o != "" { // test hook; still HTTPS only
			base = strings.TrimRight(o, "/") + "/" + tag
		}
		dlFinal := filepath.Join(a.Cams.Base, "downloads", ver)
		dl = filepath.Join(dlFinal, ".staging")
		kitPath = filepath.Join(dl, kitFile)
		finalTar = filepath.Join(dlFinal, kitFile)
		a.UI.Say(fmt.Sprintf("  Downloading the %s kit %s from %s (HTTPS only; nothing from it is run before it is verified).", model, ver, base))
		if !dry {
			_ = os.RemoveAll(dl) // nothing left over from an earlier attempt
			if err := os.MkdirAll(dl, 0o700); err != nil {
				return nil, a.UI.Fail("cannot create "+dl+": "+err.Error(), "")
			}
			staging, finalDL = dl, dlFinal
			defer func() {
				if keepStaging != nil && !*keepStaging {
					_ = os.RemoveAll(staging)
				}
			}()
		}
		op.plan("Download the kit", "Download the checksums (SHA256SUMS)", "Download the signature", "Verify and unpack the kit")
		verifyItem = 3
		for i, f := range []string{kitFile, "SHA256SUMS"} {
			op.start(i)
			op.status("HTTPS GET " + base + "/" + f + "   (nothing from it is run)")
			lim := int64(kit.MaxKitDownload)
			if f == "SHA256SUMS" {
				lim = kit.MaxSmallDownload
			}
			if err := a.download(base+"/"+f, filepath.Join(dl, f), lim); err != nil {
				var he *kit.HTTPError
				if errors.As(err, &he) && (he.Code == 404 || he.Code == 410) {
					return nil, a.UI.Fail(fmt.Sprintf("firmware release %s was not found, or it has no kit for %s (%s).", ver, models.Label(model), f),
						fmt.Sprintf("see https://github.com/%s/releases for the versions that exist, or use a kit you already have: %s get-kit --kit FILE (or build from source: %s build).", kitRepo, Prog, Prog))
				}
				return nil, a.UI.Fail(fmt.Sprintf("download of %s failed (%v).", f, err),
					fmt.Sprintf("check your network and retry, or use a kit you already have: %s get-kit --kit FILE.", Prog))
			}
		}
		op.start(2)
		sig := filepath.Join(dl, "SHA256SUMS.sig")
		if err := a.download(base+"/SHA256SUMS.sig", sig, kit.MaxSmallDownload); err != nil {
			var he *kit.HTTPError
			if errors.As(err, &he) && (he.Code == 404 || he.Code == 410) {
				a.UI.Warn("no signature file in this release: the kit would be checked by checksum only.")
			} else {
				return nil, a.UI.Fail(fmt.Sprintf("download of SHA256SUMS.sig failed (%v).", err), "check your network and retry.")
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
		return nil, a.UI.Fail("kit not found: "+kitPath, "check the path given to --kit.")
	}

	ext := a.kitNewDir()
	if dry {
		ext = "" // a rehearsal unpacks nothing: the verifier works in a private temporary folder
	}
	if statErr != nil { // rehearsal, nothing downloaded
		a.UI.Say(fmt.Sprintf("  WOULD verify %s (SHA256SUMS, signature, safe archive layout, MANIFEST) and extract it to %s   (kit not downloaded in a rehearsal)", kitPath, a.kitRealDir()))
		_ = a.St.Set("mode", "kit")
		_ = a.St.Set("kit_path", kitPath)
		_ = a.St.Set("kit_sha", "dry-run")
		_ = a.St.Set("kit_format", "1")
		_ = a.St.MarkDone("get-kit")
		return nil, nil
	}

	op.start(verifyItem)
	op.status("checking SHA256SUMS, the signature and the archive layout (read only; nothing from the kit is run)")
	unsigned := a.Opt.AllowUnsigned
	if ext != "" {
		_ = os.RemoveAll(ext)
	}
	shown := kitPath
	if staging != "" {
		shown = finalTar // the staging folder is an internal detail
	}
	a.UI.Say(fmt.Sprintf("  Verifying %s (read-only; nothing from the kit is run).", shown))
	opts := kit.Options{
		AllowedSigners: a.allowedSigners(),
		AllowUnsigned:  a.Opt.AllowUnsigned,
		Expect:         a.Opt.ExpectSHA256,
		ExtractTo:      ext,
		Say: func(s string) {
			if ext != "" { // the unpack folders are internal details: name the places they end up in
				s = strings.ReplaceAll(s, ext, a.kitRealDir())
			}
			a.UI.Say("  " + s)
		},
		Warn: func(s string) { fmt.Fprintln(a.Streams.Err, "WARNING: "+s) },
	}
	var man map[string]string
	var err error
	if st.IsDir() {
		// Directory mode cannot check a signature at all: it is only ever accepted as an unsigned kit.
		if !unsigned {
			return nil, a.UI.Fail("the kit is a folder, which cannot be authenticated (no signature to check).",
				"pass the signed .tar.xz, or --allow-unsigned if you trust where this folder came from.")
		}
		man, err = a.verifyKitDir(opts, kitPath, ext)
	} else {
		man, err = a.verifyKitFile(opts, kitPath, &unsigned)
	}
	if err != nil {
		return nil, err
	}
	fmtv := man["kit_format"]
	if !containsStr(kit.SupportedKitFormats, fmtv) {
		if fmtv == "" {
			fmtv = "?"
		}
		return nil, a.UI.Fail(fmt.Sprintf("this kit has format '%s', which this program does not support.", fmtv),
			fmt.Sprintf("use the release that ships with this kit, or build from source (%s build).", Prog))
	}
	var sha string
	if st.IsDir() {
		sha = fileSHA(filepath.Join(kitPath, "MANIFEST.txt"))
	} else {
		sha = fileSHA(kitPath)
	}
	if downloadModel != "" && man["model"] != downloadModel {
		return nil, a.UI.Fail(fmt.Sprintf("the downloaded kit is for the camera model %s, not %s.", models.Label(man["model"]), models.Label(downloadModel)),
			"do not use it. Download it again or get it from the official release page.")
	}
	if p := a.kitModelProblem(man["model"]); p != "" {
		return nil, a.UI.Fail(p, "use the kit of this camera's model: "+Prog+" releases lists the firmware that has one, or pass --kit FILE of the right kit.")
	}
	sk := &stagedKit{newDir: ext, path: kitPath, sha: sha, format: fmtv, unsigned: unsigned, version: man["akam_version"], model: man["model"], staging: staging, finalDL: finalDL}
	if staging != "" { // a download: the staged files take the place of the recorded ones only at commit
		sk.path = finalTar
		sk.stagedPath = kitPath
	}
	op.done(verifyItem, "format "+fmtv)
	a.UI.Good(fmt.Sprintf("  Kit verified (format %s, sha256 %s...).", fmtv, sha[:12]))
	if a.Opt.ExpectSHA256 == "" {
		a.UI.Say("  Tip: compare that sha256 with the one in the release notes, or pass --expect-sha256 HEX.")
	}
	kept = true // the staging folder now belongs to sk
	return sk, nil
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
	if ext != "" {
		if err := kit.CopyTree(dir, ext); err != nil {
			return nil, a.UI.Fail("cannot copy the kit folder: "+err.Error(), "")
		}
	}
	return man, nil
}

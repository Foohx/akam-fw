package cli

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
)

// The camera model (tools/release/models/<id>.txt, embedded). It is learned from facts the assistant already
// reads: the stock APP sha256 of an accepted backup (kernel line as a fallback), or the MANIFEST an Akam camera
// carries (/usr/local/akam/MANIFEST.txt). It is recorded in the camera's state as camera_model and decides which
// firmware kits are offered and accepted.

const keyCameraModel = "camera_model"

// camModel is the recorded model of the chosen camera ("" when it was never recognised).
func (a *App) camModel() string { return a.St.Get(keyCameraModel) }

// recordModel stores the model of the camera. An unknown model ("") is not stored: it never erases a known one.
func (a *App) recordModel(id string) {
	if id != "" && kit.ValidModel(id) && id != a.camModel() {
		a.setState(keyCameraModel, id)
	}
}

// detectModelFromBackup recognises the model from the stock APP of the backup folder d (sha256 of mtdblock7.bin)
// and records it. It returns the id ("" when the fingerprint is not in the registry).
func (a *App) detectModelFromBackup(d string) string {
	m, _, ok := models.ByAppSHA(fileSHA(filepath.Join(d, "mtdblock7.bin")))
	if !ok {
		return ""
	}
	a.recordModel(m.ID)
	return m.ID
}

// wantModel is the model whose firmware is wanted: --model, else the camera's recorded model, else the only
// model of the registry. ok is false when several models exist and none is known.
func (a *App) wantModel() (id string, ok bool) {
	if a.Opt.Model != "" {
		return strings.TrimSpace(a.Opt.Model), true
	}
	if m := a.camModel(); m != "" {
		return m, true
	}
	if m, only := models.Only(); only {
		return m.ID, true
	}
	return "", false
}

// needModel is wantModel with the user-facing failure.
func (a *App) needModel() (string, error) {
	id, ok := a.wantModel()
	if !ok {
		return "", a.UI.Fail("this camera's model is not known yet and several camera models are supported.",
			fmt.Sprintf("take the backup first (it recognises the model), or pass --model ID (%s).", modelIDs()))
	}
	if !kit.ValidModel(id) {
		return "", a.UI.Fail(fmt.Sprintf("'%s' is not a valid camera model id.", id), fmt.Sprintf("known models: %s.", modelIDs()))
	}
	return id, nil
}

// modelIDs lists the ids of the registry, for messages.
func modelIDs() string {
	var ids []string
	for _, m := range models.All() {
		ids = append(ids, m.ID)
	}
	return strings.Join(ids, ", ")
}

// kitModelProblem is "" when a kit made for kitModel may be used with this camera: the camera's recorded model
// (or --model) must equal it. When the camera's model is not known, nothing can be compared and the kit passes
// (the hardware check of the image step still compares the stock APP with the kit's own list).
func (a *App) kitModelProblem(kitModel string) string {
	if want := a.camModel(); want != "" && want != kitModel {
		return fmt.Sprintf("this kit is for the camera model %s, but this camera is %s. A kit only fits the model it was built for.", models.Label(kitModel), models.Label(want))
	}
	if want := strings.TrimSpace(a.Opt.Model); want != "" && want != kitModel {
		return fmt.Sprintf("this kit is for the camera model %s, but --model asks for %s.", models.Label(kitModel), models.Label(want))
	}
	return ""
}

// modelLine is the status line about the camera model ("" until it is known, so that a fresh camera prints
// exactly what the bash wizard does).
func (a *App) modelLine() string {
	if id := a.camModel(); id != "" {
		return "  Camera model: " + models.Label(id)
	}
	return ""
}

// kitFileOf is the file name of the kit of a model and version.
func kitFileOf(model, ver string) string { return kit.KitFile(model, ver) }

// downloadedKitFile is where a downloaded kit of the model is kept: downloads/<ver>/akam-kit-<model>-<ver>.tar.xz.
func (a *App) downloadedKitFile(model, ver string) string {
	return filepath.Join(a.Cams.Base, "downloads", ver, kitFileOf(model, ver))
}

// kitIsDownloaded is true when the kit of that model and version is in the downloads folder.
func (a *App) kitIsDownloaded(model, ver string) bool {
	st, err := os.Stat(a.downloadedKitFile(model, ver))
	return err == nil && st.Mode().IsRegular()
}

// wantedModel is the model id of wantModel, "" when it is not known.
func (a *App) wantedModel() string {
	id, _ := a.wantModel()
	return id
}

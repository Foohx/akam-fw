package cli

import (
	"fmt"
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

// wantModel is the model whose firmware is wanted: --model, else the camera's recorded model, else the model
// the stock APP of the recorded backup (state backup_sha) belongs to, else the only model of the registry.
// ok is false when several models exist and none is known.
func (a *App) wantModel() (id string, ok bool) {
	if a.Opt.Model != "" {
		return strings.TrimSpace(a.Opt.Model), true
	}
	if m := a.camModel(); m != "" {
		return m, true
	}
	if sha := a.St.Get("backup_sha"); sha != "" {
		if m, _, found := models.ByAppSHA(sha); found {
			return m.ID, true
		}
	}
	if m, only := models.Only(); only {
		return m.ID, true
	}
	return "", false
}

// needModel is wantModel with the user-facing failure. When the model is unknown and several exist (a guided
// first install that downloads the kit before the backup), an interactive run asks the user to choose from
// the registry (nothing is preselected); a non-interactive run fails and names --model. The model that is
// chosen, or given with --model, is recorded in the camera's state.
func (a *App) needModel() (string, error) {
	id, ok := a.wantModel()
	if !ok && !a.UI.Dry() && a.UI.Interactive() {
		id, ok = a.askModel()
	}
	if !ok {
		return "", a.UI.Fail("this camera's model is not known yet and several camera models are supported.",
			fmt.Sprintf("take the backup first (it recognises the model), or pass --model ID (%s).", modelIDs()))
	}
	if !kit.ValidModel(id) {
		return "", a.UI.Fail(fmt.Sprintf("'%s' is not a valid camera model id.", id), fmt.Sprintf("known models: %s.", modelIDs()))
	}
	if _, known := models.ByID(id); known && !a.UI.Dry() {
		a.recordModel(id)
	}
	return id, nil
}

// askModel lets the user pick the camera model from the registry; the last item (the default, nothing is
// preselected) leaves it unknown.
func (a *App) askModel() (string, bool) {
	all := models.All()
	items := make([]string, 0, len(all)+1)
	for _, m := range all {
		items = append(items, models.Label(m.ID))
	}
	items = append(items, "I do not know (cancel)")
	i, err := a.UI.Pick("Which camera model is this? (the model decides which firmware kit is used)", items)
	if err != nil || i < 0 || i >= len(all) {
		return "", false
	}
	return all[i].ID, true
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

// downloadedKitFile is where a downloaded kit of the model is kept: downloads/<ver>/akam-kit-<model>-<ver>.tar.xz.
func (a *App) downloadedKitFile(model, ver string) string {
	return filepath.Join(a.Cams.Base, "downloads", ver, kit.KitFile(model, ver))
}

// wantedModel is the model id of wantModel, "" when it is not known.
func (a *App) wantedModel() string {
	id, _ := a.wantModel()
	return id
}

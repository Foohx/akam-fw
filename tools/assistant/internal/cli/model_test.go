package cli_test

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/kit/kittest"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testfake"
)

// The camera model: recorded in the state, shown by status, decides which firmware releases are listed and which
// kit is accepted. Everything is synthetic (one HTTPS test server plays GitHub).

const lscModel = kittest.DefaultModel

func TestStatusShowsTheCameraModelOnlyOnceKnown(t *testing.T) {
	e := newEnv(t)
	r := e.run("", "status")
	expectRC(t, r, 0)
	notContains(t, r.out, "Camera model")
	e.writeState("camera_model=" + lscModel + "\n")
	r = e.run("", "status")
	expectRC(t, r, 0)
	contains(t, r.out, "Camera model: LSC Smart Indoor 1080p ("+lscModel+")")
}

func TestReleasesListsOnlyFirmwareReleasesWithAKitForTheModel(t *testing.T) {
	list := relList(
		relJSON("v1.0.0", false, false),
		relJSONFor("other-model", "v2.0.0", false, false), // newer, but no kit for this camera
		`{"tag_name":"assistant-v3.0.0","draft":false,"prerelease":false,"assets":[{"name":"akam-linux-amd64"}]}`,
		relJSON("v0.9.0", true, false), // a draft
		relJSON("v0.8.0", false, true), // a prerelease
	)
	e := newEnv(t)
	useAPI(t, e, newFwAPI(t, list))
	e.writeState("camera_model=" + lscModel + "\n")
	r := e.run("", "releases")
	expectRC(t, r, 0)
	contains(t, r.out, "Camera model: LSC Smart Indoor 1080p", "1.0.0")
	notContains(t, r.out, "2.0.0")
	notContains(t, r.out, "3.0.0")
	notContains(t, r.out, "0.9.0")
	notContains(t, r.out, "0.8.0")
	r = e.run("", "releases", "--include-prereleases")
	contains(t, r.out, "1.0.0", "0.8.0  ")
	// another model of the same releases sees the release it has a kit for, and none of the others
	e.writeState("camera_model=other-model\n")
	r = e.run("", "releases", "--model", "other-model")
	notContains(t, r.out, "1.0.0")
	contains(t, r.out, "2.0.0", "latest")
}

func TestKitOfAnotherModelIsRefused(t *testing.T) {
	other := kittest.Build(t, t.TempDir(), kittest.Spec{Model: "other-model"})
	e := getKitEnv(t, nil, &other)
	e.writeState("step_doctor_done=2026-11-11T00:00:00Z\ncamera_model=" + lscModel + "\n")
	r := e.run("", "get-kit", "--kit", other.Tarball)
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "this kit is for the camera model other-model, but this camera is LSC Smart Indoor 1080p ("+lscModel+").")
	if e.readState("kit_model") != "" {
		t.Error("a refused kit must not be recorded")
	}
	// --model that contradicts the recorded model is refused too
	mine := kittest.Build(t, t.TempDir(), kittest.Spec{})
	e = getKitEnv(t, nil, &mine)
	e.writeState("step_doctor_done=2026-11-11T00:00:00Z\ncamera_model=" + lscModel + "\n")
	r = e.run("", "get-kit", "--kit", mine.Tarball, "--model", "other-model")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "this kit is for the camera model LSC Smart Indoor 1080p ("+lscModel+"), but --model asks for other-model.")
}

func TestKitIsAcceptedWhenTheModelMatchesOrIsNotKnownYet(t *testing.T) {
	mine := kittest.Build(t, t.TempDir(), kittest.Spec{})
	e := getKitEnv(t, nil, &mine)
	r := e.run("", "get-kit", "--kit", mine.Tarball)
	expectRC(t, r, 0)
	if got := e.readState("kit_model"); got != lscModel {
		t.Errorf("kit_model = %q", got)
	}
	// camera of the same model
	e = getKitEnv(t, nil, &mine)
	e.writeState("step_doctor_done=2026-11-11T00:00:00Z\ncamera_model=" + lscModel + "\n")
	expectRC(t, e.run("", "get-kit", "--kit", mine.Tarball), 0)
}

func TestGetKitWithoutVersionTakesTheNewestReleaseOfTheModel(t *testing.T) {
	a := newFwAPI(t, relList(relJSON("v1.0.0", false, false), relJSONFor("other-model", "v2.0.0", false, false)))
	e := newEnv(t)
	useAPI(t, e, a)
	e.writeState("step_doctor_done=2026-11-11T00:00:00Z\n")
	r := e.run("", "get-kit")
	expectRC(t, r, 0)
	contains(t, r.out, "Newest firmware release for LSC Smart Indoor 1080p ("+lscModel+"): 1.0.0.")
	if got := strings.Join(a.hits, " "); !strings.HasPrefix(got, "/firmware-v1.0.0/akam-kit-"+lscModel+"-1.0.0.tar.xz") {
		t.Errorf("downloads: %s", got)
	}
	if _, err := os.Stat(e.state + "/downloads/1.0.0/akam-kit-" + lscModel + "-1.0.0.tar.xz"); err != nil {
		t.Errorf("the downloaded kit is kept under its model name: %v", err)
	}
	if e.readState("kit_model") != lscModel {
		t.Errorf("kit_model = %q", e.readState("kit_model"))
	}
}

func TestUnknownModelWithSeveralModelsAsksForOne(t *testing.T) {
	// a bad --model id is refused before anything is asked of the network
	e := newEnv(t)
	e.writeState("step_doctor_done=2026-11-11T00:00:00Z\n")
	r := e.run("", "get-kit", "--version", "1.0.0", "--model", "Bad_Model")
	expectRC(t, r, 1)
	contains(t, r.out+r.err, "'Bad_Model' is not a valid camera model id.")
}

// writeStockBackup writes the folder of a stock backup (the fake camera's content) and returns it with the sha256
// of its APP partition.
func writeStockBackup(t *testing.T) (dir, appSHA string) {
	t.Helper()
	dir = t.TempDir()
	var sums strings.Builder
	for i := 0; i < 8; i++ {
		c := testfake.StockMtdContent(i)
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("mtdblock%d.bin", i)), c, 0o600); err != nil {
			t.Fatal(err)
		}
		fmt.Fprintf(&sums, "%s  mtdblock%d.bin\n", shaOf(c), i)
		if i == 7 {
			appSHA = shaOf(c)
		}
	}
	if err := os.WriteFile(filepath.Join(dir, "SHA256SUMS"), []byte(sums.String()), 0o600); err != nil {
		t.Fatal(err)
	}
	return dir, appSHA
}

func TestBackupRecognisesTheModelFromTheStockAPP(t *testing.T) {
	dir, appSHA := writeStockBackup(t)
	// the fixture's APP is not in the registry: nothing is recorded, nothing breaks
	w := newStockWorld(t, testfake.StockOptions{})
	w.writeState("")
	r := w.run("y\ny\n", "backup", "--backup-dir", dir, "--backup-copy", filepath.Join(t.TempDir(), "second"))
	expectRC(t, r, 0)
	contains(t, r.out, "Camera model: not recognised")
	if w.readState("camera_model") != "" {
		t.Error("an unknown stock APP must not record a model")
	}
	// once the registry knows that APP, the model is recorded and shown by status
	remove := models.AddForTests(models.Model{ID: "fixture-cam-model", Name: "Fixture camera",
		Records: []models.Record{{Status: "validated", Device: "fixture", AppSHA: appSHA}}})
	defer remove()
	w = newStockWorld(t, testfake.StockOptions{})
	w.writeState("kit_model=" + lscModel + "\n")
	r = w.run("y\ny\n", "backup", "--backup-dir", dir, "--backup-copy", filepath.Join(t.TempDir(), "second2"))
	expectRC(t, r, 0)
	contains(t, r.out, "Camera model recognised from the stock firmware: Fixture camera (fixture-cam-model).",
		"the kit on record is for LSC Smart Indoor 1080p")
	if got := w.readState("camera_model"); got != "fixture-cam-model" {
		t.Errorf("camera_model = %q", got)
	}
	r = w.run("", "status")
	contains(t, r.out, "Camera model: Fixture camera (fixture-cam-model)")
}

// With several models in the registry and none known, a non-interactive run names --model; the stock APP of a
// recorded backup (backup_sha) is enough to know the model; --model is recorded in the state.
func TestSeveralModelsKnownFromBackupShaOrAsked(t *testing.T) {
	sha := strings.Repeat("ab", 32)
	remove := models.AddForTests(models.Model{ID: "fixture-cam-model", Name: "Fixture camera",
		Records: []models.Record{{Status: "validated", Device: "fixture", AppSHA: sha}}})
	defer remove()
	e := newEnv(t)
	useAPI(t, e, newFwAPI(t, relList(relJSON("v1.0.0", false, false))))
	r := e.run("", "releases")
	expectRC(t, r, 1)
	contains(t, r.out, "--model")
	// the recorded backup's stock APP names the model
	e.writeState("backup_sha=" + sha + "\n")
	r = e.run("", "releases")
	contains(t, r.out, "Camera model: Fixture camera")
	// --model is recorded in the camera's state
	e = newEnv(t)
	useAPI(t, e, newFwAPI(t, relList(relJSON("v1.0.0", false, false))))
	e.run("", "releases", "--model", lscModel)
	if got := e.readState("camera_model"); got != lscModel {
		t.Errorf("camera_model = %q, want %q", got, lscModel)
	}
}

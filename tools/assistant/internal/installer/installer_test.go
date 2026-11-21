package installer_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/Foohx/akam-fw/tools/assistant/internal/installer"
	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
)

func TestForKind(t *testing.T) {
	for kind, want := range map[string]installer.Flow{models.KindNativeApp: installer.NativeApp, models.KindCardlessAppfs: installer.Appfs} {
		if got, err := installer.ForKind(kind); err != nil || got != want {
			t.Errorf("%s: %q %v", kind, got, err)
		}
	}
	// fail closed: another kind, an unknown one, an empty one, a malformed one have no installer
	for _, kind := range []string{models.KindDecloudOverlay, "", "native_app", "Native-App", "native-app ", "something-new", "cardless-appfs\n"} {
		if got, err := installer.ForKind(kind); err == nil {
			t.Errorf("%q got the installer %q", kind, got)
		}
	}
	if appfs.Kind != models.KindCardlessAppfs {
		t.Fatal("the appfs installer and the registry disagree on the kind name")
	}
}

// Every model of the registry is served by the installer of its kind, and the two Flows predicates of models agree
// with the dispatch: a camera can never be offered both installers.
func TestEveryRegistryModelHasTheInstallerOfItsKind(t *testing.T) {
	seen := map[installer.Flow]bool{}
	for _, m := range models.All() {
		f, err := installer.For(m)
		if err != nil {
			if m.NativeFlows() || m.AppfsFlows() {
				t.Errorf("%s: a flow predicate is true but there is no installer: %v", m.ID, err)
			}
			continue
		}
		seen[f] = true
		if m.NativeFlows() != (f == installer.NativeApp) || m.AppfsFlows() != (f == installer.Appfs) {
			t.Errorf("%s: predicates and dispatch disagree (%s)", m.ID, f)
		}
		if m.NativeFlows() && m.AppfsFlows() {
			t.Errorf("%s: both installers", m.ID)
		}
	}
	if !seen[installer.NativeApp] || !seen[installer.Appfs] {
		t.Errorf("the registry should hold a native-app and a cardless-appfs model: %v", seen)
	}
}

// The dispatch is on the kind: no model id appears in the installer packages' code.
func TestInstallersDoNotKnowModelIDs(t *testing.T) {
	for _, m := range models.All() {
		err := filepath.WalkDir(".", func(p string, d os.DirEntry, err error) error {
			if err != nil || d.IsDir() || !strings.HasSuffix(p, ".go") || strings.HasSuffix(p, "_test.go") {
				return err
			}
			b, _ := os.ReadFile(p)
			if strings.Contains(string(b), m.ID) {
				t.Errorf("%s mentions the model id %s: dispatch on the firmware kind instead", p, m.ID)
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
	}
}

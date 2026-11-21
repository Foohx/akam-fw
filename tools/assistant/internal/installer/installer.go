// Package installer maps the firmware kind of a model (the `kind` header of its registry file, see internal/models)
// to the installer that serves it. The installer is chosen by the KIND, never by the model id: a new camera of an
// existing kind needs no code, a new kind needs a new installer and is refused until one is registered here.
//
//	native-app      nativeapp  the APP partition is replaced by an Akam image (the validated 1080p): image, flash, sdcard
//	cardless-appfs  appfs      the appfs partition is replaced, Akam runs from flash with no SD card (the 2K)
//
// Any other kind, a malformed one included, has no installer: ForKind fails closed.
package installer

import (
	"fmt"

	"github.com/Foohx/akam-fw/tools/assistant/internal/installer/appfs"
	"github.com/Foohx/akam-fw/tools/assistant/internal/models"
)

// Flow names an installer.
type Flow string

const (
	NativeApp Flow = "nativeapp"
	Appfs     Flow = "appfs"
)

// ForKind is the installer of a firmware kind (models.Model.EffectiveKind).
func ForKind(kind string) (Flow, error) {
	switch kind {
	case models.KindNativeApp:
		return NativeApp, nil
	case appfs.Kind: // == models.KindCardlessAppfs, pinned by a test
		return Appfs, nil
	}
	return "", fmt.Errorf("no installer for the firmware kind %q", kind)
}

// For is the installer of a model.
func For(m models.Model) (Flow, error) { return ForKind(m.EffectiveKind()) }

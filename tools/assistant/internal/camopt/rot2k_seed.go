package camopt

// The install-time side of the 2K options: the answers of the setup questions of the card-less install become the camera's own settings file
// (/config/akam/settings, "key=value" lines, read by the firmware at start). The same catalogue and the same validators as the options screen
// (Catalog2K, Validate2K), so a value that is accepted here is one the camera accepts later; the camera still validates the whole file when it
// loads it (the whole file or nothing).

import (
	"fmt"
	"sort"
	"strings"
)

// SetupKeys2K are the options asked at install time, in the order they are asked: the speaker and the picture (the other options have their own
// screen and no install-time question). Only options the camera keeps across a restart (Persisted == PersistCamera) can be asked: a RAM option
// would look set and be gone at the first reboot.
var SetupKeys2K = []string{"audio.out_enable", "audio.out_volume", "audio.talk_enable", "isp.saturation", "isp.denoise"}

// SettingsFileMax is the size limit of the camera's settings file (STREG_FILE_MAX in the firmware).
const SettingsFileMax = 4096

// SettingsText2K validates the answers (registry key to typed value) and returns the settings file text: canonical values, one "key=value" line
// each, in catalogue order. A key that is unknown, read only or not kept by the camera (PersistRAM) is refused. An empty map gives "".
func SettingsText2K(answers map[string]string) (string, error) {
	var b strings.Builder
	keys := make([]string, 0, len(answers))
	for k := range answers {
		keys = append(keys, k)
	}
	order := map[string]int{}
	for i, o := range Catalog2K() {
		order[o.Key] = i
	}
	for _, k := range keys {
		o, ok := Find2K(k)
		if !ok || o.ReadOnly {
			return "", fmt.Errorf("%s is not a setting of the 2K that can be written to its settings file", k)
		}
		if o.Persisted != PersistCamera {
			return "", fmt.Errorf("%s is not kept by the camera across a restart, so it cannot be written to its settings file", k)
		}
	}
	sort.Slice(keys, func(i, j int) bool { return order[keys[i]] < order[keys[j]] })
	for _, k := range keys {
		v, err := Validate2K(k, answers[k])
		if err != nil {
			return "", err
		}
		if strings.ContainsAny(v, "\r\n") {
			return "", fmt.Errorf("%s: a value is one line", k)
		}
		fmt.Fprintf(&b, "%s=%s\n", k, v)
	}
	if b.Len() > SettingsFileMax {
		return "", fmt.Errorf("the settings file would be %d bytes, the camera reads at most %d", b.Len(), SettingsFileMax)
	}
	return b.String(), nil
}

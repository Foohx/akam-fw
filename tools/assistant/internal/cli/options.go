package cli

import (
	"fmt"
	"github.com/Foohx/akam-fw/tools/assistant/internal/testhook"
	"os"
	"regexp"
	"strconv"
)

// Prog is how the commands name themselves in messages. It is "./akam" so that the texts equal
// the bash wizard's (golden tests compare them); both programs share the same help text.
const Prog = "./akam"

// Options are the command-line flags (same names as ./akam help, plus --trust-new-host-key).
type Options struct {
	Kit, Version, ExpectSHA256, BackupDir, BackupCopy, IP, Identity, Image string
	Out, Comp, Device, Pubkey, SSID, Profile, Camera, StateDir             string
	TrustNewHostKey                                                        string
	Builder                                                                string // --builder native|tools (the image step)
	PasswordFile                                                           string
	AllowUnsigned, AllowUnknownComp, ManualTelnet, Plain, DryRun, NoColor  bool
	Stock, IncludePre                                                      bool // --stock (update: back to the stock firmware), --include-prereleases (releases, update)
	UntestedOK                                                             bool // --i-understand-untested (the native image builder)
	// TelnetNegotiate (hidden): answer telnet option negotiation (default: stay silent, as validated on hardware).
	TelnetNegotiate bool
	Args            []string // command and its words
}

var negNum = regexp.MustCompile(`^-[0-9]`)

// ParseArgs mirrors the option loop of ./akam. A usage error is returned as *UsageError text.
func ParseArgs(argv []string) (*Options, error) {
	o := &Options{}
	val := func(i *int, name string) (string, error) {
		if *i+1 >= len(argv) {
			return "", usageErr(fmt.Sprintf("akam: %s needs a value (see %s help)", name, Prog))
		}
		*i++
		return argv[*i], nil
	}
	for i := 0; i < len(argv); i++ {
		a := argv[i]
		var err error
		set := func(dst *string) {
			var v string
			if v, err = val(&i, a); err == nil {
				*dst = v
			}
		}
		switch a {
		case "--dry-run":
			o.DryRun = true
		case "--no-color":
			o.NoColor = true
			os.Setenv("NO_COLOR", "1")
		case "--plain":
			o.Plain = true
		case "--allow-unsigned":
			o.AllowUnsigned = true
		case "--allow-unknown-stock-comp":
			o.AllowUnknownComp = true
		case "--i-understand-untested":
			o.UntestedOK = true
		case "--builder":
			set(&o.Builder)
		case "--telnet-negotiate": // hidden: not in the help text
			o.TelnetNegotiate = true
		case "--stock":
			o.Stock = true
		case "--include-prereleases":
			o.IncludePre = true
		case "--manual-telnet":
			o.ManualTelnet = true
		case "--kit":
			set(&o.Kit)
		case "--version":
			set(&o.Version)
		case "--expect-sha256":
			set(&o.ExpectSHA256)
		case "--backup-dir":
			set(&o.BackupDir)
		case "--backup-copy":
			set(&o.BackupCopy)
		case "--ip":
			set(&o.IP)
		case "--identity":
			set(&o.Identity)
		case "--image":
			set(&o.Image)
		case "--out":
			set(&o.Out)
		case "--comp":
			set(&o.Comp)
		case "--device":
			set(&o.Device)
		case "--pubkey":
			set(&o.Pubkey)
		case "--ssid":
			set(&o.SSID)
		case "--profile":
			set(&o.Profile)
		case "--state-dir":
			set(&o.StateDir)
		case "--trust-new-host-key":
			set(&o.TrustNewHostKey)
		case "--password-file":
			set(&o.PasswordFile)
		case "--camera":
			set(&o.Camera)
			if err == nil && o.Camera == "" {
				err = usageErr(fmt.Sprintf("akam: --camera needs a short name (see %s help)", Prog))
			}
		case "-h", "--help":
			o.Args = append(o.Args, "help")
		default:
			switch {
			case negNum.MatchString(a):
				// a negative number after the command is a value (./akam picture contrast -20)
				if len(o.Args) > 0 {
					o.Args = append(o.Args, a)
				} else {
					return nil, usageErr(fmt.Sprintf("akam: unknown option %s (see %s help)", a, Prog))
				}
			case len(a) > 0 && a[0] == '-':
				return nil, usageErr(fmt.Sprintf("akam: unknown option %s (see %s help)", a, Prog))
			default:
				o.Args = append(o.Args, a)
			}
		}
		if err != nil {
			return nil, err
		}
	}
	if o.Stock && (len(o.Args) == 0 || o.Args[0] != "help") {
		if len(o.Args) > 0 && o.Args[0] != "update" {
			return nil, usageErr("akam: --stock is for the update command (see " + Prog + " help)")
		}
		if o.Kit != "" || o.Version != "" || o.Image != "" {
			return nil, usageErr("akam: --stock cannot be combined with --kit, --version or --image: choose one firmware (see " + Prog + " help)")
		}
	}
	switch o.Builder {
	case "", "native", "tools":
	default:
		if len(o.Args) == 0 || o.Args[0] != "help" {
			return nil, usageErr("akam: --builder must be native or tools")
		}
	}
	switch o.Comp {
	case "", "xz", "gzip":
	default:
		if len(o.Args) == 0 || o.Args[0] != "help" {
			return nil, usageErr("akam: --comp must be xz or gzip")
		}
	}
	return o, nil
}

// Env holds the AKAM_* test and port hooks (cam_port and friends).
type Env struct {
	PortSSH, PortRTSP, PortCTL, PortTelnet int
	// PortSSH2 is the RAM Dropbear of the SSH reflash (2222); PortFTP, PortTelnet23 and PortHTTP are the
	// extra ports verify checks (PortHTTP 0: 80, or the ONVIF port when ONVIF is on).
	PortSSH2, PortFTP, PortTelnet23, PortHTTP int
	// WaitSecs is how long verify waits for the camera (AKAM_WAIT_SECS, default 240).
	WaitSecs int
}

func envPort(name string, def int) int {
	v := os.Getenv(name)
	if name == "AKAM_PORT_SSH2" {
		v = testhook.Env(name) // test hook: a release build always uses 2222
	}
	if v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 && n < 65536 {
			return n
		}
	}
	return def
}

// LoadEnv reads the port hooks.
func LoadEnv() Env {
	return Env{
		PortSSH: envPort("AKAM_PORT_SSH", 22), PortRTSP: envPort("AKAM_PORT_RTSP", 554),
		PortCTL: envPort("AKAM_PORT_CTL", 8091), PortTelnet: envPort("AKAM_PORT_TELNET", 24),
		PortSSH2: envPort("AKAM_PORT_SSH2", 2222), PortFTP: envPort("AKAM_PORT_FTP", 21),
		PortTelnet23: envPort("AKAM_PORT_TELNET23", 23), PortHTTP: envPort("AKAM_PORT_HTTP", 0),
		WaitSecs: envPort("AKAM_WAIT_SECS", 240),
	}
}

package cli

import (
	"context"
	"fmt"
	"net"
	"runtime"
	"strings"
	"time"

	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// DiscoverHooks replace the network in tests; nil fields use the real network.
type DiscoverHooks struct {
	Dial       discover.DialFunc
	WSDTarget  string // multicast address, or a loopback one in tests
	MDNSTarget string
	Interfaces func() ([]net.Interface, error)
	Addrs      func(*net.Interface) ([]net.Addr, error)
	Lookup     func(ctx context.Context, host string) ([]string, error)
}

const probeTimeout = 1500 * time.Millisecond

func (a *App) ports() discover.Ports {
	return discover.Ports{Telnet: a.Env.PortTelnet, SSH: a.Env.PortSSH, RTSP: a.Env.PortRTSP, CTL: a.Env.PortCTL}
}

func (a *App) probe(ip string, ports ...int) []int {
	return discover.OpenPorts(a.Ctx, ip, ports, probeTimeout, a.Hooks.Discover.Dial)
}

func joinPorts(p []int) string {
	var b strings.Builder
	for _, x := range p {
		fmt.Fprintf(&b, "%d ", x) // net_probe leaves a trailing space; the table keeps it
	}
	return b.String()
}

func isIPv4(s string) bool { ip := net.ParseIP(s); return ip != nil && ip.To4() != nil }

// resolveName turns a typed host name into an address: the system resolver, then mDNS
// (<name>.local). Both only run because the user typed a name.
func (a *App) resolveName(name string) (string, bool) {
	h := a.Hooks.Discover
	lookup := h.Lookup
	if lookup == nil {
		lookup = func(ctx context.Context, host string) ([]string, error) {
			return net.DefaultResolver.LookupHost(ctx, host)
		}
	}
	ctx, cancel := context.WithTimeout(a.Ctx, 3*time.Second)
	defer cancel()
	if addrs, err := lookup(ctx, name); err == nil {
		for _, ad := range addrs {
			if isIPv4(ad) {
				return ad, true
			}
		}
	}
	target := h.MDNSTarget
	if target == "" {
		target = discover.MDNSTarget
	}
	mdns := name
	if !strings.HasSuffix(mdns, ".local") {
		mdns += ".local"
	}
	if ip, err := discover.MDNSLookup(a.Ctx, mdns, target, 2*time.Second); err == nil {
		return ip.String(), true
	}
	return "", false
}

// wsdItem is the menu line of one WS-Discovery answer. The name and the address come from the
// network: no control or escape character may reach the terminal.
func wsdItem(f discover.Found) string {
	d := ui.Sanitize(f.IP)
	if f.Name != "" {
		d += "  " + ui.Sanitize(f.Name)
	}
	if len(f.XAddrs) > 0 {
		d += "  " + ui.Sanitize(f.XAddrs[0])
	}
	return d
}

// askIP is cam_ask_ip, extended: a typed name is resolved, and when nothing is typed the user is
// offered the opt-in searches (WS-Discovery, then a scan of the local network).
func (a *App) askIP() (string, error) {
	def := a.Opt.IP
	if def == "" {
		def = a.St.Get("camera_ip")
	}
	ip := a.Opt.IP
	if ip == "" {
		a.UI.Say("  Find the address in your router's client list (the maker is shown as 'AltoBeam').")
		label := "  Camera IP address:"
		if def != "" {
			label = fmt.Sprintf("  Camera IP address [%s]:", def)
		}
		v, err := a.UI.Prompt(label, def)
		if err != nil {
			return "", a.UI.Fail("no input.", "pass --ip ADDRESS")
		}
		ip = v
		if ip == "" {
			if found, ok := a.searchNetwork(); ok {
				ip = found
			}
		}
	}
	if ip != "" && validHost(ip) && !isIPv4(ip) {
		if r, ok := a.resolveName(ip); ok {
			a.UI.Say(fmt.Sprintf("  '%s' is at %s.", ip, r))
			ip = r
		}
	}
	if !validHost(ip) {
		return "", a.UI.Fail(fmt.Sprintf("'%s' is not a valid address.", ip), "type the IPv4 address, for example 192.0.2.50")
	}
	return ip, nil
}

// searchNetwork offers the opt-in searches. Nothing is sent before a yes.
func (a *App) searchNetwork() (string, bool) {
	if !a.UI.Interactive() {
		return "", false
	}
	h := a.Hooks.Discover
	a.UI.Say("  No address typed. The camera can also be searched for on your network; each search is optional.")
	ok, _ := a.UI.AskYN("  Search with ONVIF discovery (one small UDP packet to the multicast address 239.255.255.250:3702; Akam cameras with ONVIF on answer)?")
	if ok {
		target := h.WSDTarget
		if target == "" {
			target = discover.WSDTarget
		}
		a.Log.Line("CMD ws-discovery probe -> %s", target)
		var found []discover.Found
		var err error
		if h.WSDTarget == "" {
			// real network: once per eligible interface, not through the OS default one
			found, err = a.wsdEachInterface(target)
		} else {
			found, err = discover.WSDiscover(a.Ctx, target, 2*time.Second)
		}
		if err != nil {
			a.UI.Warn("the search failed: " + err.Error())
		}
		if len(found) == 0 {
			a.UI.Say("  No camera answered the discovery.")
		} else {
			items := make([]string, 0, len(found)+1)
			for _, f := range found {
				items = append(items, wsdItem(f))
			}
			items = append(items, "None of these")
			i, err := a.UI.Pick("Which one is your camera?", items)
			if err == nil && i < len(found) {
				return found[i].IP, true
			}
		}
	}
	subs, err := discover.Subnets(h.Interfaces, h.Addrs)
	if err != nil || len(subs) == 0 {
		return "", false
	}
	for _, sn := range subs {
		a.UI.Say(fmt.Sprintf("  Local network %s: %d addresses, the ports %d, %d, %d and %d only (at most 64 connections at a time, 2 s each).",
			sn.Describe(), len(sn.Hosts()), a.Env.PortTelnet, a.Env.PortSSH, a.Env.PortRTSP, a.Env.PortCTL))
		ok, _ := a.UI.AskYN(fmt.Sprintf("  Scan %s now (it sends connection attempts to every address in it)?", sn.CIDR))
		if !ok {
			continue
		}
		a.Log.Line("CMD scan %s ports %d %d %d %d", sn.CIDR, a.Env.PortTelnet, a.Env.PortSSH, a.Env.PortRTSP, a.Env.PortCTL)
		hosts := discover.Scan(a.Ctx, sn, a.ports(), 64, 2*time.Second, h.Dial)
		if len(hosts) == 0 {
			a.UI.Say("  Nothing answered on those ports.")
			continue
		}
		items := make([]string, 0, len(hosts)+1)
		for _, ho := range hosts {
			items = append(items, fmt.Sprintf("%s  ports %s (%s)", ho.IP, strings.TrimSpace(joinPorts(ho.Open)), ho.Kind))
		}
		items = append(items, "None of these")
		i, err := a.UI.Pick("Which one is your camera?", items)
		if err == nil && i < len(hosts) {
			return hosts[i].IP, true
		}
	}
	return "", false
}

func (a *App) stepNeed(step string) error {
	if a.stepStatus(step) == "stale" {
		a.UI.Warn(fmt.Sprintf("this step was done before, but %s. It will run again.", a.stepValidate(step)))
		_ = a.St.ClearDone(step)
	}
	if b := a.stepBlockers(step); len(b) > 0 {
		return a.UI.Fail(fmt.Sprintf("step '%s' needs these steps first: %s", step, strings.Join(b, " ")), fmt.Sprintf("run: %s step %s", Prog, b[0]))
	}
	return nil
}

func (a *App) stepBanner(step string) {
	a.UI.Banner(fmt.Sprintf("Step %d/%d - %s", stepNum(step), len(state.Steps), stepTitle(step)))
	a.UI.Say("  " + stepWhy(step))
}

func (a *App) cmdFindCamera() error {
	// Known difference with the bash wizard: a camera that already runs Akam needs no backup card,
	// so a missing prepare-backup-sd does not stop the search; it only stops it when the camera
	// turns out not to run Akam (checked after the probe).
	deferred := false
	if !a.UI.Dry() && a.stepStatus("find-camera") == "blocked" {
		if b := a.stepBlockers("find-camera"); len(b) == 1 && b[0] == "prepare-backup-sd" {
			deferred = true
		}
	}
	if !deferred {
		if err := a.stepNeed("find-camera"); err != nil {
			return err
		}
	}
	a.stepBanner("find-camera")
	p := a.ports()
	if a.UI.Dry() {
		a.UI.Say(fmt.Sprintf("  WOULD ask for the camera address, probe the ports %d (telnet), %d (ssh), %d (rtsp), %d (control),", p.Telnet, p.SSH, p.RTSP, p.CTL))
		a.UI.Say(fmt.Sprintf("  and tell a stock camera with the toolkit card (telnet %d, no ssh) from a running Akam (ssh + rtsp).", p.Telnet))
		_ = a.St.Set("camera_ip", "<camera-ip>")
		_ = a.St.Set("camera_kind", "stock")
		_ = a.St.Set("camera_telnet", "1")
		_ = a.St.MarkDone("find-camera")
		return nil
	}
	ip, err := a.askIP()
	if err != nil {
		return err
	}
	a.UI.Say(fmt.Sprintf("  Probing %s on ports %d, %d, %d and %d ...", ip, p.Telnet, p.SSH, p.RTSP, p.CTL))
	open := a.probe(ip, p.Telnet, p.SSH, p.RTSP, p.CTL)
	a.UI.Say("  Open ports: " + orNone(joinPorts(open)))
	if deferred && discover.Classify(open, p) != discover.Akam {
		return a.stepNeed("find-camera")
	}
	if len(open) == 0 {
		return a.UI.Fail(fmt.Sprintf("%s does not answer on any expected port.", ip),
			fmt.Sprintf("check the address in your router; the camera needs about 2 minutes after power-on; the backup card must be in the camera (telnet appears on port %d); the PC and the camera must be on the same network.", p.Telnet))
	}
	kind := discover.ClassifyHost(a.Ctx, ip, open, p, a.Hooks.Discover.Dial)
	tel := "0"
	switch kind {
	case discover.Stock:
		tel = "1"
		k, err := a.identifyTelnet(ip)
		if err != nil {
			return err
		}
		kind = discover.Kind(k)
	case discover.Akam:
		a.UI.Say(fmt.Sprintf("  Ports %d, %d and %d are open and telnet is closed: this is a camera running Akam.", p.SSH, p.RTSP, p.CTL))
	case discover.Partial:
		return a.UI.Fail(fmt.Sprintf("only SSH answers. A camera running Akam also opens %d and %d once it has finished booting.", p.RTSP, p.CTL), "wait a minute and run this step again.")
	default:
		return a.UI.Fail(fmt.Sprintf("port %d is closed and this is not Akam. Nothing usable answers on %s.", p.Telnet, ip), "is this the camera's address? Is the toolkit card in?")
	}
	host, fw := "", ""
	if kind == discover.Akam {
		host, fw = a.identifyAkam(ip, p.CTL)
	}
	_ = state.RecordSeen(a.St, ip, host, fw, time.Now())
	_ = a.St.Set("camera_kind", string(kind))
	_ = a.St.Set("camera_telnet", tel)
	_ = a.St.MarkDone("find-camera")
	switch kind {
	case discover.Stock:
		a.UI.Good(fmt.Sprintf("  Camera found: stock firmware with the toolkit shell on port %d (%s). The backup can start.", p.Telnet, ip))
	case discover.Akam:
		a.UI.Good(fmt.Sprintf("  Camera found: it already runs Akam (%s). Updates go over SSH; the stock backup must already exist.", ip))
	}
	return nil
}

// wsdEachInterface sends the WS-Discovery probe once per eligible interface.
func (a *App) wsdEachInterface(target string) ([]discover.Found, error) {
	h := a.Hooks.Discover
	list := h.Interfaces
	if list == nil {
		list = net.Interfaces
	}
	addrs := h.Addrs
	if addrs == nil {
		addrs = func(i *net.Interface) ([]net.Addr, error) { return i.Addrs() }
	}
	l, err := list()
	if err != nil {
		return nil, err
	}
	return discover.WSDiscoverEach(a.Ctx, runtime.GOOS, discover.Ifaces(l, addrs), target, 2*time.Second)
}

// identifyAkam reads fw.version and sys.hostname from the control port (read-only). An older image
// that answers ERR is "Akam, version unknown". Both values come from the network: sanitised here.
func (a *App) identifyAkam(ip string, port int) (host, fw string) {
	a.Log.Line("CMD identify %s:%d (GET fw.version, GET sys.hostname, QUIT)", ip, port)
	id, err := discover.Identify(a.Ctx, ip, port, a.Hooks.Discover.Dial)
	if err != nil && !id.Akam {
		a.UI.Say("  (the control port did not tell the firmware version)")
		return "", ""
	}
	host, fw = ui.Sanitize(id.Hostname), ui.Sanitize(id.Version)
	if fw == "" {
		a.UI.Say("  Akam, firmware version unknown (an older image).")
	} else if host != "" {
		a.UI.Say(fmt.Sprintf("  Firmware %s, host name %s.", fw, host))
	} else {
		a.UI.Say(fmt.Sprintf("  Firmware %s.", fw))
	}
	return host, fw
}

func orNone(s string) string {
	if s == "" {
		return "none"
	}
	return s
}

var _ = ui.ErrFailed

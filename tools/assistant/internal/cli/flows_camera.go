package cli

import (
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	gossh "golang.org/x/crypto/ssh"

	camssh "github.com/Foohx/akam-fw/tools/assistant/internal/camera/ssh"
	"github.com/Foohx/akam-fw/tools/assistant/internal/discover"
	"github.com/Foohx/akam-fw/tools/assistant/internal/state"
	"github.com/Foohx/akam-fw/tools/assistant/internal/ui"
)

// The camera chooser (spec 11, 3.3). Written once against ui.UI: the table is the list of a Pick, so
// the plain and the full-screen interface show the same lines. Network traffic only goes where the
// user asked for it: a remembered camera is checked on request, the WS-Discovery search and the scan
// of the local network each ask one yes/no (default No) that names what will be sent.

// chooser is the state of one visit to the screen.
type chooser struct {
	known []discover.Known
	found []discover.Camera
	keys  map[string]gossh.PublicKey // host key presented by an address (from the SSH handshake only)
	note  string                     // what the last search covered, shown in the title
}

// knownCameras reads the remembered cameras from the state folders. The pinned host key is the one
// recorded in Akam's own known_hosts under the camera's address.
func (a *App) knownCameras() []discover.Known {
	var out []discover.Known
	for _, n := range a.Cams.Known() {
		s := a.Cams.ReadSeen(n)
		if s.IP == "" {
			continue
		}
		k := discover.Known{Name: n, IP: s.IP, Host: s.Host, FW: s.FW, Seen: s.At}
		k.HostKey = camssh.PinnedFingerprint(a.knownHostsFile(), s.IP)
		out = append(out, k)
	}
	return out
}

func (a *App) discoverDial() camssh.DialFunc { return camssh.DialFunc(a.Hooks.Discover.Dial) }

// probeCamera looks at one address: the four ports, then for an Akam camera the control-port identity
// (read-only) and the SSH host key. Nothing it reads is trusted as text: it is sanitised before being
// shown or stored, and only the host key decides that two addresses are the same camera.
func (a *App) probeCamera(ip string) (discover.Camera, gossh.PublicKey, bool) {
	p := a.ports()
	open := a.probe(ip, p.Telnet, p.SSH, p.RTSP, p.CTL)
	kind := discover.Classify(open, p)
	if kind != discover.Akam && kind != discover.Stock && kind != discover.Partial {
		return discover.Camera{}, nil, false
	}
	cam := discover.Camera{IP: ip, Kind: kind}
	var key gossh.PublicKey
	if kind == discover.Akam || kind == discover.Partial {
		if kind == discover.Akam {
			if id, err := discover.Identify(a.Ctx, ip, p.CTL, a.Hooks.Discover.Dial); err == nil || id.Akam {
				cam.Host, cam.FW = ui.Sanitize(id.Hostname), ui.Sanitize(id.Version)
			}
		}
		if k, err := camssh.ProbeHostKey(a.Ctx, ip, p.SSH, a.discoverDial(), 3*time.Second); err == nil {
			key = k
			cam.HostKey = camssh.Fingerprint(k)
		}
	}
	return cam, key, true
}

// probeMany checks addresses in parallel (at most 8 at a time).
func (a *App) probeMany(ips []string, ch *chooser) {
	var mu sync.Mutex
	var wg sync.WaitGroup
	sem := make(chan struct{}, 8)
	for _, ip := range ips {
		wg.Add(1)
		sem <- struct{}{}
		go func() {
			defer wg.Done()
			defer func() { <-sem }()
			cam, key, ok := a.probeCamera(ip)
			if !ok {
				return
			}
			mu.Lock()
			ch.addFound(cam, key)
			mu.Unlock()
		}()
	}
	wg.Wait()
}

func (ch *chooser) addFound(cam discover.Camera, key gossh.PublicKey) {
	for _, f := range ch.found {
		if f.IP == cam.IP {
			return
		}
	}
	ch.found = append(ch.found, cam)
	if key != nil {
		if ch.keys == nil {
			ch.keys = map[string]gossh.PublicKey{}
		}
		ch.keys[cam.IP] = key
	}
}

func kindLabel(k discover.Kind) string {
	switch k {
	case discover.Akam:
		return "Akam"
	case discover.Stock:
		return "Stock + card"
	case discover.Partial:
		return "Akam (starting)"
	}
	return "-"
}

// rowLine is one line of the table (every network-supplied field passes through ui.Sanitize).
func rowLine(r discover.Row, now time.Time) string {
	name, addr, kind, fw, rem := "(not named)", "", "-", "-", "no"
	if r.Name != "" {
		name = r.Name
		rem = "yes: " + r.Name
	}
	if r.Found {
		addr = r.IP
		kind = kindLabel(r.Kind)
		if r.FW != "" {
			fw = ui.Sanitize(r.FW)
		}
		if r.Name == "" && r.Host != "" {
			name = "(" + ui.Sanitize(r.Host) + ")"
		}
		if r.OldIP != "" {
			rem += " (was " + r.OldIP + ")"
		}
	} else {
		addr = "last " + r.LastIP
		kind = "not answering"
		fw = "seen " + discover.Ago(r.LastSeen, now)
	}
	return fmt.Sprintf("%-14s %-17s %-15s %-16s %s", name, addr, kind, fw, rem)
}

const rowHeader = "Name           Address           Kind            Firmware         Remembered"

// chooseCamera is the chooser screen. It returns true when a camera was chosen (its state is then the
// current one and its address is recorded).
func (a *App) chooseCamera() (bool, error) {
	if err := a.UI.RequireTTY("choosing a camera"); err != nil {
		return false, err
	}
	a.UI.Banner("Choose the camera")
	ch := &chooser{known: a.knownCameras()}
	for {
		now := time.Now()
		rows := discover.Merge(ch.known, ch.found, now)
		a.UI.Say("  Cameras on your network" + ch.note)
		a.UI.Say("    " + rowHeader)
		items := make([]string, 0, len(rows)+6)
		for _, r := range rows {
			items = append(items, rowLine(r, now))
		}
		nrows := len(rows)
		items = append(items,
			"Check the remembered cameras now (4 ports each)",
			"Scan my network (asks first)",
			"Search with ONVIF discovery (asks first)",
			"Type an address or a name",
			"Forget a remembered camera",
			"Back")
		if nrows == 0 {
			a.UI.Say("  No camera is known yet. A stock camera only answers while the toolkit card is in it and it has had 2 minutes to start;")
			a.UI.Say("  your router's client list (maker 'AltoBeam') shows its address.")
		}
		i, err := a.UI.Pick("Which camera?", items)
		if err != nil {
			return false, nil
		}
		switch i - nrows {
		case 0:
			var ips []string
			for _, k := range ch.known {
				ips = append(ips, k.IP)
			}
			if len(ips) == 0 {
				a.UI.Say("  No remembered camera has an address yet.")
				continue
			}
			a.Log.Line("CMD probe remembered cameras %s", strings.Join(ips, " "))
			a.probeMany(ips, ch)
			ch.note = "  (remembered cameras checked just now)"
		case 1:
			a.chooserScan(ch)
		case 2:
			a.chooserWSD(ch)
		case 3:
			a.chooserType(ch)
		case 4:
			a.forgetCameraScreen(ch)
			ch.known = a.knownCameras()
		case 5:
			return false, nil
		default:
			if i < nrows {
				ok, err := a.selectRow(ch, rows[i])
				if err != nil || ok {
					return ok, err
				}
			}
		}
	}
}

func (a *App) scanPorts() []int {
	p := a.ports()
	l := []int{p.Telnet, p.SSH, p.RTSP, p.CTL}
	sort.Ints(l)
	return l
}

// chooserScan asks, per local network, one yes/no that shows the range (and the part cut from a
// large network) and the ports; nothing is sent before a yes.
func (a *App) chooserScan(ch *chooser) {
	h := a.Hooks.Discover
	subs, err := discover.Subnets(h.Interfaces, h.Addrs)
	if err != nil || len(subs) == 0 {
		a.UI.Say("  No usable local network was found (virtual, VPN and loopback interfaces are never scanned). Type an address instead.")
		return
	}
	for _, sn := range subs {
		ok, _ := a.UI.AskYN("  " + sn.Question(a.scanPorts()))
		if !ok {
			continue
		}
		a.Log.Line("CMD scan %s ports %v", sn.CIDR, a.scanPorts())
		t0 := time.Now()
		hosts := discover.Scan(a.Ctx, sn, a.ports(), 64, 2*time.Second, h.Dial)
		var ips []string
		for _, ho := range hosts {
			ips = append(ips, ho.IP)
		}
		a.probeMany(ips, ch)
		ch.note = fmt.Sprintf("  (scanned %s in %d s)", sn.CIDR, int(time.Since(t0).Seconds()))
		if len(ch.found) == 0 {
			a.UI.Say("  Nothing that looks like a camera answered on those ports.")
		}
	}
}

// chooserWSD is the ONVIF WS-Discovery search: one small UDP packet to the multicast address.
func (a *App) chooserWSD(ch *chooser) {
	h := a.Hooks.Discover
	ok, _ := a.UI.AskYN("  Search with ONVIF discovery (one small UDP packet to the multicast address 239.255.255.250:3702; Akam cameras with ONVIF on answer)?")
	if !ok {
		return
	}
	target := h.WSDTarget
	if target == "" {
		target = discover.WSDTarget
	}
	a.Log.Line("CMD ws-discovery probe -> %s", target)
	var found []discover.Found
	var err error
	if h.WSDTarget == "" {
		found, err = a.wsdEachInterface(target)
	} else {
		found, err = discover.WSDiscover(a.Ctx, target, 2*time.Second)
	}
	if err != nil {
		a.UI.Warn("the search failed: " + err.Error())
	}
	var ips []string
	for _, f := range found {
		if isIPv4(f.IP) {
			ips = append(ips, f.IP)
		}
	}
	a.probeMany(ips, ch)
	if len(found) == 0 {
		a.UI.Say("  No camera answered the discovery.")
	}
}

// chooserType takes an address, or a name that is resolved (DNS, then mDNS): both only because the user typed them.
func (a *App) chooserType(ch *chooser) {
	v, err := a.UI.Prompt("  Camera address or name:", "")
	if err != nil || v == "" {
		return
	}
	if !validHost(v) {
		a.UI.Say(fmt.Sprintf("  '%s' is not a valid address.", ui.Sanitize(v)))
		return
	}
	if !isIPv4(v) {
		r, ok := a.resolveName(v)
		if !ok {
			a.UI.Say(fmt.Sprintf("  '%s' could not be resolved.", v))
			return
		}
		a.UI.Say(fmt.Sprintf("  '%s' is at %s.", v, r))
		v = r
	}
	a.probeMany([]string{v}, ch)
	for _, f := range ch.found {
		if f.IP == v {
			return
		}
	}
	a.UI.Say(fmt.Sprintf("  %s does not answer on the camera ports %v.", v, a.scanPorts()))
}

// selectRow chooses the camera of a row: a remembered one is switched to, a new one is named first.
func (a *App) selectRow(ch *chooser, r discover.Row) (bool, error) {
	ip := r.IP
	if !r.Found {
		ip = r.LastIP
		a.UI.Warn(fmt.Sprintf("%s did not answer just now (last seen at %s). It is chosen anyway; check that it is on.", r.Name, ip))
	}
	if r.Found && r.Kind == discover.Partial {
		a.UI.Say("  Only SSH answers: an Akam camera opens the video and control ports once it has finished booting. Wait a minute and check again.")
		return false, nil
	}
	name := r.Name
	if name == "" {
		var ok bool
		if name, ok = a.askNewCameraName(); !ok {
			return false, nil
		}
	}
	if r.Found && r.OldIP != "" {
		a.offerRepin(ch, name, r.OldIP, ip)
	}
	if err := a.useCamera(name); err != nil {
		return false, a.UI.Fail("cannot open the state of "+name+": "+err.Error(), "")
	}
	if r.Found {
		_ = state.RecordSeen(a.St, ip, r.Host, r.FW, time.Now())
		switch r.Kind {
		case discover.Akam:
			_ = a.St.Set("camera_kind", "akam")
			_ = a.St.Set("camera_telnet", "0")
		case discover.Stock:
			_ = a.St.Set("camera_kind", "stock")
			_ = a.St.Set("camera_telnet", "1")
		}
	} else {
		_ = a.St.Set("camera_ip", ip)
	}
	a.UI.Good(fmt.Sprintf("  Camera: %s at %s.", name, ip))
	return true, nil
}

// offerRepin is "same camera, new address": the host key presented by the new address (read by this
// program in the SSH handshake, never taken from text the camera sent) equals the one pinned for the
// old address, so one yes/no (default No) pins it under the new address too.
func (a *App) offerRepin(ch *chooser, name, oldIP, newIP string) {
	key := ch.keys[newIP]
	if key == nil {
		return
	}
	fp := camssh.Fingerprint(key)
	if camssh.PinnedFingerprint(a.knownHostsFile(), oldIP) != fp {
		return
	}
	if oldIP == newIP {
		return
	}
	// A key already pinned for the new address is never replaced or doubled: if it differs, that is a
	// changed key at one address, which stays a hard error at connection time (no re-pin path here).
	if have := camssh.PinnedFingerprint(a.knownHostsFile(), newIP); have != "" {
		if have != fp {
			a.UI.Warn(fmt.Sprintf("%s already has a DIFFERENT SSH host key pinned (%s): nothing is pinned. If you are sure that you changed the camera, forget that address first.", newIP, have))
		}
		return
	}
	a.UI.Say(fmt.Sprintf("  %s was at %s and now answers at %s with the same SSH host key (%s).", name, oldIP, newIP, fp))
	ok, _ := a.UI.AskYN(fmt.Sprintf("  Same camera, new address: remember the host key for %s too?", newIP))
	if !ok {
		a.UI.Say("  Not pinned: the first connection to " + newIP + " will show the fingerprint and ask.")
		return
	}
	if err := camssh.PinHostKey(a.knownHostsFile(), newIP, key); err != nil {
		a.UI.Warn("could not write the pin: " + err.Error())
		return
	}
	a.Log.Line("PIN host key %s for %s (same key as %s)", fp, newIP, oldIP)
	a.UI.Good("  Pinned.")
}

// askNewCameraName asks a short name for a camera that is not remembered yet.
func (a *App) askNewCameraName() (string, bool) {
	a.UI.Say(fmt.Sprintf("  Give this camera a short name, for example 'salon' (lowercase letters, digits and '-', at most %d).", state.NameMax))
	for {
		v, err := a.UI.Prompt("  Camera name:", "")
		if err != nil || v == "" {
			return "", false
		}
		switch {
		case !state.NameValid(v):
			a.UI.Say(fmt.Sprintf("  '%s' is not valid: use lowercase letters, digits and '-' (at most %d, not starting or ending with '-').", ui.Sanitize(v), state.NameMax))
		case v == "default" || state.CameraExists(a.Cams.Base, v):
			a.UI.Say(fmt.Sprintf("  A camera called '%s' already exists (or the name is reserved): pick another one.", v))
		default:
			return v, true
		}
	}
}

// useCamera makes name the current camera: its state and log are opened and it is remembered for the
// next run. The address of the camera comes from its own state from now on.
func (a *App) useCamera(name string) error {
	a.Cams.Name = name
	a.Opt.IP = ""
	a.camIP = ""
	if err := a.reopen(); err != nil {
		return err
	}
	_ = a.Cams.Remember(name)
	return nil
}

// forgetCameraScreen forgets the network identity of a remembered camera (address, host name,
// firmware and the pinned host key). Its progress, logs and backups are kept.
func (a *App) forgetCameraScreen(ch *chooser) {
	if len(ch.known) == 0 {
		a.UI.Say("  No camera is remembered.")
		return
	}
	items := make([]string, 0, len(ch.known)+1)
	for _, k := range ch.known {
		items = append(items, fmt.Sprintf("%s  %s", k.Name, k.IP))
	}
	items = append(items, "Back")
	i, err := a.UI.Pick("Forget which camera?", items)
	if err != nil || i >= len(ch.known) {
		return
	}
	k := ch.known[i]
	a.UI.Say(fmt.Sprintf("  This forgets the address (%s) and the SSH host key pinned for %s. Its progress, logs and backups are kept.", k.IP, k.Name))
	ok, _ := a.UI.AskYN(fmt.Sprintf("  Forget the address and host key of %s?", k.Name))
	if !ok {
		a.UI.Say("  Left as it was.")
		return
	}
	if _, err := camssh.ForgetHostKey(a.knownHostsFile(), k.IP); err != nil {
		a.UI.Warn("could not update the pin file: " + err.Error())
	}
	var st *state.Store
	if k.Name == a.Cams.Name {
		st = a.St
	} else if s, err := state.Open(a.Cams.DirOf(k.Name), a.Opt.DryRun, a.Secrets); err == nil {
		st = s
	}
	if st != nil {
		for _, key := range []string{"camera_ip", "camera_host", "camera_fw", "camera_seen"} {
			_ = st.Unset(key)
		}
	}
	a.Log.Line("FORGET camera address and pin of %s (%s)", k.Name, k.IP)
	a.UI.Good("  Forgotten.")
}

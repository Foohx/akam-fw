package appfs

import (
	"fmt"
	"strings"
	"time"
)

// The hot update (card-less boot, no SD card): the camera side is ONE detached on-camera runner (scripts/akam_hot.sh,
// uploaded verbatim: it is camera code, not ported). This side uploads it, starts it, polls /tmp/akam_hot/status and
// answers its checkpoints. No answer within the runner's dead-man time = the runner restores by itself, nothing written.

func (r *runner) hotDry() {
	t := r.p.Target
	r.say("[dry-run] the whole camera-side sequence runs as ONE detached on-camera runner (nohup, setsid if present, fds redirected, oom_score_adj -1000 when panic_on_oom is 0): this program uploads the helper, starts it, polls %s/status and answers the checkpoints (wpa, dbalt, db22, upload, go) with a touch over a fresh ssh; no answer within 120 s = the runner restores by itself and exits, nothing written. After the write starts it never waits for the PC", hotDir)
	r.say("[dry-run] H1 ssh %s: upload %s; card-less proof (no mmcblk mounted, /mnt/sdcard = tmpfs bind of the Akam flash tree); tools (flashcp md5sum nohup dd sh cat awk sed ip cut sync mkdir rmdir sleep) not in /appfs", t, hotSh)
	r.say("[dry-run] H1 list every process with an /appfs exe, cwd, open file or mapping; each must be stub|init|supervisor|app|ntp|keeper|feeder|dropbear|wpa_supplicant, anything else = abort, nothing stopped")
	r.say("[dry-run] H1 capture the command lines (and AKAM_* environment) of supervisor, ntp, keeper, feeder, wpa_supplicant, dropbear for a restore")
	r.say("[dry-run] H2 copy busybox, dropbearmulti, wpa_supplicant to %s (md5-checked, a reused copy too; refuse if one maps another /appfs file)", hotDir)
	r.say("[dry-run] H2 start a new feeder from /tmp, prove it alive (oom_score_adj -1000; watchdog exclusive: SIGKILL old, start new at once; no magic close), then soak it alone >= 25 s; the fallback feeder is verified alive")
	r.say("[dry-run] H2 detached swap of wpa_supplicant to the /tmp copy (same arguments), checkpoint wpa: rolled back unless a fresh ssh round trip confirms within 120 s; already on /tmp (after a rehearsal) = skipped")
	r.say("[dry-run] H2 dropbear from /tmp on port %d; reconnect through it; stop the /appfs dropbears; dropbear from /tmp on port 22; stop the %d listener", r.p.HotPort, r.p.HotPort)
	r.say("[dry-run] H2 stop supervisor (waits for the app), app, ntp helper, keeper, then SIGKILL the stub and the resident init.sh; rescan: NO process may touch /appfs (else abort + restore)")
	r.say("[dry-run] H3 drop_caches, MemFree and MemAvailable logged at every gate, MemFree > %d kB (2 x %d kB + %d kB), /tmp free > %d kB; checkpoint upload: the PC stages the images in %s; checkpoint go; final gate; write by the existing worker", hotNeedKB, PartitionSize/1024, memMarginKB, hotTmpKB, ramStage)
	r.say("[dry-run] any failure above, or a missing confirmation: the runner restarts supervisor/ntp/keeper from the captured command lines, deletes %s, nothing written", ramStage)
}

// waitSSH is a plain ssh round trip on the connection selected by port (0 = normal).
func (r *runner) waitSSH(port, tries int) bool {
	for i := 0; i < tries; i++ {
		if c, err := r.d.Dial(port); err == nil {
			res, e := c.Run(r.d.Ctx, "true", nil)
			c.Close()
			if e == nil && res.Status == 0 {
				return true
			}
		}
		r.sleep(3 * time.Second)
	}
	return false
}

func (r *runner) confirmOn(port int, name string) {
	c, err := r.d.Dial(port)
	if err != nil {
		return
	}
	defer c.Close()
	_, _ = c.Run(r.d.Ctx, "touch "+hotDir+"/confirm."+name, nil)
}

func (r *runner) hotStatus() string {
	res, err := r.camq("cat " + hotDir + "/status 2>/dev/null")
	if err != nil {
		return ""
	}
	return trim(res.Stdout)
}

func (r *runner) hotImages() error {
	if err := r.upload(r.p.Image, r.stage+"/appfs-new.bin"); err != nil {
		return err
	}
	if err := r.upload(r.rollF, r.stage+"/appfs-backup.bin"); err != nil {
		return err
	}
	if err := r.putWorkers(); err != nil {
		return err
	}
	_, _ = r.camq("sync")
	return nil
}

// hotRun uploads the helper, starts the ONE detached runner, then only polls its status and answers its
// checkpoints. Sets r.hotRes (the worker result) for mode hot.
func (r *runner) hotRun(mode string) error {
	t := r.p.Target
	if _, err := r.must("mkdir -p "+hotDir+" && chmod 700 "+hotDir, "cannot create "+hotDir+" on the camera: nothing changed"); err != nil {
		return err
	}
	// single instance: never start (or overwrite the helper of) a runner while another one is alive
	if res, err := r.camq(`p=$(cat ` + hotDir + `/run.lock/pid 2>/dev/null); [ -n "$p" ] && grep -q akam_hot /proc/$p/cmdline 2>/dev/null`); err == nil && res.Status == 0 {
		return r.fail("a runner is already alive on the camera (pid in %s/run.lock/pid): not starting a second one. ssh %s 'cat %s/status %s/run.log'", hotDir, t, hotDir, hotDir)
	}
	if res, err := r.camqIn("cat > "+hotSh+" && chmod 755 "+hotSh, []byte(hotHelper)); err != nil || res.Status != 0 {
		return r.fail("cannot upload the helper: nothing changed")
	}
	if res, err := r.camq("md5sum <" + hotSh); err != nil || firstField(res.Stdout) != md5hex([]byte(hotHelper)) {
		return r.fail("the uploaded helper is corrupted: nothing changed")
	}
	_, _ = r.camq("rm -f " + hotDir + "/status " + hotDir + "/confirm.* " + hotDir + "/run.log")
	r.say("starting the detached on-camera runner (%s): the whole sequence runs on the camera; this program polls %s/status and answers the checkpoints", mode, hotDir)
	r.say("if this program dies the runner restores by itself when a confirmation does not arrive in time; by hand: ssh %s 'sh %s restore'; log: %s/run.log", t, hotSh, hotDir)
	start := fmt.Sprintf("cd / && if command -v setsid >/dev/null 2>&1; then S=setsid; else S=; fi; AKAM_HOT_PORT=%d nohup $S sh %s run %s %d %d %d >%s/run.log 2>&1 </dev/null &",
		r.p.HotPort, hotSh, mode, hotNeedKB, hotTmpKB, memMarginKB-1024, hotDir)
	if res, err := r.camq(start); err != nil || res.Status != 0 {
		return r.fail("cannot start the runner: nothing was changed unless the log says so (ssh %s 'cat %s/run.log %s/status')", t, hotDir, hotDir)
	}
	var last string
	elapsed, nost, up, iv, warned := 0, 0, false, 2, false
	for elapsed < 3000 {
		r.sleep(time.Duration(iv) * time.Second)
		elapsed += iv
		stl := r.hotStatus()
		if stl == "" {
			nost += iv
			if nost >= 300 {
				return r.fail("no status from the camera for 5 minutes. The runner restores by itself without confirmations (or finishes a write that already started: do NOT power off). Check: ssh %s 'cat %s/status %s/run.log'", t, hotDir, hotDir)
			}
			continue
		}
		nost = 0
		if stl != last {
			r.say("camera: %s", stl)
			last = stl
		}
		phase, msg, _ := strings.Cut(stl, "|")
		switch phase {
		case "WRITING":
			// the write runs: poll rarely (every ssh session forks a dropbear at oom -1000)
			iv = 15
			rv, _ := r.camq(`cat /tmp/akam_flash.result 2>/dev/null; echo ---; p=$(cat ` + hotDir + `/run.lock/pid 2>/dev/null); if [ -n "$p" ] && grep -q akam_hot /proc/$p/cmdline 2>/dev/null; then echo alive; else echo dead; fi`)
			wr, alive, _ := strings.Cut(string(rv.Stdout), "---")
			if wr = strings.Join(strings.Fields(wr), ""); wr != "" {
				r.hotLog("/tmp/akam_flash.log")
				r.hotRes = wr
				return nil
			}
			if strings.Contains(alive, "dead") && !warned {
				r.say("WARNING: the runner is no longer alive but the worker has no result yet: it may still be writing. Do NOT power off; waiting for /tmp/akam_flash.result")
				warned = true
			}
		case "WAIT_wpa", "WAIT_db22", "WAIT_go":
			if r.waitSSH(0, 3) {
				r.confirmOn(0, strings.TrimPrefix(phase, "WAIT_"))
			}
		case "WAIT_dbalt":
			if r.waitSSH(r.p.HotPort, 3) {
				r.confirmOn(r.p.HotPort, "dbalt")
			}
		case "WAIT_upload":
			if !up {
				r.say("uploading the images (md5-checked) into %s", r.stage)
				if err := r.hotImages(); err != nil {
					return err
				}
				up = true
			}
			r.confirmOn(0, "upload")
		case "DONE_RESTORED":
			r.hotLog(hotDir + "/run.log")
			return nil
		case "DONE_RESTORED_NO_WIFI", "ABORTED_RESTORED_NO_WIFI":
			r.hotLog(hotDir + "/run.log")
			return r.fail("%s. The camera is restored but NO wpa_supplicant runs: it has no Wi-Fi. NOTHING was written. Power-cycle it (a reboot returns to the normal state)", msg)
		case "DONE_RESTORE_FAILED", "ABORTED_RESTORE_FAILED":
			r.hotLog(hotDir + "/run.log")
			return r.fail("%s. NOTHING was written. Wait 3 minutes (the on-camera dead-man restores Wi-Fi), else power-cycle the camera: a reboot returns to the normal state", msg)
		case "ABORTED_RESTORED", "ABORTED_UNCHANGED":
			r.hotLog(hotDir + "/run.log")
			return r.fail("the camera runner aborted: %s", msg)
		case "WRITTEN":
			r.hotLog("/tmp/akam_flash.log")
			if msg == "NONE" {
				msg = ""
			}
			r.hotRes = msg
			return nil
		}
	}
	return r.fail("no end state from the runner after 50 minutes (the camera may still be writing: do NOT power it off; ssh %s 'cat %s/status %s/run.log /tmp/akam_flash.result'); a re-run is safe, step (a) aborts when mtd3 != the expected image", t, hotDir, hotDir)
}

func (r *runner) hotLog(path string) {
	if res, err := r.camq("cat " + path); err == nil {
		for _, l := range strings.Split(trim(res.Stdout), "\n") {
			if l != "" {
				r.say("camera: %s", l)
			}
		}
	}
}

func firstField(b []byte) string {
	if f := strings.Fields(string(b)); len(f) > 0 {
		return f[0]
	}
	return ""
}

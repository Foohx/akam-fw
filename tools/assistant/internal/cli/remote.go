package cli

import (
	"fmt"
	"regexp"
	"strings"
)

// Every command line sent to the camera is built here and nowhere else, so that a test can check
// the whole set against the safety rule: no reboot, poweroff, halt, watchdog or flash write. The only
// code that may write the flash is internal/flash (reached through flashSSH in flashcmds.go): it does not
// use this guard, which therefore refuses the reflash commands on every other path.

const (
	hostnameCmd = "/usr/sbin/akam_hostname.sh"
	rtspAuthCmd = "/usr/sbin/akam_rtsp_auth.sh"
	debugCmd    = "/usr/sbin/akam_debug.sh"
	onvifCmd    = "/usr/sbin/akam_onvif.sh"
)

// remoteTrue is cam_ssh_ok.
func remoteTrue() string { return "true" }

// remoteTestX is the capability probe of the hostname helper.
func remoteTestX(path string) string { return "test -x " + path }

func remoteHostnameSet(name string) string { return hostnameCmd + " set " + name }

func remoteRtspAuth(args ...string) string { return rtspAuthCmd + " " + strings.Join(args, " ") }

func remoteDebug(action string, minutes string) string {
	s := debugCmd + " " + action
	if minutes != "" {
		s += " " + minutes
	}
	return s
}

func remoteOnvif(args ...string) string { return onvifCmd + " " + strings.Join(args, " ") }

// remoteRegister reads the camera's name and the md5 of its APP partition (read-only).
func remoteRegister() string {
	return `echo "REG_HOSTNAME=$(hostname)"; echo "REG_MTD7_MD5=$(dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum | cut -d" " -f1)"`
}

// allRemoteCommands lists one instance of every constructor (used by the safety test).
func allRemoteCommands() []string {
	return []string{
		remoteTrue(), remoteTestX(hostnameCmd), remoteHostnameSet("salon"),
		remoteRtspAuth("status"), remoteRtspAuth("show"), remoteRtspAuth("set-creds"), remoteRtspAuth("disable"),
		remoteDebug("on", "60"), remoteDebug("off", ""), remoteDebug("status", ""),
		remoteOnvif("enable", "80"), remoteOnvif("disable"), remoteOnvif("status"),
		remoteRegister(),
	}
}

var forbiddenRe = regexp.MustCompile(`(?i)\b(reboot|poweroff|halt|shutdown|init\s+[0-6]|telinit|killall|kill)\b|watchdog|flash_tool|flashcp|flash_erase|nandwrite|mtd_debug|sysrq|/dev/mtd[0-9]*\s*$|of=/dev/|akam_reflash|i-understand-no-uart`)

// forbiddenRemote reports why a command would break the safety rule (empty when fine).
func forbiddenRemote(cmd string) string {
	if forbiddenRe.MatchString(cmd) {
		return fmt.Sprintf("forbidden camera command: %s", cmd)
	}
	return ""
}

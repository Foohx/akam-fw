#!/bin/sh
# include_closure.sh -- the files a set of C sources pulls in through #include "...": the sources themselves and, recursively, every quoted include
# that resolves in the including file's directory or in one of the -I directories. Needs no compiler (so the stale-binary guards work on a host
# without the cross toolchain). It ignores #if, so it is a superset of what a build reads: safe for a hash, and it never misses a header that is
# only included under a macro. Angle includes (<...>) are system or vendor headers and are not followed. -include FILE options are followed too.
#   include_closure.sh [-I dir]... [-include file]... source.c...
# Prints one absolute path per line, sorted (LC_ALL=C), no duplicates. A quoted include that resolves nowhere is silently skipped (generated
# headers such as version_info.h, or headers of a third-party tree that is not on the -I list). Relative arguments are taken from the cwd.
set -eu
cwd=$(pwd -P)
for a in "$@"; do printf '%s\n' "$a"; done | awk -v cwd="$cwd" '
function norm(p,    n, i, k, out, parts) {
    if (p !~ /^\//) p = cwd "/" p
    n = split(p, parts, "/"); k = 0
    for (i = 1; i <= n; i++) {
        if (parts[i] == "" || parts[i] == ".") continue
        if (parts[i] == "..") { if (k > 0) k--; continue }
        out[++k] = parts[i]
    }
    p = ""
    for (i = 1; i <= k; i++) p = p "/" out[i]
    return p
}
function exists(p,    l, r) { r = (getline l < p); if (r >= 0) { close(p); return 1 } return 0 }
function dirname(p) { sub(/\/[^\/]*$/, "", p); return p }
function resolve(dir, name,    i, c) {
    c = norm(dir "/" name); if (exists(c)) return c
    for (i = 1; i <= ninc; i++) { c = norm(inc[i] "/" name); if (exists(c)) return c }
    return ""
}
function visit(f,    line, name, r, dir) {
    if (f == "" || (f in seen)) return
    seen[f] = 1
    dir = dirname(f)
    while ((getline line < f) > 0) {
        if (match(line, /^[ \t]*#[ \t]*include[ \t]*"[^"]*"/)) {
            name = substr(line, RSTART, RLENGTH); sub(/^[^"]*"/, "", name); sub(/"$/, "", name)
            r = resolve(dir, name)
            if (r != "") visit(r)
        }
    }
    close(f)
}
{ args[++na] = $0 }
END {
    for (i = 1; i <= na; i++) {
        if (args[i] == "-I") inc[++ninc] = norm(args[++i])
        else if (args[i] ~ /^-I./) inc[++ninc] = norm(substr(args[i], 3))
        else if (args[i] == "-include") hdr[++nh] = args[++i]
        else src[++ns] = args[i]
    }
    for (i = 1; i <= nh; i++) visit(resolve("", hdr[i]))
    for (i = 1; i <= ns; i++) { f = norm(src[i]); if (exists(f)) visit(f) }
    for (f in seen) print f
}' | LC_ALL=C sort -u

#!/usr/bin/env bash
# The server with the Xpeccy tree it was built from out of reach - what a
# binary copied to another machine sees. Run by tests/smoke.sh; also on its own:
#
#   tests/relocated.sh <path-to-xspeccy-mcp> [xpeccy-source-tree]
#
# The tree is found in the CMakeCache.txt beside the binary when not given. The
# smoke suite cannot catch this by itself: it runs where the build left the
# tree, so a server that quietly reads its machines from there passes every
# check and then, copied anywhere else, comes up with no zx48, no evo-tsconf,
# a ZX48 on the default geometry and a Pentagon on the 48K rom.
#
# Hiding a directory from one process is per-platform: sandbox-exec on macOS,
# bwrap or an unprivileged mount namespace on Linux. Where none of them works
# (Windows, a container without user namespaces) the check says so and skips
# rather than passing - a skip is visible, a pass that checked nothing is not.
set -u

BIN="${1:?usage: relocated.sh <xspeccy-mcp> [xpeccy-source-tree]}"
SRC="${2:-}"
BINDIR=$(cd "$(dirname "$BIN")" && pwd)
CACHE=""
for c in "$BINDIR/CMakeCache.txt" "$BINDIR/../CMakeCache.txt"; do	# ../: multi-config
	[ -f "$c" ] && { CACHE="$c"; break; }
done
if [ -z "$SRC" ] && [ -n "$CACHE" ]; then
	SRC=$(sed -n 's/^XPECCY_SRC:[A-Z]*=//p' "$CACHE" | head -1)
fi
if [ -z "$SRC" ] || [ ! -d "$SRC" ]; then
	echo "  SKIP relocated binary (no Xpeccy tree known for $BIN)"
	exit 0
fi
# Without built-in roms the binary looks for them in the tree on purpose, so
# the rom check only means something when they are compiled in.
ROMS_BUILTIN=1
if [ -n "$CACHE" ] && grep -q '^XSP_BUILTIN_ROM:BOOL=OFF' "$CACHE"; then
	ROMS_BUILTIN=0
fi

hidden() {	# hidden <command...> - runs it with $SRC unreadable
	case "$(uname -s)" in
	Darwin)
		sandbox-exec -p "(version 1)(allow default)(deny file-read* (subpath \"$SRC\"))" "$@"
		;;
	Linux)
		if command -v bwrap >/dev/null 2>&1; then
			bwrap --dev-bind / / --tmpfs "$SRC" "$@"
		else
			unshare -rm sh -c 'mount -t tmpfs none "$0" && exec "$@"' "$SRC" "$@"
		fi
		;;
	*)
		return 125
		;;
	esac
}

# The hiding has to be shown to work before its result means anything: a
# sandbox that silently allowed the read would make this a second smoke run.
if hidden cat "$SRC/src/libxpeccy/spectrum.h" >/dev/null 2>&1 ||
   ! hidden true >/dev/null 2>&1; then
	echo "  SKIP relocated binary (cannot hide $SRC from a process on $(uname -s))"
	exit 0
fi

TMPD=$(mktemp -d)
mkdir -p "$TMPD/cfg"	# an empty configuration: nothing of the host's own
OUT="$TMPD/out.txt"
{
	echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"relocated","version":"1"}}}'
	echo '{"jsonrpc":"2.0","method":"notifications/initialized"}'
	# before anything is asked for: the machine a bare server starts on
	echo '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"machine_config","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"ay_state","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"ZX48","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"zx48","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"evo-tsconf","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'
} | hidden "$BIN" --config "$TMPD/cfg" > "$OUT" 2>/dev/null

fail=0
reply() { grep "\"id\":$1," "$OUT" | head -1; }
expect() {	# expect <label> <id> <pattern> [absent-pattern]
	local line
	line=$(reply "$2")
	if [ -n "$line" ] && printf '%s' "$line" | grep -q -e "$3" &&
	   { [ -z "${4:-}" ] || ! printf '%s' "$line" | grep -q -e "$4"; }; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		[ -n "$line" ] && printf '       %s\n' "$(printf '%s' "$line" | cut -c1-200)"
		fail=1
	fi
}

# A server with no configuration starts on the Pentagon definition: 128K, its
# own geometry, TR-DOS and an AY - and says so, rather than naming the
# profile's romset and geometry, which a definition never uses.
expect "default: the Pentagon definition"    8 '"machine\\": *\\"pent'
expect "default: 128K"                       8 'memory_kb\\": *128,'
expect "default: Pentagon geometry"          8 'ULA\.Pentagon'
expect "default: TR-DOS"                     8 'Beta Disk (TR-DOS)'
expect "default: names its definition"       8 "machine definition 'pent'" 'romset=ZX48'
expect "default: an AY"                      9 'present\\": *true'
expect "relocated: ZX48 on its own geometry" 2 'ULA\.48' '"warning'
expect "relocated: ZX48 at 69888 T a frame"  2 'per_frame\\": *69888'
expect "relocated: zx48 definition known"    3 '"machine\\": *\\"zx48'
expect "relocated: evo-tsconf known"         4 '"machine\\": *\\"evo-tsconf'
expect "relocated: evo-tsconf geometry"      4 'ULA\.TSConf'
if [ $ROMS_BUILTIN -eq 1 ]; then
	expect "default: on the Pentagon's own roms" 8 'romset\\": *\\"machine:pent' 'roms: fell back'
	expect "relocated: Pentagon on its own roms" 5 'romset\\": *\\"machine:pent' 'roms: fell back'
	# the GS firmware and the text font are roms too, loaded by name
	expect "relocated: Pentagon has its GS rom"  5 'machine:pent' 'gs rom'
	expect "relocated: evo-tsconf has its font"  4 'evo-tsconf' "font '"
else
	echo "  SKIP relocated: Pentagon roms (built with XSP_BUILTIN_ROM=OFF)"
fi

rm -rf "$TMPD"
exit $fail

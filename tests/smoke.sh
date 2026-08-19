#!/usr/bin/env bash
# End-to-end check: drives the server over stdio the way an MCP client does.
# Usage: tests/smoke.sh [path-to-xspeccy-mcp]
set -u

BIN="${1:-$(dirname "$0")/../build/xspeccy-mcp}"
OUT=$(mktemp)
SHOT=$(mktemp -u --suffix=.png)
WAV=$(mktemp -u --suffix=.wav)
GIF=$(mktemp -u --suffix=.gif)
LABELS=$(mktemp --suffix=.labels)
LST=$(mktemp --suffix=.lst)
TAP=$(mktemp --suffix=.tap)

# Every path below travels inside a JSON string, so the shell never sees it again
# and cannot translate it. On Windows the server is a native program: the /tmp/x
# this shell means is C:\msys64\tmp\x, while the one the server opens is C:\tmp\x.
# Hand the server the native form and keep the shell form for our own checks.
if command -v cygpath >/dev/null 2>&1; then
	native() { cygpath -m "$1"; }
else
	native() { printf '%s' "$1"; }
fi
SHOT_N=$(native "$SHOT"); WAV_N=$(native "$WAV"); GIF_N=$(native "$GIF")
LABELS_N=$(native "$LABELS"); LST_N=$(native "$LST"); TAP_N=$(native "$TAP")

# One-block .tap: a 19-byte standard header, length word first. It exists so the
# tape block range has something real to be a range of - without a tape loaded,
# every block number is refused for the other reason.
printf '\x13\x00\x00\x03SMOKE     \x00\x00\x00\x00\x00\x00\x7c' > "$TAP"

cat > "$LST" <<'LSTF'
# file main.asm
     1   0000              ; demo
     2   8000              main:
     3   8000 F3           di
     4   8001 3E 10        ld a,16
     5   8003 EE 10        xor 16
     6   8005 D3 FE        out (254),a
LSTF

# "BB:OOOO name" is a bank and an offset inside it, not a CPU address: bank 5 is
# seen at $4000, bank 2 at $8000, anything else at $C000, FF is a CPU address.
cat > "$LABELS" <<'LBL'
FF:8000 main
FF:8010 draw_loop
sprite_data: EQU $9000
02:1000 bank2_main
02:1003 bank2_beep
05:0100 bank5_label
07:0001 bank7_const
:8020 colon_only
LBL

{
	echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}'
	echo '{"jsonrpc":"2.0","method":"notifications/initialized"}'
	echo '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
	echo '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"machine_state","arguments":{}}}'
	# before anything starts it, the profiler must say so rather than report zeros
	echo '{"jsonrpc":"2.0","id":97,"method":"tools/call","params":{"name":"profile","arguments":{"action":"read"}}}'
	echo '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_registers","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"disassemble","arguments":{"address":"0x0000","count":3}}}'
	echo '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["ld a,5","ld hl,$4000","ld (hl),a","ret"]}}}'
	echo '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"0x8000","length":8}}}'
	echo '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"0x8000"}}}'
	echo '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"0x8003"}}}'
	echo '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":100}}}'
	echo '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"0x4000","length":1}}}'
	echo '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"reset","arguments":{"boot_frames":150}}}'
	echo '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"screen_text","arguments":{}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\",\"params\":{\"name\":\"screenshot\",\"arguments\":{\"path\":\"$SHOT_N\"}}}"
	echo '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"nonexistent","arguments":{}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"tools/call\",\"params\":{\"name\":\"load_labels\",\"arguments\":{\"path\":\"$LABELS_N\"}}}"
	echo '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["ld hl,sprite_data","jp draw_loop"]}}}'
	echo '{"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"disassemble","arguments":{"address":"$8000","count":2}}}'
	echo '{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"frame_timing","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"beam_position","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start"}}}'
	echo '{"jsonrpc":"2.0","id":23,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":5}}}'
	echo '{"jsonrpc":"2.0","id":24,"method":"tools/call","params":{"name":"profile","arguments":{"action":"read"}}}'
	echo '{"jsonrpc":"2.0","id":25,"method":"tools/call","params":{"name":"trace","arguments":{"action":"enable","size":64}}}'
	echo '{"jsonrpc":"2.0","id":26,"method":"tools/call","params":{"name":"step","arguments":{"count":4}}}'
	echo '{"jsonrpc":"2.0","id":27,"method":"tools/call","params":{"name":"trace","arguments":{"action":"dump","count":4}}}'
	echo '{"jsonrpc":"2.0","id":28,"method":"tools/call","params":{"name":"reset","arguments":{"boot_frames":120}}}'
	echo '{"jsonrpc":"2.0","id":29,"method":"tools/call","params":{"name":"type_text","arguments":{"text":"p1\n","frames":4}}}'
	echo '{"jsonrpc":"2.0","id":30,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":25}}}'
	echo '{"jsonrpc":"2.0","id":31,"method":"tools/call","params":{"name":"screen_text","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":32,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["di","ld a,16","xor 16","out (254),a","ld b,60","djnz 32777","jr 32771"]}}}'
	echo '{"jsonrpc":"2.0","id":33,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"tools/call\",\"params\":{\"name\":\"audio_capture\",\"arguments\":{\"frames\":25,\"path\":\"$WAV_N\"}}}"
	echo '{"jsonrpc":"2.0","id":35,"method":"tools/call","params":{"name":"ay_state","arguments":{}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"tools/call\",\"params\":{\"name\":\"load_listing\",\"arguments\":{\"path\":\"$LST_N\"}}}"
	echo '{"jsonrpc":"2.0","id":37,"method":"tools/call","params":{"name":"source_at","arguments":{"address":"$8003"}}}'
	echo '{"jsonrpc":"2.0","id":38,"method":"tools/call","params":{"name":"coverage","arguments":{"action":"start"}}}'
	echo '{"jsonrpc":"2.0","id":39,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":20000}}}'
	echo '{"jsonrpc":"2.0","id":40,"method":"tools/call","params":{"name":"coverage","arguments":{"action":"read","from":"$8000","to":"$8020","min_gap":2}}}'
	echo '{"jsonrpc":"2.0","id":41,"method":"tools/call","params":{"name":"sound_state","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":42,"method":"tools/call","params":{"name":"tape","arguments":{"action":"state"}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":43,\"method\":\"tools/call\",\"params\":{\"name\":\"record_video\",\"arguments\":{\"frames\":20,\"path\":\"$GIF_N\",\"border\":false}}}"
	# labels: bank:offset mapping, and label names as address arguments
	echo '{"jsonrpc":"2.0","id":45,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":46,"method":"tools/call","params":{"name":"resolve_symbol","arguments":{"name":"bank5_label"}}}'
	echo '{"jsonrpc":"2.0","id":47,"method":"tools/call","params":{"name":"resolve_symbol","arguments":{"name":"bank7_const"}}}'
	echo '{"jsonrpc":"2.0","id":48,"method":"tools/call","params":{"name":"resolve_symbol","arguments":{"name":"colon_only"}}}'
	echo '{"jsonrpc":"2.0","id":49,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"bank2_main","lines":["di","ld a,16","xor 16","out (254),a","ld b,60","djnz 36873","jr 36867"]}}}'
	echo '{"jsonrpc":"2.0","id":50,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"bank2_main"}}}'
	echo '{"jsonrpc":"2.0","id":51,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"bank2_beep"}}}'
	echo '{"jsonrpc":"2.0","id":52,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":100}}}'
	echo '{"jsonrpc":"2.0","id":53,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":54,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start","ranges":[{"tag":"beeploop","start":"bank2_main","end":"$9010"}]}}}'
	echo '{"jsonrpc":"2.0","id":55,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":5}}}'
	echo '{"jsonrpc":"2.0","id":56,"method":"tools/call","params":{"name":"profile","arguments":{"action":"read"}}}'
	echo '{"jsonrpc":"2.0","id":57,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"no_such_label"}}}'
	echo '{"jsonrpc":"2.0","id":58,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start","ranges":[{"tag":"bad","start":"bank2_main","end":"no_such_label"}]}}}'
	echo '{"jsonrpc":"2.0","id":59,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":"not a number"}}}'
	echo '{"jsonrpc":"2.0","id":60,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","geometry":"Pentagon","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":61,"method":"tools/call","params":{"name":"frame_timing","arguments":{}}}'
	# A test effect: four calls to a subroutine that pokes one attribute cell,
	# then EI/HALT and round again - one HALT per frame, four calls per frame,
	# and a screen that changes every frame.
	#   8000 ld b,4 / 8002 call 800B / 8005 djnz 8002 / 8007 ei / 8008 halt
	#   8009 jr 8000 / 800B ld hl,5800 / 800E ld a,(8017) / 8011 inc a
	#   8012 ld (8017),a / 8015 ld (hl),a / 8016 ret / 8017 counter
	echo '{"jsonrpc":"2.0","id":62,"method":"tools/call","params":{"name":"reset","arguments":{"boot_frames":150}}}'
	echo '{"jsonrpc":"2.0","id":63,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"06 04 CD 0B 80 10 FB FB 76 18 F5 21 00 58 3A 17 80 3C 32 17 80 77 C9 00"}}}'
	echo '{"jsonrpc":"2.0","id":64,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":65,"method":"tools/call","params":{"name":"frame_cost","arguments":{"frames":3}}}'
	echo '{"jsonrpc":"2.0","id":66,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":3}}}'
	# same state twice must hash the same, a changed screen must not
	echo '{"jsonrpc":"2.0","id":67,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8017","hex":"00"}}}'
	echo '{"jsonrpc":"2.0","id":68,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1}}}'
	echo '{"jsonrpc":"2.0","id":69,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8017","hex":"00"}}}'
	echo '{"jsonrpc":"2.0","id":70,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1}}}'
	echo '{"jsonrpc":"2.0","id":71,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$5810","hex":"FF"}}}'
	echo '{"jsonrpc":"2.0","id":72,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1}}}'
	# four calls per frame, three frames
	echo '{"jsonrpc":"2.0","id":73,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start","ranges":[{"tag":"sub","start":"$800B","end":"$8017"}]}}}'
	echo '{"jsonrpc":"2.0","id":74,"method":"tools/call","params":{"name":"frame_cost","arguments":{"frames":3}}}'
	echo '{"jsonrpc":"2.0","id":75,"method":"tools/call","params":{"name":"profile","arguments":{"action":"read"}}}'
	echo '{"jsonrpc":"2.0","id":76,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start","ranges":"symbols"}}}'
	echo '{"jsonrpc":"2.0","id":77,"method":"tools/call","params":{"name":"frame_cost","arguments":{"frames":2}}}'
	echo '{"jsonrpc":"2.0","id":78,"method":"tools/call","params":{"name":"profile","arguments":{"action":"read"}}}'
	# T-states straight from the emulator: prefixes, block ops, conditionals
	echo '{"jsonrpc":"2.0","id":79,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$9100","hex":"DD 7E 05 ED B0 20 FE 10 FE E3"}}}'
	echo '{"jsonrpc":"2.0","id":80,"method":"tools/call","params":{"name":"disassemble","arguments":{"address":"$9100","count":5,"t_states":true}}}'
	echo '{"jsonrpc":"2.0","id":81,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start","ranges":"symbols","from":"$D000","to":"$D010"}}}'
	# a failed start must not leave the profiler looking like it measured zeros
	echo '{"jsonrpc":"2.0","id":82,"method":"tools/call","params":{"name":"profile","arguments":{"action":"reset"}}}'
	echo '{"jsonrpc":"2.0","id":83,"method":"tools/call","params":{"name":"profile","arguments":{"action":"stop"}}}'
	echo '{"jsonrpc":"2.0","id":84,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start","ranges":[{"tag":"a","start":"$8000","end":"$8010"},{"tag":"b","start":"vanished_label","end":"$9000"}]}}}'
	echo '{"jsonrpc":"2.0","id":85,"method":"tools/call","params":{"name":"profile","arguments":{"action":"read"}}}'
	# calls must not count a nested callee returning. caller 9200 calls 9300 twice
	# per invocation and is itself called four times a frame from the test effect.
	#   9200 call 9300 / 9203 call 9300 / 9206 ret        (the caller)
	#   9300 ld a,(9017) / 9303 ret                       (the leaf)
	echo '{"jsonrpc":"2.0","id":86,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$9200","hex":"CD 00 93 CD 00 93 C9"}}}'
	echo '{"jsonrpc":"2.0","id":87,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$9300","hex":"3A 17 80 C9"}}}'
	# rewrite the effect subroutine to call the caller: 800B call 9200 / 800E ret
	echo '{"jsonrpc":"2.0","id":88,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$800B","hex":"CD 00 92 C9"}}}'
	echo '{"jsonrpc":"2.0","id":89,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":90,"method":"tools/call","params":{"name":"profile","arguments":{"action":"start","ranges":[{"tag":"caller","start":"$9200","end":"$9207"},{"tag":"leaf","start":"$9300","end":"$9304"}]}}}'
	echo '{"jsonrpc":"2.0","id":91,"method":"tools/call","params":{"name":"frame_cost","arguments":{"frames":3}}}'
	echo '{"jsonrpc":"2.0","id":92,"method":"tools/call","params":{"name":"profile","arguments":{"action":"read"}}}'
	# skip_until, and the T-states a run cost
	echo '{"jsonrpc":"2.0","id":93,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":94,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"skip_until":"$8008"}}}'
	echo '{"jsonrpc":"2.0","id":95,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":500}}}'
	echo '{"jsonrpc":"2.0","id":96,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"skip_until":"no_such_place"}}}'
	# a ZX machine must come with the disk controller TR-DOS needs, and a model the core
	# cannot run without a GUI must be refused rather than take the server down with it
	echo '{"jsonrpc":"2.0","id":98,"method":"tools/call","params":{"name":"disk","arguments":{"action":"state"}}}'
	echo '{"jsonrpc":"2.0","id":99,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"GameBoy"}}}'
	echo '{"jsonrpc":"2.0","id":100,"method":"tools/call","params":{"name":"machine_state","arguments":{}}}'
	# labels the assembled block defines itself, backwards and forwards, and the
	# names that must be refused rather than silently swallowed
	echo '{"jsonrpc":"2.0","id":98,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$9400","lines":["top: dec a","jr nz,top","call sub","jr done","sub: ret","done: jp top"]}}}'
	echo '{"jsonrpc":"2.0","id":99,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$9400","lines":["c: nop"]}}}'
	echo '{"jsonrpc":"2.0","id":100,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$9400","lines":["dup: nop","dup: ret"]}}}'
	# A breakpoint on a bank that is not paged in at the time. Page 7 in, leave a
	# nop sled in it, page it back out, arm 07:0000 - and it must fire only once
	# the bank returns to the window. Armed against the CPU address instead, this
	# would have marked bank 0 and never fired.
	echo '{"jsonrpc":"2.0","id":101,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":102,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["ld bc,$7ffd","ld a,7","out (c),a","ret"]}}}'
	echo '{"jsonrpc":"2.0","id":103,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":104,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":4}}}'
	echo '{"jsonrpc":"2.0","id":105,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$C000","hex":"00 00 00 C9"}}}'
	echo '{"jsonrpc":"2.0","id":106,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["ld bc,$7ffd","ld a,0","out (c),a","ret"]}}}'
	echo '{"jsonrpc":"2.0","id":107,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":108,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":4}}}'
	echo '{"jsonrpc":"2.0","id":109,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"07:0000"}}}'
	echo '{"jsonrpc":"2.0","id":110,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["ld bc,$7ffd","ld a,7","out (c),a","jp $C000"]}}}'
	echo '{"jsonrpc":"2.0","id":111,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":112,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":20}}}'
	echo '{"jsonrpc":"2.0","id":113,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"$8000","scope":"nonsense"}}}'
	# the raster follows the geometry, so a ZX48K on the default layout is timed
	# as a Pentagon - it must say so rather than report 71680 as the machine's
	# operands written the way a source file writes them: a space after the comma
	echo '{"jsonrpc":"2.0","id":116,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$9500","write":false,"lines":["ld hl, $1234","ld a, (ix + 5)"]}}}'
	# A digest over plain memory, for the tables the screen does not show. The
	# machine is parked in a halt loop first: screen_digest runs a frame before it
	# hashes, and by this point in the script the CPU is wherever the bank test
	# left it, free to scribble over the bytes we are about to hash.
	echo '{"jsonrpc":"2.0","id":117,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":118,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"FB 76 18 FC"}}}'
	echo '{"jsonrpc":"2.0","id":119,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":120,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$9600","hex":"01 02 03 04 05 06 07 08"}}}'
	echo '{"jsonrpc":"2.0","id":121,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"from":"$9600","to":"$9607"}}}'
	echo '{"jsonrpc":"2.0","id":122,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$9603","hex":"FF"}}}'
	echo '{"jsonrpc":"2.0","id":123,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"from":"$9600","to":"$9607"}}}'
	echo '{"jsonrpc":"2.0","id":124,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"from":"$9600"}}}'
	echo '{"jsonrpc":"2.0","id":114,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"ZX48K","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":115,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'
	# Raster debugging. The fixture is our own border-flipping loop rather than
	# the ROM: after a reset the ROM spends its first frames in the RAM test with
	# interrupts off, so $0038 does not run and a log watching it stays empty for
	# reasons that have nothing to do with the log.
	#   $8000 di / $8001 ld a,16 / $8003 xor 16 / $8005 out (254),a
	#   $8007 ld b,60 / $8009 djnz $8009 / $800B jr $8003
	# $8005 therefore executes once per pass, many times a frame, forever.
	echo '{"jsonrpc":"2.0","id":150,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["di","ld a,16","xor 16","out (254),a","ld b,60","djnz 32777","jr 32771"]}}}'
	echo '{"jsonrpc":"2.0","id":151,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":152,"method":"tools/call","params":{"name":"raster_log","arguments":{"action":"enable","addresses":["$8005"]}}}'
	echo '{"jsonrpc":"2.0","id":153,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":40000}}}'
	echo '{"jsonrpc":"2.0","id":154,"method":"tools/call","params":{"name":"raster_log","arguments":{"action":"dump","count":3}}}'
	echo '{"jsonrpc":"2.0","id":155,"method":"tools/call","params":{"name":"raster_log","arguments":{"action":"enable"}}}'
	echo '{"jsonrpc":"2.0","id":156,"method":"tools/call","params":{"name":"run_to_beam","arguments":{"line":100}}}'
	echo '{"jsonrpc":"2.0","id":157,"method":"tools/call","params":{"name":"run_to_beam","arguments":{"line":99999}}}'
	echo '{"jsonrpc":"2.0","id":158,"method":"tools/call","params":{"name":"frame_digest","arguments":{"frames":2,"lines":true}}}'
	echo '{"jsonrpc":"2.0","id":159,"method":"tools/call","params":{"name":"raster_log","arguments":{"action":"disable"}}}'
	# Arguments no caller means, kept last because their whole point is that the
	# server is still there afterwards. Every one of these used to be a way to
	# end the session: three read outside an array, two never returned, and the
	# rest answered a question nobody asked.
	echo '{"jsonrpc":"2.0","id":130,"method":"tools/call","params":{"name":"coverage","arguments":{"from":100000}}}'
	echo '{"jsonrpc":"2.0","id":131,"method":"tools/call","params":{"name":"coverage","arguments":{"to":-10}}}'
	echo '{"jsonrpc":"2.0","id":132,"method":"tools/call","params":{"name":"ay_writes","arguments":{"offset":-1}}}'
	echo '{"jsonrpc":"2.0","id":133,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":0}}}'
	echo '{"jsonrpc":"2.0","id":134,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":-1}}}'
	echo '{"jsonrpc":"2.0","id":135,"method":"tools/call","params":{"name":"step_over","arguments":{"max_instructions":-1}}}'
	echo '{"jsonrpc":"2.0","id":136,"method":"tools/call","params":{"name":"machine_config","arguments":{"memory":2147483647}}}'
	echo '{"jsonrpc":"2.0","id":137,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"GG"}}}'
	echo '{"jsonrpc":"2.0","id":138,"method":"tools/call","params":{"name":"find_bytes","arguments":{"pattern":"ZZ"}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":139,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TAP_N\"}}}"
	echo '{"jsonrpc":"2.0","id":140,"method":"tools/call","params":{"name":"tape","arguments":{"action":"rewind","block":5}}}'
	echo '{"jsonrpc":"2.0","id":141,"method":"tools/call","params":{"name":"tape","arguments":{"action":"rewind","block":0}}}'
	echo '{"jsonrpc":"2.0","id":142,"method":"tools/call","params":{"name":"get_registers","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":44,"method":"ping"}'
} | "$BIN" > "$OUT" 2>/tmp/xspeccy-smoke.err

fail=0
check() {	# check <label> <jq-ish grep pattern>
	if grep -q "$2" "$OUT"; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		fail=1
	fi
}

echo "responses: $(wc -l < "$OUT")"
check "initialize"        '"serverInfo"'
check "tools/list"        '"tools":\['
check "machine_state"     'model'
check "registers"         'flags'
# The register bunch ends at REG_EOT, and its terminating entry carries no name.
# Walking it by name reads uninitialised stack and segfaults about half the time,
# so check the far end of the set arrived rather than just that something did.
check "shadow registers"  "HL'"
check "register set ends" '\\"SP\\":'
check "disassemble ROM"   'di'
check "assemble"          '3E 05'
check "assembled in mem"  '"3E 05 21 00 40 77 C9'
check "breakpoint hit"    'breakpoint'
check "screen_text"       '1982 Sinclair Research Ltd'
check "screenshot"        "width"
check "unknown tool"      '"isError":true'
check "labels loaded"     'sprite_data'
check "assemble w/ label" '21 00 90'
check "disasm annotated"  'target_label'
check "frame timing"      't_states_per_frame'
check "beam position"     'zone'
check "profiler"          'hot_pages'
check "trace"             'capacity'
check "typed into BASIC"  '0 OK, 0:1'
check "beeper heard"      '"silent\\": false'
check "ay decoded"        'AY-3-8910'
check "sound sources"     'dominant_source'
check "listing loaded"    '"file\\": \\"main\.asm'	# from source_at, not from an assemble echo
check "coverage"          'unexecuted_gaps'
check "sound_state"       'mixed_output'
check "tape state"        'playing'
check "video recorded"    'frames_recorded'
check "bank 5 -> \$4000"   '\$4100'
check "bank 7 -> \$C000"   '\$C001'
check "bare colon = cpu"  '\$8020'
check "label as address"  '"pc_hex\\": \\"\$9003'
check "label range counts" 't_states\\": [1-9][0-9]*[^}]*tag\\": \\"beeploop'
check "unknown label"     'no such label'
check "unreadable number" 'is not a number'
check "real frame rate"   '"fps_real\\": 48\.8'
check "frame cost"        '"interrupts_per_frame\\": 1'
check "frame headroom"    '"headroom_percent\\": 9[0-9]'
check "frame idle split"  '"idle_t\\": [1-9][0-9]*'
check "calls counted"     '"calls\\": 12'
check "t per call"        '"t_per_call\\": [1-9][0-9]*'
check "symbols split"     'tag\\": \\"main\\"'
check "unattributed"      '"unattributed\\"'
check "t-states DD"       't_states\\": 19'
check "t-states ldir"     't_states\\": 21'
check "t-states jr cc"    't_states_text\\": \\"12/7\\"'
check "t-states djnz"     't_states_text\\": \\"13/8\\"'
check "t-states ex(sp)"   't_states\\": 19'
check "t-states total"    '"t_states_total\\": [1-9][0-9]*'
check "symbols needs range" 'no non-local labels'
check "unarmed profile"   'the profiler has never been started'
check "unarmed flag"      '"armed\\": false'
check "failed start seen" 'the last profile start FAILED'
check "failed start named" '"last_start_failed\\"'
# caller: 4 invocations a frame x 3 frames = 12 calls, 24 nested returns on top
check "calls skip returns" '"calls\\": 12,[^}]*"entries\\": 36'
check "leaf calls"        '"calls\\": 24,[^}]*"entries\\": 24'
# the caller runs 528 T of its own and spends 552 in the leaf: 1080 inclusive,
# 90 per call against 44 self. A leaf's two numbers are the same by definition.
check "inclusive caller"  '"t_states_inclusive\\": 1080'
check "inclusive per call" '"t_per_call_inclusive\\": 90\.0'
check "inclusive leaf"    '"t_states\\": 552,[^}]*"t_states_inclusive\\": 552'
check "run reports T"     '"t_states\\": [1-9][0-9]*'
check "skip_until"        '"skipped_to\\"'
check "skip_until fails"  'never reached\|no such label'
# top at $9400, sub at $9408, done at $9409
check "asm label back"    '"hex\\": \\"20 FD'
check "asm label forward" '"hex\\": \\"CD 08 94'
check "asm label list"    '"sub\\": {[^}]*\$9408'
check "asm reserved name" 'is a register or condition name'
check "asm double label"  "defined twice"
check "bank brk armed"    '"bank\\": 7,[^}]*"bank_paged_in\\": false'
check "bank brk fires"    '"pc_hex\\": \\"\$C000\\"[^}]*"reason\\": \\"breakpoint'
check "brk scope refused" 'scope must be'
check "geometry warning"  'but a ZX48K runs 69888'
# "ld hl, $1234" and "ld a, (ix + 5)" - spaces where a source file puts them
check "asm spaced comma"  '"hex\\": \\"21 34 12'
check "asm spaced index"  '"hex\\": \\"DD 7E 05'
# md5 of 01..08, then the same eight bytes with $9603 poked to FF
check "memory digest"     '"digest\\": \\"0ee0646c1c77'
check "memory digest sees a poke" '"digest\\": \\"1bc531b4312b'
check "digest range halved" 'give both from and to'
check "beta disk present" '"interface\\": \\"Beta Disk'
check "bad model refused" 'not usable here'
check "server survived"   '"model\\": \\"Pentagon'

# The orientation text sent with initialize. A tool description says what one
# tool does; this is the only place the server says which to reach for, so its
# quiet disappearance would cost an agent more than a missing tool would.
check "initialize instructs" '"instructions":"A ZX Spectrum'
check "instructs on digests" 'depends on screen memory AND on when the bank is switched'

# Raster debugging. 40000 instructions of a 64-instruction loop is 625 passes,
# and that number depends on the loop alone, not on anything run before it.
check "raster log records" '"events_total\\": 625'
check "raster log address" '"address_hex\\": \\"\$8005'
check "raster log jitter"  '"jitter_t\\":'
check "raster log needs addresses" 'enable needs addresses\[\]'
# The beam only moves between instructions, so landing past the target is
# normal - landing on a different line is not.
check "run_to_beam lands"  '"line\\": 100,[^}]*"t_states_frame'
check "run_to_beam exact"  '"overshoot_lines\\": 0'
check "run_to_beam range"  'bad line: 99999 is outside 0\.\.319'
check "stop reports beam"  '"beam\\": {'
# Pentagon draws 320x248 with the border on; the digest is of the drawn frame,
# so an unchanging picture repeats and says which frame it matched.
check "frame_digest size"  '"width\\": 320,\|"height\\": 248,'
check "frame_digest repeats" '"same_as_frame\\": 0'
check "frame_digest lines" '"changed_lines\\":'

# Bad arguments: refused by name, and the server still answers afterwards.
check "addr above space"  'bad from: 100000 is outside the address space'
check "addr below space"  'bad to: -10 is outside the address space'
check "negative offset"   'bad offset: -1 is outside 0\.\.'
check "zero frame count"  'bad count: 0 is outside 1\.\.'
check "no budget for run" 'bad max_instructions: -1 is outside 1\.\.'
check "no budget to step" 'error in step_over: bad max_instructions'
check "absurd ram size"   'memory size must be at most 4096K'
check "hex typo refused"  "bad hex: 'GG' is not a hex byte"
check "pattern typo"      "bad pattern: 'ZZ' is not a hex byte"
check "tape block range"  'bad block: 5 is outside 0\.\.0'
check "tape block ok"     '"blocks\\": 1'
check "alive after bad args" '"id":142'

# Answering every request is not the same as ending well. A tape has been loaded
# by now, and freeing one used to abort the process on the way out - after the
# last answer, so nothing above notices. Only the allocator says so, and only on
# a platform whose allocator checks.
if grep -qE 'double free|corruption|Aborted|munmap_chunk' /tmp/xspeccy-smoke.err; then
	echo "  FAIL the allocator complained on the way out"
	fail=1
else
	echo "  ok   exits clean"
fi

# screen_digest: the same screen must hash the same, a changed one must not.
# Digests appear in file order: 3 from id 66, then one each from 68, 70 and 72.
mapfile -t DIG < <(grep -o 'digest\\": \\"[0-9a-f]\{12\}' "$OUT" | sed 's/.*\\"//')
if [ "${#DIG[@]}" -ge 6 ]; then
	if [ "${DIG[0]}" != "${DIG[1]}" ] && [ "${DIG[1]}" != "${DIG[2]}" ]; then
		echo "  ok   digest follows the screen (${DIG[0]} ${DIG[1]} ${DIG[2]})"
	else
		echo "  FAIL digest did not change with the picture"
		fail=1
	fi
	if [ "${DIG[3]}" = "${DIG[4]}" ]; then
		echo "  ok   digest is deterministic (${DIG[3]})"
	else
		echo "  FAIL same screen hashed differently: ${DIG[3]} vs ${DIG[4]}"
		fail=1
	fi
	if [ "${DIG[4]}" != "${DIG[5]}" ]; then
		echo "  ok   digest sees a poked attribute (${DIG[5]})"
	else
		echo "  FAIL poking an attribute did not change the digest"
		fail=1
	fi
else
	echo "  FAIL expected 6 digests, got ${#DIG[@]}"
	fail=1
fi

if [ -f "$SHOT" ]; then
	echo "  ok   png written: $SHOT ($(stat -c%s "$SHOT") bytes)"
else
	echo "  FAIL png missing"
	fail=1
fi

if [ -s "$WAV" ]; then
	echo "  ok   wav written: $WAV ($(stat -c%s "$WAV") bytes)"
else
	echo "  FAIL wav missing"
	fail=1
fi

if [ -s "$GIF" ] && head -c 6 "$GIF" | grep -q GIF89a; then
	echo "  ok   gif written: $GIF ($(stat -c%s "$GIF") bytes)"
else
	echo "  FAIL gif missing or malformed"
	fail=1
fi

echo
echo "stderr:"; sed 's/^/  /' /tmp/xspeccy-smoke.err
[ $fail -eq 0 ] && echo "SMOKE OK" || echo "SMOKE FAILED"
echo "full log: $OUT"
exit $fail

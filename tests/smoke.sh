#!/usr/bin/env bash
# End-to-end check: drives the server over stdio the way an MCP client does.
# Usage: tests/smoke.sh [path-to-xspeccy-mcp]
set -u

BIN="${1:-$(dirname "$0")/../build/xspeccy-mcp}"
# One directory, and every working file named inside it. The obvious spelling
# is `mktemp --suffix=.png`, and it is a GNU extension: BSD mktemp, which is the
# one macOS ships, answers `unrecognized option` and prints nothing. The variable
# is then empty, every path built from it is the directory itself, and the suite
# fails seventeen checks in a row for a reason that has nothing to do with the
# server. Naming files inside one directory needs no extension anywhere.
OUT=$(mktemp)
TMPD=$(mktemp -d)
SHOT="$TMPD/shot.png"
WAV="$TMPD/capture.wav"
GIF="$TMPD/capture.gif"
MIX="$TMPD/blended.png"
LABELS="$TMPD/symbols.labels"
LST="$TMPD/source.lst"
TAP="$TMPD/tape.tap"
SPG="$TMPD/test.spg"
SDCARD="$TMPD/card.img"
ERR="$TMPD/stderr.txt"

# The emulator reads Xpeccy's own configuration when it finds one, which is the
# right behaviour for a user and the wrong one for a test: the expected values
# below were measured against the built-in defaults, so on a machine that has
# Xpeccy installed the romset and the screen geometry are somebody else's and
# three checks fail. An empty directory is a configuration that exists and says
# nothing, which pins the defaults without disabling the code path.
CFG="$TMPD/xpeccy-config"
mkdir -p "$CFG"
# Passed through native() like every other path the server is handed. This one
# is an argv rather than a string inside JSON, and MSYS2 usually converts those
# on the way through, but "usually" is not a thing to rely on when the failure
# is the server quietly reading a different directory.

# `stat -c%s` is GNU and `stat -f%z` is BSD. wc is neither.
filesize() { wc -c < "$1" | tr -d ' '; }

# Every path below travels inside a JSON string, so the shell never sees it again
# and cannot translate it. On Windows the server is a native program: the /tmp/x
# this shell means is C:\msys64\tmp\x, while the one the server opens is C:\tmp\x.
# Hand the server the native form and keep the shell form for our own checks.
if command -v cygpath >/dev/null 2>&1; then
	native() { cygpath -m "$1"; }
else
	native() { printf '%s' "$1"; }
fi
SHOT_N=$(native "$SHOT"); WAV_N=$(native "$WAV"); GIF_N=$(native "$GIF"); MIX_N=$(native "$MIX")
LABELS_N=$(native "$LABELS"); LST_N=$(native "$LST"); TAP_N=$(native "$TAP")
SPG_N=$(native "$SPG"); SDCARD_N=$(native "$SDCARD")

# One-block .tap: a 19-byte standard header, length word first. It exists so the
# tape block range has something real to be a range of - without a tape loaded,
# every block number is refused for the other reason.
printf '\x13\x00\x00\x03SMOKE     \x00\x00\x00\x00\x00\x00\x7c' > "$TAP"

# A minimal .spg: the 256-byte header ("SpectrumProg", format 1.0, pc=$8000,
# sp=$6000), the 768-byte block table, and one uncompressed 512-byte block
# into page 5 filled with A5. Small enough to write from a shell, real enough
# for the loader's signature, version and block walk to all engage.
{
	head -c 32 /dev/zero
	printf 'SpectrumProg'
	printf '\x10\x00\x00\x00'	# format + date
	printf '\x00\x80'		# pc
	printf '\x00\x60'		# sp
	printf '\x00\x04'		# page3, flags (interrupts on)
	head -c 4 /dev/zero		# pager/resident addresses
	printf '\x01\x00'		# one block
	head -c 196 /dev/zero		# rest of the header
	printf '\x80\x00\x05'		# last | offset 0 ; 512 bytes raw ; page 5
	head -c 765 /dev/zero
	# an ASCII fill, not \245 through tr: under a UTF-8 locale tr expands
	# bytes above 7F into multibyte and the block below decodes wrong
	head -c 512 /dev/zero | tr '\0' 'A'
} > "$SPG"

# A raw SD card image for the sdcard tool: 4MB is past the 1MB floor the
# controller protocol needs and small enough to create instantly.
dd if=/dev/zero of="$SDCARD" bs=1024 count=4096 2>/dev/null

# Two malformed .spg files: one cut a hundred bytes into its only block, and
# one whose block count walks past the fixed 256-entry descriptor table the
# core reads. Both used to "load" - the first copying uninitialised memory
# into RAM, the second reading past the array it indexed.
SPG_TRUNC="$TMPD/trunc.spg"; SPG_BAD="$TMPD/bad.spg"
head -c $((0x100 + 768 + 100)) "$SPG" > "$SPG_TRUNC"
cp "$SPG" "$SPG_BAD"
printf '\x02' | dd of="$SPG_BAD" bs=1 seek=$((0x3b)) conv=notrunc 2>/dev/null	# 513 blocks

# A card image past what the core's 32-bit capacity math survives: one sector
# past 1 GiB its doubling loop overflows a signed int. Sparse - no real disk
# spent.
SDCARD_HUGE="$TMPD/huge.img"
dd if=/dev/zero of="$SDCARD_HUGE" bs=512 count=0 seek=2097153 2>/dev/null
SPG_TRUNC_N=$(native "$SPG_TRUNC"); SPG_BAD_N=$(native "$SPG_BAD"); SDCARD_HUGE_N=$(native "$SDCARD_HUGE")
# Where the snapshot the file-machine test makes of itself is written.
SNAP="$TMPD/shot.sna"; SNAP_N=$(native "$SNAP")

# A BK-format .bin: little-endian load address and length, then the payload.
# The loader this API always had writes the low byte of each word at the even
# address (and refuses anything landing past $8000, the BK's own limit), so
# the four payload words land as every other byte starting at $6000.
BINFILE="$TMPD/prog.bin"; BINFILE_N=$(native "$BINFILE")
printf '\x00\x60\x08\x00\xAA\xBB\xCC\xDD\xEE\xFF\x00\x11' > "$BINFILE"

# A .td0 that ends where its header does: valid signature, density, side count
# and version, then nothing. The core's stream reader reports EOF as success
# to a track loop that only stops on the 0xff marker, so this used to be a
# call that never returned at all.
TD0_TRUNC="$TMPD/trunc.td0"; TD0_TRUNC_N=$(native "$TD0_TRUNC")
printf 'TD\x00\x00\x15\x00\x00\x00\x00\x01\x00\x00' > "$TD0_TRUNC"

# A real .td0: the same header, then one track carrying one raw 512-byte
# sector (dlen counts the block-type byte too), then the single 0xff that
# terminates the stream - a byte, not a record.
TD0_OK="$TMPD/ok.td0"; TD0_OK_N=$(native "$TD0_OK")
{
	printf 'TD\x00\x00\x15\x00\x00\x00\x00\x01\x00\x00'
	printf '\x01\x00\x00\x00'				# track: 1 sector, cyl 0, head 0
	printf '\x00\x00\x01\x02\x00\x00'			# sector header: C0 H0 R1, size 2 (512b)
	printf '\x01\x02'				# data length 513 = btype + 512 payload
	printf '\x00'					# btype 0: raw bytes follow
	head -c 512 /dev/zero
	printf '\xff'					# the lone-byte terminator
} > "$TD0_OK"

# And one with a weaponized size code: the sector is empty (ctrl 0x10), so no
# data follows - but the size code still reaches diskFormTrack, where
# 128 << 23 wraps the gap arithmetic positive and the track memset writes
# megabytes past the floppy buffer. Refused before the loader sees it.
TD0_BSZ="$TMPD/bsz.td0"; TD0_BSZ_N=$(native "$TD0_BSZ")
{
	printf 'TD\x00\x00\x15\x00\x00\x00\x00\x01\x00\x00'
	printf '\x03\x00\x00\x00'				# track: 3 sectors, cyl 0, head 0
	printf '\x00\x00\x01\x17\x10\x00'			# C0 H0 R1, size code 23, empty
	printf '\x00\x00\x02\x17\x10\x00'
	printf '\x00\x00\x03\x17\x10\x00'
	printf '\xff'
} > "$TD0_BSZ"

# Two .dsk files: one whose track-info declares 255 sectors (the table holds
# 29, and the core would keep reading sector descriptors off its stack), and
# one real single-track image to prove a good .dsk still lands on the +3.
DSK_BAD="$TMPD/bad.dsk"; DSK_OK="$TMPD/ok.dsk"
DSK_BAD_N=$(native "$DSK_BAD"); DSK_OK_N=$(native "$DSK_OK")
{
	printf 'MV - CPCEMU Disk-File\r\n'
	head -c $((0x30 - 23)) /dev/zero
	printf '\x01\x01\x00\x01'		# 1 track, 1 side, track length 0x100
	head -c $((0x100 - 0x34)) /dev/zero
	printf 'Track-Info\r\n'
	head -c $((0x15 - 12)) /dev/zero
	printf '\xff'				# secCount: the table tops out at 29
	head -c $((0x200 - 0x100 - 0x16)) /dev/zero
} > "$DSK_BAD"
{
	printf 'MV - CPCEMU Disk-File\r\n'
	head -c $((0x30 - 23)) /dev/zero
	printf '\x01\x01\x00\x03'		# 1 track, 1 side, track length 0x300
	head -c $((0x100 - 0x34)) /dev/zero
	printf 'Track-Info\r\n'
	head -c $((0x15 - 12)) /dev/zero
	printf '\x01'				# secCount 1: secCount 0 divides by zero upstream
	printf '\x00\x00'				# gap3Size + filler, TrackInfBlock ends at +0x17
	printf '\x00\x00\x01\x02\x00\x00\x00\x00'	# sib at +0x18: C0 H0 R1, size 2 -> 512 bytes
	head -c $((0x100 - 0x18 - 8)) /dev/zero
	head -c 512 /dev/zero			# the sector's data
} > "$DSK_OK"

# Same disk but the sector's size code is 23: the descriptor is copied into
# Sector.sz verbatim and diskFormTrack shifts 128 by it - the gap arithmetic
# wraps positive and the memset walks off the floppy's track buffer.
DSK_BSZ="$TMPD/bsz.dsk"; DSK_BSZ_N=$(native "$DSK_BSZ")
cp "$DSK_OK" "$DSK_BSZ"
printf '\x17' | dd of="$DSK_BSZ" bs=1 seek=$((0x100 + 0x18 + 3)) conv=notrunc 2>/dev/null

# Same weapon, softer hands: legal size code 6 (8K) on a DD drive - the
# track can never be packed into trklen 6250, so diskFormTrack quietly
# keeps the old track while the load reports success. A .td0 variant and
# an extended .dsk whose sector data is shorter than the 128<<(sz&3) the
# core writes into the track (the tail comes from a previous sector, and is
# blanked after the load).
DSK_FIT="$TMPD/fit.dsk"; DSK_FIT_N=$(native "$DSK_FIT")
DSK_SHORT="$TMPD/short.dsk"; DSK_SHORT_N=$(native "$DSK_SHORT")
TD0_FIT="$TMPD/fit.td0"; TD0_FIT_N=$(native "$TD0_FIT")
python3 - "$DSK_FIT" "$DSK_SHORT" "$TD0_FIT" <<'PY'
import sys, struct
# plain .dsk: 1 track, 1 sector, size code 6 -> 8192 counted, cannot fit 6250
f = open(sys.argv[1], 'wb')
f.write(b'MV - CPCEMU Disk-File\r\n' + bytes(0x30 - 23))
f.write(struct.pack('<BBH', 1, 1, 0x300))	# 1 trk, 1 side, rtlen
f.write(bytes(0x100 - 0x34))
f.write(b'Track-Info\r\n' + bytes(0x15 - 12) + bytes([1]) + bytes(2))
f.write(struct.pack('<6BH', 0, 0, 1, 6, 0, 0, 0))	# C0 H0 R1 sz6, sr12, len
f.write(bytes(0x100 - 0x18 - 8))
f.write(bytes(0x1800))				# slen for size>5 is 0x1800
f.close()
# extended .dsk: sz=3 means 1K written, but the file only carries 512 bytes
f = open(sys.argv[2], 'wb')
f.write(b'EXTENDED CPC DSK File\r\n' + bytes(0x30 - 23))
f.write(struct.pack('<BBH', 1, 1, 0))		# ext: rtlen lives in the table
f.write(bytes(0x34 - 0x34) + bytes([0x20]))	# rtlen = 0x2000
f.write(bytes(0x100 - 0x35))
f.write(b'Track-Info\r\n' + bytes(0x15 - 12) + bytes([1]) + bytes(2))
f.write(struct.pack('<6BH', 0, 0, 1, 3, 0, 0, 512))	# sz=3, slen=512
f.write(bytes(0x100 - 0x18 - 8))
f.write(bytes(512))
f.close()
# .td0: one sector, size code 6, carrying 512 raw bytes - valid to parse,
# impossible to fit on a DD track
f = open(sys.argv[3], 'wb')
f.write(b'TD\x00\x00\x15\x00\x00\x00\x00\x01\x00\x00')
f.write(b'\x01\x00\x00\x00')			# track: 1 sector
f.write(b'\x00\x00\x01\x06\x00\x00')		# C0 H0 R1, size code 6
f.write(struct.pack('<H', 513) + b'\x00')		# dlen 513: btype + 512 raw
f.write(bytes(512))
f.write(b'\xff')
f.close()
PY

# An .fdi whose only track declares zero sectors: diskFormTrack divides the
# track length by the sector count, so this used to be a SIGFPE.
FDI_ZERO="$TMPD/zero.fdi"; FDI_ZERO_N=$(native "$FDI_ZERO")
{
	printf 'FDI\x00\x01\x00\x01\x00\x00\x00\x00\x00\x00\x00'	# 1 cyl, 1 head
	printf '\x00\x00\x00\x00\x00\x00\x00'			# track 0/0: 0 sectors
} > "$FDI_ZERO"

# Two weaponized .tzx blocks. #11 declares a zero data length: the loader
# mallocs it and freads len-1 = SIZE_MAX into the 0-byte heap. #19's pilot
# table (asp x (1 + npp*2)) is read with no bound against the block - a
# one-byte buffer and a 255-pulse alphabet walks 506 bytes past the malloc.
TZX_11Z="$TMPD/zero.tzx"; TZX_19B="$TMPD/bad19.tzx"
TZX_11Z_N=$(native "$TZX_11Z"); TZX_19B_N=$(native "$TZX_19B")
{
	printf 'ZXTape!\x1a\x01\x20'
	printf '\x11'
	head -c 15 /dev/zero			# timings + used-bits + pause
	printf '\x00\x00\x00'				# datalen = 0
} > "$TZX_11Z"
{
	printf 'ZXTape!\x1a\x01\x20'
	printf '\x19'
	printf '\x13\x00\x00\x00'			# block len = 19 (14 fixed + 5)
	printf '\x00\x00'				# pause
	printf '\x01\x00\x00\x00'			# totp = 1
	printf '\xff\x01'				# npp 255, asp 1
	printf '\x01\x00\x00\x00'			# totd = 1
	printf '\x01\x01'				# npd 1, asd 1
	head -c 5 /dev/zero				# the block's data
} > "$TZX_19B"

# Header-only snapshots: the loaders fread the RAM pages unchecked and
# report success - a 27-byte .sna writes the caller's stack into emulated
# RAM. A 30-byte .z80 does the same.
SNA_27="$TMPD/head.sna"; Z80_30="$TMPD/head.z80"
SNA_27_N=$(native "$SNA_27"); Z80_30_N=$(native "$Z80_30")
head -c 27 /dev/zero > "$SNA_27"
{ head -c 6 /dev/zero; printf '\x01\x00'; head -c 22 /dev/zero; } > "$Z80_30"

# A .scl that is signature and file count only: the loader freads the
# missing descriptors anyway and formats a disk out of stack bytes.
SCL_TRUNC="$TMPD/trunc.scl"; SCL_TRUNC_N=$(native "$SCL_TRUNC")
printf 'SINCLAIR\x01' > "$SCL_TRUNC"

# A .tap whose second block is just its length word: the loader counts it
# as a real block built of the previous one's bytes.
TAP_TRUNC="$TMPD/trunc.tap"; TAP_TRUNC_N=$(native "$TAP_TRUNC")
{ cat "$TAP"; printf '\x13\x00'; } > "$TAP_TRUNC"

# A .udi whose one track record declares an unknown format: the core mallocs
# its track buffer and frees it only for type 0 - the rest leaks, unused.
UDI_BADT="$TMPD/badt.udi"; UDI_BADT_N=$(native "$UDI_BADT")
{
	printf 'UDI!\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
	printf '\x01\x00\x00\x00\x00'			# type 1, len 0
} > "$UDI_BADT"

# A 1-byte .trd and a truncated .ima: both loaders used to report failure
# (or worse, success) after the machine had already switched and cold-filled.
TRD_BAD="$TMPD/bad.trd"; TRD_BAD_N=$(native "$TRD_BAD")
IMA_SHORT="$TMPD/short.ima"; IMA_SHORT_N=$(native "$IMA_SHORT")
printf '\x00' > "$TRD_BAD"
head -c 1000 /dev/zero > "$IMA_SHORT"
# And a real 160-track .trd, all zeros: proves the good path still loads.
TRD_OK="$TMPD/ok.trd"; TRD_OK_N=$(native "$TRD_OK")
head -c $((160 * 4096)) /dev/zero > "$TRD_OK"

# A .tzx block 0x14 with a 0-length data field: like 0x11, the core still
# freads a last byte, eating the next block's id (or EOF) as tape data.
TZX_14Z="$TMPD/zero14.tzx"; TZX_14Z_N=$(native "$TZX_14Z")
{
	printf 'ZXTape!\x1a\x01\x20'
	printf '\x14'
	printf '\x57\x03\x56\x06'		# bit0 855, bit1 1622
	printf '\x08'				# used bits in last byte
	printf '\xe8\x03'			# pause 1000ms
	printf '\x00\x00\x00'			# datalen = 0
} > "$TZX_14Z"

# A 131103-byte 128K .sna whose paged-in bank is 2: with the current bank
# never in the load loop's {0,1,3,4,6,7}, upstream expects all six remaining
# pages - a real bank-2 save is 147487 bytes. This one is short.
SNA_B2="$TMPD/bank2.sna"; SNA_B2_N=$(native "$SNA_B2")
{ head -c 49181 /dev/zero; printf '\x02'; head -c $((131103 - 49182)) /dev/zero; } > "$SNA_B2"

# A 48K .z80 v2: hardware byte 0, three raw pages numbered 4/5/8 - the odd
# 48K layout z80_block_bank maps to banks 2/0/5. Valid by upstream rules.
Z80_48V="$TMPD/valid48.z80"; Z80_48V_N=$(native "$Z80_48V")
# Same file with a 22-byte extended header: malformed-but-tolerated, and
# upstream starts the page walk at 55 regardless - so must the validator.
Z80_H22="$TMPD/hlen22.z80"; Z80_H22_N=$(native "$Z80_H22")
# Duplicates first, real pages after: the loader's per-block count stops
# before the later pages, so the file must be refused, not half-loaded.
Z80_DUP="$TMPD/dupreal.z80"; Z80_DUP_N=$(native "$Z80_DUP")
python3 - "$Z80_48V" "$Z80_H22" "$Z80_DUP" <<'PY'
import sys, struct
hdr = bytearray(32)			# 30-byte header + hlen field
hdr[30:32] = struct.pack('<H', 23)
body = b''
for pg in (4, 5, 8):
    body += struct.pack('<H', 0xffff) + bytes([pg]) + bytes(0x4000)
open(sys.argv[1], 'wb').write(bytes(hdr) + bytes(23) + body)
hdr[30:32] = struct.pack('<H', 22)
open(sys.argv[2], 'wb').write(bytes(hdr) + bytes(23) + body)
# pages 4,4,4,5,8: distinct banks 2,0,5 are all nominally present, but the
# loader counts blocks - three duplicates zero it before page 5 is read
hdr[30:32] = struct.pack('<H', 23)
dup = b''.join(struct.pack('<H', 0xffff) + bytes([pg]) + bytes([v]) * 0x4000
	       for pg, v in ((4, 0x41), (4, 0x42), (4, 0x43), (5, 0x55), (8, 0x88)))
open(sys.argv[3], 'wb').write(bytes(hdr) + bytes(23) + dup)
PY

# A well-formed .fdi whose one track carries a size-6 (8K) sector: it parses
# fine but can never be packed into a DD track - upstream's diskFormTrack
# silently keeps the old track while the load reports success.
FDI_BIG="$TMPD/big.fdi"; FDI_BIG_N=$(native "$FDI_BIG")
python3 - "$FDI_BIG" <<'PY'
import sys, struct
f = open(sys.argv[1], 'wb')
f.write(b'FDI\x00' + struct.pack('<5H', 1, 1, 0, 28, 0))	# 1cyl 1head, data@28
f.write(struct.pack('<IHB', 0, 0, 1))			# toff=0, 1 sector
f.write(struct.pack('<5BH', 0, 0, 1, 6, 0, 0))		# sec 1, size code 6
f.write(bytes([0x5a]) * 8192)
f.close()
PY

# A .trd that ends mid-track: 159 full tracks of A5 plus one sector of 5A.
# The loader still formats the tail's other fifteen sectors - from the A5
# buffer the previous fread left; the fixup must blank them instead.
TRD_TAIL="$TMPD/tail.trd"; TRD_TAIL_N=$(native "$TRD_TAIL")
TRD_SAVED="$TMPD/tail-saved.trd"; TRD_SAVED_N=$(native "$TRD_SAVED")
python3 - "$TRD_TAIL" <<'PY'
import sys
open(sys.argv[1], 'wb').write(bytes([0xa5]) * (159 * 4096) + bytes([0x5a]) * 256)
PY

# Two .spg files with a compressed block (the pack flag in the descriptor's
# second byte): a stream of nothing but set bits as hrust, and a real
# megalz stream. MLZ flags are MSB-first bytes interleaved with the literal
# bytes they mark: literal 'A', then 0xFF (= "eight literals") and eight 'B's,
# repeated - it decodes inside the loader's buffers and must load. The hrust
# one does not and must be refused - before the machine switch, so the model
# stays.
SPG_CMP_OK="$TMPD/cmpok.spg"; SPG_CMP_FF="$TMPD/cmpff.spg"
SPG_CMP_OK_N=$(native "$SPG_CMP_OK"); SPG_CMP_FF_N=$(native "$SPG_CMP_FF")
cp "$SPG" "$SPG_CMP_OK"; cp "$SPG" "$SPG_CMP_FF"
# set the pack flag on the first (and only) descriptor at +0x101
printf '\x40' | dd of="$SPG_CMP_OK" bs=1 seek=$((0x101)) conv=notrunc 2>/dev/null
printf '\x80' | dd of="$SPG_CMP_FF" bs=1 seek=$((0x101)) conv=notrunc 2>/dev/null
# the declared payload is 512 bytes: overwrite the ASCII fill it carried. The
# stream is literals ('A' + 55 groups of flag 0xFF + eight 'B's = 441 of them)
# closed by a proper end marker, not by input exhaustion - a decoder hitting
# EOF means the core would keep reading past its malloc'd block, which is
# exactly what the bounded mirror refuses. Marker: flag byte 0xD8 gives the
# run two more literals then 0 to leave literal mode, 11 picks the long form,
# eight zero bits count the length to 9 = terminator, then one zero flag bit
# selects a one-byte offset read (0x01, unused).
{
	printf 'A'
	i=0
	while [ $i -lt 55 ]; do
		printf '\xffBBBBBBBB'
		i=$((i + 1))
	done
	printf '\xD8BB\x00\x01'
	head -c 11 /dev/zero
} | dd of="$SPG_CMP_OK" bs=1 seek=$((0x400)) conv=notrunc 2>/dev/null
head -c 512 /dev/zero | tr '\0' '\377' | dd of="$SPG_CMP_FF" bs=1 seek=$((0x400)) conv=notrunc 2>/dev/null

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
	# A Pentagon does not come up in BASIC: its reset bank lands on the 128K
	# menu (resbank=RES_128 in the definition, as the hardware behaves). The
	# way into 48 BASIC is the menu - three downs, an enter, and the long
	# self-test the selection starts.
	echo '{"jsonrpc":"2.0","id":231,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"down","frames":4,"release_frames":4}}}'
	echo '{"jsonrpc":"2.0","id":232,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"down","frames":4,"release_frames":4}}}'
	echo '{"jsonrpc":"2.0","id":233,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"down","frames":4,"release_frames":4}}}'
	echo '{"jsonrpc":"2.0","id":234,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"enter","frames":4,"release_frames":10}}}'
	echo '{"jsonrpc":"2.0","id":235,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":300}}}'
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
	# same menu walk again: reset lands on it every time
	echo '{"jsonrpc":"2.0","id":236,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"down","frames":4,"release_frames":4}}}'
	echo '{"jsonrpc":"2.0","id":237,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"down","frames":4,"release_frames":4}}}'
	echo '{"jsonrpc":"2.0","id":238,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"down","frames":4,"release_frames":4}}}'
	echo '{"jsonrpc":"2.0","id":239,"method":"tools/call","params":{"name":"press_key","arguments":{"key":"enter","frames":4,"release_frames":10}}}'
	echo '{"jsonrpc":"2.0","id":240,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":300}}}'
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
	echo '{"jsonrpc":"2.0","id":101,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$9400","lines":["top: dec a","jr nz,top","call sub","jr done","sub: ret","done: jp top"]}}}'
	echo '{"jsonrpc":"2.0","id":102,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$9400","lines":["c: nop"]}}}'
	echo '{"jsonrpc":"2.0","id":103,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$9400","lines":["dup: nop","dup: ret"]}}}'
	# A breakpoint on a bank that is not paged in at the time. Page 7 in, leave a
	# nop sled in it, page it back out, arm 07:0000 - and it must fire only once
	# the bank returns to the window. Armed against the CPU address instead, this
	# would have marked bank 0 and never fired.
	echo '{"jsonrpc":"2.0","id":127,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":128,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["ld bc,$7ffd","ld a,7","out (c),a","ret"]}}}'
	echo '{"jsonrpc":"2.0","id":129,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
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
	# di first - the wild run before this can leave IM 2 and a garbage I, and then
	# the digest's own frame services an interrupt through a vector that scribbles
	# over the very bytes it is about to hash
	echo '{"jsonrpc":"2.0","id":118,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"F3 76 18 FC"}}}'
	echo '{"jsonrpc":"2.0","id":119,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	# and the stack parked out of the window too, in case an NMI lands anyway
	echo '{"jsonrpc":"2.0","id":114,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"SP","value":"$F800"}}}'
	echo '{"jsonrpc":"2.0","id":120,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$9600","hex":"01 02 03 04 05 06 07 08"}}}'
	echo '{"jsonrpc":"2.0","id":121,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"from":"$9600","to":"$9607"}}}'
	echo '{"jsonrpc":"2.0","id":122,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$9603","hex":"FF"}}}'
	echo '{"jsonrpc":"2.0","id":123,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"from":"$9600","to":"$9607"}}}'
	echo '{"jsonrpc":"2.0","id":124,"method":"tools/call","params":{"name":"screen_digest","arguments":{"frames":1,"from":"$9600"}}}'
	# Xpeccy-plus loads the model's own layout with it, so the mismatch has to
	# be asked for - a ZX48 on Pentagon geometry still runs 69888 T a frame
	echo '{"jsonrpc":"2.0","id":125,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"ZX48K","geometry":"ULA.Pentagon","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":126,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'
	# Raster debugging. The fixture is our own border-flipping loop rather than
	# the ROM: after a reset the ROM spends its first frames in the RAM test with
	# interrupts off, so $0038 does not run and a log watching it stays empty for
	# reasons that have nothing to do with the log.
	#   $8000 di / $8001 ld a,16 / $8003 xor 16 / $8005 out (254),a
	#   $8007 ld b,60 / $8009 djnz $8009 / $800B jr $8003
	# $8005 therefore executes once per pass, many times a frame, forever.
	echo '{"jsonrpc":"2.0","id":150,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","lines":["di","ld a,16","xor 16","out (254),a","ld b,60","djnz 32777","jr 32771"]}}}'
	echo '{"jsonrpc":"2.0","id":151,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":152,"method":"tools/call","params":{"name":"beam_log","arguments":{"action":"enable","addresses":["$8005"]}}}'
	echo '{"jsonrpc":"2.0","id":153,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":40000}}}'
	echo '{"jsonrpc":"2.0","id":154,"method":"tools/call","params":{"name":"beam_log","arguments":{"action":"dump","count":3}}}'
	echo '{"jsonrpc":"2.0","id":155,"method":"tools/call","params":{"name":"beam_log","arguments":{"action":"enable"}}}'
	echo '{"jsonrpc":"2.0","id":156,"method":"tools/call","params":{"name":"run_to_beam","arguments":{"line":100}}}'
	echo '{"jsonrpc":"2.0","id":157,"method":"tools/call","params":{"name":"run_to_beam","arguments":{"line":99999}}}'
	echo '{"jsonrpc":"2.0","id":158,"method":"tools/call","params":{"name":"frame_digest","arguments":{"frames":2,"lines":true}}}'
	echo '{"jsonrpc":"2.0","id":159,"method":"tools/call","params":{"name":"beam_log","arguments":{"action":"disable"}}}'
	# A flicker to blend: every interrupt the attributes go from paper black to
	# paper bright white and back, which is a gigascreen with one colour in it.
	# A raw frame is one of the two and looks like neither; the blend of the two
	# is the grey a person watching the screen sees.
	# The machine has to have booted first: the Pentagon's rom serves IM1
	# through a trampoline at $5B00 its boot writes into RAM, and on cold ram
	# those bytes are whatever the power-on pattern left - an interrupt that
	# lands on noise is an rst #38 loop, not a flicker.
	echo '{"jsonrpc":"2.0","id":241,"method":"tools/call","params":{"name":"reset","arguments":{"boot_frames":150}}}'
	echo '{"jsonrpc":"2.0","id":160,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","text":"di\nim 1\nld hl,$4000\nld de,$4001\nld bc,6143\nld (hl),0\nldir\nxor a\nld ($9000),a\nei\nflick:\nld a,($9000)\nxor $78\nld ($9000),a\nld hl,$5800\nld de,$5801\nld bc,767\nld (hl),a\nldir\nhalt\njr flick"}}}'
	echo '{"jsonrpc":"2.0","id":161,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":162,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":20}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":163,\"method\":\"tools/call\",\"params\":{\"name\":\"screenshot\",\"arguments\":{\"border\":false,\"blend\":2,\"path\":\"$MIX_N\"}}}"
	# The mix itself, pinned. Black and white averaged in linear light is #B6B6B6
	# at gamma 2.2 - averaging the bytes instead would give #808080, and the whole
	# point of the blend is that it does not. A changed digest here means the
	# colour changed, which is a decision, not a detail.
	echo '{"jsonrpc":"2.0","id":164,"method":"tools/call","params":{"name":"frame_digest","arguments":{"frames":1,"border":false,"blend":2}}}'
	echo '{"jsonrpc":"2.0","id":165,"method":"tools/call","params":{"name":"video_config","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":166,"method":"tools/call","params":{"name":"video_config","arguments":{"blend":99}}}'
	# Settings: one table drives the report, the setter and the file, so the
	# checks below are on all three faces of the same row.
	echo '{"jsonrpc":"2.0","id":170,"method":"tools/call","params":{"name":"settings","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":171,"method":"tools/call","params":{"name":"settings","arguments":{"action":"set","name":"timing.contPattern","value":"b"}}}'
	echo '{"jsonrpc":"2.0","id":172,"method":"tools/call","params":{"name":"settings","arguments":{"action":"set","name":"video.gamma","value":9}}}'
	echo '{"jsonrpc":"2.0","id":173,"method":"tools/call","params":{"name":"settings","arguments":{"action":"set","name":"timing.contPattern","value":"purple"}}}'
	echo '{"jsonrpc":"2.0","id":174,"method":"tools/call","params":{"name":"settings","arguments":{"action":"set","name":"nope.nothing","value":1}}}'
	echo '{"jsonrpc":"2.0","id":175,"method":"tools/call","params":{"name":"settings","arguments":{"action":"reset","name":"timing.contPattern"}}}'
	echo '{"jsonrpc":"2.0","id":176,"method":"tools/call","params":{"name":"settings","arguments":{"action":"nonsense","name":"video.gamma"}}}'
	# Which screen page is on air. The program below writes a different attribute
	# byte into each of the two screens and then puts bank 7 on air with bit 3 of
	# $7FFD. Reading through the CPU's view would answer $38 here - bank 5 is the
	# one mapped at $4000 whatever the ULA is doing - so $28 is the whole point:
	# it is the screen a person is actually looking at.
	echo '{"jsonrpc":"2.0","id":180,"method":"tools/call","params":{"name":"assemble","arguments":{"address":"$8000","text":"di\nld bc,$7ffd\nld a,7\nout (c),a\nld hl,$d800\nld de,$d801\nld bc,767\nld (hl),$28\nldir\nld hl,$5800\nld de,$5801\nld bc,767\nld (hl),$38\nldir\nld bc,$7ffd\nld a,$0f\nout (c),a\nspin:\njr spin"}}}'
	echo '{"jsonrpc":"2.0","id":181,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":182,"method":"tools/call","params":{"name":"run_frames","arguments":{"count":3}}}'
	echo '{"jsonrpc":"2.0","id":183,"method":"tools/call","params":{"name":"beam_position","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":184,"method":"tools/call","params":{"name":"screen_attrs","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":185,"method":"tools/call","params":{"name":"screen_attrs","arguments":{"page":5}}}'
	echo '{"jsonrpc":"2.0","id":186,"method":"tools/call","params":{"name":"screen_attrs","arguments":{"page":999}}}'

	# TSConf and its storage: the machine at the far end of the configuration
	# system - 4M of RAM in 256-byte pages, its own layout, an SD host. And the
	# .spg snapshot format that only exists there, plus "bank:offset" reads of
	# physical RAM pages that the CPU mapping never shows.
	echo '{"jsonrpc":"2.0","id":210,"method":"tools/call","params":{"name":"sdcard","arguments":{"action":"state"}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":211,\"method\":\"tools/call\",\"params\":{\"name\":\"sdcard\",\"arguments\":{\"action\":\"mount\",\"path\":\"$SDCARD_N\"}}}"
	echo '{"jsonrpc":"2.0","id":212,"method":"tools/call","params":{"name":"sdcard","arguments":{"action":"lock"}}}'
	echo '{"jsonrpc":"2.0","id":213,"method":"tools/call","params":{"name":"sdcard","arguments":{"action":"mount","path":"/no/such/card.img"}}}'
	echo '{"jsonrpc":"2.0","id":214,"method":"tools/call","params":{"name":"sdcard","arguments":{"action":"bogus"}}}'
	echo '{"jsonrpc":"2.0","id":215,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"TSConf","boot_frames":5}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":216,\"method\":\"tools/call\",\"params\":{\"name\":\"sdcard\",\"arguments\":{\"action\":\"mount\",\"path\":\"$SDCARD_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":217,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SPG_N\"}}}"
	echo '{"jsonrpc":"2.0","id":218,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"5:0","length":4}}}'
	echo '{"jsonrpc":"2.0","id":219,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"100:0","length":4}}}'
	echo '{"jsonrpc":"2.0","id":220,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"5:01FC","length":4}}}'
	echo '{"jsonrpc":"2.0","id":221,"method":"tools/call","params":{"name":"sdcard","arguments":{"action":"eject"}}}'
	# malformed .spg and oversized card images are refused, not half-applied;
	# and a stem name must resolve to its own definition, not the first that
	# shares the hardware (zxplus2 and zx128 are both ZX128 boards)
	echo "{\"jsonrpc\":\"2.0\",\"id\":223,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SPG_TRUNC_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":224,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SPG_BAD_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":225,\"method\":\"tools/call\",\"params\":{\"name\":\"sdcard\",\"arguments\":{\"action\":\"mount\",\"path\":\"$SDCARD_HUGE_N\"}}}"
	echo '{"jsonrpc":"2.0","id":226,"method":"tools/call","params":{"name":"read_memory","arguments":{"length":4}}}'
	echo '{"jsonrpc":"2.0","id":227,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"100000000:0","length":4}}}'
	echo '{"jsonrpc":"2.0","id":228,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"zxplus2","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":222,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'
	# the file decides the machine (xcore/filemachine.cpp fm_pick): a 128K
	# snapshot has nowhere to run on a ZX48, so loading one takes the server
	# to zx128, and an .spg already proved it lands on evo-tsconf. The
	# upstream alias for the same board is answered too.
	echo "{\"jsonrpc\":\"2.0\",\"id\":229,\"method\":\"tools/call\",\"params\":{\"name\":\"save_snapshot\",\"arguments\":{\"path\":\"$SNAP_N\"}}}"
	echo '{"jsonrpc":"2.0","id":230,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"zx48","boot_frames":0}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":242,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SNAP_N\"}}}"
	echo '{"jsonrpc":"2.0","id":243,"method":"tools/call","params":{"name":"machine_state","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":244,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"TSLab","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":245,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'

	# A .bin keeps the old contract: bytes into RAM on the machine that is
	# already here (FMN_ANY - it switches nothing). The low byte of each
	# payload word lands at the even address, so FF-written bytes survive
	# between them as proof the odd ones were never touched.
	echo '{"jsonrpc":"2.0","id":246,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$6000","hex":"FF FF FF FF FF FF FF FF"}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":247,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$BINFILE_N\"}}}"
	echo '{"jsonrpc":"2.0","id":248,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"$6000","length":8}}}'
	# Malformed disk images are refused by the adapter before the core's
	# parsers - which hang or overflow - can see them. The good .dsk also
	# proves the file machine lands a Plus3DOS image on the one board that
	# has the drive for it.
	echo "{\"jsonrpc\":\"2.0\",\"id\":249,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TD0_TRUNC_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":250,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_BAD_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":251,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_OK_N\"}}}"
	# a real td0 from a machine with the wrong controller: zxplus3 has the
	# +3 drive, not Beta Disk, so the file picks the Pentagon back up
	echo "{\"jsonrpc\":\"2.0\",\"id\":253,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TD0_OK_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":258,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TD0_BSZ_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":259,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_BSZ_N\"}}}"
	# round-11: tracks the drive cannot pack must be refused, not formatted
	# silently over the old disk - and an ext .dsk sector shorter than the
	# bytes written into the track loads with the stale tail blanked
	echo "{\"jsonrpc\":\"2.0\",\"id\":283,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_OK_N\",\"drive\":4}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":284,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_OK_N\",\"drive\":-1}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":280,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_FIT_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":281,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TD0_FIT_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":282,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_SHORT_N\"}}}"
	# round-3 malformed media: each of these used to be a crash, a hang, a
	# leak or a fabricated disk reported as loaded - all refused before the
	# machine switch now
	echo "{\"jsonrpc\":\"2.0\",\"id\":260,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$FDI_ZERO_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":261,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TZX_11Z_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":262,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TZX_19B_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":263,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SNA_27_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":264,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$Z80_30_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":265,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SCL_TRUNC_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":266,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TAP_TRUNC_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":267,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$UDI_BADT_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":268,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TRD_BAD_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":269,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$IMA_SHORT_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":270,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TRD_OK_N\"}}}"
	echo '{"jsonrpc":"2.0","id":271,"method":"tools/call","params":{"name":"disk","arguments":{"action":"state","drive":0}}}'
	# round-7 fixes: the 48K z80 page map (4/5/8, not page-3) and the
	# tolerated 22-byte v2 header; a bank-2 sna of the wrong size; a
	# zero-length pure-data tzx block
	echo "{\"jsonrpc\":\"2.0\",\"id\":272,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$Z80_48V_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":273,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$Z80_H22_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":279,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$Z80_DUP_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":274,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SNA_B2_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":275,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TZX_14Z_N\"}}}"
	# round-8: a partial-tail .trd loads, its fabricated tail is blanked, and
	# disk save writes zeros where the file ended instead of copied bytes;
	# an unformattable .fdi (an 8K sector on a DD drive) is refused instead
	# of reporting success over the old track
	echo "{\"jsonrpc\":\"2.0\",\"id\":276,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TRD_TAIL_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":277,\"method\":\"tools/call\",\"params\":{\"name\":\"disk\",\"arguments\":{\"action\":\"save\",\"path\":\"$TRD_SAVED_N\"}}}"
	echo "{\"jsonrpc\":\"2.0\",\"id\":278,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$FDI_BIG_N\"}}}"
	# compressed .spg blocks are exercised by the bounded mirrors now: the
	# crafted-literals mlz stream decodes inside the loader's buffers and
	# lands on TSConf, the all-ones hrust stream cannot and is refused -
	# before the machine switch, so the model stays
	echo "{\"jsonrpc\":\"2.0\",\"id\":254,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SPG_CMP_OK_N\"}}}"
	echo '{"jsonrpc":"2.0","id":257,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"5:0","length":4}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":255,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$SPG_CMP_FF_N\"}}}"
	echo '{"jsonrpc":"2.0","id":256,"method":"tools/call","params":{"name":"machine_state","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":252,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'

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
	# A budget that turns the loop off rather than shortening it. execLoop reads
	# a negative maximum as no maximum, so this used to be a call that never
	# came back, and a reviewer trying one negative number found it before we
	# did. Both budget arguments, because they are read in different places.
	echo '{"jsonrpc":"2.0","id":200,"method":"tools/call","params":{"name":"screen_digest","arguments":{"skip_until":"$FFF0","skip_max_instructions":-1}}}'
	echo '{"jsonrpc":"2.0","id":201,"method":"tools/call","params":{"name":"frame_cost","arguments":{"max_instructions":-1}}}'
	echo '{"jsonrpc":"2.0","id":202,"method":"tools/call","params":{"name":"trace","arguments":{"action":"dump","count":1048576}}}'
	# Requests that are valid JSON and not valid JSON-RPC. Reading a field off
	# the wrong shape throws, and a throw used to reach the read loop, which
	# logged it and sent nothing - leaving a client that had sent an id waiting
	# for an answer that was never coming.
	echo '[1,2]'
	echo '42'
	echo '{"jsonrpc":"2.0","id":203,"method":5}'
	echo '{"jsonrpc":"2.0","id":204,"method":"tools/call","params":"nonsense"}'
	echo '{"jsonrpc":"2.0","id":205,"method":"tools/call","params":{"name":5}}'
	# and the stream carries on afterwards
	echo '{"jsonrpc":"2.0","id":44,"method":"ping"}'
} | "$BIN" --config "$(native "$CFG")" > "$OUT" 2>"$ERR"
SERVER_RC=${PIPESTATUS[1]}

fail=0
check() {	# check <label> <jq-ish grep pattern>
	if grep -q "$2" "$OUT"; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		fail=1
	fi
}

# check() searches the whole transcript, which is enough when the pattern could
# only have come from one answer and too little when it could not. This one
# looks inside the reply to one request, so "the machine survived that" cannot
# be satisfied by an identical-looking answer from earlier in the session.
# Replies are one object per line and nlohmann sorts the keys, so the id is
# always "id":N followed by a comma.
check_id() {	# check_id <label> <request id> <pattern>
	local line
	line=$(grep "\"id\":$2," "$OUT" | head -1)
	if [ -z "$line" ]; then
		echo "  FAIL $1 (no reply to id $2)"
		fail=1
	elif printf '%s' "$line" | grep -q -e "$3"; then	# -e: a pattern may start with a dash
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		printf '       %s\n' "$(printf '%s' "$line" | cut -c1-160)"
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
# 90.0 per call against 44.0 self - exact T now, counted from the core's own
# tick delta rather than divided back out of rounded nanoseconds
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
check "geometry warning"  'but a ZX48 runs 69888'
# "ld hl, $1234" and "ld a, (ix + 5)" - spaces where a source file puts them
check "asm spaced comma"  '"hex\\": \\"21 34 12'
check "asm spaced index"  '"hex\\": \\"DD 7E 05'
# md5 of 01..08, then the same eight bytes with $9603 poked to FF
check "memory digest"     '"digest\\": \\"0ee0646c1c77'
check "memory digest sees a poke" '"digest\\": \\"1bc531b4312b'
check "digest range halved" 'give both from and to'
check "beta disk present" '"interface\\": \\"Beta Disk'
check "bad model refused" 'unknown machine\|not usable here'
# Against the machine_state that follows the refused GameBoy, not against any
# Pentagon in the transcript: the first reply of the session is also a Pentagon,
# so the loose form of this check passed before the thing it tests even ran.
check_id "server survived"   100 'Pentagon'

# Settings. The report is the only way to ask what the machine is set to, and
# every value has to say where it came from.
check "settings report"    '"count\\": 19'
check "settings source"    '"source\\": \\"core default'
check "settings scope"     '"scope\\": \\"needs reset'
check "settings set"       '"source\\": \\"this session'
check "settings warns on timing" 'no longer comparable'
# Refused, never clamped: a silently corrected setting does not do what it says.
check "settings range"     'video.gamma: 9 is outside'
check "settings enum"      "not one of: none, a, b"
check "settings unknown"   'no such setting: nope.nothing'
check "settings action"    'action must be report, set or reset'
check "settings reset"     '"value\\": \\"a\\"'
# Blending: telling flicker from animation is the difference between undoing a
# flicker and quietly returning motion blur.
check "blend pattern"      '"pattern\\": \\"'


# The orientation text sent with initialize. A tool description says what one
# tool does; this is the only place the server says which to reach for, so its
# quiet disappearance would cost an agent more than a missing tool would.
check "initialize instructs" '"instructions":"A ZX Spectrum'
check "instructs on digests" 'depends on screen memory AND on when the bank is switched'

# Raster debugging. 40000 instructions of a 64-instruction loop is 625 passes,
# and that number depends on the loop alone, not on anything run before it.
check "beam log records" '"events_total\\": 625'
check "beam log address" '"address_hex\\": \\"\$8005'
check "beam log jitter"  '"jitter_t\\":'
check "beam log needs addresses" 'enable needs addresses\[\]'
# The beam only moves between instructions, so landing past the target is
# normal - landing on a different line is not.
check "run_to_beam lands"  '"line\\": 100,[^}]*"t_states_frame'
check "run_to_beam exact"  '"overshoot_lines\\": 0'
check "run_to_beam range"  'bad line: 99999 is outside 0\.\.319'
check "stop reports beam"  '"beam\\": {'
# Pentagon draws 320x240 with the border on (Xpeccy-plus' medium border mode is
# 32x24 dots; upstream's continuous half-border measured 320x248); the digest is
# of the drawn frame, so an unchanging picture repeats and says which it matched.
check "frame_digest size"  '"width\\": 320,\|"height\\": 240,'
check "frame_digest repeats" '"same_as_frame\\": 0'
check "frame_digest lines" '"changed_lines\\":'

# Blending, which is what makes a picture that flickers on purpose readable.
check "beam knows the page" '"screen_page\\": 7'
check "beam blank coords"   '"blank_x\\":'
check "beam locates INT"    '"interrupt_at\\":'
check "attrs follow the ULA" '"page_on_air\\": 7'
check "attrs read page 7"   '"attributes\\": \\"28 28 28'
check "attrs page 5 asked"  '"attributes\\": \\"38 38 38'
check "page range refused"  'is outside 0..255'
check "video_config"      '"frame_history\\":'
check "blend refused"     'bad blend: 99'
check "blend saw two frames" '"distinct_frames\\": 2'
check "blend mixes in linear light" '"digest\\": \\"45760c0fa5f8'

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
# $ERR, not a path nothing writes to. This check spent its whole life grepping
# /tmp/xspeccy-smoke.err, which the suite has never created, so it passed
# whatever the server did on the way out - the one thing it exists to catch.
if grep -qE 'double free|corruption|Aborted|munmap_chunk|Segmentation' "$ERR"; then
	echo "  FAIL the allocator complained on the way out"
	sed -n '/double free\|corruption\|Aborted\|munmap_chunk\|Segmentation/p' "$ERR" | sed 's/^/       /' | head -3
	fail=1
else
	echo "  ok   exits clean"
fi

# And the status itself. Answering every request is not the same as ending well:
# a crash after the last reply leaves a full transcript behind.
if [ "${SERVER_RC:-1}" = "0" ]; then
	echo "  ok   server exited 0"
else
	echo "  FAIL server exited $SERVER_RC"
	fail=1
fi

# screen_digest: the same screen must hash the same, a changed one must not.
# Digests appear in file order: 3 from id 66, then one each from 68, 70 and 72.
# mapfile arrived in bash 4; macOS still ships 3.2, where it is a syntax error
# that takes DIG and every check below it with it.
DIG=()
while IFS= read -r _line; do DIG+=("$_line"); done < <(grep -o 'digest\\": \\"[0-9a-f]\{12\}' "$OUT" | sed 's/.*\\"//')
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
	echo "  ok   png written: $SHOT ($(filesize "$SHOT") bytes)"
else
	echo "  FAIL png missing"
	fail=1
fi

if [ -s "$MIX" ] && head -c 4 "$MIX" | tail -c 3 | grep -q PNG; then
	echo "  ok   blended png written: $MIX"
else
	echo "  FAIL blended png missing"
	fail=1
fi

if [ -s "$WAV" ]; then
	echo "  ok   wav written: $WAV ($(filesize "$WAV") bytes)"
else
	echo "  FAIL wav missing"
	fail=1
fi

if [ -s "$GIF" ] && head -c 6 "$GIF" | grep -q GIF89a; then
	echo "  ok   gif written: $GIF ($(filesize "$GIF") bytes)"
else
	echo "  FAIL gif missing or malformed"
	fail=1
fi

# the partial .trd's tail: track 159 carries one 5A sector from the file and
# fifteen sectors that were never in it - those must read as zeros, not as
# the previous track's bytes the loader's buffer still held
if python3 - "$TRD_SAVED" <<'PY'
import sys
try:
    d = open(sys.argv[1], 'rb').read()
except OSError:
    sys.exit(1)
t = 159 * 4096
ok = (len(d) >= t + 4096 and d[t:t + 256] == b'\x5a' * 256 and
      d[t + 256:t + 4096] == bytes(3840))
sys.exit(0 if ok else 1)
PY
then
	echo "  ok   partial trd tail saved as zeros"
else
	echo "  FAIL partial trd tail kept the buffer's old bytes"
	fail=1
fi

# ---- budgets and malformed requests -----------------------------------------
#
# Both of these are things a reviewer finds by typing one wrong value, so they
# are checked against the reply to that exact request rather than against the
# transcript as a whole.
check_id "sdcard empty"              210 '"mounted\\": false'
check_id "sdcard mounted"            211 '"mounted\\": true'
check_id "sdcard capacity"           211 '"capacity_mb\\": 4'
check_id "sdcard locked"             212 '"locked\\": true'
check_id "sdcard mount missing"      213 'no such image file'
check_id "sdcard bad action"         214 'unknown sdcard action'
check_id "tsconf model"              215 '"model\\": \\"TSConf'
check_id "tsconf 4M ram"             215 '"memory_kb\\": 4096'
check_id "tsconf geometry"           215 'ULA\.TSConf'
check_id "tsconf roms"               215 'machine:evo-tsconf'
check_id "sdcard on tsconf"          216 '"mounted\\": true'
check_id "sdcard remount unlocks"    216 '"locked\\": false'
check_id "spg loads"                 217 'pc_hex\\": \\"\$8000'
check_id "spg names its machine"     217 '"machine\\": \\"evo-tsconf'
check_id "spg wrote page 5"          218 '41 41 41 41'
check_id "bank out of range"         219 'no RAM bank 256'
check_id "bank tail reads"           220 '41 41 41 41'
check_id "sdcard ejected"            221 '"mounted\\": false'
check_id "sdcard fields cleared"     221 '"capacity_mb\\": 0'
check_id "truncated spg refused"     223 'truncated \.spg'
check_id "bad block count refused"   224 'block count 513'
check_id "huge sdcard refused"       225 'image too large'
check_id "address required"          226 'address is required'
check_id "oversized bank refused"    227 'no RAM bank'
check_id "zxplus2 own definition"    228 '"machine\\": \\"zxplus2'
check_id "back on pentagon"          222 'Pentagon'
check_id "snapshot written"          229 '"ok\\": true'
check_id "switched to zx48"          230 '"machine\\": \\"zx48'
check_id "sna picked its machine"    242 '"machine\\": \\"zx128'
check_id "state reports it too"      243 '"machine\\": \\"zx128'
check_id "tslab alias resolves"      244 '"machine\\": \\"evo-tsconf'
check_id "bin stayed on pentagon"    247 '"machine\\": \\"pent'
check_id "bin writes even bytes"     248 'AA FF CC FF EE FF 00 FF'
check_id "truncated td0 refused"     249 'truncated \.td0'
check_id "255-sector dsk refused"    250 'overrun the track-info'
check_id "good dsk loads"            251 '"ok\\": true'
check_id "dsk lands on plus3"        251 '"machine\\": \\"zxplus3'
check_id "good td0 loads"            253 '"ok\\": true'
check_id "0-sector fdi refused"      260 'declares 0 sectors'
check_id "0-length tzx refused"      261 '0-length data field'
check_id "bad 0x19 tzx refused"      262 'pilot table'
check_id "header-only sna refused"   263 'truncated \.sna'
check_id "header-only z80 refused"   264 'truncated \.z80'
check_id "dangling scl refused"      265 'truncated \.scl'
check_id "dangling tap refused"      266 'truncated \.tap'
check_id "unknown udi refused"       267 'unknown format'
check_id "1-byte trd refused"        268 'bad \.trd'
check_id "short ima refused"         269 'truncated \.ima'
check_id "good trd loads"            270 '"ok\\": true'
check_id "trd lands on pentagon"     270 '"machine\\": \\"pent'
check_id "trd inserted"              271 '"inserted\\": true'
check_id "48k z80 pages load"        272 '"ok\\": true'
check_id "hlen-22 z80 tolerated"     273 '"ok\\": true'
check_id "dup-then-real z80 refused" 279 'duplicate pages'
check_id "drive 4 refused"           283 'drive must be 0..3'
check_id "drive -1 refused"          284 'drive must be 0..3'
check_id "oversized dsk refused"     280 'bytes of room'
check_id "oversized td0 refused"     281 'bytes of room'
check_id "short ext dsk loads"       282 '"ok\\": true'
check_id "bank-2 sna refused"        274 'cannot hold this 128K image'
check_id "0-len tzx 0x14 refused"    275 '0-length data field'
check_id "partial trd loads"         276 '"ok\\": true'
check_id "saved trd written"         277 '"path\\":'
check_id "unformattable fdi refused" 278 'bytes of room'
check_id "td0 lands on pentagon"     253 '"machine\\": \\"pent'
check_id "size-23 td0 refused"       258 'size code 23'
check_id "size-23 dsk refused"       259 'size code 23'
check_id "compressed spg loads"      254 '"ok\\": true'
check_id "spg lands on tsconf"       254 '"machine\\": \\"evo-tsconf'
check_id "mlz literals landed"       257 '"hex\\": \\"41 42 42 42'
check_id "malformed spg refused"     255 'cannot be unpacked'
check_id "refusal left machine"      256 '"machine\\": \\"evo-tsconf'

check_id "negative skip budget refused"   200 "outside 1\.\."
check_id "negative frame budget refused"  201 "outside 1\.\."
check_id "oversized trace dump refused"   202 "outside 1\.\."
check_id "non-string method refused"      203 "-32600"
check_id "non-object params refused"      204 "-32602"
check_id "tools/call without a name"      205 "params.name"
check_id "the stream carried on"           44 '"result"'

# ---- the command line ------------------------------------------------------
#
# Not the protocol, but the same class of mistake: a server started with a
# misspelt option used to come up on a configuration nobody asked for and say
# nothing about it. These are the only checks that run the binary without
# speaking JSON to it.
cli() {	# cli <label> <expected exit> <pattern> <args...>
	local label="$1" want="$2" pattern="$3"; shift 3
	local out rc
	out=$("$BIN" "$@" 2>&1); rc=$?
	if [ "$rc" = "$want" ] && printf '%s' "$out" | grep -q "$pattern"; then
		echo "  ok   $label"
	else
		echo "  FAIL $label (exit $rc, wanted $want)"
		printf '%s\n' "$out" | sed 's/^/       /' | head -3
		fail=1
	fi
}

echo
# A failed load must leave the session alone: patching zxplus3's disk away
# makes .dsk unloadable on every machine, and the check used to be a switch -
# each candidate cold-filled RAM, so refusing the file also wiped it. Nothing
# is applied while definitions are judged now, so the byte and the machine
# both stay put.
CFG2="$TMPD/cfg-nop3"; mkdir -p "$CFG2/machines"
printf '[storage]\ndisk = none\n' > "$CFG2/machines/zxplus3.conf"
OUT2=$(mktemp)
{
	echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}'
	echo '{"jsonrpc":"2.0","method":"notifications/initialized"}'
	echo '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$6000","hex":"AA"}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$DSK_OK_N\"}}}"
	echo '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"$6000","length":1}}}'
	echo '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"machine_state","arguments":{}}}'
	# a rejected .trd used to reach the Pentagon switch and cold-fill anyway:
	# the size rule now runs before the file machine is even asked
	echo "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\",\"params\":{\"name\":\"load_file\",\"arguments\":{\"path\":\"$TRD_BAD_N\"}}}"
	echo '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"$6000","length":1}}}'
} | "$BIN" --config "$(native "$CFG2")" > "$OUT2" 2>/dev/null
check2() {	# check2 <label> <request id> <pattern> - check_id against $OUT2
	local line
	line=$(grep "\"id\":$2," "$OUT2" | head -1)
	if [ -n "$line" ] && printf '%s' "$line" | grep -q -e "$3"; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		[ -n "$line" ] && printf '       %s\n' "$(printf '%s' "$line" | cut -c1-160)"
		fail=1
	fi
}
check2 "unloadable dsk refused"      12 'needs a machine'
check2 "failed load kept the byte"   13 'hex\\": \\"AA'
check2 "failed load kept pentagon"   14 '"machine\\": \\"pent'
check2 "bad trd refused pre-switch"  15 'bad \.trd'
check2 "bad trd kept the byte"       16 'hex\\": \\"AA'

# Media whose cost is out of proportion to the file. A .tzx is a program the
# reader runs, a TapeSignal per pulse in buffers it never checks: four bytes
# of #12 in a 65535-pass loop are four billion pulses, a one-symbol #19
# alphabet emits its whole count without reading data, a #18 CSW stream
# inflates without a cap - each refused before the core allocates, while a
# looping tape of real size still loads. A disk too big for the DD drive is
# refused before the machine switch, so memory, PC and machine stay. An .ima
# leaves its drive HD: eject and any DD image take it back to DD, or the next
# .trd is formatted at 17700 and an .fdi too big for DD slips in. A dsk length
# entry of 0x80 and up is read signed by the core, so where the next track
# comes from depends on the host; a short ext .dsk sector loads with its tail
# blanked, not holding the previous track's bytes. An SD image of exactly
# 1 GiB is within the core's 32-bit card sizing.
MED_BOMB="$TMPD/bomb.tzx"; MED_SYM="$TMPD/sym.tzx"; MED_CSW="$TMPD/csw.tzx"
MED_TZX="$TMPD/loop.tzx"; MED_FDI="$TMPD/big11k.fdi"; MED_IMA="$TMPD/hd.ima"
MED_SIGNED="$TMPD/signed.dsk"; MED_TAIL="$TMPD/shorttail.dsk"
MED_SAVED="$TMPD/shorttail-saved.trd"; MED_SD1G="$TMPD/card1g.img"
python3 - "$TMPD" <<'PY'
import sys, struct, zlib, os
d = sys.argv[1]
H = b'ZXTape!\x1a\x01\x20'
w = lambda n, b: open(os.path.join(d, n), 'wb').write(b)
w('bomb.tzx', H + b'\x24' + struct.pack('<H', 65535) +
  b'\x12' + struct.pack('<HH', 1000, 65535) + b'\x25')
# #19: no pilot, totd 2^31-1 data symbols of a one-symbol (zero-bit) alphabet
w('sym.tzx', H + b'\x19' + struct.pack('<IHIBBIBB', 17, 0, 0, 0, 0, 0x7fffffff, 1, 1) +
  b'\x00\xe8\x03')
# #18 compression 2: 72 MiB of one-sample pulses, deflated to some 70K
z = zlib.compressobj(9)
csw = b''.join(z.compress(b'\x01' * 0x100000) for _ in range(72)) + z.flush()
w('csw.tzx', H + b'\x18' + struct.pack('<IHHBBI', 10 + len(csw), 0, 44100, 0, 2, 0) + csw)
hdr = bytes([0, 3]) + b'LOOP      ' + struct.pack('<HHH', 2, 0x8000, 0x8000)
ck = 0
for b in hdr: ck ^= b
w('loop.tzx', H + b'\x10' + struct.pack('<HH', 1000, len(hdr) + 1) + hdr + bytes([ck]) +
  b'\x24' + struct.pack('<H', 10) + b'\x12' + struct.pack('<HH', 2168, 100) + b'\x25' +
  b'\x20' + struct.pack('<H', 500))
# one track of ten 1K sectors: 10240 + 82 per sector = 11060 bytes of room
f = b'FDI\x00' + struct.pack('<5H', 1, 1, 0, 14 + 7 + 70, 0) + struct.pack('<IHB', 0, 0, 10)
f += b''.join(struct.pack('<5BH', 0, 0, s + 1, 3, 0, s * 1024) for s in range(10))
w('big11k.fdi', f + b'\x5a' * 10240)
w('hd.ima', bytes(1474560))
def ext_dsk(name, tracks, sides, recs):
    h = b'EXTENDED CPC DSK File\r\nDisk-Info\r\n'
    h += bytes(0x30 - len(h)) + bytes([tracks, sides, 0, 0]) + bytes(r[0] for r in recs)
    w(name, h + bytes(0x100 - len(h)) + b''.join(r[1] for r in recs))
def tib(trk, side, secs):
    t = b'Track-Info\r\n' + bytes(4) + bytes([trk, side, 0, 0, 1, len(secs), 0x4e, 0xe5])
    t += b''.join(struct.pack('<6BH', trk, side, r, n, 0, 0, len(x)) for r, n, x in secs)
    return t + bytes(0x100 - len(t)) + b''.join(x for _, _, x in secs)
body = tib(0, 0, [(1, 2, bytes(512))])
ext_dsk('signed.dsk', 1, 1, [(0x80, body + bytes(0x8000 - len(body)))])
# TR-DOS shaped, 80x2x16x256 so disk save can write it back as a .trd;
# track 0 sector 1 is AA, track 1 sector 1 carries 100 bytes of 55
recs = []
for i in range(160):
    secs = [(r, 1, b'\xaa' * 256 if (i, r) == (0, 1) else
             b'\x55' * 100 if (i, r) == (1, 1) else bytes(256)) for r in range(1, 17)]
    t = tib(i >> 1, i & 1, secs)
    recs.append((0x11, t + bytes(0x1100 - len(t))))
ext_dsk('shorttail.dsk', 80, 2, recs)
PY
dd if=/dev/zero of="$MED_SD1G" bs=1048576 count=0 seek=1024 2>/dev/null
OUTM="$TMPD/media.out"
mreq() {	# mreq <id> <tool> <arguments json>
	echo "{\"jsonrpc\":\"2.0\",\"id\":$1,\"method\":\"tools/call\",\"params\":{\"name\":\"$2\",\"arguments\":$3}}"
}
{
	echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}'
	echo '{"jsonrpc":"2.0","method":"notifications/initialized"}'
	mreq 10 machine_config '{"model":"zx48","boot_frames":0}'
	mreq 11 write_memory '{"address":"$8000","hex":"01 02 03 04 05 06 07 08"}'
	mreq 12 set_register '{"name":"PC","value":"$1234"}'
	mreq 13 load_file "{\"path\":\"$(native "$MED_FDI")\"}"
	mreq 14 read_memory '{"address":"$8000","length":8}'
	mreq 15 get_registers '{}'
	mreq 16 machine_state '{}'
	mreq 17 load_file "{\"path\":\"$(native "$MED_BOMB")\"}"
	mreq 18 load_file "{\"path\":\"$(native "$MED_SYM")\"}"
	mreq 19 load_file "{\"path\":\"$(native "$MED_CSW")\"}"
	mreq 20 load_file "{\"path\":\"$(native "$MED_TZX")\"}"
	mreq 21 tape '{"action":"state"}'
	mreq 30 machine_config '{"model":"Pentagon","boot_frames":0}'
	mreq 31 disk "{\"action\":\"insert\",\"path\":\"$(native "$MED_IMA")\"}"
	mreq 32 disk '{"action":"eject"}'
	mreq 33 disk "{\"action\":\"insert\",\"path\":\"$TRD_OK_N\"}"
	mreq 34 disk "{\"action\":\"insert\",\"path\":\"$(native "$MED_IMA")\"}"
	mreq 35 disk "{\"action\":\"insert\",\"path\":\"$TRD_OK_N\"}"
	mreq 36 disk "{\"action\":\"insert\",\"path\":\"$(native "$MED_IMA")\"}"
	mreq 37 disk '{"action":"eject"}'
	mreq 38 load_file "{\"path\":\"$(native "$MED_FDI")\"}"
	mreq 40 load_file "{\"path\":\"$(native "$MED_SIGNED")\"}"
	mreq 41 load_file "{\"path\":\"$(native "$MED_TAIL")\"}"
	mreq 42 disk "{\"action\":\"save\",\"path\":\"$(native "$MED_SAVED")\"}"
	mreq 50 sdcard "{\"action\":\"mount\",\"path\":\"$(native "$MED_SD1G")\"}"
	mreq 51 sdcard '{"action":"eject"}'
} | "$BIN" --config "$(native "$CFG")" > "$OUTM" 2>/dev/null &
medpid=$!
# the old reader spent minutes and gigabytes on these tapes; a refusal is
# immediate, so a session still running after half a minute is a failure
i=0
while kill -0 "$medpid" 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
if kill -0 "$medpid" 2>/dev/null; then
	kill "$medpid" 2>/dev/null
	echo "  FAIL media session still running after 30s"
	fail=1
fi
wait "$medpid" 2>/dev/null
checkm() {	# checkm <label> <request id> <pattern> - check_id against $OUTM
	local line
	line=$(grep "\"id\":$2," "$OUTM" | head -1)
	if [ -n "$line" ] && printf '%s' "$line" | grep -q -e "$3"; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		[ -n "$line" ] && printf '       %s\n' "$(printf '%s' "$line" | cut -c1-160)"
		fail=1
	fi
}
checkm "fdi too big for DD refused"     13 'bytes of room, the drive packs 6250'
checkm "refused disk kept the memory"   14 'hex\\": \\"01 02 03 04 05 06 07 08'
checkm "refused disk kept the PC"       15 'pc\\": 4660'
checkm "refused disk kept the zx48"     16 '"machine\\": \\"zx48'
checkm "looped #12 tzx refused"         17 'plays out to more than'
checkm "one-symbol #19 tzx refused"     18 'plays out to more than'
checkm "deflate-bomb #18 tzx refused"   19 'inflates past'
checkm "looping real-size tzx loads"    20 '"ok\\": true'
checkm "looping tzx has its blocks"     21 'blocks\\": 2'
checkm "ima makes the drive HD"         31 'track_bytes\\": 17700'
checkm "eject takes the drive to DD"    32 'track_bytes\\": 6250'
checkm "trd after ima eject is DD"      33 'track_bytes\\": 6250'
checkm "trd over an ima is DD"          35 'track_bytes\\": 6250'
checkm "fdi after ima eject refused"    38 'bytes of room'
checkm "signed dsk length refused"      40 'no real track size'
checkm "short-sector ext dsk loads"     41 '"ok\\": true'
checkm "1 GiB sd image mounts"          50 'capacity_mb\\": 1024'
# track 1 sector 1: the file's 100 bytes, then zeros - not track 0's AA
if python3 - "$MED_SAVED" <<'PY'
import sys
try:
    d = open(sys.argv[1], 'rb').read()
except OSError:
    sys.exit(1)
t = 4096
sys.exit(0 if d[t:t + 100] == b'\x55' * 100 and d[t + 100:t + 256] == bytes(156) else 1)
PY
then
	echo "  ok   short dsk sector tail blanked"
else
	echo "  FAIL short dsk sector tail kept stale bytes"
	fail=1
fi

# A frame is read in the mode it was drawn in. The Profi program below flips
# hires on and off in mid-frame (DFFD bit 7, then a delay, then back), so the
# mode the machine is in when a capture is taken says nothing about the frame
# it reads: the width used to alternate 640/320 and record_video dropped every
# frame that did not match the first.
OUT3=$(mktemp)
{
	echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}'
	echo '{"jsonrpc":"2.0","method":"notifications/initialized"}'
	echo '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"profi"}}}'
	echo '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"F3 01 FD DF 3E C0 ED 79 CD 20 80 3E 40 ED 79 CD 20 80 18 F0"}}}'
	echo '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8020","hex":"11 82 0A 1B 7A B3 20 FB C9"}}}'
	echo '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"frame_digest","arguments":{"frames":1}}}'
	echo '{"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"frame_digest","arguments":{"frames":1}}}'
	# Profi paper is 256 dots by 240 lines, not the 256x192 of the ZX layout
	echo '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"frame_digest","arguments":{"frames":1,"border":false}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"tools/call\",\"params\":{\"name\":\"record_video\",\"arguments\":{\"frames\":20,\"path\":\"$(native "$TMPD/modes.gif")\"}}}"
	# a bad romset is refused before the model moves, not after
	echo '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"ZX48","romset":"NoSuchRomset"}}}'
	echo '{"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"machine_state","arguments":{}}}'
	# a geometry change with no boot frames shows a cleared frame, not the
	# last one read on the new pitch
	echo '{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":100}}}'
	echo '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"machine_config","arguments":{"geometry":"ULA.128","boot_frames":0}}}'
	echo "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\",\"params\":{\"name\":\"screenshot\",\"arguments\":{\"path\":\"$(native "$TMPD/regeom.png")\"}}}"
} | "$BIN" --config "$(native "$CFG")" > "$OUT3" 2>/dev/null
check3() {	# check3 <label> <request id> <pattern> - check_id against $OUT3
	local line
	line=$(grep "\"id\":$2," "$OUT3" | head -1)
	if [ -n "$line" ] && printf '%s' "$line" | grep -q -e "$3"; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		[ -n "$line" ] && printf '       %s\n' "$(printf '%s' "$line" | cut -c1-160)"
		fail=1
	fi
}
check3 "mixed-mode frame read wide"    14 'width\\": 640'
check3 "next mixed-mode frame too"     15 'width\\": 640'
check3 "profi paper is 512x240"        16 'height\\": 240.*width\\": 512'
check3 "recording kept every frame"    17 'frames_recorded\\": 20'
check3 "bad romset refused"            18 'unknown romset'
check3 "bad romset left the model"     19 '"machine\\": \\"profi'
# one colour compresses to well under a kilobyte; the skewed menu does not
if [ -f "$TMPD/regeom.png" ] && [ "$(filesize "$TMPD/regeom.png")" -lt 1200 ]; then
	echo "  ok   geometry change cleared the frame"
else
	echo "  FAIL geometry change cleared the frame"; fail=1
fi

# Breakpoints and bank:offset addresses. A cell armed under the Pentagon's RAM
# mask has to be the cell clear_breakpoints clears, even after a 64K machine
# in between; bank FF is the symbol file's "no bank"; a bank the machine does
# not have, or an offset past one bank, is an error and not a wrapped address;
# and a port break on $FE is the ULA's port whatever A puts on the high byte.
OUTBRK=$(mktemp)
{
	echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}'
	echo '{"jsonrpc":"2.0","method":"notifications/initialized"}'
	echo '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"07:0005"}}}'
	echo '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"alf","boot_frames":0}}}'
	echo '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"machine_config","arguments":{"model":"Pentagon","boot_frames":0}}}'
	# page bank 7 in at $C000, then park a loop at $C005 and run it with some
	# other breakpoint armed - the stale cell used to stop it there
	echo '{"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"F3 01 FD 7F 3E 17 ED 79 18 FE"}}}'
	echo '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":17,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":10}}}'
	echo '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$C005","hex":"00 18 FD"}}}'
	echo '{"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$C005"}}}'
	echo '{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"$9000","scope":"address"}}}'
	echo '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":100}}}'
	echo '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":30,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"FF:8003"}}}'
	echo '{"jsonrpc":"2.0","id":31,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"0F:0005"}}}'
	echo '{"jsonrpc":"2.0","id":32,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"08:0000"}}}'
	echo '{"jsonrpc":"2.0","id":33,"method":"tools/call","params":{"name":"set_breakpoint","arguments":{"address":"05:4000"}}}'
	echo '{"jsonrpc":"2.0","id":34,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
	echo '{"jsonrpc":"2.0","id":40,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"AB CD"}}}'
	echo '{"jsonrpc":"2.0","id":41,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"FF:8000","length":2}}}'
	echo '{"jsonrpc":"2.0","id":42,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"05:3FFE","length":4}}}'
	echo '{"jsonrpc":"2.0","id":43,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"05:-1","length":1}}}'
	echo '{"jsonrpc":"2.0","id":44,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"05:4000","length":1}}}'
	echo '{"jsonrpc":"2.0","id":45,"method":"tools/call","params":{"name":"read_memory","arguments":{"address":"05:3FFE","length":2}}}'
	echo '{"jsonrpc":"2.0","id":50,"method":"tools/call","params":{"name":"set_port_breakpoint","arguments":{"port":"$FE","access":"write"}}}'
	echo '{"jsonrpc":"2.0","id":51,"method":"tools/call","params":{"name":"write_memory","arguments":{"address":"$8000","hex":"F3 3E 07 D3 FE 18 FE"}}}'
	echo '{"jsonrpc":"2.0","id":52,"method":"tools/call","params":{"name":"set_register","arguments":{"name":"PC","value":"$8000"}}}'
	echo '{"jsonrpc":"2.0","id":53,"method":"tools/call","params":{"name":"run","arguments":{"max_instructions":100}}}'
	echo '{"jsonrpc":"2.0","id":54,"method":"tools/call","params":{"name":"clear_breakpoints","arguments":{}}}'
} | "$BIN" --config "$(native "$CFG")" > "$OUTBRK" 2>/dev/null
checkb() {	# checkb <label> <request id> <pattern> - check_id against $OUTBRK
	local line
	line=$(grep "\"id\":$2," "$OUTBRK" | head -1)
	if [ -n "$line" ] && printf '%s' "$line" | grep -q -e "$3"; then
		echo "  ok   $1"
	else
		echo "  FAIL $1"
		[ -n "$line" ] && printf '       %s\n' "$(printf '%s' "$line" | cut -c1-160)"
		fail=1
	fi
}
checkb "bank 7 cell armed"              11 'bank_paged_in'
checkb "cleared cell stays cleared"     21 'reason\\": \\"budget'
checkb "FF:8003 is a plain address"     30 'phys_address\\": 32771'
checkb "bank 15 refused on 128K"        31 'no RAM bank 15'
checkb "bank 8 refused on 128K"         32 'no RAM bank 8'
checkb "offset past a bank refused"     33 '\$0000\.\.\$3FFF'
checkb "FF:8000 reads the CPU address"  41 'address\\": 32768.*hex\\": \\"AB CD'
checkb "read past a bank refused"       42 'runs past the end of bank 5'
checkb "negative offset refused"        43 'offset must be hex'
checkb "offset 4000 refused"            44 '\$0000\.\.\$3FFF'
checkb "read to the bank end works"     45 'offset\\": 16382'
checkb "port FE matches the low byte"   50 'match\\": \\"low byte'
checkb "out (\$FE),a with A=7 caught"   53 'reason\\": \\"breakpoint'

echo
cli "--version reports both versions" 0 "Xpeccy" --version
cli "--help prints usage"             0 "usage:" --help
cli "unknown option refused"          1 "unknown argument" --setings x
cli "missing --config value refused"  1 "needs a directory" --config
cli "unreadable --config refused"     1 "no such directory" --config "$TMPD/not-a-directory"
cli "unreadable --settings refused"   1 "no such file" --settings "$TMPD/not-a-file.conf"

# Everything above runs where the build left the Xpeccy tree, so a server that
# reads its machines or roms from there passes it all and breaks the moment the
# binary is copied anywhere else. This runs it once more with the tree hidden.
echo
bash "$(dirname "$0")/relocated.sh" "$BIN" || fail=1

echo
echo "stderr:"; sed 's/^/  /' "$ERR"
[ $fail -eq 0 ] && echo "SMOKE OK" || echo "SMOKE FAILED"
echo "full log: $OUT"

# The working files go; the transcript stays, because it is the thing anyone
# reads after a failure. A suite that leaves a couple of hundred kilobytes of
# PNG and GIF behind on every run is a suite that fills a laptop by the month.
rm -rf "$TMPD"
exit $fail

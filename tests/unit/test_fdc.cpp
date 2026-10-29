#include "unit.h"
#include "xsp_fdc.h"

#include <cstring>
#include <vector>

extern "C" {
#include "filetypes/filetypes.h"
}

using namespace xsp;

namespace {

void noIrq(int, void*) {}

// A DD drive with a disk in it and one formatted side: 16 sectors of 256,
// TR-DOS's 1 9 2 10 order, each filled with its own number plus the offset.
Floppy* formattedDrive() {
	Floppy* flp = flpCreate(0, noIrq, nullptr);
	flp_set_hd(flp, 0);
	flp->insert = 1;
	flp->door = 1;
	static const int order[16] = {1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15, 8, 16};
	std::vector<Sector> secs(16);
	for (int i = 0; i < 16; i++) {
		Sector& s = secs[(size_t)i];
		memset(&s, 0, sizeof(s));
		s.trk = 0; s.head = 0; s.sec = (unsigned char)order[i]; s.sz = 1; s.type = 0xfb;
		for (int k = 0; k < 256; k++) s.data[k] = (unsigned char)(order[i] + k);
	}
	diskFormTrack(flp, 0, secs.data(), 16);
	return flp;
}

} // namespace

void test_fdc() {
	unit::begin("fdc");

	// ---- command classes, as wd1793.c:vgComTab decodes them --------------
	CHECK(fdc::kindOf(0x08) == fdc::Kind::Restore);
	CHECK(fdc::kindOf(0x1f) == fdc::Kind::Seek);
	CHECK(fdc::kindOf(0x3c) == fdc::Kind::Step);
	CHECK(fdc::kindOf(0x58) == fdc::Kind::StepIn);
	CHECK(fdc::kindOf(0x78) == fdc::Kind::StepOut);
	CHECK(fdc::kindOf(0x80) == fdc::Kind::ReadSector);
	CHECK(fdc::kindOf(0x9c) == fdc::Kind::ReadSector);	// m, s, e, c bits
	// bit 0 set is no read sector at all: the core's table falls through to
	// its do-nothing plan
	CHECK(fdc::kindOf(0x81) == fdc::Kind::None);
	CHECK(fdc::kindOf(0xa1) == fdc::Kind::WriteSector);	// deleted mark
	CHECK(fdc::kindOf(0xc4) == fdc::Kind::ReadAddress);	// e bit
	CHECK(fdc::kindOf(0xc1) == fdc::Kind::None);
	CHECK(fdc::kindOf(0xe0) == fdc::Kind::ReadTrack);
	CHECK(fdc::kindOf(0xf4) == fdc::Kind::WriteTrack);
	CHECK(fdc::kindOf(0xd0) == fdc::Kind::Force);
	CHECK(fdc::kindOf(0xdf) == fdc::Kind::Force);
	CHECK(fdc::kindFromName("read_sector") == fdc::Kind::ReadSector);
	CHECK(fdc::kindFromName("nonsense") == fdc::Kind::None);

	// ---- phases: the plan steps of wd1793.c ------------------------------
	CHECK(fdc::phaseOf(fdc::Kind::ReadSector, 1, true) == fdc::Phase::FindId);
	CHECK(fdc::phaseOf(fdc::Kind::ReadSector, 4, true) == fdc::Phase::Transfer);
	CHECK(fdc::phaseOf(fdc::Kind::ReadSector, 4, false) == fdc::Phase::Idle);
	CHECK(fdc::phaseOf(fdc::Kind::WriteSector, 5, true) == fdc::Phase::Transfer);
	CHECK(fdc::phaseOf(fdc::Kind::ReadAddress, 2, true) == fdc::Phase::Transfer);
	CHECK(fdc::phaseOf(fdc::Kind::Seek, 1, true) == fdc::Phase::Seek);

	// ---- status bits by class --------------------------------------------
	{
		const auto one = fdc::statusFlags(0x26, fdc::Kind::Restore);
		CHECK_EQ(one.size(), (size_t)3);
		CHECK_EQ(one[0], std::string("head_loaded"));
		CHECK_EQ(one[2], std::string("index"));
		const auto two = fdc::statusFlags(0x1c, fdc::Kind::ReadSector);
		CHECK_EQ(two.size(), (size_t)3);
		CHECK_EQ(two[0], std::string("not_found"));
		CHECK_EQ(two[2], std::string("lost_data"));
		CHECK_EQ(fdc::statusFlags(0x20, fdc::Kind::WriteSector)[0], std::string("write_fault"));
		CHECK_EQ(fdc::statusFlags(0x20, fdc::Kind::ReadSector)[0], std::string("deleted_mark"));
	}

	// ---- CRC: against CRC-CCITT computed elsewhere -----------------------
	//
	// Reference values from Python's binascii.crc_hqx(A1 A1 A1 + field, 0xFFFF),
	// which owes nothing to this code or to the core's add_crc_16.
	{
		const unsigned char id[] = {0xfe, 0, 0, 1, 1};
		CHECK_EQ(fdc::fieldCrc(id, 5), (unsigned short)0xfa0c);
		std::vector<unsigned char> data{0xfb};
		for (int i = 0; i < 256; i++) data.push_back((unsigned char)i);
		CHECK_EQ(fdc::fieldCrc(data.data(), (int)data.size()), (unsigned short)0x9f77);
		data[0] = 0xf8;
		CHECK_EQ(fdc::fieldCrc(data.data(), (int)data.size()), (unsigned short)0xdecf);
	}

	// ---- the layout of a track the core formatted ------------------------
	Floppy* flp = formattedDrive();
	{
		const fdc::TrackLayout lay = fdc::trackLayout(flp, 0, 0);
		CHECK_EQ(lay.trklen, 6250);
		CHECK_EQ(lay.sectors.size(), (size_t)16);
		std::string order;
		bool crcs = true;
		for (const auto& s : lay.sectors) {
			order += std::to_string(s.s) + " ";
			crcs = crcs && s.idCrcOk && s.dataCrcOk && s.mark == 0xfb && s.size == 256;
		}
		CHECK_EQ(order, std::string("1 9 2 10 3 11 4 12 5 13 6 14 7 15 8 16 "));
		CHECK(crcs);
		// the data field holds what was formatted into it
		const auto& s1 = lay.sectors[0];
		CHECK_EQ((int)flp->data[0].byte[s1.dataAt], 1);
		CHECK_EQ((int)flp->data[0].byte[s1.dataAt + 255], (1 + 255) & 0xff);
		int idx = -1;
		CHECK_EQ(fdc::fieldAt(flp, 0, 0, s1.idAt + 2, lay, &idx), std::string("id"));
		CHECK_EQ(idx, 0);
		CHECK_EQ(fdc::fieldAt(flp, 0, 0, s1.dataAt + 10, lay, &idx), std::string("data"));
		CHECK_EQ(fdc::fieldAt(flp, 0, 0, s1.dataAt + 256, lay, &idx), std::string("crc"));
		CHECK_EQ(fdc::fieldAt(flp, 0, 0, s1.dataAt + 300, lay, &idx), std::string("gap"));
		CHECK_EQ(idx, -1);
	}

	// ---- faults, and taking them off byte for byte -----------------------
	{
		std::vector<unsigned char> before(flp->data[0].byte, flp->data[0].byte + flp->trklen);
		std::vector<unsigned char> fieldsBefore(flp->data[0].field, flp->data[0].field + flp->trklen);
		fdc::Faults faults;
		std::string err;
		CHECK(faults.add(flp, 0, 0, 0, 9, "crc_data", err));
		CHECK(faults.add(flp, 0, 0, 0, 2, "no_id", err));
		CHECK(faults.add(flp, 0, 0, 0, 10, "deleted", err));
		CHECK(faults.add(flp, 0, 0, 0, 3, "crc_id", err));
		CHECK(!faults.add(flp, 0, 0, 0, 17, "crc_data", err));		// no such sector
		CHECK(!faults.add(flp, 0, 0, 0, 4, "melted", err));		// no such fault
		CHECK(err.find("unknown fault") != std::string::npos);

		const fdc::TrackLayout lay = fdc::trackLayout(flp, 0, 0);
		CHECK_EQ(lay.sectors.size(), (size_t)15);		// sector 2 lost its ID
		for (const auto& s : lay.sectors) {
			if (s.s == 2) CHECK(false);
			if (s.s == 9) CHECK(!s.dataCrcOk && s.idCrcOk);
			if (s.s == 10) CHECK(s.mark == 0xf8 && s.dataCrcOk);
			if (s.s == 3) CHECK(!s.idCrcOk && s.dataCrcOk);
			if (s.s == 1) CHECK(s.idCrcOk && s.dataCrcOk);
		}
		CHECK_EQ(faults.list().size(), (size_t)4);
		CHECK_EQ(faults.kindAt(0, 0, 0, 9), std::string("crc_data"));

		Floppy* drives[4] = {flp, nullptr, nullptr, nullptr};
		CHECK_EQ(faults.clear(drives, 0), 4);
		CHECK(faults.list().empty());
		CHECK(memcmp(before.data(), flp->data[0].byte, before.size()) == 0);
		CHECK(memcmp(fieldsBefore.data(), flp->data[0].field, fieldsBefore.size()) == 0);
		CHECK_EQ(fdc::trackLayout(flp, 0, 0).sectors.size(), (size_t)16);
	}
	flpDestroy(flp);

	// ---- TR-DOS: every entry has a name of its own and a signature -------
	{
		int adr = 0;
		CHECK(fdc::trdosFind("trdos.read_bytes", adr));
		CHECK_EQ(adr, 0x3fe5);
		CHECK(fdc::trdosFind("trdos.ret", adr));
		CHECK_EQ(adr, 0x3d2f);
		CHECK(!fdc::trdosFind("trdos.nothing", adr));
		for (const auto& e : fdc::trdosRom()) {
			CHECK(std::strlen(e.signature) >= 4 && std::strlen(e.signature) % 2 == 0);
			CHECK(std::string(e.name).rfind("trdos.", 0) == 0);
		}
	}
}

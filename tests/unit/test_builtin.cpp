// The files that ship with the core, compiled in: the machine definitions, the
// layouts and the roms. What they replace is a read from the source tree at
// run time, which passed every test run where the tree was and failed every
// binary that was copied somewhere it was not. tests/relocated.sh checks the
// binary with the tree hidden; this checks the layering underneath it - the
// compiled-in copy is the shipped layer, and the user's files still sit on top.
#include "unit.h"
#include "xsp_builtin.h"
#include "xsp_config.h"

#include <string>

using namespace xsp;

void test_builtin() {
	unit::begin("builtin");

	// ---- what was compiled in -----------------------------------------------
	CHECK(builtin::kMachines.find("zx48.conf") != nullptr);
	CHECK(builtin::kMachines.find("evo-tsconf.conf") != nullptr);
	CHECK(builtin::kMachines.find("zx48") == nullptr);	// by file name, not stem
	CHECK_EQ((int)builtin::kLayouts.count, 1);
	for (const builtin::File& f : builtin::kMachines)
		CHECK(f.size > 0);
#ifdef XSP_BUILTIN_ROMS
	const builtin::File* rom48 = builtin::kRoms.find("48.rom");
	CHECK(rom48 != nullptr);
	if (rom48) CHECK_EQ((int)rom48->size, 16384);
#else
	CHECK_EQ((int)builtin::kRoms.count, 0);
#endif

	// ---- an empty configuration gets the shipped machines --------------------
	//
	// Nothing on disk names a machine or a layout: whatever comes back came
	// from the binary itself.
	{
		unit::TempDir tmp("builtin-empty");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		Env env;
		env.load(tmp.path());
		const MachineDef* z = env.findMachine("zx48");
		CHECK(z != nullptr);
		if (z) {
			CHECK_EQ(z->hw, std::string("ZX48"));
			CHECK_EQ(z->geometry, std::string("ULA.48"));
		}
		CHECK(env.findMachine("evo-tsconf") != nullptr);
		const Layout* l = env.findLayout("ULA.48");
		CHECK(l != nullptr);
		if (l) {
			// 448 x 312 dots is 224 x 312 = 69888 T, the 48K's frame
			CHECK_EQ(l->full_x, 448);
			CHECK_EQ(l->full_y, 312);
		}
#ifdef XSP_BUILTIN_ROMS
		// every rom a shipped definition names arrives as bytes, so loading
		// it never goes looking for a directory
		const MachineDef* p = env.findMachine("pent");
		CHECK(p != nullptr);
		if (p) {
			CHECK(!p->roms.empty());
			for (const RomFile& rf : p->roms) {
				CHECK(rf.data != nullptr);
				CHECK(rf.dataSize > 0);
			}
		}
#endif
	}

	// ---- the user's files still win ----------------------------------------
	{
		unit::TempDir tmp("builtin-user");
		tmp.file("config.conf", "[PROFILES]\ncurrent = default\n");
		tmp.file("machines/zx48.conf", "[machine]\nmemory = 96\n");
		tmp.file("layouts.conf", "layout = ULA.48:456:320:64:56:64:16:64:8:120:256:192\n");
		tmp.file("roms/128p-0.rom", std::string(16384, '\x5a'));
		Env env;
		env.load(tmp.path());
		const MachineDef* z = env.findMachine("zx48");
		CHECK(z != nullptr);
		if (z) {
			CHECK_EQ(z->memoryKb, 96);			// the patch
			CHECK_EQ(z->geometry, std::string("ULA.48"));	// the shipped rest
		}
		const Layout* l = env.findLayout("ULA.48");
		CHECK(l != nullptr);
		if (l) CHECK_EQ(l->full_y, 320);	// replaced by name, last wins
#ifdef XSP_BUILTIN_ROMS
		// a file of the same name in the user's rom directory is read from
		// there; the names it does not have still come from the binary
		const MachineDef* p = env.findMachine("pent");
		CHECK(p != nullptr);
		if (p) {
			for (const RomFile& rf : p->roms) {
				if (rf.name == "128p-0.rom") CHECK(rf.data == nullptr);
				else CHECK(rf.data != nullptr);
			}
		}
#endif
	}
}

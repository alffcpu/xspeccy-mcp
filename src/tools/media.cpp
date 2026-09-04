// Files: snapshots, tapes and disks.
#include "tools/common.h"

namespace xsp {
namespace tools {

void registerMediaTools() {
	tool("load_file",
	     "Load a file into the machine by extension: .sna/.z80 snapshots, .tap/.tzx tapes, "
	     ".trd/.scl disks (drive A by default), .bin raw. Snapshots start running immediately.",
	     json{{"properties", {
		     {"path", {{"type", "string"}}},
		     {"drive", {{"type", "integer"}, {"description", "disk drive 0..3, default 0"}}}
	     }}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string path = argStr(a, "path");
		     if (path.empty()) throw std::runtime_error("path required");
		     std::string ext;
		     size_t dot = path.find_last_of('.');
		     if (dot != std::string::npos) ext = path.substr(dot + 1);
		     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
		     int drv = argNumOr(a, "drive", 0);
		     Computer* c = mach().comp();
		     int res;
		     if (ext == "sna") res = loadSNA(c, path.c_str(), drv);
		     else if (ext == "z80") res = loadZ80(c, path.c_str(), drv);
		     else if (ext == "tap") res = loadTAP(c, path.c_str(), drv);
		     else if (ext == "tzx") res = loadTZX(c, path.c_str(), drv);
		     else if (ext == "trd") res = loadTRD(c, path.c_str(), drv);
		     else if (ext == "scl") res = loadSCL(c, path.c_str(), drv);
		     else if (ext == "bin") res = loadBIN(c, path.c_str(), drv);
		     else throw std::runtime_error("unsupported extension '" + ext + "'");
		     if (res != ERR_OK) throw std::runtime_error("loader failed with code " + std::to_string(res));
		     // the frames still in the history were drawn by whatever ran before
		     xsp::video::historyClear();
		     return json{{"path", path}, {"format", ext}, {"ok", true},
			     {"pc", cpu_get_pc(c->cpu)}, {"pc_hex", hex16(cpu_get_pc(c->cpu))}};
	     });

	tool("tape",
	     "Tape deck: state (what is loaded, which block, is it playing), play, stop, rewind, next "
	     "block, eject, or blocks to list what is on the tape. A .tap/.tzx must be loaded first "
	     "with load_file - loading does not press play.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "state | play | stop | rewind | next | eject | blocks"}}},
		     {"block", {{"type", "integer"}, {"description", "block to rewind to, default 0"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = mach().comp();
		     Tape* t = c->tape;
		     std::string action = argStr(a, "action", "state");
		     // The core stores whatever block number it is handed and indexes
		     // blkData with it on the next play, so the range check has to
		     // happen here - upstream has none, and -1 is a segfault one call
		     // later rather than an error on the call that caused it.
		     if (action == "play") {
			     if (t->block < 0 || t->block >= t->blkCount)
				     throw std::runtime_error("tape is not at a playable block (" +
							      std::to_string(t->block) + " of " +
							      std::to_string(t->blkCount) + ")");
			     tapPlay(t);
		     }
		     else if (action == "stop") tapStop(t);
		     else if (action == "rewind") {
			     if (t->blkCount < 1) throw std::runtime_error("no tape loaded - see load_file");
			     tapRewind(t, argNumRange(a, "block", 0, 0, t->blkCount - 1));
		     }
		     else if (action == "next") tapNextBlock(t);
		     else if (action == "eject") tapEject(t);
		     else if (action != "state" && action != "blocks")
			     throw std::runtime_error("unknown tape action '" + action + "'");

		     json res{
			     {"path", t->path ? t->path : ""},
			     {"playing", t->on ? true : false},
			     {"recording", t->rec ? true : false},
			     {"block", t->block},
			     {"blocks", t->blkCount}
		     };
		     if (action == "blocks") {
			     std::vector<TapeBlockInfo> info((size_t)std::max(0, t->blkCount));
			     int n = info.empty() ? 0 : tapGetBlocksInfo(t, info.data(), (int)info.size());
			     json list = json::array();
			     for (int i = 0; i < n; i++)
				     list.push_back({{"index", i}, {"name", std::string(info[i].name)},
						     {"type", info[i].type}, {"size", info[i].size},
						     {"time_ms", info[i].time / 1000000}});
			     res["block_list"] = list;
		     }
		     return res;
	     });

	tool("disk",
	     "Floppy drives: state, insert a .trd/.scl image, eject, or save the current image back to "
	     "a file. Drive 0 is A.",
	     json{{"properties", {
		     {"action", {{"type", "string"}, {"description", "state | insert | eject | save"}}},
		     {"drive", {{"type", "integer"}, {"description", "0..3, default 0"}}},
		     {"path", {{"type", "string"}}}
	     }}},
	     [](const json& a) {
		     Computer* c = mach().comp();
		     int drv = argNumOr(a, "drive", 0);
		     if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
		     Floppy* flp = c->dif->fdc->flop[drv];
		     std::string action = argStr(a, "action", "state");
		     std::string path = argStr(a, "path");

		     if (action == "insert") {
			     if (path.empty()) throw std::runtime_error("path required");
			     std::string ext;
			     size_t dot = path.find_last_of('.');
			     if (dot != std::string::npos) ext = path.substr(dot + 1);
			     for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
			     int res = (ext == "scl") ? loadSCL(c, path.c_str(), drv)
						      : loadTRD(c, path.c_str(), drv);
			     if (res != ERR_OK)
				     throw std::runtime_error("loader failed with code " + std::to_string(res));
		     } else if (action == "eject") {
			     flp->insert = 0;
		     } else if (action == "save") {
			     if (path.empty()) throw std::runtime_error("path required");
			     int res = saveTRD(c, path.c_str(), drv);
			     if (res != ERR_OK)
				     throw std::runtime_error("save failed with code " + std::to_string(res));
		     } else if (action != "state") {
			     throw std::runtime_error("unknown disk action '" + action + "'");
		     }
		     // an inserted image the machine has no controller for reads as a working setup
		     // right up to the point the ROM waits for a drive that will never answer
		     return json{{"drive", drv}, {"inserted", flp->insert ? true : false},
			     {"path", flp->path ? flp->path : ""},
			     {"changed", flp->changed ? true : false},
			     {"interface", diskInterfaceName(c->dif->type)}};
	     });

	tool("disk_catalog",
	     "The TR-DOS catalogue of the disk in a drive: file names, extensions, start address, "
	     "length and where they sit on the disk.",
	     json{{"properties", {{"drive", {{"type", "integer"}}}}}},
	     [](const json& a) {
		     int drv = argNumOr(a, "drive", 0);
		     if (drv < 0 || drv > 3) throw std::runtime_error("drive must be 0..3");
		     Floppy* flp = mach().comp()->dif->fdc->flop[drv];
		     if (!flp->insert) throw std::runtime_error("no disk in drive " + std::to_string(drv));
		     std::vector<TRFile> cat(128);
		     int n = diskGetTRCatalog(flp, cat.data());
		     json list = json::array();
		     for (int i = 0; i < n; i++) {
			     const TRFile& f = cat[i];
			     std::string name((const char*)f.name, 8);
			     while (!name.empty() && name.back() == ' ') name.pop_back();
			     list.push_back({
				     {"name", name},
				     {"ext", std::string(1, (char)f.ext)},
				     {"start", (f.hst << 8) | f.lst},
				     {"start_hex", hex16((f.hst << 8) | f.lst)},
				     {"length", (f.hlen << 8) | f.llen},
				     {"sectors", f.slen},
				     {"track", f.trk},
				     {"sector", f.sec}
			     });
		     }
		     return json{{"drive", drv}, {"files", (int)n}, {"catalog", list}};
	     });

	tool("save_snapshot",
	     "Save the current machine state to a .sna file. The standard way to skip a long "
	     "precalculation: run once to the main loop, save here, and load that snapshot for "
	     "every later measurement instead of paying for the precalculation each time.",
	     json{{"properties", {{"path", {{"type", "string"}}}}}, {"required", json::array({"path"})}},
	     [](const json& a) {
		     std::string path = argStr(a, "path");
		     int res = saveSNA(mach().comp(), path.c_str(), 0);
		     if (res != ERR_OK) throw std::runtime_error("save failed with code " + std::to_string(res));
		     return json{{"path", path}, {"ok", true}};
	     });
}

} // namespace tools
} // namespace xsp

// The settings table is the server's claim that every emulator setting it can
// change lives in exactly one place. That claim is only worth anything if the
// rows are consistent with each other, so most of this file walks the table and
// checks it against itself rather than against a list of expected names, which
// would have to be edited every time a row is added and would then be a copy of
// the table rather than a test of it.
#include "unit.h"
#include "xsp_settings.h"
#include "xsp_platform.h"

#include <set>
#include <string>

using namespace xsp;
using namespace xsp::settings;

void test_settings() {
	unit::begin("settings");

	const std::vector<Setting>& rows = table();
	CHECK(rows.size() > 10);

	// ---- the table is well formed -----------------------------------------
	{
		std::set<std::string> names;
		for (const Setting& s : rows) {
			CHECK(s.name != nullptr && s.name[0] != '\0');
			CHECK(s.affects != nullptr && s.affects[0] != '\0');
			CHECK(s.get != nullptr);
			CHECK(s.set != nullptr);

			// A duplicated name makes one of the two rows unreachable, and
			// which one depends on the order of a list nobody reads.
			CHECK(names.insert(s.name).second);

			// Every name is section.key, which is what makes the file format
			// work: [timing] contPattern is timing.contPattern.
			const std::string name = s.name;
			CHECK(name.find('.') != std::string::npos);
			CHECK(name.find('.') > 0);
			CHECK(name.find('.') < name.size() - 1);

			if (s.type == Type::Enum) {
				CHECK(s.enumNames != nullptr);
				int n = 0;
				while (s.enumNames[n]) {
					// parseValue lower-cases what it is given and compares
					// it to these, so an entry with a capital in it can
					// never be selected by name.
					for (const char* c = s.enumNames[n]; *c; c++)
						CHECK(*c < 'A' || *c > 'Z');
					n++;
				}
				CHECK(n >= 2);
				CHECK(s.def >= 0);
				CHECK(s.def < n);
			} else {
				// Only enums carry a name list; a stale one on another row
				// would be read by formatValue and print nonsense.
				CHECK(s.enumNames == nullptr);
				CHECK(s.lo <= s.hi);
				CHECK(s.def >= s.lo);
				CHECK(s.def <= s.hi);
			}
			if (s.type == Type::Bool) {
				CHECK_EQ(s.lo, 0.0);
				CHECK_EQ(s.hi, 1.0);
			}
		}
	}

	// The default of every row survives a trip through text and back, which is
	// what the settings file does to it on every start.
	{
		for (const Setting& s : rows) {
			const std::string text = formatValue(s, s.def);
			CHECK(!text.empty());
			double back = -12345.0;
			std::string err;
			CHECK(parseValue(s, text, back, err));
			CHECK(err.empty());
			if (s.type == Type::Double) CHECK(back > s.def - 1e-9 && back < s.def + 1e-9);
			else CHECK_EQ(back, s.def);
		}
	}

	// Every option of every enum is reachable by its own name and prints back
	// as that same name.
	{
		for (const Setting& s : rows) {
			if (s.type != Type::Enum) continue;
			for (int i = 0; s.enumNames[i]; i++) {
				double v = -1;
				std::string err;
				CHECK(parseValue(s, s.enumNames[i], v, err));
				CHECK_EQ((int)v, i);
				CHECK_EQ(formatValue(s, v), std::string(s.enumNames[i]));
			}
		}
	}

	// ---- find -------------------------------------------------------------
	{
		const Setting* gamma = find("video.gamma");
		CHECK(gamma != nullptr);
		// Case-insensitive, because the name arrives from a settings file a
		// person typed and from a tool argument an agent generated.
		CHECK(find("VIDEO.GAMMA") == gamma);
		CHECK(find("Video.Gamma") == gamma);
		CHECK(find("video.gama") == nullptr);
		CHECK(find("") == nullptr);
		CHECK(find("video") == nullptr);	// a section is not a setting
	}

	// ---- parsing values ---------------------------------------------------
	{
		const Setting* boolean = find("video.greyscale");
		const Setting* whole = find("video.blend");
		const Setting* real = find("video.gamma");
		const Setting* enumerated = find("timing.contPattern");
		CHECK(boolean != nullptr);
		CHECK(whole != nullptr);
		CHECK(real != nullptr);
		CHECK(enumerated != nullptr);
		if (!boolean || !whole || !real || !enumerated) return;

		double v = 0;
		std::string err;

		// All four spellings of yes and of no, in any case, with surrounding
		// space, because that is what a hand-edited file contains.
		const char* yes[] = {"1", "yes", "true", "on", "YES", " True ", nullptr};
		for (int i = 0; yes[i]; i++) {
			CHECK(parseValue(*boolean, yes[i], v, err));
			CHECK_EQ(v, 1.0);
		}
		const char* no[] = {"0", "no", "false", "off", "OFF", "\tfalse\t", nullptr};
		for (int i = 0; no[i]; i++) {
			CHECK(parseValue(*boolean, no[i], v, err));
			CHECK_EQ(v, 0.0);
		}
		CHECK(!parseValue(*boolean, "maybe", v, err));
		CHECK(!err.empty());
		CHECK(err.find("maybe") != std::string::npos);

		// An Int refuses a fraction rather than truncating it: "2 frames of
		// blending" and "2.5 frames" are not the same request, and rounding one
		// into the other hides the mistake.
		CHECK(parseValue(*whole, "3", v, err));
		CHECK_EQ(v, 3.0);
		CHECK(!parseValue(*whole, "2.5", v, err));
		CHECK(err.find("whole") != std::string::npos);

		// A Double takes a fraction, and neither type takes trailing rubbish:
		// strtod would happily read "2.2rubbish" as 2.2.
		CHECK(parseValue(*real, "2.4", v, err));
		CHECK(v > 2.39 && v < 2.41);
		CHECK(!parseValue(*real, "2.2rubbish", v, err));
		CHECK(!parseValue(*real, "rubbish", v, err));
		CHECK(!parseValue(*real, "", v, err));
		CHECK(!parseValue(*real, "   ", v, err));

		// An unknown enum option names the ones that exist, because the caller
		// has no other way to find out.
		CHECK(!parseValue(*enumerated, "sideways", v, err));
		CHECK(err.find("sideways") != std::string::npos);
		CHECK(err.find(enumerated->enumNames[0]) != std::string::npos);
	}

	// ---- where the value came from ----------------------------------------
	//
	// "why is the machine like this" is unanswerable without it, and the
	// default for a setting nobody has touched is the core's own.
	{
		CHECK(sourceOf("video.gamma") == Source::Default);
		markSource("video.gamma", Source::File);
		CHECK(sourceOf("video.gamma") == Source::File);
		CHECK(sourceOf("VIDEO.GAMMA") == Source::File);	// same setting
		markSource("video.gamma", Source::Default);

		CHECK_EQ(std::string(sourceName(Source::Default)), std::string("core default"));
		CHECK_EQ(std::string(sourceName(Source::XpeccyProfile)), std::string("xpeccy profile"));
		CHECK_EQ(std::string(sourceName(Source::File)), std::string("settings file"));
		CHECK_EQ(std::string(sourceName(Source::Session)), std::string("this session"));
	}

	// ---- where the file lives ---------------------------------------------
	{
		CHECK_EQ(defaultSavePath(""), std::string("xspeccy-mcp.conf"));
		CHECK_EQ(defaultSavePath("/conf"),
			 platform::join("/conf", "xspeccy-mcp.conf"));
	}

	// ---- rewriting the file -----------------------------------------------
	//
	// Saving one setting must leave everything else exactly as the person wrote
	// it: comments, blank lines, ordering, indentation, and keys this build has
	// never heard of. A tool that reformats the file is a tool nobody lets near
	// a file they care about.
	{
		unit::TempDir tmp("settings-save");
		const std::string path = tmp.file("xspeccy-mcp.conf",
			"# my machine\n"
			"\n"
			"[video]\n"
			"\tgamma = 2.2\t# linear light\n"
			"blend = 1\n"
			"\n"
			"[future]\n"
			"something = this build does not know\n");

		std::string used, err;
		CHECK(save(path, "video.gamma", "2.4", used, err));
		CHECK(err.empty());
		CHECK_EQ(used, path);

		const std::string after = unit::readWholeFile(path);
		CHECK(after.find("# my machine") != std::string::npos);
		CHECK(after.find("[future]") != std::string::npos);
		CHECK(after.find("something = this build does not know") != std::string::npos);
		CHECK(after.find("blend = 1") != std::string::npos);
		CHECK(after.find("gamma = 2.4") != std::string::npos);
		CHECK(after.find("gamma = 2.2") == std::string::npos);
		// The key kept its indentation and its short form, because it is inside
		// a section: writing "video.gamma" back into [video] would make it
		// video.video.gamma on the next read.
		CHECK(after.find("\tgamma = 2.4") != std::string::npos);

		// A setting the file does not mention is appended rather than dropped.
		CHECK(save(path, "video.greyscale", "yes", used, err));
		const std::string appended = unit::readWholeFile(path);
		CHECK(appended.find("video.greyscale = yes") != std::string::npos);
		CHECK(appended.find("[future]") != std::string::npos);

		// Saving the same setting again replaces the line rather than adding a
		// second one, which would leave the file saying two things at once.
		CHECK(save(path, "video.greyscale", "no", used, err));
		const std::string twice = unit::readWholeFile(path);
		CHECK(twice.find("video.greyscale = no") != std::string::npos);
		CHECK(twice.find("video.greyscale = yes") == std::string::npos);
		size_t first = twice.find("video.greyscale");
		CHECK(twice.find("video.greyscale", first + 1) == std::string::npos);
	}

	// A file that does not exist yet is created, so the first save works on a
	// machine that has never had one.
	{
		unit::TempDir tmp("settings-new");
		const std::string path = tmp.reserve("xspeccy-mcp.conf");
		std::string used, err;
		CHECK(!platform::isFile(path));
		CHECK(save(path, "video.blend", "2", used, err));
		CHECK(platform::isFile(path));
		CHECK(unit::readWholeFile(path).find("video.blend = 2") != std::string::npos);
	}

	// A path that cannot be written says so rather than reporting success and
	// losing the setting.
	{
		unit::TempDir tmp("settings-bad");
		std::string used, err;
		const std::string path =
			platform::join(platform::join(tmp.path(), "no-such-dir"), "x.conf");
		CHECK(!save(path, "video.blend", "2", used, err));
		CHECK(!err.empty());
	}
}

#include "unit.h"

// Ordered so the ones that need nothing but arithmetic run first: when several
// break at once, the first failure is usually the cause of the rest.
int main() {
	test_args();
	test_blend();
	test_labels();
	test_listing();
	test_keyboard();
	test_platform();
	test_config();
	test_settings();
	test_machine();
	test_server();
	test_png();
	test_gif();
	test_audio();
	return unit::summary();
}

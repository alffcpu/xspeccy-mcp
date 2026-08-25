#include "unit.h"

int main() {
	test_blend();
	test_labels();
	test_listing();
	test_png();
	return unit::summary();
}

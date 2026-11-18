#include <stdarg.h>

#include "minitest.h"

jmp_buf mt_jmp;
int mt_failed;

void mt_fail(const char *file, int line, const char *fmt, ...)
{
	va_list args;
	const char *base = strrchr(file, '/');
	const char *base_win = strrchr(file, '\\');

	if (base_win && (!base || base_win > base)) {
		base = base_win;
	}
	base = base ? base + 1 : file;

	printf("    %s:%d: ", base, line);
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	printf("\n");
	fflush(stdout);
	mt_failed = 1;
	longjmp(mt_jmp, 1);
}

static int run_case(const struct mt_case *c, void (*setup)(void))
{
	mt_failed = 0;
	printf("RUN  %s\n", c->name);
	fflush(stdout);
	if (setjmp(mt_jmp) == 0) {
		if (setup) {
			setup();
		}
		c->fn();
	}
	printf("%s %s\n", mt_failed ? "FAIL" : "PASS", c->name);
	fflush(stdout);
	return mt_failed;
}

int mt_main(int argc, char **argv, const struct mt_case *cases, size_t count,
	    void (*setup)(void))
{
	int failures = 0;

	if (argc >= 2 && strcmp(argv[1], "--list") == 0) {
		for (size_t i = 0; i < count; i++) {
			printf("%s\n", cases[i].name);
		}
		return 0;
	}

	if (argc >= 3 && strcmp(argv[1], "--run") == 0) {
		for (size_t i = 0; i < count; i++) {
			if (strcmp(cases[i].name, argv[2]) == 0) {
				return run_case(&cases[i], setup) ? 1 : 0;
			}
		}
		printf("FAIL %s: unknown test\n", argv[2]);
		return 2;
	}

	for (size_t i = 0; i < count; i++) {
		failures += run_case(&cases[i], setup);
	}
	printf("%u of %u tests failed\n", (unsigned int)failures,
	       (unsigned int)count);
	return failures ? 1 : 0;
}

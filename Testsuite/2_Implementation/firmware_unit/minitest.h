/*
 * minitest - a tiny, dependency-free C unit test harness.
 *
 * Every test executable built by build.py links exactly one test file that
 * ends with MT_MAIN(...). The executable understands:
 *
 *   <exe> --list        print one test name per line
 *   <exe> --run NAME    run a single test in this (fresh) process
 *   <exe>               run all tests in sequence
 *
 * Results are printed as "PASS <name>" / "FAIL <name>: <file>:<line>: <msg>"
 * so pytest (2_Implementation/tests/firmware_unit/) can collect each C test case as its own
 * pytest item. Running one test per process gives every case fresh module
 * statics, which matters for firmware modules with file-scope state.
 */
#ifndef MINITEST_H
#define MINITEST_H

#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*mt_fn)(void);

struct mt_case {
	const char *name;
	mt_fn fn;
};

extern jmp_buf mt_jmp;
extern int mt_failed;

void mt_fail(const char *file, int line, const char *fmt, ...);

#define MT_TEST(name) static void name(void)
#define MT_CASE(name) { #name, name }

#define MT_ASSERT(cond)                                                        \
	do {                                                                   \
		if (!(cond)) {                                                 \
			mt_fail(__FILE__, __LINE__, "assertion failed: %s",    \
				#cond);                                        \
		}                                                              \
	} while (0)

#define MT_ASSERT_MSG(cond, ...)                                               \
	do {                                                                   \
		if (!(cond)) {                                                 \
			mt_fail(__FILE__, __LINE__, __VA_ARGS__);              \
		}                                                              \
	} while (0)

#define MT_ASSERT_EQ(expected, actual)                                         \
	do {                                                                   \
		long long mt_e_ = (long long)(expected);                       \
		long long mt_a_ = (long long)(actual);                         \
		if (mt_e_ != mt_a_) {                                          \
			mt_fail(__FILE__, __LINE__,                            \
				"%s == %s: expected %lld, got %lld",           \
				#expected, #actual, mt_e_, mt_a_);             \
		}                                                              \
	} while (0)

#define MT_ASSERT_MEM_EQ(expected, actual, len)                                \
	do {                                                                   \
		if (memcmp((expected), (actual), (len)) != 0) {                \
			mt_fail(__FILE__, __LINE__,                            \
				"memory differs: %s vs %s (%u bytes)",         \
				#expected, #actual, (unsigned int)(len));      \
		}                                                              \
	} while (0)

#define MT_ASSERT_STR_CONTAINS(haystack, needle)                               \
	do {                                                                   \
		if (strstr((haystack), (needle)) == NULL) {                    \
			mt_fail(__FILE__, __LINE__,                            \
				"\"%s\" does not contain \"%s\"",              \
				(haystack), (needle));                         \
		}                                                              \
	} while (0)

int mt_main(int argc, char **argv, const struct mt_case *cases, size_t count,
	    void (*setup)(void));

/* Optional per-test setup hook: define MT_SETUP before MT_MAIN to use it. */
#define MT_MAIN_WITH_SETUP(setup_fn, ...)                                      \
	static const struct mt_case mt_cases_[] = { __VA_ARGS__ };             \
	int main(int argc, char **argv)                                        \
	{                                                                      \
		return mt_main(argc, argv, mt_cases_,                          \
			       sizeof(mt_cases_) / sizeof(mt_cases_[0]),       \
			       setup_fn);                                      \
	}

#define MT_MAIN(...) MT_MAIN_WITH_SETUP(NULL, __VA_ARGS__)

#endif /* MINITEST_H */

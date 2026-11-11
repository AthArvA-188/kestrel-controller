// Minimal test support. A test is a void function listed in test_main.c. It
// fails when any of its checks fails.
#pragma once

#include <stdio.h>

extern int t_checks, t_failed_checks;
extern const char *t_out_dir;

#define CHECK(cond)                                                          \
    do {                                                                     \
        t_checks++;                                                          \
        if (!(cond)) {                                                       \
            t_failed_checks++;                                               \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long a_ = (long long)(a), b_ = (long long)(b);                  \
        t_checks++;                                                          \
        if (a_ != b_) {                                                      \
            t_failed_checks++;                                               \
            printf("    FAIL %s:%d: %s == %s (%lld, %lld)\n", __FILE__,      \
                   __LINE__, #a, #b, a_, b_);                                \
        }                                                                    \
    } while (0)

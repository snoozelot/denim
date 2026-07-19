#!/usr/bin/env -S ccraft
// tankfahrt.t — tests for tankfahrt.c CLI output
//
// Tests run the binary and assert on what it prints to stdout.
// Nothing in here re-implements a formula; if the binary changes its
// calculation, a test breaks.

#include "reef/tt.h"
#include <stdio.h>
#include <string.h>

// =============================================================================
// HELPER
// =============================================================================

// Derive binary path from this file's path at compile time.
// tankfahrt.t lives next to tankfahrt.c; just swap the extension.
static const char *
bin(void) {
    static char path[512];
    if (!path[0]) {
        snprintf(path, sizeof(path), "%s", __FILE__);
        char *dot = strrchr(path, '.');
        if (dot) strcpy(dot, ".c");
    }
    return path;
}

static const char *
run(const char *args) {
    static char buf[4096];
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s 2>/dev/null", bin(), args);
    FILE *fp = popen(cmd, "r");
    if (!fp) { buf[0] = '\0'; return buf; }
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    pclose(fp);
    return buf;
}

// =============================================================================
// TESTS
// =============================================================================

TEST(not_worth_it_below_doener_threshold) {
    // cheap=1.80, exp=1.92, fill=30, cons=6.3, dist=10
    // gross=3.60, detour=2.27, net=1.33 — below 8 EUR Döner threshold
    const char *out = run("-c 1.80 -e 1.92 -f 30 --consumption 6.3 -d 10");
    ASSERT_TRUE("net_value",    strstr(out, "1.33") != NULL);
    ASSERT_TRUE("not_worth_it", strstr(out, "nicht mal") != NULL);
}

TEST(worth_it_above_doener_threshold) {
    // cheap=1.60, exp=2.00, fill=60, cons=6.3, dist=1
    // gross=24.00, detour=0.20, net=23.80 — above 8 EUR Döner threshold
    const char *out = run("-c 1.60 -e 2.00 -f 60 --consumption 6.3 -d 1");
    ASSERT_TRUE("net_value",  strstr(out, "23.80") != NULL);
    ASSERT_TRUE("worth_it",   strstr(out, "drinne") != NULL);
    ASSERT_FALSE("no_nicht_mal", strstr(out, "nicht mal") != NULL);
}

TEST(same_price_detour_costs_money) {
    // NEVER: detour to a station with identical price must show negative savings.
    // diff=0 → gross=0, net=-(detour cost)=-2.27
    const char *out = run("-c 1.80 -e 1.80 -f 30 --consumption 6.3 -d 10");
    ASSERT_TRUE("negative_net", strstr(out, "-2.27") != NULL);
    ASSERT_TRUE("not_worth_it", strstr(out, "nicht mal") != NULL);
}

TEST(at_breakeven_distance_net_is_zero) {
    // fill=40, diff=0.10, cons=4.0, cheap=2.00, dist=25
    // breakeven_distance = 40*0.10*50 / (4.0*2.00) = 25 km exactly
    // So net = gross - detour = 4.00 - 4.00 = 0.00
    const char *out = run("-c 2.00 -e 2.10 -f 40 --consumption 4.0 -d 25");
    ASSERT_TRUE("net_zero", strstr(out, "  0.00 EUR") != NULL);
}

TEST(breakeven_figures_in_output) {
    const char *out = run("-c 1.80 -e 1.92 -f 30 --consumption 6.3 -d 10");
    ASSERT_TRUE("distance_max",   strstr(out, "distance max") != NULL);
    ASSERT_TRUE("price_diff_min", strstr(out, "price diff min") != NULL);
    ASSERT_TRUE("fill_min",       strstr(out, "fill min") != NULL);
}

TEST(zero_consumption_suppresses_breakeven) {
    // can_calculate_breakeven() blocks division by zero on consumption=0.
    const char *out = run("-c 1.80 -e 1.92 -f 30 --consumption 0 -d 10");
    ASSERT_TRUE("undefined",    strstr(out, "undefined") != NULL);
    ASSERT_FALSE("no_dist_max", strstr(out, "distance max") != NULL);
}

// =============================================================================

int
main(int argc, char **argv) {
    return tt_main(argc, argv);
}

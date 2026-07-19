#!/usr/bin/env -S ccraft
// tankfahrt - break-even analysis for detour to cheaper gas station
//
// Is it worth driving to the cheaper gas station?
// Calculates savings minus fuel cost for the detour.
//
// Usage:
//   ./tankfahrt.c [OPTIONS]
//
// Options:
//   --cheap, -c      price at cheap station (EUR/l)
//   --expensive, -e  price at expensive station (EUR/l)
//   --fill, -f       fill amount (liters)
//   --consumption    consumption (l/100km)
//   --distance, -d   distance to cheap station, one way (km)
//   --help, -h       show this help
//
// Examples:
//   ./tankfahrt.c --cheap 1.75 --expensive 1.89 --fill 40 --distance 8
//   ./tankfahrt.c -c 1.75 -e 1.89 -f 40 -d 8
//
// Formula:
//   savings = fill_amount * price_diff - detour_cost
//   detour_cost = 2 * distance * consumption/100 * price_cheap
//
// Break-even (savings = 0) solved for:
//   distance_max = fill_amount * price_diff * 50 / (consumption * price_cheap)
//   price_diff_min = distance * consumption * price_cheap / (50 * fill_amount)
//   fill_amount_min = distance * consumption * price_cheap / (50 * price_diff)

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#define WORTH_IT_THRESHOLD 8.0  /* EUR - price of a döner */

static int
is_worth_it(double savings) {
    return savings >= WORTH_IT_THRESHOLD;
}

static int
can_calculate_breakeven(double price, double consumption, double diff) {
    return price > 0 && consumption > 0 && diff > 0;
}

/* Fuel cost for round-trip detour */
static double
detour_fuel_cost(double distance, double consumption, double price) {
    return (2 * distance * consumption / 100) * price;
}

/* Maximum one-way distance where detour breaks even */
static double
breakeven_distance(double fill, double diff, double consumption, double price) {
    return (fill * diff * 50) / (consumption * price);
}

/* Minimum price difference needed to break even */
static double
breakeven_price_diff(double distance, double consumption, double price, double fill) {
    return (distance * consumption * price) / (50 * fill);
}

/* Minimum fill amount needed to break even */
static double
breakeven_fill(double distance, double consumption, double price, double diff) {
    return (distance * consumption * price) / (50 * diff);
}

struct params {
    double price_cheap;
    double price_expensive;
    double fill_amount;
    double consumption;
    double distance;
};

static void
print_report(struct params p) {
    double price_diff = p.price_expensive - p.price_cheap;
    double detour_cost = detour_fuel_cost(p.distance, p.consumption, p.price_cheap);
    double gross_savings = p.fill_amount * price_diff;
    double net_savings = gross_savings - detour_cost;

    printf("\n");
    printf("  TANKFAHRT · fuel detour calculator\n");
    printf("  ───────────────────────────────────────────────────\n");
    printf("  price cheap      %6.2f EUR/l\n", p.price_cheap);
    printf("  price expensive  %6.2f EUR/l\n", p.price_expensive);
    printf("  price diff       %6.2f EUR/l\n", price_diff);
    printf("  fill amount      %6.0f l\n", p.fill_amount);
    printf("  consumption      %6.1f l/100km\n", p.consumption);
    printf("  distance oneway  %6.0f km\n", p.distance);
    printf("  distance round   %6.0f km\n", p.distance * 2);
    printf("  ───────────────────────────────────────────────────\n");
    printf("  saved at pump    %6.2f EUR\n", gross_savings);
    printf("  spent on detour  %6.2f EUR\n", detour_cost);
    printf("  ───────────────────────────────────────────────────\n");
    printf("  NET SAVINGS      %6.2f EUR  %s\n", net_savings,
           is_worth_it(net_savings) ? "<-- Döner is drinne" : "<-- nicht mal ein Döner drinne");
    printf("  ───────────────────────────────────────────────────\n");

    if (can_calculate_breakeven(p.price_cheap, p.consumption, price_diff)) {
        double dist_max = breakeven_distance(p.fill_amount, price_diff, p.consumption, p.price_cheap);
        double diff_min = breakeven_price_diff(p.distance, p.consumption, p.price_cheap, p.fill_amount);
        double fill_min = breakeven_fill(p.distance, p.consumption, p.price_cheap, price_diff);
        printf("  break-even\n");
        printf("    distance max   %6.1f km\n", dist_max);
        printf("    price diff min %6.2f EUR/l\n", diff_min);
        printf("    fill min       %6.1f l\n", fill_min);
    } else {
        printf("  break-even: undefined (check inputs)\n");
    }
    printf("\n");
}

static void
usage(void) {
    printf("Usage: tankfahrt [OPTIONS]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c, --cheap        price at cheap station (EUR/l)\n");
    printf("  -e, --expensive    price at expensive station (EUR/l)\n");
    printf("  -f, --fill         fill amount (liters)\n");
    printf("      --consumption  consumption (l/100km)\n");
    printf("  -d, --distance     distance to cheap station, one way (km)\n");
    printf("  -h, --help         show this help\n");
}

int
main(int argc, char **argv) {
    struct params p = {
        .price_cheap = 1.80,
        .price_expensive = 1.92,
        .fill_amount = 30,
        .consumption = 6.3,
        .distance = 10,
    };

    static struct option long_options[] = {
        {"cheap",       required_argument, 0, 'c'},
        {"expensive",   required_argument, 0, 'e'},
        {"fill",        required_argument, 0, 'f'},
        {"consumption", required_argument, 0, 'C'},
        {"distance",    required_argument, 0, 'd'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:e:f:d:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'c': p.price_cheap = atof(optarg); break;
        case 'e': p.price_expensive = atof(optarg); break;
        case 'f': p.fill_amount = atof(optarg); break;
        case 'C': p.consumption = atof(optarg); break;
        case 'd': p.distance = atof(optarg); break;
        case 'h': usage(); return 0;
        default: usage(); return 1;
        }
    }

    print_report(p);
    return 0;
}

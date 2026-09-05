/* P00 characterization test for the tile reader / map matcher as it exists
 * today.
 *
 * Purpose: record what `get_speed_and_name_at()` currently answers for a fixed
 * sequence of fixes over the committed dataset, so P04 can prove the extracted
 * matcher is *equivalent* instead of merely "looking better". This test asserts
 * nothing about correctness: a wrong answer that the device produces today is
 * still recorded, and changing it must be a deliberate, explained commit.
 *
 * Two properties of the current code shape this test and are documented here as
 * baseline findings, not as accepted design:
 *
 *  1. `get_speed_and_name_at()` keeps hidden function-static state
 *     (locked_name / locked_speed / have_lock / locked_anon) with no reset
 *     entry point. The result of fix N therefore depends on fixes 1..N-1, so
 *     the fix list below is an ordered sequence, and the golden file is only
 *     valid for that exact order run in a fresh process.
 *  2. The tile cache is process-global with no teardown, so cache counters are
 *     cumulative for the whole run.
 *
 * Fixtures were derived from the committed dataset (python/tiles) by taking the
 * midpoint of a named segment with a non-zero limit inside a spread of tiles,
 * and the course over ground along that segment. See the trailing comment on
 * each row for the street the dataset holds there; the matcher is free to
 * disagree and the golden records what it actually says.
 *
 * Usage:
 *   ./test_tile_reader_baseline                 compare against the golden file
 *   ./test_tile_reader_baseline --update        rewrite the golden file
 *   HOST_LOG_LEVEL=4 ./test_tile_reader_baseline    show component logs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tile_reader.h"
#include "tile_cache.h"

#ifndef GOLDEN_PATH
#define GOLDEN_PATH "fixtures/tile_reader_baseline.golden"
#endif

typedef struct {
    float lat;
    float lon;
    float cog_deg;
    float speed_kmh;
    const char *note;
} fix_t;

/* Ordered sequence. Speeds straddle HEADING_MIN_SPEED_KMH (8 km/h) so both the
 * heading-weighted and the heading-blind paths are exercised, and the two
 * repeated fixes at the end exercise the stickiness lock. */
static const fix_t FIXES[] = {
    { -34.4728243f, -58.4916759f, 302.0f, 40.0f, "Sebastian Elcano, limit 40, tile -34473_-58494" },
    { -34.4810114f, -58.5093140f, 243.5f, 55.0f, "Avenida de la Unidad Nacional, limit 60, tile -34482_-58512" },
    { -34.4854611f, -58.4910674f, 149.6f, 30.0f, "Lavalle, limit 40, tile -34488_-58494" },
    { -34.4909406f, -58.5493577f, 163.4f,  4.0f, "Uspallata, limit 40, below heading threshold" },
    { -34.4954183f, -58.5189738f, 241.7f, 25.0f, "Cordoba, limit 40, tile -34497_-58521" },
    { -34.5023523f, -58.4918448f, 240.9f, 60.0f, "Doctor Jose Ingenieros, limit 40, tile -34503_-58494" },
    { -34.5030632f, -58.5504492f, 221.4f, 12.0f, "Eduardo Wilde, limit 40, tile -34506_-58551" },
    { -34.5092059f, -58.5117383f, 241.5f, 45.0f, "Carlos Gardel, limit 40, tile -34512_-58512" },
    { -34.5123804f, -58.5570657f,  43.6f, 20.0f, "Juan Agustin Maza, limit 40, tile -34515_-58560" },
    { -34.5203296f, -58.5003588f, 238.5f, 70.0f, "Ingeniero Guillermo Marconi, limit 40, tile -34521_-58503" },
    /* Repeat of the previous fix: same position, cache hit and lock in place. */
    { -34.5203296f, -58.5003588f, 238.5f, 70.0f, "repeat, exercises cache hit and stickiness" },
    /* Null-island junk fix: current code rejects it without touching the card. */
    {   0.0000000f,   0.0000000f,   0.0f, 30.0f, "null-island fix, expected reject" },
    /* Middle of the Rio de la Plata, inside the dataset bbox but off any road. */
    { -34.4000000f, -58.3000000f,  90.0f, 80.0f, "off-dataset position, expected no match" },
};

#define NFIXES (sizeof(FIXES) / sizeof(FIXES[0]))

/* One deterministic line per fix. Coordinates are printed at the precision the
 * fixture carries so a diff points at the fix that moved. */
static void render(char *out, size_t out_len, size_t idx, const fix_t *f)
{
    int speed = -1;
    char street[MAX_STREET_NAME];
    street[0] = '\0';

    bool ok = get_speed_and_name_at(f->lat, f->lon, f->cog_deg, f->speed_kmh,
                                    &speed, street, (int)sizeof(street));

    snprintf(out, out_len,
             "%02zu lat=%.7f lon=%.7f cog=%.1f v=%.1f -> match=%d speed=%d street=\"%s\"",
             idx, (double)f->lat, (double)f->lon, (double)f->cog_deg,
             (double)f->speed_kmh, ok ? 1 : 0, ok ? speed : -1,
             ok ? street : "");
}

static int run(FILE *sink, char lines[NFIXES + 1][512])
{
    for (size_t i = 0; i < NFIXES; ++i) {
        render(lines[i], sizeof(lines[i]), i, &FIXES[i]);
        if (sink) {
            fprintf(sink, "%s\n", lines[i]);
        }
    }

    uint32_t hits = 0, misses = 0, entries = 0;
    size_t bytes = 0;
    tile_cache_stats(&hits, &misses, &bytes, &entries);
    snprintf(lines[NFIXES], sizeof(lines[NFIXES]),
             "cache hits=%u misses=%u bytes=%zu entries=%u",
             (unsigned)hits, (unsigned)misses, bytes, (unsigned)entries);
    if (sink) {
        fprintf(sink, "%s\n", lines[NFIXES]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool update = (argc > 1 && strcmp(argv[1], "--update") == 0);
    const char *golden = GOLDEN_PATH;

    static char lines[NFIXES + 1][512];

    if (update) {
        FILE *f = fopen(golden, "w");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", golden);
            return 2;
        }
        fprintf(f, "# Generated by test_tile_reader_baseline --update.\n"
                   "# Baseline behaviour of the pre-P04 matcher. Do not edit by hand;\n"
                   "# a change here must come with the commit that explains it.\n");
        run(f, lines);
        fclose(f);
        printf("golden updated: %s\n", golden);
        return 0;
    }

    FILE *f = fopen(golden, "r");
    if (!f) {
        fprintf(stderr,
                "missing golden file %s -- run with --update once and review the diff\n",
                golden);
        return 2;
    }

    run(NULL, lines);

    int failures = 0;
    size_t idx = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n == 0 || buf[0] == '#') {
            continue;
        }
        if (idx >= NFIXES + 1) {
            fprintf(stderr, "golden has more rows than the fix list\n");
            ++failures;
            break;
        }
        if (strcmp(buf, lines[idx]) != 0) {
            fprintf(stderr, "mismatch on row %zu\n  golden: %s\n  actual: %s\n",
                    idx, buf, lines[idx]);
            ++failures;
        }
        ++idx;
    }
    fclose(f);

    if (idx != NFIXES + 1) {
        fprintf(stderr, "golden has %zu rows, expected %zu\n", idx, NFIXES + 1);
        ++failures;
    }

    if (failures) {
        fprintf(stderr, "tile_reader baseline FAILED (%d)\n", failures);
        return 1;
    }
    printf("tile_reader baseline: %zu fixes match the golden\n", NFIXES);
    return 0;
}

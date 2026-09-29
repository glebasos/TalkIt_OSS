/*
 * test_dllload.c — the run-time loader (src/dllload.c) against the build-time
 * extractors (tools/extract_*.py), table by table, byte for byte.
 *
 *   test_dllload TIBASE32.DLL TIENG32.DLL TISPAN32.DLL
 *
 * Built only when all three DLLs were given at configure time, so the
 * extractors' output is linked in to compare against.
 */

#include "tispeech/dllload.h"

#include <stdio.h>
#include <string.h>

extern const sv_frame_tables sv_base_tables;
extern const sv_expr_tables sv_base_tables_expression;
extern const sv_phoneme_names sv_base_tables_phonemes;
extern const uint8_t sv_base_image[];
extern const uint32_t sv_base_image_va, sv_base_image_size;
extern const uint8_t sv_eng_image[];
extern const uint32_t sv_eng_image_va, sv_eng_image_size;
extern const uint32_t sv_eng_image_desc[10];
extern const sv_image sv_eng_image_text[];
extern const size_t sv_eng_image_text_count;
extern const uint8_t sv_span_image[];
extern const uint32_t sv_span_image_va, sv_span_image_size;
extern const uint32_t sv_span_desc_image_desc[10];
extern const sv_image sv_span_image_text[];
extern const size_t sv_span_image_text_count;

static int failures;

#define CHECK(cond, what) do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static void same_image(const sv_image *got, const sv_image *want, const char *what)
{
    CHECK(got->va == want->va && got->size == want->size
          && memcmp(got->bytes, want->bytes, want->size) == 0, what);
}

static void same_lang(const sv_lang_image *got, const uint8_t *bytes, uint32_t va,
                      uint32_t size, const uint32_t desc[10], const sv_image *text,
                      size_t text_count, const char *what)
{
    sv_image want = {bytes, va, size};
    same_image(&got->image, &want, what);
    CHECK(memcmp(got->desc, desc, sizeof got->desc) == 0, what);
    CHECK(got->text_count == text_count, what);
    for (size_t i = 0; i < text_count && i < got->text_count; i++)
        same_image(&got->text[i], &text[i], what);
}

/* A ruleset is one blob addressed by 256 bucket pointers and a fallback;
 * compare the blobs and every pointer's offset into them. */
static const unsigned char *rules_lo(const sv_ruleset_t *r, size_t *len)
{
    const unsigned char *lo = r->fallback, *hi = r->fallback;
    for (int c = 0; c < 256; c++) {
        if (!r->buckets[c])
            continue;
        if (r->buckets[c] < lo)
            lo = r->buckets[c];
        if (r->buckets[c] > hi)
            hi = r->buckets[c];
    }
    *len = (size_t)(hi - lo) + strlen((const char *)hi) + 1;
    return lo;
}

static void same_rules(const sv_ruleset_t *got, const sv_ruleset_t *want, const char *what)
{
    size_t glen, wlen;
    const unsigned char *g = rules_lo(got, &glen), *w = rules_lo(want, &wlen);
    CHECK(memcmp(got->charclass, want->charclass, 256 * sizeof *want->charclass) == 0, what);
    CHECK(got->qmark_sub == want->qmark_sub, what);
    CHECK(glen == wlen && memcmp(g, w, wlen) == 0, what);
    CHECK(got->fallback - g == want->fallback - w, what);
    for (int c = 0; c < 256; c++)
        CHECK(!got->buckets[c] == !want->buckets[c]
              && (!want->buckets[c] || got->buckets[c] - g == want->buckets[c] - w), what);
}

int main(int argc, char **argv)
{
    sv_pe base, eng, span;
    sv_arena a = {0};
    sv_base_data b;
    sv_ruleset_t rules_eng, rules_span;
    sv_lang_image eng_img, span_img;

    if (argc != 4) {
        fprintf(stderr, "usage: test_dllload TIBASE32.DLL TIENG32.DLL TISPAN32.DLL\n");
        return 2;
    }
    if (sv_pe_open(&base, argv[1]) || sv_pe_open(&eng, argv[2]) || sv_pe_open(&span, argv[3])) {
        fprintf(stderr, "FAIL: cannot open the DLLs\n");
        return 1;
    }

    CHECK(sv_load_base(&base, &a, &b) == 0, "load TIBASE32");
    CHECK(memcmp(b.frames.resonator, sv_base_tables.resonator, SV_RESONATOR_BYTES) == 0,
          "resonator");
    CHECK(memcmp(b.frames.amplitude, sv_base_tables.amplitude, 0x102) == 0, "amplitude");
    for (int i = 0; i < 12; i++)
        CHECK(memcmp(b.frames.glottal[i], sv_base_tables.glottal[i], 1024 * 2) == 0,
              "glottal");
    CHECK(memcmp(b.frames.noise, sv_base_tables.noise, 2048 * 2) == 0, "noise");
    CHECK(memcmp(b.frames.output_curve, sv_base_tables.output_curve, 401) == 0,
          "output curve");
    CHECK(memcmp(b.expr.flutter, sv_base_tables_expression.flutter, 0x101) == 0, "flutter");
    CHECK(memcmp(b.expr.lfo, sv_base_tables_expression.lfo, 0x100) == 0, "lfo");
    CHECK(memcmp(b.expr.voices, sv_base_tables_expression.voices, 74 * 20) == 0, "voices");
    CHECK(memcmp(b.expr.source_map, sv_base_tables_expression.source_map, 0x10e * 5) == 0,
          "source map");
    for (int i = 0; i < 0x3c; i++) {
        const uint8_t *g = b.phonemes.lists[i], *w = sv_base_tables_phonemes.lists[i];
        size_t n = 0;
        CHECK(!g == !w, "phoneme list presence");
        if (g && w) {
            while (w[n] != 0)
                n += 2;
            CHECK(memcmp(g, w, n + 2) == 0, "phoneme list");
        }
    }
    {
        sv_image want = {sv_base_image, sv_base_image_va, sv_base_image_size};
        same_image(&b.image, &want, "TIBASE32 .data image");
    }

    CHECK(sv_load_rules(&eng, 0, &a, &rules_eng) == 0, "load English rules");
    same_rules(&rules_eng, &sv_lang_data_eng, "English rules");
    CHECK(sv_load_rules(&span, 1, &a, &rules_span) == 0, "load Spanish rules");
    same_rules(&rules_span, &sv_lang_data_span, "Spanish rules");

    CHECK(sv_load_eng_image(&eng, &a, &eng_img) == 0, "load English image");
    same_lang(&eng_img, sv_eng_image, sv_eng_image_va, sv_eng_image_size, sv_eng_image_desc,
              sv_eng_image_text, sv_eng_image_text_count, "English image");
    CHECK(sv_load_span_image(&eng, &span, &a, &span_img) == 0, "load Spanish image");
    same_lang(&span_img, sv_span_image, sv_span_image_va, sv_span_image_size,
              sv_span_desc_image_desc, sv_span_image_text, sv_span_image_text_count,
              "Spanish image");

    /* The wrong DLL for each loader is refused, not misread. */
    CHECK(sv_load_base(&eng, &a, &b) != 0, "TIENG32 as TIBASE32");
    CHECK(sv_load_rules(&base, 0, &a, &rules_eng) != 0, "TIBASE32 as English rules");
    CHECK(sv_load_rules(&eng, 1, &a, &rules_span) != 0, "TIENG32 as Spanish rules");
    CHECK(sv_load_eng_image(&span, &a, &eng_img) != 0, "TISPAN32 as TIENG32");

    sv_arena_free(&a);
    sv_pe_close(&base);
    sv_pe_close(&eng);
    sv_pe_close(&span);
    if (failures)
        return 1;
    puts("PASS: run-time tables match the build-time extraction");
    return 0;
}

/*
 * dllload.c — run-time counterpart of tools/extract_*.py; see dllload.h.
 *
 * Each function below is a line-for-line port of one extractor, including
 * the way it reads the file: extract_generator.py's Image (a VA resolves in
 * the first section that covers it, and anything past that section's raw data
 * reads as zero), extract_lang.py's PE (raw file offsets, an uninitialised
 * tail is an error) and extract_base.py's PE (sections laid out flat at their
 * RVAs). They differ at the edges, and the edges are what the tables use.
 */

#include "tispeech/dllload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

/* ------------------------------------------------------------------------ */
/* Files and allocations.                                                   */
/* ------------------------------------------------------------------------ */

int sv_pe_open(sv_pe *pe, const char *path)
{
    FILE *f;
    long size;
    uint32_t hdr, optional;
    uint16_t optional_size;

    memset(pe, 0, sizeof *pe);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0x40 || size > (64L << 20)
        || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    pe->file = malloc((size_t)size);
    if (!pe->file || fread(pe->file, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        sv_pe_close(pe);
        return -1;
    }
    fclose(f);
    pe->file_size = (size_t)size;

    if (pe->file[0] != 'M' || pe->file[1] != 'Z')
        goto bad;
    hdr = rd32(pe->file + 0x3c);
    if (hdr > pe->file_size - 24 || memcmp(pe->file + hdr, "PE\0\0", 4) != 0)
        goto bad;
    if (rd16(pe->file + hdr + 4) != 0x14c)                  /* i386 */
        goto bad;
    pe->nsections = rd16(pe->file + hdr + 6);
    optional_size = rd16(pe->file + hdr + 20);
    optional = hdr + 24;
    if (pe->nsections > 32 || optional + 60 > pe->file_size
        || optional + optional_size + 40u * (uint32_t)pe->nsections > pe->file_size)
        goto bad;
    pe->base = rd32(pe->file + optional + 28);
    pe->image_size = rd32(pe->file + optional + 56);
    for (int i = 0; i < pe->nsections; i++) {
        const uint8_t *s = pe->file + optional + optional_size + 40 * i;
        memcpy(pe->sections[i].name, s, 8);
        pe->sections[i].name[8] = '\0';
        pe->sections[i].vsize = rd32(s + 8);
        pe->sections[i].rva = rd32(s + 12);
        pe->sections[i].rawsize = rd32(s + 16);
        pe->sections[i].rawoff = rd32(s + 20);
    }
    return 0;
bad:
    sv_pe_close(pe);
    return -1;
}

void sv_pe_close(sv_pe *pe)
{
    free(pe->file);
    memset(pe, 0, sizeof *pe);
}

static void *arena_alloc(sv_arena *a, size_t n)
{
    void *p;
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 16;
        void **b = realloc(a->blocks, cap * sizeof *b);
        if (!b)
            return NULL;
        a->blocks = b;
        a->cap = cap;
    }
    p = calloc(n ? n : 1, 1);
    if (p)
        a->blocks[a->n++] = p;
    return p;
}

void sv_arena_free(sv_arena *a)
{
    for (size_t i = 0; i < a->n; i++)
        free(a->blocks[i]);
    free(a->blocks);
    memset(a, 0, sizeof *a);
}

/* ------------------------------------------------------------------------ */
/* extract_generator.py's Image.                                            */
/* ------------------------------------------------------------------------ */

static uint32_t sec_span(const sv_pe *pe, int i)
{
    uint32_t v = pe->sections[i].vsize, r = pe->sections[i].rawsize;
    return v > r ? v : r;
}

/* Image.read: the first section covering `va`, zero past its raw data. */
static int img_read(const sv_pe *pe, uint32_t va, uint32_t n, uint8_t *out)
{
    for (int i = 0; i < pe->nsections; i++) {
        uint32_t start = pe->base + pe->sections[i].rva;
        if (va >= start && va - start < sec_span(pe, i)) {
            uint32_t off = va - start, rawsize = pe->sections[i].rawsize;
            size_t have = off < rawsize ? rawsize - off : 0, from;
            if (have > n)
                have = n;
            from = (size_t)pe->sections[i].rawoff + off;
            /* Python slicing stops at the end of the file. */
            if (from >= pe->file_size)
                have = 0;
            else if (have > pe->file_size - from)
                have = pe->file_size - from;
            if (have)
                memcpy(out, pe->file + from, have);
            memset(out + have, 0, n - have);
            return 0;
        }
    }
    return -1;
}

static int img_u32(const sv_pe *pe, uint32_t va, uint32_t *v)
{
    uint8_t b[4];
    if (img_read(pe, va, 4, b))
        return -1;
    *v = rd32(b);
    return 0;
}

static int img_section(const sv_pe *pe, const char *name)
{
    for (int i = 0; i < pe->nsections; i++)
        if (strcmp(pe->sections[i].name, name) == 0)
            return i;
    return -1;
}

/* descriptor_fields(): decode LoadLanguage's stores (module +0x1030).
 * `have` gets bit (offset / 4) for every field published. */
static int descriptor_fields(const sv_pe *pe, uint32_t desc[10], uint32_t *have)
{
    uint32_t va = pe->base + 0x1030, end = va + 0xa0;
    memset(desc, 0, 10 * sizeof *desc);
    *have = 0;
    while (va < end) {
        uint8_t op[2];
        if (img_read(pe, va, 2, op))
            return -1;
        if (op[0] == 0xa1) {                        /* mov eax, [imm32] */
            va += 5;
        } else if (op[0] == 0xc7 && op[1] == 0x40) { /* mov [eax+disp8], imm32 */
            uint8_t disp;
            uint32_t v;
            if (img_read(pe, va + 2, 1, &disp) || img_u32(pe, va + 3, &v))
                return -1;
            if (disp < 0x28 && disp % 4 == 0) {
                desc[disp / 4] = v;
                *have |= 1u << (disp / 4);
            }
            va += 7;
        } else if (op[0] == 0xc7 && op[1] == 0x00) { /* mov [eax], imm32 */
            if (img_u32(pe, va + 2, &desc[0]))
                return -1;
            *have |= 1u;
            va += 6;
        } else if (op[0] == 0x33) {                 /* xor reg, reg */
            va += 2;
        } else if (op[0] == 0xc3) {
            break;
        } else {
            va += 1;
        }
    }
    /* LoadLanguage must publish both phoneme tables. */
    return (*have & (1u << 5)) && (*have & (1u << 6)) ? 0 : -1;
}

static const uint8_t *mem_find(const uint8_t *hay, size_t n, const uint8_t *needle,
                               size_t m)
{
    for (size_t i = 0; m <= n && i <= n - m; i++)
        if (memcmp(hay + i, needle, m) == 0)
            return hay + i;
    return NULL;
}

/* find_rate_table(): the interpolation-rate table, anchored on FUN_1c203870's
 * ramp constant `mov eax, 0x01000100` and the `mov ebx, [table + eax*4]` after
 * it. */
static int find_rate_table(const sv_pe *pe, uint32_t *table)
{
    static const uint8_t ramp[5] = {0xb8, 0x00, 0x01, 0x00, 0x01};
    static const uint8_t load[3] = {0x8b, 0x1c, 0x85};
    const uint8_t *raw, *a, *l;
    size_t n, window;
    int t = img_section(pe, ".text");

    if (t < 0 || pe->sections[t].rawoff > pe->file_size)
        return -1;
    raw = pe->file + pe->sections[t].rawoff;
    n = pe->sections[t].rawsize;
    if (n > pe->file_size - pe->sections[t].rawoff)
        n = pe->file_size - pe->sections[t].rawoff;
    a = mem_find(raw, n, ramp, sizeof ramp);
    if (!a || mem_find(a + 1, n - (size_t)(a + 1 - raw), ramp, sizeof ramp))
        return -1;
    window = n - (size_t)(a - raw);
    if (window > 0x100)
        window = 0x100;
    l = mem_find(a, window, load, sizeof load);
    if (!l || (size_t)(l + 7 - raw) > n)
        return -1;
    *table = rd32(l + 3);
    return 0;
}

/* extract_images.py: a section read whole through Image.read. */
static int load_section(const sv_pe *pe, const char *name, sv_arena *a, sv_image *out)
{
    int s = img_section(pe, name);
    uint8_t *b;
    if (s < 0)
        return -1;
    out->va = pe->base + pe->sections[s].rva;
    out->size = sec_span(pe, s);
    b = arena_alloc(a, out->size);
    if (!b || img_read(pe, out->va, out->size, b))
        return -1;
    out->bytes = b;
    return 0;
}

static int load_range(const sv_pe *pe, uint32_t lo, uint32_t hi, uint32_t at,
                      sv_arena *a, sv_image *out)
{
    uint8_t *b = arena_alloc(a, hi - lo);
    if (!b || img_read(pe, lo, hi - lo, b))
        return -1;
    out->bytes = b;
    out->va = at;
    out->size = hi - lo;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* extract_lang.py.                                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint32_t image_base, charclass, bucket_base, fallback, qmark_sub;
    uint32_t latin1[4][2];   /* codepoint, VA of its bucket pointer */
} lang_layout;

static const lang_layout LAYOUT_ENG = {
    0x1c200000, 0x1c209c20, 0x1c24c744, 0x1c24c8c4, 0x1c24c8c8,
    {{0xc4, 0x1c24c8b0}, {0xd6, 0x1c24c8b4}, {0xdc, 0x1c24c8bc}, {0xdf, 0x1c24c8b8}},
};
static const lang_layout LAYOUT_SPAN = {
    0x1c400000, 0x1c409aa0, 0x1c40e23c, 0x1c40e3bc, 0x1c40e3c0,
    {{0xc4, 0x1c40e3a8}, {0xd6, 0x1c40e3ac}, {0xdc, 0x1c40e3b4}, {0xdf, 0x1c40e3b0}},
};

/* PE.off: a file offset, refusing the uninitialised tail of a section. */
static int raw_off(const sv_pe *pe, uint32_t va, size_t *off)
{
    uint32_t rva = va - pe->base;
    for (int i = 0; i < pe->nsections; i++) {
        uint32_t sva = pe->sections[i].rva;
        if (rva >= sva && rva - sva < sec_span(pe, i)) {
            uint32_t d = rva - sva;
            if (d >= pe->sections[i].rawsize)
                return -1;
            *off = (size_t)pe->sections[i].rawoff + d;
            return *off < pe->file_size ? 0 : -1;
        }
    }
    return -1;
}

static int raw_u32(const sv_pe *pe, uint32_t va, uint32_t *v)
{
    size_t o;
    if (raw_off(pe, va, &o) || o + 4 > pe->file_size)
        return -1;
    *v = rd32(pe->file + o);
    return 0;
}

/* A bucket must point at rule text: '[' ... ']' ... '=' within 64 bytes. */
static int looks_like_rules(const sv_pe *pe, uint32_t ptr)
{
    size_t o, n;
    const uint8_t *w, *lb, *rb;
    if (raw_off(pe, ptr, &o))
        return 0;
    w = pe->file + o;
    n = pe->file_size - o < 64 ? pe->file_size - o : 64;
    lb = memchr(w, '[', n);
    rb = memchr(w, ']', n);
    return lb && rb && lb < rb && memchr(rb, '=', n - (size_t)(rb - w)) != NULL;
}

int sv_load_rules(const sv_pe *pe, int spanish, sv_arena *a, sv_ruleset_t *out)
{
    const lang_layout *L = spanish ? &LAYOUT_SPAN : &LAYOUT_ENG;
    uint32_t buckets[256] = {0}, fallback, lo, hi;
    uint16_t *charclass;
    const unsigned char **table;
    unsigned char *blob;
    size_t o, start, end, len;

    if (pe->base != L->image_base)
        return -1;
    charclass = arena_alloc(a, 256 * sizeof *charclass);
    table = arena_alloc(a, 256 * sizeof *table);
    if (!charclass || !table)
        return -1;
    for (int i = 0; i < 256; i++) {
        if (raw_off(pe, L->charclass + 2u * (uint32_t)i, &o) || o + 2 > pe->file_size)
            return -1;
        charclass[i] = rd16(pe->file + o);
    }

    /* Only 'A'..'Z' are reachable through the signed-char-indexed table. */
    for (int c = 0x41; c < 0x5b; c++) {
        uint32_t ptr;
        if (raw_u32(pe, L->bucket_base + 4u * (uint32_t)c, &ptr))
            continue;
        if (ptr && looks_like_rules(pe, ptr))
            buckets[c] = ptr;
    }
    /* The four Latin-1 letters the matcher switches on explicitly. */
    for (int i = 0; i < 4; i++) {
        uint32_t ptr;
        if (raw_u32(pe, L->latin1[i][1], &ptr))
            return -1;
        if (ptr && looks_like_rules(pe, ptr))
            buckets[L->latin1[i][0]] = ptr;
    }
    if (raw_u32(pe, L->fallback, &fallback) || raw_off(pe, L->qmark_sub, &o))
        return -1;
    out->qmark_sub = pe->file[o];

    /* One contiguous blob from the lowest bucket to the first NUL after the
     * highest; buckets are offsets into it. */
    lo = hi = fallback;
    for (int c = 0; c < 256; c++) {
        if (!buckets[c])
            continue;
        if (buckets[c] < lo)
            lo = buckets[c];
        if (buckets[c] > hi)
            hi = buckets[c];
    }
    if (raw_off(pe, lo, &start) || raw_off(pe, hi, &end))
        return -1;
    while (end < pe->file_size && pe->file[end] != 0)
        end++;
    if (end < start)
        return -1;
    len = end - start;
    blob = arena_alloc(a, len + 1);                  /* + the NUL terminator */
    if (!blob)
        return -1;
    memcpy(blob, pe->file + start, len);

    /* Drop pointers implausibly far from the cluster. */
    for (int c = 0; c < 256; c++)
        table[c] = buckets[c] && buckets[c] - lo < len ? blob + (buckets[c] - lo) : NULL;
    out->charclass = charclass;
    out->buckets = table;
    out->fallback = blob + (fallback - lo);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* extract_base.py.                                                         */
/* ------------------------------------------------------------------------ */

enum {
    BASE_IMAGE = 0x1c000000,
    RESONATOR = 0x1c002570,
    AMPLITUDE = 0x1c001308, AMPLITUDE_BYTES = 0x102,
    NOISE = 0x1c001570, NOISE_SAMPLES = 2048,
    OUTPUT_CURVE = 0x1c0010fe, OUTPUT_CURVE_BYTES = 401,
    GLOTTAL_PTRS = 0x1c013bd0, GLOTTAL_COUNT = 12, GLOTTAL_SAMPLES = 1024,
    PHONEME_PTRS = 0x1c012a68, PHONEME_LISTS = 0x3c,
    FRAME_RATE = 0x1c0120e8,
    FLUTTER = 0x1c001470, FLUTTER_BYTES = 0x101,
    LFO = 0x1c013430, LFO_BYTES = 0x100,
    VOICES = 0x1c013600, VOICE_STRIDE = 74, VOICE_ROWS = 20,
    SOURCE_MAP = 0x1c012ee8, SOURCE_MAP_BYTES = 0x10e * 5,
    LOOP_VA = 0x1c004120,
    AMPLITUDE_READ_VA = 0x1c004857,
};

typedef struct {
    uint8_t *img;
    uint32_t size;
} flat_image;

/* extract_base.py's PE: every section's raw data at its RVA. */
static int flat_map(const sv_pe *pe, sv_arena *a, flat_image *f)
{
    if (pe->base != BASE_IMAGE || pe->image_size == 0 || pe->image_size > (64u << 20))
        return -1;
    f->size = pe->image_size;
    f->img = arena_alloc(a, f->size);
    if (!f->img)
        return -1;
    for (int i = 0; i < pe->nsections; i++) {
        size_t n = pe->sections[i].rawsize, from = pe->sections[i].rawoff;
        uint32_t rva = pe->sections[i].rva;
        if (from >= pe->file_size || rva >= f->size)
            continue;
        if (n > pe->file_size - from)
            n = pe->file_size - from;
        if (n > f->size - rva)
            n = f->size - rva;
        memcpy(f->img + rva, pe->file + from, n);
    }
    return 0;
}

static const uint8_t *flat_at(const flat_image *f, uint32_t va, uint32_t n)
{
    uint32_t off = va - BASE_IMAGE;
    if (va < BASE_IMAGE || off > f->size || n > f->size - off)
        return NULL;
    return f->img + off;
}

static int flat_u32(const flat_image *f, uint32_t va, uint32_t *v)
{
    const uint8_t *p = flat_at(f, va, 4);
    if (!p)
        return -1;
    *v = rd32(p);
    return 0;
}

static const uint8_t *flat_copy(const flat_image *f, uint32_t va, uint32_t n, sv_arena *a)
{
    const uint8_t *p = flat_at(f, va, n);
    uint8_t *b;
    if (!p || !(b = arena_alloc(a, n)))
        return NULL;
    memcpy(b, p, n);
    return b;
}

static const int16_t *flat_int16(const flat_image *f, uint32_t va, uint32_t count,
                                 sv_arena *a)
{
    const uint8_t *p = flat_at(f, va, count * 2);
    int16_t *out;
    if (!p || !(out = arena_alloc(a, count * sizeof *out)))
        return NULL;
    for (uint32_t i = 0; i < count; i++)
        out[i] = (int16_t)rd16(p + 2 * i);
    return out;
}

/* check_build(): refuse a DLL that is not the build these addresses are from. */
static int check_base_build(const flat_image *f)
{
    /* mov edx, [ebx+0x2fc] -- the verified sample loop */
    static const uint8_t loop[6] = {0x8b, 0x93, 0xfc, 0x02, 0x00, 0x00};
    /* movzx edi, byte [esi+0x10]; mov ax, [edi+0x1c001308] */
    static const uint8_t amp[11] = {0x0f, 0xb6, 0x7e, 0x10, 0x66, 0x8b, 0x87,
                                    0x08, 0x13, 0x00, 0x1c};
    const uint8_t *p = flat_at(f, LOOP_VA, sizeof loop);
    if (!p || memcmp(p, loop, sizeof loop))
        return -1;
    p = flat_at(f, AMPLITUDE_READ_VA, sizeof amp);
    if (!p || memcmp(p, amp, sizeof amp))
        return -1;
    p = flat_at(f, FRAME_RATE, 4);
    return p && rd16(p) == 150 && rd16(p + 2) == 60 ? 0 : -1;
}

int sv_load_base(const sv_pe *pe, sv_arena *a, sv_base_data *out)
{
    flat_image f;
    const int16_t **glottal;
    uint32_t v;

    memset(out, 0, sizeof *out);
    if (flat_map(pe, a, &f) || check_base_build(&f))
        return -1;

    /* The voice table ends at the first NULL name pointer: 20 named rows. */
    for (uint32_t row = 0; row < VOICE_ROWS; row++)
        if (flat_u32(&f, VOICES + row * VOICE_STRIDE, &v) || v == 0)
            return -1;
    if (flat_u32(&f, VOICES + VOICE_ROWS * VOICE_STRIDE, &v) || v != 0)
        return -1;
    if (SOURCE_MAP + SOURCE_MAP_BYTES + 2 != LFO)
        return -1;

    /* Twelve waveform pointers; the thirteenth dword is ASCII, not a pointer. */
    glottal = arena_alloc(a, GLOTTAL_COUNT * sizeof *glottal);
    if (!glottal)
        return -1;
    for (uint32_t i = 0; i < GLOTTAL_COUNT; i++) {
        if (flat_u32(&f, GLOTTAL_PTRS + 4 * i, &v)
            || !(glottal[i] = flat_int16(&f, v, GLOTTAL_SAMPLES, a)))
            return -1;
    }
    if (flat_u32(&f, GLOTTAL_PTRS + 4 * GLOTTAL_COUNT, &v)
        || (v >= BASE_IMAGE && v - BASE_IMAGE < f.size))
        return -1;

    /* Phoneme-name pair lists; entry 0x3b is NULL in this build. */
    for (uint32_t i = 0; i < PHONEME_LISTS; i++) {
        uint32_t step;
        const uint8_t *p;
        if (flat_u32(&f, PHONEME_PTRS + 4 * i, &v))
            return -1;
        if (v == 0)
            continue;
        for (step = 0; step < 64; step++) {
            if (!(p = flat_at(&f, v + 2 * step, 2)))
                return -1;
            if (p[0] == 0)
                break;
        }
        if (step == 64 || !(out->phonemes.lists[i] = flat_copy(&f, v, 2 * (step + 1), a)))
            return -1;
    }
    if (out->phonemes.lists[0x3b] != NULL)
        return -1;

    out->frames.resonator = flat_copy(&f, RESONATOR, SV_RESONATOR_BYTES, a);
    out->frames.amplitude = flat_copy(&f, AMPLITUDE, AMPLITUDE_BYTES, a);
    out->frames.glottal = glottal;
    out->frames.noise = flat_int16(&f, NOISE, NOISE_SAMPLES, a);
    out->frames.output_curve = flat_copy(&f, OUTPUT_CURVE, OUTPUT_CURVE_BYTES, a);
    out->expr.flutter = flat_copy(&f, FLUTTER, FLUTTER_BYTES, a);
    out->expr.lfo = flat_copy(&f, LFO, LFO_BYTES, a);
    out->expr.voices = flat_copy(&f, VOICES, VOICE_STRIDE * VOICE_ROWS, a);
    out->expr.source_map = flat_copy(&f, SOURCE_MAP, SOURCE_MAP_BYTES, a);
    if (!out->frames.resonator || !out->frames.amplitude || !out->frames.noise
        || !out->frames.output_curve || !out->expr.flutter || !out->expr.lfo
        || !out->expr.voices || !out->expr.source_map)
        return -1;

    return load_section(pe, ".data", a, &out->image);
}

/* ------------------------------------------------------------------------ */
/* Language images.                                                         */
/* ------------------------------------------------------------------------ */

int sv_load_eng_image(const sv_pe *eng, sv_arena *a, sv_lang_image *out)
{
    uint32_t have;
    memset(out, 0, sizeof *out);
    if (eng->base != LAYOUT_ENG.image_base || load_section(eng, ".data", a, &out->image)
        || descriptor_fields(eng, out->desc, &have))
        return -1;
    /* CMakeLists.txt's --text-range arguments for TIENG32. */
    if (load_range(eng, 0x1c205fb9, 0x1c206780, 0x1c205fb9, a, &out->text[0])
        || load_range(eng, 0x1c209c20, 0x1c249310, 0x1c209c20, a, &out->text[1]))
        return -1;
    out->text_count = 2;
    return 0;
}

/* extract_span_frontend.py. */
enum {
    BUF_DVA = 0x1c24b000,     /* TIENG32 .data VA: langgen.c's coordinate space */
    BUF_HI = 0x1c412000,
    TEXT_LO_ENG = 0x1c205ff4, TEXT_HI_ENG = 0x1c206780,
    SPAN_TEXT_LO = 0x1c409aa0, SPAN_TEXT_HI = 0x1c40b600,
    SPAN_DATA_LO = 0x1c40d000,
};

static const uint32_t OVERLAY_TABLES[3][2] = {
    {0x1c24c3f0, 16}, {0x1c24c420, 64}, {0x1c24c460, 1000},
};
static const uint32_t IDENTITY_CHECKS[3][2] = {
    {0x1c24c0d4, 0x1c24c117 - 0x1c24c0d4},
    {0x1c24c1a0, 0x1c24c1b0 - 0x1c24c1a0},
    {0x1c24c400, 0x1c24c420 - 0x1c24c400},
};

static int same_bytes(const sv_pe *x, uint32_t xva, const sv_pe *y, uint32_t yva,
                      uint32_t n)
{
    uint8_t *p = malloc(n), *q = malloc(n);
    int same = p && q && !img_read(x, xva, n, p) && !img_read(y, yva, n, q)
               && memcmp(p, q, n) == 0;
    free(p);
    free(q);
    return same;
}

int sv_load_span_image(const sv_pe *eng, const sv_pe *span, sv_arena *a,
                       sv_lang_image *out)
{
    uint32_t rate_eng, rate_span, delta, code_delta, have, fe[10], size;
    int s = img_section(eng, ".data");
    uint8_t *buf;
    uint32_t eng_len;

    memset(out, 0, sizeof *out);
    if (s < 0 || eng->base + eng->sections[s].rva != BUF_DVA)
        return -1;
    if (find_rate_table(eng, &rate_eng) || find_rate_table(span, &rate_span)
        || descriptor_fields(eng, fe, &have) || !(have & 1u)
        || descriptor_fields(span, out->desc, &have) || !(have & 1u))
        return -1;
    delta = rate_span - rate_eng;          /* mod 2^32, as the VAs are */
    code_delta = out->desc[0] - fe[0];

    /* The "universal constants" claim, checked, not assumed. */
    for (int i = 0; i < 3; i++)
        if (!same_bytes(eng, IDENTITY_CHECKS[i][0], span, IDENTITY_CHECKS[i][0] + delta,
                        IDENTITY_CHECKS[i][1]))
            return -1;
    if (!same_bytes(eng, TEXT_LO_ENG, span, TEXT_LO_ENG + code_delta,
                    TEXT_HI_ENG - TEXT_LO_ENG))
        return -1;

    size = BUF_HI - BUF_DVA;
    buf = arena_alloc(a, size);
    eng_len = sec_span(eng, s);
    if (!buf || eng_len > size || img_read(eng, BUF_DVA, eng_len, buf))
        return -1;
    for (int i = 0; i < 3; i++)
        if (img_read(span, OVERLAY_TABLES[i][0] + delta, OVERLAY_TABLES[i][1],
                     buf + (OVERLAY_TABLES[i][0] - BUF_DVA)))
            return -1;
    if (SPAN_TEXT_LO - BUF_DVA < eng_len
        || img_read(span, SPAN_TEXT_LO, SPAN_TEXT_HI - SPAN_TEXT_LO,
                    buf + (SPAN_TEXT_LO - BUF_DVA))
        || img_read(span, SPAN_DATA_LO, BUF_HI - SPAN_DATA_LO,
                    buf + (SPAN_DATA_LO - BUF_DVA)))
        return -1;
    out->image.bytes = buf;
    out->image.va = BUF_DVA;
    out->image.size = size;

    /* langgen.c's TB() range, read from TISPAN32 at TIENG32's addresses. */
    if (load_range(span, TEXT_LO_ENG + code_delta, TEXT_HI_ENG + code_delta,
                   TEXT_LO_ENG, a, &out->text[0]))
        return -1;
    out->text_count = 1;
    return 0;
}

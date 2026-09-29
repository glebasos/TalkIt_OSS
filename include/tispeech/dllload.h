/*
 * dllload.h — read the SoftVoice tables out of the original DLLs at run time.
 *
 * The C counterpart of tools/extract_*.py. Those write the tables into the
 * build tree as C source; these fill the same structures in memory, so a
 * library built without any DLL can be given the user's copies when it runs.
 * Same contract as the extractors: each DLL is read as a file and parsed. It is
 * never mapped, relocated or executed, and every table is checked for shape
 * exactly as the extractor checks it.
 *
 * tests/test_dllload.c compares everything here byte for byte against the
 * extractors' output.
 */

#ifndef TISPEECH_DLLLOAD_H
#define TISPEECH_DLLLOAD_H

#include "tispeech/expression.h"
#include "tispeech/frames.h"
#include "tispeech/narrate.h"
#include "tispeech/ruleset.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A PE image read into memory as a file. */
typedef struct sv_pe {
    uint8_t *file;
    size_t file_size;
    uint32_t base;          /* ImageBase */
    uint32_t image_size;    /* SizeOfImage */
    int nsections;
    struct {
        char name[9];
        uint32_t rva, vsize, rawsize, rawoff;
    } sections[32];
} sv_pe;

/* 0 on success; -1 if the file cannot be read or is not an i386 PE image. */
int sv_pe_open(sv_pe *pe, const char *path);
void sv_pe_close(sv_pe *pe);

/* Every allocation behind one loaded set of tables; sv_arena_free releases
 * them together. */
typedef struct sv_arena {
    void **blocks;
    size_t n, cap;
} sv_arena;

void sv_arena_free(sv_arena *a);

/* TIBASE32: tools/extract_base.py (frame and expression tables, phoneme
 * names) and tools/extract_images.py (the .data image). */
typedef struct sv_base_data {
    sv_frame_tables frames;
    sv_expr_tables expr;
    sv_phoneme_names phonemes;
    sv_image image;
} sv_base_data;

int sv_load_base(const sv_pe *pe, sv_arena *a, sv_base_data *out);

/* Letter-to-sound tables: tools/extract_lang.py. `spanish` selects TISPAN32's
 * table addresses instead of TIENG32's. */
int sv_load_rules(const sv_pe *pe, int spanish, sv_arena *a, sv_ruleset_t *out);

/* A language module's .data image, its LoadLanguage descriptor and the tables
 * it keeps in .text: tools/extract_images.py --language. */
typedef struct sv_lang_image {
    sv_image image;
    uint32_t desc[10];
    sv_image text[2];
    size_t text_count;
} sv_lang_image;

/* TIENG32, with the two .text ranges CMakeLists.txt passes the extractor. */
int sv_load_eng_image(const sv_pe *eng, sv_arena *a, sv_lang_image *out);

/* Spanish: tools/extract_span_frontend.py's merged image and text segment,
 * with the descriptor from TISPAN32 (extract_images.py --language). */
int sv_load_span_image(const sv_pe *eng, const sv_pe *span, sv_arena *a,
                       sv_lang_image *out);

#ifdef __cplusplus
}
#endif

#endif

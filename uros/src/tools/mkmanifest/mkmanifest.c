/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * mkmanifest — host build-time tool that compiles a plain-text
 * Uros capability manifest source (.manifest) into the binary TLV
 * blob (.cmf) consumed by cap_server (#216 v2.1).
 *
 * Why a binary format produced at build time: cap_server is in the
 * trusted base; a YAML/JSON/TOML parser is 1-10k lines of complex
 * C that would more than double the server's surface.  Compiling
 * at host build catches syntax errors at "ninja", not at boot.
 *
 * Source grammar (line-based, lenient):
 *
 *     # comments after '#' to end of line, blank lines ignored
 *     name        <server_name>            # mandatory, once
 *     required    <type-int> <ops-hex>     # zero or more
 *     delegatable <type-int> <ops-hex>     # zero or more
 *
 * Types are the cap_resource_type_t enum values from
 * <mach/cap_types.h> (4 == RESOURCE_BLK_DEVICE, etc.).  Ops are a
 * uint64 bitmask in C hex (0x3 == BLK_READ|BLK_WRITE).  Symbolic
 * names for both are out of scope for v0.1 — the goal here is the
 * smallest possible host tool that round-trips correctly.
 *
 * Usage:
 *     mkmanifest -o <out.cmf> <in.manifest>
 */

#include <mach/cap_manifest.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static void
die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mkmanifest: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/*
 * Trim trailing whitespace and stop at the first '#'.  Returns the
 * possibly-shortened string in place.
 */
static char *
strip_line(char *s)
{
    char *hash = strchr(s, '#');
    if (hash) *hash = '\0';
    /* trim trailing whitespace */
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    /* trim leading whitespace */
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* ------------------------------------------------------------------ */
/*  Accumulator state                                                   */
/* ------------------------------------------------------------------ */

static char                  g_name[CAP_MANIFEST_MAX_NAME];
static cap_manifest_entry_t  g_required[CAP_MANIFEST_MAX_ENTRIES];
static unsigned              g_required_count;
static cap_manifest_entry_t  g_delegatable[CAP_MANIFEST_MAX_ENTRIES];
static unsigned              g_delegatable_count;

static void
parse_entry(const char *kw, char *args, cap_manifest_entry_t *out_arr,
            unsigned *out_count, int lineno)
{
    char *type_s, *id_s, *ops_s, *end;
    unsigned long long type, id, ops;

    if (*out_count >= CAP_MANIFEST_MAX_ENTRIES)
        die("line %d: too many %s entries (max %d)",
            lineno, kw, CAP_MANIFEST_MAX_ENTRIES);

    /*
     * 🔴 THREE FIELDS IN v2, AND A TWO-FIELD LINE IS REFUSED RATHER THAN
     * READ AS "ANY".  A manifest written for version 1 means "any resource of
     * this type", and accepting it silently here would compile a file whose
     * author was describing one world into a policy for another -- the exact
     * shape of a permission that is wider than the person who wrote it
     * believes.  The refusal names the line; adding `any' is one word.
     */
    type_s = strtok(args, " \t");
    id_s   = strtok(NULL, " \t");
    ops_s  = strtok(NULL, " \t");
    if (!type_s || !id_s || !ops_s)
        die("line %d: %s needs <type> <id|any> <ops> — v2 added the resource "
            "id, and a v1 line meant `any'", lineno, kw);

    errno = 0;
    type = strtoull(type_s, &end, 0);
    if (errno || *end || type > 0xffffffffULL)
        die("line %d: bad type '%s'", lineno, type_s);

    if (strcmp(id_s, "any") == 0) {
        id = CAP_MANIFEST_ANY_ID;
    } else {
        errno = 0;
        id = strtoull(id_s, &end, 0);
        if (errno || *end)
            die("line %d: bad resource id '%s'", lineno, id_s);
    }

    errno = 0;
    ops = strtoull(ops_s, &end, 0);
    if (errno || *end)
        die("line %d: bad ops '%s'", lineno, ops_s);

    out_arr[*out_count].type = (uint32_t)type;
    out_arr[*out_count].resource_id = id;
    out_arr[*out_count]._pad = 0;
    out_arr[*out_count].ops  = (uint64_t)ops;
    (*out_count)++;
}

static void
parse_source(FILE *in)
{
    char line[512];
    int lineno = 0;

    while (fgets(line, sizeof(line), in)) {
        lineno++;
        char *p = strip_line(line);
        if (!*p) continue;

        char *kw   = strtok(p,    " \t");
        char *rest = strtok(NULL, "");
        if (!kw) continue;

        if (strcmp(kw, "name") == 0) {
            if (!rest || !*rest)
                die("line %d: name needs a value", lineno);
            while (*rest && isspace((unsigned char)*rest)) rest++;
            if (strlen(rest) >= CAP_MANIFEST_MAX_NAME)
                die("line %d: name too long (max %d)",
                    lineno, CAP_MANIFEST_MAX_NAME - 1);
            if (g_name[0])
                die("line %d: name already set to '%s'", lineno, g_name);
            strcpy(g_name, rest);
        } else if (strcmp(kw, "required") == 0) {
            if (!rest) die("line %d: required needs args", lineno);
            parse_entry("required", rest, g_required,
                        &g_required_count, lineno);
        } else if (strcmp(kw, "delegatable") == 0) {
            if (!rest) die("line %d: delegatable needs args", lineno);
            parse_entry("delegatable", rest, g_delegatable,
                        &g_delegatable_count, lineno);
        } else {
            die("line %d: unknown keyword '%s'", lineno, kw);
        }
    }

    if (!g_name[0])
        die("missing 'name' directive");
}

/* ------------------------------------------------------------------ */
/*  Emit                                                                */
/* ------------------------------------------------------------------ */

static void
emit(FILE *out)
{
    cap_manifest_header_t h;
    uint32_t off;
    uint32_t name_len = (uint32_t)strlen(g_name) + 1u;
    uint32_t req_bytes = g_required_count    * sizeof(cap_manifest_entry_t);
    uint32_t del_bytes = g_delegatable_count * sizeof(cap_manifest_entry_t);

    memset(&h, 0, sizeof(h));
    h.magic       = CAP_MANIFEST_MAGIC;
    h.version     = (uint16_t)CAP_MANIFEST_VERSION;
    h.header_size = (uint16_t)sizeof(h);

    off = (uint32_t)sizeof(h);
    h.required_offset = off;            h.required_count    = g_required_count;
    off += req_bytes;
    h.delegatable_offset = off;         h.delegatable_count = g_delegatable_count;
    off += del_bytes;
    h.name_offset = off;                h.name_length       = name_len;
    off += name_len;

    /* Pad total_size up to 4-byte boundary so consumers can read
     * past-the-end without UB if they want trailing alignment. */
    while (off & 3u) off++;
    h.total_size = off;

    if (h.total_size > CAP_MANIFEST_MAX_BYTES)
        die("blob too large (%u > %u)",
            h.total_size, CAP_MANIFEST_MAX_BYTES);

    if (fwrite(&h, sizeof(h), 1, out) != 1)
        die("write header: %s", strerror(errno));
    if (req_bytes &&
        fwrite(g_required, req_bytes, 1, out) != 1)
        die("write required: %s", strerror(errno));
    if (del_bytes &&
        fwrite(g_delegatable, del_bytes, 1, out) != 1)
        die("write delegatable: %s", strerror(errno));
    if (fwrite(g_name, name_len, 1, out) != 1)
        die("write name: %s", strerror(errno));
    /* Trailing pad — zeros, may be empty */
    while ((uint32_t)ftell(out) < h.total_size) {
        if (fputc(0, out) == EOF)
            die("write pad: %s", strerror(errno));
    }

    fprintf(stderr, "mkmanifest: %s -> %u bytes "
            "(name='%s' required=%u delegatable=%u)\n",
            g_name, h.total_size, g_name,
            g_required_count, g_delegatable_count);
}

/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    const char *in_path  = NULL;
    const char *out_path = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (argv[i][0] == '-') {
            die("unknown option '%s'", argv[i]);
        } else if (!in_path) {
            in_path = argv[i];
        } else {
            die("extra positional arg '%s'", argv[i]);
        }
    }
    if (!in_path || !out_path)
        die("usage: mkmanifest -o <out.cmf> <in.manifest>");

    FILE *in = fopen(in_path, "r");
    if (!in) die("open %s: %s", in_path, strerror(errno));
    parse_source(in);
    fclose(in);

    FILE *out = fopen(out_path, "wb");
    if (!out) die("open %s: %s", out_path, strerror(errno));
    emit(out);
    fclose(out);

    return 0;
}

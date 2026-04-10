/*
 * template/template_vars.c — Variable substitution engine
 *
 * Supports:
 *   ${var}           — basic substitution
 *   $var             — bash-style
 *   {{var}}          — Jinja2-style
 *   ${var:-default}  — default if unset
 *   ${var:+alt}      — alternate if set
 *   ${var^^}         — uppercase
 *   ${var,,}         — lowercase
 *
 * Max 10 iterations to resolve nested variables.
 *
 * Copyright 2026 Gritiva
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../internal.h"
#include "template_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITERATIONS 10

/* ─── Lookup ─────────────────────────────────────────────────────── */

const char *tmpl_var_get(const tmpl_var_t *vars, int count, const char *key)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(vars[i].key, key) == 0)
            return vars[i].value;
    }
    return NULL;
}

int tmpl_var_set(tmpl_var_t **vars, int *count, int *capacity,
                  const char *key, const char *value)
{
    /* Update existing */
    for (int i = 0; i < *count; i++) {
        if (strcmp((*vars)[i].key, key) == 0) {
            gscope_strlcpy((*vars)[i].value, value, sizeof((*vars)[i].value));
            return 0;
        }
    }

    /* Add new */
    if (*count >= *capacity) {
        int new_cap = *capacity == 0 ? 32 : *capacity * 2;
        tmpl_var_t *new_vars = realloc(*vars, (size_t)new_cap * sizeof(tmpl_var_t));
        if (!new_vars) return -1;
        *vars = new_vars;
        *capacity = new_cap;
    }

    gscope_strlcpy((*vars)[*count].key, key, sizeof((*vars)[*count].key));
    gscope_strlcpy((*vars)[*count].value, value, sizeof((*vars)[*count].value));
    (*count)++;
    return 0;
}

/* ─── String Helpers ─────────────────────────────────────────────── */

static void str_upper(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static void str_lower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* ─── Single-pass Substitution ───────────────────────────────────── */

/*
 * Substitute variables in `input`, write result to `output`.
 * Returns number of substitutions made.
 */
static int substitute_once(const char *input, char *output, size_t output_size,
                            const tmpl_var_t *vars, int var_count)
{
    int subs = 0;
    size_t out_pos = 0;
    const char *p = input;

    while (*p && out_pos < output_size - 1) {
        /* Check for {{var}} */
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                char key[128];
                size_t klen = (size_t)(end - p - 2);
                if (klen < sizeof(key)) {
                    memcpy(key, p + 2, klen);
                    key[klen] = '\0';

                    /* Trim whitespace */
                    char *k = key;
                    while (*k == ' ') k++;
                    char *ke = k + strlen(k) - 1;
                    while (ke > k && *ke == ' ') *ke-- = '\0';

                    const char *val = tmpl_var_get(vars, var_count, k);
                    if (val) {
                        size_t vlen = strlen(val);
                        if (out_pos + vlen < output_size) {
                            memcpy(output + out_pos, val, vlen);
                            out_pos += vlen;
                        }
                        p = end + 2;
                        subs++;
                        continue;
                    }
                }
            }
        }

        /* Check for ${var}, ${var:-default}, ${var^^}, etc. */
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (end) {
                char expr[4096];
                size_t elen = (size_t)(end - p - 2);
                if (elen < sizeof(expr)) {
                    memcpy(expr, p + 2, elen);
                    expr[elen] = '\0';

                    /* Check for modifiers */
                    char *mod;
                    char key[128];
                    const char *result = NULL;
                    char modified[4096];

                    if ((mod = strstr(expr, ":-")) != NULL) {
                        /* ${var:-default} */
                        *mod = '\0';
                        gscope_strlcpy(key, expr, sizeof(key));
                        const char *val = tmpl_var_get(vars, var_count, key);
                        result = (val && val[0]) ? val : (mod + 2);
                    } else if ((mod = strstr(expr, ":+")) != NULL) {
                        /* ${var:+alternate} */
                        *mod = '\0';
                        gscope_strlcpy(key, expr, sizeof(key));
                        const char *val = tmpl_var_get(vars, var_count, key);
                        result = (val && val[0]) ? (mod + 2) : "";
                    } else if (elen >= 2 && expr[elen-2] == '^' && expr[elen-1] == '^') {
                        /* ${var^^} uppercase */
                        expr[elen-2] = '\0';
                        const char *val = tmpl_var_get(vars, var_count, expr);
                        if (val) {
                            gscope_strlcpy(modified, val, sizeof(modified));
                            str_upper(modified);
                            result = modified;
                        }
                    } else if (elen >= 2 && expr[elen-2] == ',' && expr[elen-1] == ',') {
                        /* ${var,,} lowercase */
                        expr[elen-2] = '\0';
                        const char *val = tmpl_var_get(vars, var_count, expr);
                        if (val) {
                            gscope_strlcpy(modified, val, sizeof(modified));
                            str_lower(modified);
                            result = modified;
                        }
                    } else {
                        /* ${var} basic */
                        result = tmpl_var_get(vars, var_count, expr);
                    }

                    if (result) {
                        size_t rlen = strlen(result);
                        if (out_pos + rlen < output_size) {
                            memcpy(output + out_pos, result, rlen);
                            out_pos += rlen;
                        }
                        p = end + 1;
                        subs++;
                        continue;
                    }
                }
            }
        }

        /* Check for $var (simple, word-boundary) */
        if (p[0] == '$' && p[1] != '{' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            const char *start = p + 1;
            const char *e = start;
            while (isalnum((unsigned char)*e) || *e == '_') e++;

            char key[128];
            size_t klen = (size_t)(e - start);
            if (klen < sizeof(key)) {
                memcpy(key, start, klen);
                key[klen] = '\0';

                const char *val = tmpl_var_get(vars, var_count, key);
                if (val) {
                    size_t vlen = strlen(val);
                    if (out_pos + vlen < output_size) {
                        memcpy(output + out_pos, val, vlen);
                        out_pos += vlen;
                    }
                    p = e;
                    subs++;
                    continue;
                }
            }
        }

        /* No match — copy literal character */
        output[out_pos++] = *p++;
    }

    output[out_pos] = '\0';
    return subs;
}

/* ─── Public: Multi-pass Substitution ────────────────────────────── */

int tmpl_vars_substitute(const char *input, char *output, size_t output_size,
                          const tmpl_var_t *vars, int var_count)
{
    char buf_a[16384];
    char buf_b[16384];

    /* Use buf_a as initial input copy */
    gscope_strlcpy(buf_a, input, sizeof(buf_a));

    char *src = buf_a;
    char *dst = buf_b;
    int total_subs = 0;

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        int subs = substitute_once(src, dst, sizeof(buf_b), vars, var_count);
        total_subs += subs;

        if (subs == 0)
            break;  /* No more substitutions — done */

        /* Swap buffers for next iteration */
        char *tmp = src;
        src = dst;
        dst = tmp;
    }

    /* Copy final result to output */
    gscope_strlcpy(output, src, output_size);
    return total_subs;
}

/* ─── Add Built-in Variables ─────────────────────────────────────── */

void tmpl_vars_add_builtins(tmpl_var_t **vars, int *count, int *capacity,
                             gscope_scope_t *scope)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "%u", scope->id);
    tmpl_var_set(vars, count, capacity, "scope_id", buf);
    tmpl_var_set(vars, count, capacity, "SCOPE_ID", buf);

    tmpl_var_set(vars, count, capacity, "rootfs", scope->rootfs_merged);
    tmpl_var_set(vars, count, capacity, "rootfs_path", scope->rootfs_merged);

    char home[4096];
    snprintf(home, sizeof(home), "%s/root", scope->rootfs_merged);
    tmpl_var_set(vars, count, capacity, "home", "/root");
    tmpl_var_set(vars, count, capacity, "HOME", "/root");

    tmpl_var_set(vars, count, capacity, "tmp", "/tmp");
    tmpl_var_set(vars, count, capacity, "USER", scope->username[0] ? scope->username : "root");

    tmpl_var_set(vars, count, capacity, "APP_DIR", "/opt/app");
    tmpl_var_set(vars, count, capacity, "DATA_DIR", "/opt/data");
    tmpl_var_set(vars, count, capacity, "CONFIG_DIR", "/etc/app");
    tmpl_var_set(vars, count, capacity, "LOG_DIR", "/var/log/app");
    tmpl_var_set(vars, count, capacity, "CACHE_DIR", "/var/cache/app");
    tmpl_var_set(vars, count, capacity, "BIN_DIR", "/usr/local/bin");
    tmpl_var_set(vars, count, capacity, "LIB_DIR", "/usr/local/lib");

    tmpl_var_set(vars, count, capacity, "LANG", "en_US.UTF-8");
    tmpl_var_set(vars, count, capacity, "LC_ALL", "en_US.UTF-8");
    tmpl_var_set(vars, count, capacity, "PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    tmpl_var_set(vars, count, capacity, "DEBIAN_FRONTEND", "noninteractive");

    if (scope->netns_name[0])
        tmpl_var_set(vars, count, capacity, "netns", scope->netns_name);

    if (scope->ip_address[0])
        tmpl_var_set(vars, count, capacity, "HOST", scope->ip_address);
}

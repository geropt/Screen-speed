#include "backup_cfg.h"

#include <ctype.h>
#include <string.h>

static void cfg_zero(backup_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static size_t trim_len(const char *s, size_t n)
{
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    return n;
}

static bool key_eq(const char *k, size_t n, const char *lit)
{
    size_t i;
    for (i = 0; lit[i] != '\0'; i++) {
        if (i >= n) {
            return false;
        }
        if (tolower((unsigned char)k[i]) != (unsigned char)lit[i]) {
            return false;
        }
    }
    return i == n;
}

static bool host_ok(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || n > BACKUP_CFG_HOST_MAX) {
        return false;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '.' || c == '-' || c == ':') {
            continue;   /* ':' permite IPv6 literal acotado */
        }
        return false;
    }
    return true;
}

static backup_cfg_err_t parse_port(const char *s, size_t n, uint16_t *out)
{
    uint32_t v = 0;
    size_t i;
    if (n == 0) {
        return BACKUP_CFG_ERR_PORT;
    }
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return BACKUP_CFG_ERR_PORT;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
        if (v > 65535u) {
            return BACKUP_CFG_ERR_PORT;
        }
    }
    if (v < 1u) {
        return BACKUP_CFG_ERR_PORT;
    }
    *out = (uint16_t)v;
    return BACKUP_CFG_OK;
}

static backup_cfg_err_t apply_kv(backup_cfg_t *out, const char *key, size_t kn,
                                 const char *val, size_t vn)
{
    if (key_eq(key, kn, "host")) {
        if (!host_ok(val, vn)) {
            return BACKUP_CFG_ERR_HOST;
        }
        memcpy(out->host, val, vn);
        out->host[vn] = '\0';
        out->have_host = true;
        return BACKUP_CFG_OK;
    }
    if (key_eq(key, kn, "port")) {
        backup_cfg_err_t e = parse_port(val, vn, &out->port);
        if (e != BACKUP_CFG_OK) {
            return e;
        }
        out->have_port = true;
        return BACKUP_CFG_OK;
    }
    if (key_eq(key, kn, "ssid")) {
        if (vn == 0 || vn > BACKUP_CFG_SSID_MAX) {
            return BACKUP_CFG_ERR_NET;
        }
        if (out->net_count >= BACKUP_CFG_MAX_NETS) {
            return BACKUP_CFG_ERR_NET;
        }
        backup_cfg_net_t *n = &out->nets[out->net_count++];
        memset(n, 0, sizeof(*n));
        memcpy(n->ssid, val, vn);
        n->ssid[vn] = '\0';
        return BACKUP_CFG_OK;
    }
    if (key_eq(key, kn, "password")) {
        if (out->net_count == 0) {
            return BACKUP_CFG_ERR_NET;
        }
        if (vn > BACKUP_CFG_PASS_MAX) {
            return BACKUP_CFG_ERR_NET;
        }
        backup_cfg_net_t *n = &out->nets[out->net_count - 1];
        memcpy(n->pass, val, vn);
        n->pass[vn] = '\0';
        n->have_pass = true;
        return BACKUP_CFG_OK;
    }
    return BACKUP_CFG_ERR_SYNTAX;
}

backup_cfg_err_t backup_cfg_parse(const char *text, backup_cfg_t *out)
{
    if (!text || !out) {
        return BACKUP_CFG_ERR_EMPTY;
    }
    cfg_zero(out);

    const char *p = text;
    while (*p) {
        const char *line = p;
        while (*p && *p != '\n') {
            p++;
        }
        size_t ln = (size_t)(p - line);
        if (*p == '\n') {
            p++;
        }
        if (ln > 0 && line[ln - 1] == '\r') {
            ln--;
        }

        const char *s = skip_ws(line);
        size_t left = ln - (size_t)(s - line);
        left = trim_len(s, left);
        if (left == 0 || s[0] == '#') {
            continue;
        }

        const char *eq = NULL;
        size_t i;
        for (i = 0; i < left; i++) {
            if (s[i] == '=') {
                eq = s + i;
                break;
            }
        }
        if (!eq) {
            return BACKUP_CFG_ERR_SYNTAX;
        }

        size_t kn = trim_len(s, (size_t)(eq - s));
        const char *val = skip_ws(eq + 1);
        size_t vn = trim_len(val, left - (size_t)(val - s));
        if (kn == 0) {
            return BACKUP_CFG_ERR_SYNTAX;
        }

        backup_cfg_err_t e = apply_kv(out, s, kn, val, vn);
        if (e != BACKUP_CFG_OK) {
            return e;
        }
    }

    if (!out->have_host && !out->have_port && out->net_count == 0) {
        return BACKUP_CFG_ERR_EMPTY;
    }
    return BACKUP_CFG_OK;
}

const char *backup_cfg_err_name(backup_cfg_err_t err)
{
    switch (err) {
    case BACKUP_CFG_OK:
        return "ok";
    case BACKUP_CFG_ERR_EMPTY:
        return "vacio";
    case BACKUP_CFG_ERR_SYNTAX:
        return "sintaxis";
    case BACKUP_CFG_ERR_HOST:
        return "host";
    case BACKUP_CFG_ERR_PORT:
        return "puerto";
    case BACKUP_CFG_ERR_NET:
        return "red";
    default:
        return "desconocido";
    }
}

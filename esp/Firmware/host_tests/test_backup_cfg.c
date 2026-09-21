/* Parser de /sdcard/backup.cfg: host, puerto y redes sin pasar por el binario. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "backup_cfg.h"

static void test_vacio(void)
{
    backup_cfg_t c;
    assert(backup_cfg_parse("", &c) == BACKUP_CFG_ERR_EMPTY);
    assert(backup_cfg_parse("# solo comentario\n\n", &c) == BACKUP_CFG_ERR_EMPTY);
    assert(backup_cfg_parse(NULL, &c) == BACKUP_CFG_ERR_EMPTY);
}

static void test_host_puerto(void)
{
    backup_cfg_t c;
    const char *t =
        "host=46973.flespi.gw\n"
        "port=29043\n";
    assert(backup_cfg_parse(t, &c) == BACKUP_CFG_OK);
    assert(c.have_host);
    assert(c.have_port);
    assert(strcmp(c.host, "46973.flespi.gw") == 0);
    assert(c.port == 29043);
    assert(c.net_count == 0);
}

static void test_espacios_y_crlf(void)
{
    backup_cfg_t c;
    const char *t = "  HOST = 46973.flespi.gw \r\n  port = 443 \r\n";
    assert(backup_cfg_parse(t, &c) == BACKUP_CFG_OK);
    assert(strcmp(c.host, "46973.flespi.gw") == 0);
    assert(c.port == 443);
}

static void test_comentario(void)
{
    backup_cfg_t c;
    const char *t =
        "# canal de lab\n"
        "host=example.com\n"
        "port=1\n";
    assert(backup_cfg_parse(t, &c) == BACKUP_CFG_OK);
    assert(strcmp(c.host, "example.com") == 0);
}

static void test_host_invalido(void)
{
    backup_cfg_t c;
    assert(backup_cfg_parse("host=\n", &c) == BACKUP_CFG_ERR_HOST);
    assert(backup_cfg_parse("host=bad host\n", &c) == BACKUP_CFG_ERR_HOST);
}

static void test_puerto_invalido(void)
{
    backup_cfg_t c;
    assert(backup_cfg_parse("port=0\n", &c) == BACKUP_CFG_ERR_PORT);
    assert(backup_cfg_parse("port=65536\n", &c) == BACKUP_CFG_ERR_PORT);
    assert(backup_cfg_parse("port=abc\n", &c) == BACKUP_CFG_ERR_PORT);
}

static void test_sintaxis(void)
{
    backup_cfg_t c;
    assert(backup_cfg_parse("host 46973.flespi.gw\n", &c) == BACKUP_CFG_ERR_SYNTAX);
    assert(backup_cfg_parse("foo=bar\n", &c) == BACKUP_CFG_ERR_SYNTAX);
}

static void test_redes(void)
{
    backup_cfg_t c;
    const char *t =
        "ssid=ClaroRS\n"
        "password=secreto\n"
        "ssid=Personal-916-2.4GHz\n"
        "password=\n";
    assert(backup_cfg_parse(t, &c) == BACKUP_CFG_OK);
    assert(c.net_count == 2);
    assert(strcmp(c.nets[0].ssid, "ClaroRS") == 0);
    assert(strcmp(c.nets[0].pass, "secreto") == 0);
    assert(c.nets[0].have_pass);
    assert(strcmp(c.nets[1].ssid, "Personal-916-2.4GHz") == 0);
    assert(c.nets[1].pass[0] == '\0');
    assert(c.nets[1].have_pass);
}

static void test_password_huerfano(void)
{
    backup_cfg_t c;
    assert(backup_cfg_parse("password=x\n", &c) == BACKUP_CFG_ERR_NET);
}

static void test_ssid_demasiado_largo(void)
{
    backup_cfg_t c;
    char buf[80];
    memset(buf, 'a', 33);
    buf[33] = '\0';
    char line[96];
    snprintf(line, sizeof(line), "ssid=%s\n", buf);
    assert(backup_cfg_parse(line, &c) == BACKUP_CFG_ERR_NET);
}

int main(void)
{
    test_vacio();
    test_host_puerto();
    test_espacios_y_crlf();
    test_comentario();
    test_host_invalido();
    test_puerto_invalido();
    test_sintaxis();
    test_redes();
    test_password_huerfano();
    test_ssid_demasiado_largo();
    printf("backup_cfg tests passed\n");
    return 0;
}

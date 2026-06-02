/*
 * tests/test_decoder.c
 *
 * Standalone unit tests. Compiled separately so we can call internal helpers
 * directly. We replicate the exact packet layout used by simulated-mcu.py:
 *
 *   struct.pack("<HIIffH6s", magic, seq, timestamp, speed, batt_volt, fault, version)
 *   = uint16 uint32 uint32 float32 float32 uint16 6-bytes
 *   offsets: 0      2      6       10      14       18     20
 *   total: 26 bytes
 */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ──── minimal copies of types/helpers from telemetry_forwarder.c ──────── */

#define FIELD_NAME_MAX    64
#define FIELD_TYPE_MAX    16
#define PACKET_FIELDS_MAX 16
#define JSON_BUF_SIZE     2048
#define LOG_QUEUE_DEPTH   512
#define MQTT_QUEUE_DEPTH  256

typedef enum { FT_UINT8,FT_UINT16,FT_UINT32,FT_INT8,FT_INT16,FT_INT32,
               FT_FLOAT32,FT_FLOAT64,FT_STRING } FieldType;

typedef struct {
    char     name[FIELD_NAME_MAX];
    uint32_t offset;
    uint32_t size_bytes;
    FieldType type;
} FieldDef;

typedef struct {
    char     shm_path[256];
    uint32_t shm_size;
    uint32_t packet_size;
    int      little_endian;
    uint16_t magic_value;
    FieldDef fields[PACKET_FIELDS_MAX];
    int      nfields;
} Config;

static uint16_t read_u16_le(const uint8_t *p) { return (uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static float read_f32_le(const uint8_t *p) {
    uint32_t u = read_u32_le(p); float f; memcpy(&f,&u,4); return f;
}

static char *decode_packet(const uint8_t *raw, size_t raw_len, const Config *cfg) {
    if (raw_len < cfg->packet_size) return NULL;
    uint16_t magic = cfg->little_endian ? read_u16_le(raw) : (uint16_t)((raw[0]<<8)|raw[1]);
    if (magic != cfg->magic_value) return NULL;

    char *out = malloc(JSON_BUF_SIZE);
    if (!out) return NULL;
    int pos = 0;
    pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "{");

    for (int i = 0; i < cfg->nfields; i++) {
        const FieldDef *fd = &cfg->fields[i];
        if (fd->offset + fd->size_bytes > raw_len) continue;
        const uint8_t *fp = raw + fd->offset;
        if (i > 0) pos += snprintf(out+pos, JSON_BUF_SIZE-pos, ",");
        pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "\"%s\":", fd->name);
        switch (fd->type) {
            case FT_UINT8:  pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "%u", fp[0]); break;
            case FT_UINT16: pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "%u",
                (unsigned)(cfg->little_endian ? read_u16_le(fp) : (uint16_t)((fp[0]<<8)|fp[1]))); break;
            case FT_UINT32: pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "%u",
                (unsigned)(cfg->little_endian ? read_u32_le(fp) :
                ((uint32_t)fp[0]<<24|(uint32_t)fp[1]<<16|(uint32_t)fp[2]<<8|fp[3]))); break;
            case FT_FLOAT32: {
                float f = read_f32_le(fp);
                if (!isfinite(f)) pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "null");
                else pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "%.4g", (double)f);
                break;
            }
            case FT_STRING: {
                char tmp[64]={0};
                size_t slen = fd->size_bytes < 63 ? fd->size_bytes : 63;
                memcpy(tmp, fp, slen);
                pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "\"");
                for (int k=0;tmp[k]&&k<(int)slen;k++) {
                    unsigned char ch=(unsigned char)tmp[k];
                    if (ch=='"') pos+=snprintf(out+pos,JSON_BUF_SIZE-pos,"\\\"");
                    else if (ch=='\\') pos+=snprintf(out+pos,JSON_BUF_SIZE-pos,"\\\\");
                    else if (ch<0x20) pos+=snprintf(out+pos,JSON_BUF_SIZE-pos,"\\u%04x",ch);
                    else pos+=snprintf(out+pos,JSON_BUF_SIZE-pos,"%c",ch);
                }
                pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "\"");
                break;
            }
            default: pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "0"); break;
        }
    }
    pos += snprintf(out+pos, JSON_BUF_SIZE-pos, "}");
    return out;
}

/* ──── build the reference config (matches telemetry_config.json) ───────── */

static Config make_config(void) {
    Config c = {0};
    strncpy(c.shm_path, "/dev/shm/data", sizeof(c.shm_path)-1);
    c.shm_size     = 26;
    c.packet_size  = 26;
    c.little_endian = 1;
    c.magic_value  = 0xDEAD;

    int n = 0;
    /* magic   offset=0  size=2  uint16 */
    strncpy(c.fields[n].name,"magic",FIELD_NAME_MAX-1);
    c.fields[n].offset=0; c.fields[n].size_bytes=2; c.fields[n].type=FT_UINT16; n++;
    /* seq     offset=2  size=4  uint32 */
    strncpy(c.fields[n].name,"seq",FIELD_NAME_MAX-1);
    c.fields[n].offset=2; c.fields[n].size_bytes=4; c.fields[n].type=FT_UINT32; n++;
    /* timestamp offset=6 size=4 uint32 */
    strncpy(c.fields[n].name,"timestamp",FIELD_NAME_MAX-1);
    c.fields[n].offset=6; c.fields[n].size_bytes=4; c.fields[n].type=FT_UINT32; n++;
    /* speed   offset=10 size=4 float32 */
    strncpy(c.fields[n].name,"speed",FIELD_NAME_MAX-1);
    c.fields[n].offset=10; c.fields[n].size_bytes=4; c.fields[n].type=FT_FLOAT32; n++;
    /* batt_volt offset=14 size=4 float32 */
    strncpy(c.fields[n].name,"batt_volt",FIELD_NAME_MAX-1);
    c.fields[n].offset=14; c.fields[n].size_bytes=4; c.fields[n].type=FT_FLOAT32; n++;
    /* fault   offset=18 size=2 uint16 */
    strncpy(c.fields[n].name,"fault",FIELD_NAME_MAX-1);
    c.fields[n].offset=18; c.fields[n].size_bytes=2; c.fields[n].type=FT_UINT16; n++;
    /* software_version offset=20 size=6 string */
    strncpy(c.fields[n].name,"software_version",FIELD_NAME_MAX-1);
    c.fields[n].offset=20; c.fields[n].size_bytes=6; c.fields[n].type=FT_STRING; n++;

    c.nfields = n;
    return c;
}

/* ──── build a raw packet matching simulated-mcu.py's struct.pack ─────── */
/* struct.pack("<HIIffH6s", magic, seq, ts, speed, batt, fault, version) */

static void pack_u16_le(uint8_t *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void pack_u32_le(uint8_t *p, uint32_t v) {
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}
static void pack_f32_le(uint8_t *p, float f) { uint32_t u; memcpy(&u,&f,4); pack_u32_le(p,u); }

static void build_packet(uint8_t *buf, uint32_t seq, uint32_t ts,
                         float speed, float batt, uint16_t fault,
                         const char *ver) {
    memset(buf, 0, 26);
    pack_u16_le(buf+0,  0xDEAD);
    pack_u32_le(buf+2,  seq);
    pack_u32_le(buf+6,  ts);
    pack_f32_le(buf+10, speed);
    pack_f32_le(buf+14, batt);
    pack_u16_le(buf+18, fault);
    /* 6-byte version string, null-padded */
    memset(buf+20, 0, 6);
    strncpy((char*)(buf+20), ver, 5);
}

/* ──── simple test framework ────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define EXPECT(cond, msg) do { \
    if (cond) { g_pass++; printf("  PASS: %s\n", msg); } \
    else      { g_fail++; printf("  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

#define EXPECT_STR_CONTAINS(haystack, needle, msg) do { \
    if ((haystack) && strstr((haystack),(needle))) { g_pass++; printf("  PASS: %s\n", msg); } \
    else { g_fail++; printf("  FAIL: %s — '%s' not in '%s' (line %d)\n", \
           msg, needle, haystack?haystack:"(null)", __LINE__); } \
} while(0)

/* ──── tests ─────────────────────────────────────────────────────────────── */

static void test_valid_packet(void) {
    printf("\n[test] valid packet decode\n");
    Config cfg = make_config();
    uint8_t buf[26];
    build_packet(buf, 42, 1710000000u, 65.0f, 12.5f, 0, "1.0.0");

    char *json = decode_packet(buf, 26, &cfg);
    EXPECT(json != NULL, "decode returns non-null");
    EXPECT_STR_CONTAINS(json, "\"magic\":57005", "magic field correct");
    EXPECT_STR_CONTAINS(json, "\"seq\":42",       "seq field correct");
    EXPECT_STR_CONTAINS(json, "\"speed\":65",     "speed field correct");
    EXPECT_STR_CONTAINS(json, "\"batt_volt\":12.5","batt_volt field correct");
    EXPECT_STR_CONTAINS(json, "\"fault\":0",      "fault field correct");
    EXPECT_STR_CONTAINS(json, "\"software_version\":\"1.0.0\"", "version field correct");
    free(json);
}

static void test_bad_magic(void) {
    printf("\n[test] bad magic rejected\n");
    Config cfg = make_config();
    uint8_t buf[26];
    build_packet(buf, 1, 0, 0.0f, 0.0f, 0, "");
    pack_u16_le(buf+0, 0x1234); /* wrong magic */
    char *json = decode_packet(buf, 26, &cfg);
    EXPECT(json == NULL, "bad magic returns NULL");
    free(json);
}

static void test_short_packet(void) {
    printf("\n[test] short packet rejected\n");
    Config cfg = make_config();
    uint8_t buf[26];
    build_packet(buf, 1, 0, 0.0f, 0.0f, 0, "1.0.0");
    char *json = decode_packet(buf, 10, &cfg); /* only 10 bytes */
    EXPECT(json == NULL, "short packet returns NULL");
    free(json);
}

static void test_fault_flag(void) {
    printf("\n[test] fault flag encoded\n");
    Config cfg = make_config();
    uint8_t buf[26];
    build_packet(buf, 50, 0, 40.0f, 12.0f, 0x0001, "1.0.0");
    char *json = decode_packet(buf, 26, &cfg);
    EXPECT_STR_CONTAINS(json, "\"fault\":1", "fault flag 1 present");
    free(json);
}

static void test_seq_wrap(void) {
    printf("\n[test] sequence number at uint32 max\n");
    Config cfg = make_config();
    uint8_t buf[26];
    build_packet(buf, 0xFFFFFFFFu, 0, 0.0f, 0.0f, 0, "1.0.0");
    char *json = decode_packet(buf, 26, &cfg);
    EXPECT_STR_CONTAINS(json, "\"seq\":4294967295", "uint32 max seq encoded");
    free(json);
}

static void test_float_precision(void) {
    printf("\n[test] float precision\n");
    Config cfg = make_config();
    uint8_t buf[26];
    /* speed = 43.0, batt = 12.15 as per assessment example */
    build_packet(buf, 123, 1710000000u, 43.0f, 12.15f, 0, "1.0.0");
    char *json = decode_packet(buf, 26, &cfg);
    EXPECT(json != NULL, "decode non-null for float test");
    EXPECT_STR_CONTAINS(json, "\"speed\":43", "speed 43.0 encoded");
    /* 12.15 in float32 may be 12.15 or 12.1500x */
    EXPECT_STR_CONTAINS(json, "\"batt_volt\":12.15", "batt_volt 12.15 encoded");
    free(json);
}

static void test_version_string_no_null_leak(void) {
    printf("\n[test] version string null termination\n");
    Config cfg = make_config();
    uint8_t buf[26];
    build_packet(buf, 1, 0, 0.0f, 0.0f, 0, "1.0.0");
    /* Ensure bytes 25 is zero (already done by build_packet memset) */
    char *json = decode_packet(buf, 26, &cfg);
    /* The JSON string should close properly */
    EXPECT(json && json[strlen(json)-1] == '}', "JSON object closes correctly");
    EXPECT_STR_CONTAINS(json, "\"1.0.0\"", "version string quoted and clean");
    free(json);
}

static void test_json_string_escaping(void) {
    printf("\n[test] JSON string escaping\n");
    Config cfg = make_config();
    uint8_t buf[26];
    build_packet(buf, 1, 0, 0.0f, 0.0f, 0, "1.0.0");
    /* inject a control character into version field */
    buf[20] = 'v'; buf[21] = '\x01'; buf[22] = 0;
    char *json = decode_packet(buf, 26, &cfg);
    EXPECT(json != NULL, "decode succeeds with control char");
    EXPECT_STR_CONTAINS(json, "\\u0001", "control char escaped in JSON");
    free(json);
}

static void test_big_endian_config(void) {
    printf("\n[test] big-endian byte order\n");
    Config cfg = make_config();
    cfg.little_endian = 0;
    uint8_t buf[26] = {0};
    /* magic 0xDEAD big-endian */
    buf[0] = 0xDE; buf[1] = 0xAD;
    /* seq=7 big-endian uint32 at offset 2 */
    buf[2]=0; buf[3]=0; buf[4]=0; buf[5]=7;
    /* rest can be zero */
    char *json = decode_packet(buf, 26, &cfg);
    EXPECT(json != NULL, "big-endian magic accepted");
    EXPECT_STR_CONTAINS(json, "\"seq\":7", "big-endian seq decoded");
    free(json);
}

/* ──── entry point ───────────────────────────────────────────────────────── */

int main(void) {
    printf("=== telemetry forwarder unit tests ===\n");

    test_valid_packet();
    test_bad_magic();
    test_short_packet();
    test_fault_flag();
    test_seq_wrap();
    test_float_precision();
    test_version_string_no_null_leak();
    test_json_string_escaping();
    test_big_endian_config();

    printf("\n=== results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

/*
 * FOCAS2 Web API Server (Cross-platform: Linux + Windows)
 * Pure C HTTP server, linked against FOCAS2 library
 *
 * Linux:   g++ -m32 -o focaswebapi focaswebapi.c -I./NativeLib/Linux -L./NativeLib/Linux -lfwlib32 -lstdc++ -lpthread
 * Windows: cl /Tc focaswebapi.c /I.\NativeLib\Windows /link FWLIB32.lib ws2_32.lib
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include "fwlib32.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>
  #include <fcntl.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t_win;
  #define CLOSE_SOCKET(s) closesocket(s)
  #define CLOSE_FILE(f) _close(f)
  #define READ(fd,buf,len) recv(fd,buf,len,0)
  #define WRITE(fd,buf,len) send(fd,buf,len,0)
  #define OPEN_FILE(path) _open(path, _O_RDONLY | _O_BINARY)
  #define FSTAT_FD(fd,st) _fstat(fd,st)
  struct win_stat { st_size_t st_size; };
  #include <sys/stat.h>
  #undef CLOSE_FILE
  #undef OPEN_FILE
  #undef FSTAT_FD
  #define CLOSE_FILE(f) close(f)
  #define OPEN_FILE(path) open(path, O_RDONLY | O_BINARY)
  #define FSTAT_FD(fd,st) fstat(fd,st)
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #define CLOSE_SOCKET(s) close(s)
  #define CLOSE_FILE(f) close(f)
  #define READ(fd,buf,len) read(fd,buf,len)
  #define WRITE(fd,buf,len) write(fd,buf,len)
  #define OPEN_FILE(path) open(path, O_RDONLY)
  #define FSTAT_FD(fd,st) fstat(fd,st)
#endif

#define PORT_DEFAULT 5000
#define BUF_SIZE 65536
#define RESP_SIZE 131072
#define POINT_DIV 1000.0

static volatile int g_running = 1;
static char g_logfile[256] = "fwlibeth.log";

void handle_signal(int sig) { g_running = 0; }

/* ============ JSON helpers ============ */

static int jsn_str(char *buf, const char *key, const char *val, int last) {
    return sprintf(buf, "\"%s\":\"%s\"%s", key, val, last ? "" : ",");
}
static int jsn_int(char *buf, const char *key, int val, int last) {
    return sprintf(buf, "\"%s\":%d%s", key, val, last ? "" : ",");
}
static int jsn_dbl(char *buf, const char *key, double val, int last) {
    return sprintf(buf, "\"%s\":%.3f%s", key, val, last ? "" : ",");
}
static int jsn_bool(char *buf, const char *key, int val, int last) {
    return sprintf(buf, "\"%s\":%s%s", key, val ? "true" : "false", last ? "" : ",");
}

/* ============ CNC connection cache ============ */

typedef struct {
    ushort handle;
    char ip[64];
    ushort port;
    int timeout;
    int in_use;
    time_t last_used;
} ConnSlot;

#define MAX_CONN 16
static ConnSlot g_conn[MAX_CONN];

static void init_conn_pool() {
    memset(g_conn, 0, sizeof(g_conn));
}

static ushort get_handle(const char *ip, ushort port, int timeout) {
    for (int i = 0; i < MAX_CONN; i++) {
        if (g_conn[i].in_use && g_conn[i].handle != 0 &&
            strcmp(g_conn[i].ip, ip) == 0 && g_conn[i].port == port) {
            ODBST st;
            if (cnc_statinfo(g_conn[i].handle, &st) == EW_OK) {
                g_conn[i].last_used = time(NULL);
                return g_conn[i].handle;
            }
            cnc_freelibhndl(g_conn[i].handle);
            g_conn[i].handle = 0;
            g_conn[i].in_use = 0;
        }
    }
    int slot = -1;
    time_t oldest = time(NULL);
    for (int i = 0; i < MAX_CONN; i++) {
        if (!g_conn[i].in_use) { slot = i; break; }
        if (g_conn[i].last_used < oldest) { oldest = g_conn[i].last_used; slot = i; }
    }
    if (slot < 0) slot = 0;
    if (g_conn[slot].in_use && g_conn[slot].handle) {
        cnc_freelibhndl(g_conn[slot].handle);
        g_conn[slot].handle = 0;
        g_conn[slot].in_use = 0;
    }

    ushort h = 0;
    short ret = cnc_allclibhndl3((char*)ip, port, timeout, &h);
    if (ret == EW_OK && h != 0) {
        g_conn[slot].handle = h;
        strncpy(g_conn[slot].ip, ip, sizeof(g_conn[slot].ip)-1);
        g_conn[slot].port = port;
        g_conn[slot].timeout = timeout;
        g_conn[slot].in_use = 1;
        g_conn[slot].last_used = time(NULL);
    } else {
        h = 0;
    }
    return h;
}

/* ============ Helper functions ============ */

static const char *mode_string(short aut) {
    switch(aut) {
        case 0: return "MDI"; case 1: return "MEM(AUTO)"; case 2: return "Undefined";
        case 3: return "EDIT"; case 4: return "Handwheel"; case 5: return "JOG";
        case 6: return "teach-JOG"; case 7: return "TEACH-HNDL"; case 8: return "INC";
        case 9: return "REF(Zero)"; case 10: return "RMT(DNC)";
        default: return "Unknown";
    }
}

static const char *run_string(short run) {
    switch(run) { case 0: return "STOP"; case 1: return "PAUSE"; case 2: return "RUN"; default: return "Unknown"; }
}

static const char *focas_error(short code) {
    switch(code) {
        case 0: return "OK"; case -1: return "CNC busy (EW_BUSY)"; case -2: return "Reset/Stop";
        case -7: return "Version mismatch"; case -8: return "Invalid handle"; case -16: return "Socket error";
        case 1: return "Function not supported"; case 2: return "Data length error"; case 3: return "Variable out of range";
        case 4: return "Data type error"; case 5: return "Data error"; case 6: return "Function not available";
        case 7: return "Write protect"; case 12: return "Mode error (MEM/MDI required)";
        default: return "Unknown error";
    }
}

static const char *default_axis_names[] = {
    "X","Y","Z","A","B","C","U","V","W","X2","Y2","Z2","A2","B2","C2","U2","V2","W2",
    "X3","Y3","Z3","A3","B3","C3","U3","V3","W3","X4","Y4","Z4","A4","B4"
};

/* ============ API handlers ============ */

static int api_ping(char *resp) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[16]; strftime(ts, sizeof(ts), "%H:%M:%S", t);
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"status\":\"ok\",\"time\":\"%s\"},\"ErrorCode\":0}", ts);
}

static int api_sysinfo(ushort h, char *resp) {
    ODBSYS si;
    short ret = cnc_sysinfo(h, &si);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    char series[8], version[8], cnctype[4], axes[4];
    memcpy(series, si.series, 4); series[4]=0;
    memcpy(version, si.version, 4); version[4]=0;
    memcpy(cnctype, si.cnc_type, 2); cnctype[2]=0;
    memcpy(axes, si.axes, 2); axes[2]=0;
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{");
    n += jsn_str(resp+n, "Series", series, 0);
    n += jsn_str(resp+n, "Version", version, 0);
    n += jsn_str(resp+n, "CncType", cnctype, 0);
    n += jsn_int(resp+n, "AxisCount", atoi(axes), 1);
    n += sprintf(resp+n, "},\"ErrorCode\":0}");
    return n;
}

static int api_status(ushort h, char *resp) {
    ODBST st;
    short ret = cnc_statinfo(h, &st);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{");
    n += jsn_int(resp+n, "Aut", st.aut, 0);
    n += jsn_int(resp+n, "Run", st.run, 0);
    n += jsn_int(resp+n, "Motion", st.motion, 0);
    n += jsn_int(resp+n, "Mstb", st.mstb, 0);
    n += jsn_int(resp+n, "Emergency", st.emergency, 0);
    n += jsn_str(resp+n, "ModeName", mode_string(st.aut), 0);
    n += jsn_str(resp+n, "RunStatusName", run_string(st.run), 1);
    n += sprintf(resp+n, "},\"ErrorCode\":0}");
    return n;
}

static int api_monitor(ushort h, char *resp) {
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{");

    ODBSYS si;
    short ret = cnc_sysinfo(h, &si);
    if (ret == EW_OK) {
        char series[8], version[8], cnctype[4], axes[4];
        memcpy(series, si.series, 4); series[4]=0;
        memcpy(version, si.version, 4); version[4]=0;
        memcpy(cnctype, si.cnc_type, 2); cnctype[2]=0;
        memcpy(axes, si.axes, 2); axes[2]=0;
        int axisCount = atoi(axes);
        if (axisCount > MAX_AXIS) axisCount = MAX_AXIS;

        n += sprintf(resp+n, "\"SysInfo\":{");
        n += jsn_str(resp+n, "Series", series, 0);
        n += jsn_str(resp+n, "Version", version, 0);
        n += jsn_str(resp+n, "CncType", cnctype, 0);
        n += jsn_int(resp+n, "AxisCount", axisCount, 1);
        n += sprintf(resp+n, "},");

        int len = 28 + 4*4*MAX_AXIS;
        ODBDY2 dy;
        ret = cnc_rddynamic2(h, -1, (short)len, &dy);
        if (ret == EW_OK) {
            n += sprintf(resp+n, "\"Dynamic\":{");
            n += jsn_int(resp+n, "MainProgram", dy.prgmnum, 0);
            n += jsn_int(resp+n, "CurrentProgram", dy.prgnum, 0);
            n += jsn_int(resp+n, "SeqNum", dy.seqnum, 0);
            n += jsn_int(resp+n, "Alarm", dy.alarm, 0);
            n += jsn_int(resp+n, "ActF", dy.actf, 0);
            n += jsn_int(resp+n, "ActS", dy.acts, 1);
            n += sprintf(resp+n, "},");

            ODBAXIS absBuf;
            short retAbs = cnc_absolute(h, -1, (short)(4+4*MAX_AXIS), &absBuf);
            n += sprintf(resp+n, "\"Axes\":[");
            for (int i = 0; i < axisCount && i < 8; i++) {
                if (i > 0) n += sprintf(resp+n, ",");
                n += sprintf(resp+n, "{\"Name\":\"%s\",", default_axis_names[i]);
                n += jsn_dbl(resp+n, "Absolute", retAbs==EW_OK ? absBuf.data[i]/POINT_DIV : 0, 0);
                n += jsn_dbl(resp+n, "Machine", dy.pos.faxis.machine[i]/POINT_DIV, 0);
                n += jsn_dbl(resp+n, "Relative", dy.pos.faxis.relative[i]/POINT_DIV, 0);
                n += jsn_dbl(resp+n, "Distance", dy.pos.faxis.distance[i]/POINT_DIV, 1);
                n += sprintf(resp+n, "}");
            }
            n += sprintf(resp+n, "],");
        } else {
            n += sprintf(resp+n, "\"Dynamic\":null,\"Axes\":[],");
        }
    } else {
        n += sprintf(resp+n, "\"SysInfo\":null,\"Dynamic\":null,\"Axes\":[],");
    }

    ODBST st;
    ret = cnc_statinfo(h, &st);
    if (ret == EW_OK) {
        n += sprintf(resp+n, "\"Status\":{");
        n += jsn_int(resp+n, "Aut", st.aut, 0);
        n += jsn_int(resp+n, "Run", st.run, 0);
        n += jsn_int(resp+n, "Motion", st.motion, 0);
        n += jsn_int(resp+n, "Mstb", st.mstb, 0);
        n += jsn_int(resp+n, "Emergency", st.emergency, 0);
        n += jsn_str(resp+n, "ModeName", mode_string(st.aut), 0);
        n += jsn_str(resp+n, "RunStatusName", run_string(st.run), 1);
        n += sprintf(resp+n, "}");
    } else {
        n += sprintf(resp+n, "\"Status\":null");
    }
    n += sprintf(resp+n, "},\"ErrorCode\":0}");
    return n;
}

static int api_start(ushort h, char *resp) {
    ODBST st;
    short ret = cnc_statinfo(h, &st);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    if (st.aut != 1 && st.aut != 0)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":12,\"ErrorMsg\":\"Mode error (MEM/MDI required)\"}");
    ret = cnc_start(h);
    if (ret == EW_OK)
        return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

static int api_reset(ushort h, char *resp) {
    short ret = cnc_reset(h);
    if (ret == EW_OK) return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

static int api_alarm(ushort h, char *resp) {
    ODBALM alm;
    short ret = cnc_alarm(h, &alm);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"Alarm\":%d},\"ErrorCode\":0}", alm.data);
}

static int api_program_upload(ushort h, const char *body, int body_len, char *resp) {
    short ret = cnc_dwnstart(h);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_dwnstart: %s\"}", ret, focas_error(ret));

    char sendbuf[BUF_SIZE];
    int sendlen = 0;
    sendbuf[sendlen++] = '%';
    sendbuf[sendlen++] = '\n';
    if (body_len > 0) {
        memcpy(sendbuf + sendlen, body, body_len);
        sendlen += body_len;
    }
    if (sendlen > 0 && sendbuf[sendlen-1] != '\n') {
        sendbuf[sendlen++] = '\n';
    }
    sendbuf[sendlen++] = '%';
    sendbuf[sendlen] = '\0';

    int offset = 0;
    while (offset < sendlen) {
        int chunk = sendlen - offset;
        if (chunk > 256) chunk = 256;
        ret = cnc_download(h, sendbuf + offset, (short)chunk);
        if (ret != EW_OK) {
            cnc_dwnend(h);
            return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_download: %s\"}", ret, focas_error(ret));
        }
        offset += chunk;
    }

    ret = cnc_dwnend(h);
    /* cnc_dwnend may return non-zero even when upload succeeded (FOCAS2 behavior) */

    return sprintf(resp, "{\"Success\":true,\"Data\":{\"BytesSent\":%d},\"ErrorCode\":0}", sendlen);
}

static int api_program_download(ushort h, long prgnum, char *resp) {
    short ret = cnc_upstart(h, (short)prgnum);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_upstart: %s\"}", ret, focas_error(ret));

    int n = 0;
    int in_program = 0, percent_count = 0;
    char prog[RESP_SIZE];
    ODBUP upbuf;
    unsigned short len;

    while (1) {
        len = sizeof(upbuf);
        ret = cnc_upload(h, &upbuf, &len);
        if (len > 0) {
            for (unsigned short i = 0; i < len && n < RESP_SIZE - 2; i++) {
                char c = upbuf.data[i];
                if (c == '%') {
                    percent_count++;
                    if (percent_count == 1) { in_program = 1; continue; }
                    else if (percent_count >= 2) { in_program = 0; break; }
                }
                if (in_program && c != '\r') prog[n++] = c;
            }
        }
        if (ret != EW_OK) break;
    }
    prog[n] = '\0';
    cnc_upend(h);

    int rpos = 0;
    rpos += sprintf(resp + rpos, "{\"Success\":true,\"Data\":{\"Program\":%ld,\"Content\":\"", prgnum);
    for (int i = 0; i < n; i++) {
        char c = prog[i];
        if (c == '"') rpos += sprintf(resp + rpos, "\\\"");
        else if (c == '\\') rpos += sprintf(resp + rpos, "\\\\");
        else if (c == '\n') rpos += sprintf(resp + rpos, "\\n");
        else if (c == '\t') rpos += sprintf(resp + rpos, "\\t");
        else resp[rpos++] = c;
    }
    rpos += sprintf(resp + rpos, "\",\"Size\":%d},\"ErrorCode\":0}", n);
    return rpos;
}

static int api_program_run(ushort h, long prgnum, char *resp) {
    short ret = cnc_search(h, (short)prgnum);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_search(O%ld): %s\"}", ret, prgnum, focas_error(ret));

    ODBST st;
    ret = cnc_statinfo(h, &st);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_statinfo: %s\"}", ret, focas_error(ret));

    if (st.aut != 1 && st.aut != 0)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":12,\"ErrorMsg\":\"Mode error (current:%d, MEM/MDI required)\"}", st.aut);

    ret = cnc_start(h);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_start: %s\"}", ret, focas_error(ret));

    return sprintf(resp, "{\"Success\":true,\"Data\":{\"Program\":%ld,\"Action\":\"started\"},\"ErrorCode\":0}", prgnum);
}

/* ============ Path matching helpers ============ */

/* forward declarations */
static void parse_query(const char *query, const char *key, char *val, int vlen);

static int path_prefix(const char *path, const char *prefix) {
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

/* Extract integer after prefix */
static int path_int_after(const char *path, const char *prefix, long *val) {
    if (!path_prefix(path, prefix)) return -1;
    *val = atol(path + strlen(prefix));
    return 0;
}

/* Simple JSON value extractors */
static const char *json_str_val(const char *body, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(body, search);
    if (!p) return NULL;
    return p + strlen(search);
}

static const char *json_num_val(const char *body, const char *key, long *val) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(body, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *val = atol(p);
    return p;
}

static const char *json_dbl_val(const char *body, const char *key, double *val) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(body, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *val = atof(p);
    return p;
}

/* JSON-escape a string */
static int json_escape(const char *src, char *dst, int max) {
    int n = 0;
    for (const char *c = src; *c && n < max - 2; c++) {
        if (*c == '"') { if (n < max - 3) { dst[n++] = '\\'; dst[n++] = '"'; } }
        else if (*c == '\\') { if (n < max - 3) { dst[n++] = '\\'; dst[n++] = '\\'; } }
        else if (*c == '\n') { if (n < max - 3) { dst[n++] = '\\'; dst[n++] = 'n'; } }
        else if (*c == '\r') { }
        else if (*c == '\t') { if (n < max - 3) { dst[n++] = '\\'; dst[n++] = 't'; } }
        else dst[n++] = *c;
    }
    dst[n] = 0;
    return n;
}

/* ============ New API handlers ============ */

/* GET /api/focas/programs */
static int api_program_list(ushort h, char *resp) {
    PRGDIR2 dir[10];
    memset(dir, 0, sizeof(dir));
    short count = 10;
    short length = (short)sizeof(dir);
    short ret = cnc_rdprogdir2(h, 0, &count, &length, dir);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":[");
    int first = 1;
    for (int i = 0; i < count && i < 10; i++) {
        if (dir[i].number <= 0) continue;
        if (!first) n += sprintf(resp+n, ",");
        first = 0;
        char comment[52];
        strncpy(comment, dir[i].comment, 51); comment[51] = 0;
        char escaped[128];
        json_escape(comment, escaped, sizeof(escaped));
        n += sprintf(resp+n, "{\"Number\":\"O%04d\",\"ProgNum\":%d,\"Comment\":\"%s\",\"Length\":%ld}",
            dir[i].number, dir[i].number, escaped, dir[i].length);
    }
    n += sprintf(resp+n, "],\"ErrorCode\":0}");
    return n;
}

/* GET /api/focas/programs/{progNum} */
static int api_program_content(ushort h, long progNum, char *resp) {
    short ret = cnc_upstart(h, (short)progNum);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_upstart: %s\"}", ret, focas_error(ret));
    char content[RESP_SIZE / 2];
    int pos = 0, lineCount = 0;
    ODBUP upbuf;
    unsigned short len;
    while (1) {
        len = sizeof(upbuf);
        ret = cnc_upload(h, &upbuf, &len);
        if (len > 0) {
            for (unsigned short j = 0; j < len && pos < (int)sizeof(content) - 2; j++) {
                char c = upbuf.data[j];
                if (c == '%' || c == '\r') continue;
                content[pos++] = c;
                if (c == '\n') lineCount++;
            }
        }
        if (ret != EW_OK) break;
    }
    content[pos] = 0;
    cnc_upend(h);
    char escaped[RESP_SIZE / 2];
    json_escape(content, escaped, sizeof(escaped));
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{\"ProgNum\":%ld,\"Number\":\"O%04ld\",\"Content\":\"%s\",\"LineCount\":%d},\"ErrorCode\":0}",
        progNum, progNum, escaped, lineCount);
    return n;
}

/* PUT /api/focas/programs */
static int api_program_upload_json(ushort h, const char *body, char *resp) {
    long progNum = 0;
    if (!json_num_val(body, "progNum", &progNum))
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing progNum\"}");
    const char *cstart = json_str_val(body, "content");
    if (!cstart)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing content\"}");
    char content[RESP_SIZE];
    int clen = 0;
    while (*cstart && *cstart != '"' && clen < (int)sizeof(content) - 1)
        content[clen++] = *cstart++;
    content[clen] = 0;
    for (int i = 0; i < 5; i++) {
        short retD = cnc_delete(h, (short)progNum);
        if (retD != EW_BUSY) break;
    }
    short ret = cnc_dwnstart(h);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_dwnstart: %s\"}", ret, focas_error(ret));
    char preamble[32];
    int preLen = sprintf(preamble, "O%04ld\n", progNum);
    ret = cnc_download(h, preamble, (short)preLen);
    if (ret != EW_OK) { cnc_dwnend(h); return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"download failed\"}", ret); }
    char *line = content, *next;
    while (line && *line) {
        next = strchr(line, '\n');
        int lineLen = next ? (int)(next - line) : (int)strlen(line);
        short retW = 0;
        if (lineLen > 0) {
            char sb[512];
            memcpy(sb, line, lineLen); sb[lineLen] = '\n';
            retW = cnc_download(h, sb, (short)(lineLen + 1));
        }
        if (retW != EW_OK) { cnc_dwnend(h); return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"download failed\"}", retW); }
        line = next ? next + 1 : NULL;
    }
    ret = cnc_download(h, (char*)"%\n", 2);
    if (ret != EW_OK) { cnc_dwnend(h); return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"download %% failed\"}", ret); }
    ret = cnc_dwnend(h);
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"Program\":%ld,\"BytesSent\":%d},\"ErrorCode\":0}", progNum, preLen + clen + 2);
}

/* DELETE /api/focas/programs/{progNum} */
static int api_program_delete(ushort h, long progNum, char *resp) {
    short ret = 0;
    for (int i = 0; i < 5; i++) {
        ret = cnc_delete(h, (short)progNum);
        if (ret != EW_BUSY) break;
    }
    if (ret == EW_OK || ret == 5 || ret == 6)
        return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

/* GET /api/focas/programs/actpt */
static int api_active_pointer(ushort h, char *resp) {
    long progNo = 0, blkNo = 0;
    short ret = cnc_rdactpt(h, &progNo, &blkNo);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"ProgNo\":%ld,\"BlkNo\":%ld},\"ErrorCode\":0}", progNo, blkNo);
}

/* POST /api/focas/programs/{progNum}/run */
static int api_run_program(ushort h, long progNum, char *resp) {
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{");
    n += sprintf(resp+n, "\"ProgNum\":%ld,\"Number\":\"O%04ld\",", progNum, progNum);
    n += sprintf(resp+n, "\"StepsCompleted\":[");
    ODBST st;
    short ret = cnc_statinfo(h, &st);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"statinfo: %s\"}", ret, focas_error(ret));
    n += sprintf(resp+n, "\"check\"");
    if (st.emergency != 0)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-1,\"ErrorMsg\":\"CNC emergency\"}");
    if (st.aut != 1 && st.aut != 0)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":12,\"ErrorMsg\":\"Mode error\"}");
    n += sprintf(resp+n, ",\"search\"");
    ret = cnc_search(h, (short)progNum);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_search: %s\"}", ret, focas_error(ret));
    n += sprintf(resp+n, ",\"start\"");
    ret = cnc_start(h);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_start: %s\"}", ret, focas_error(ret));
    n += sprintf(resp+n, "],\"Mode\":\"%s\"", st.aut == 1 ? "AUTO" : "MDI");
    time_t now = time(NULL); struct tm *t = localtime(&now);
    char ts[20]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    n += sprintf(resp+n, ",\"StartedAt\":\"%s\"", ts);
    n += sprintf(resp+n, "},\"ErrorCode\":0}");
    return n;
}

/* POST /api/focas/programs/{progNum}/autostart */
static int api_autostart(ushort h, long progNum, char *resp) {
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{");
    n += sprintf(resp+n, "\"ProgNum\":%ld,\"Number\":\"O%04ld\",", progNum, progNum);
    n += sprintf(resp+n, "\"StepsCompleted\":[");
    ODBST st;
    short ret = cnc_statinfo(h, &st);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"statinfo: %s\"}", ret, focas_error(ret));
    n += sprintf(resp+n, "\"status_check\"");
    if (st.emergency != 0 || (st.aut != 1 && st.aut != 0))
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":-1,\"ErrorMsg\":\"Not ready\"}");
    n += sprintf(resp+n, ",\"reset\"");
    ret = cnc_reset(h);
    int wasReset = (ret == EW_OK);
    struct timespec ts3 = {0, 300000000L};
    nanosleep(&ts3, NULL);
    n += sprintf(resp+n, ",\"search\"");
    ret = cnc_search(h, (short)progNum);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_search: %s\"}", ret, focas_error(ret));
    n += sprintf(resp+n, ",\"start\"");
    ret = cnc_start(h);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"ErrorCode\":%d,\"ErrorMsg\":\"cnc_start: %s\"}", ret, focas_error(ret));
    n += sprintf(resp+n, "],\"WasReset\":%s,\"Mode\":\"%s\"", wasReset ? "true" : "false", st.aut == 1 ? "AUTO" : "MDI");
    time_t now = time(NULL); struct tm *t = localtime(&now);
    char ts[20]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
    n += sprintf(resp+n, ",\"StartedAt\":\"%s\"", ts);
    n += sprintf(resp+n, "},\"ErrorCode\":0}");
    return n;
}

/* ********** Macro API ********** */

/* GET /api/focas/macros/user/{macroNum} */
static int api_read_user_macro(ushort h, long macroNum, char *resp) {
    ODBM odbm;
    short ret = cnc_rdmacro(h, (short)macroNum, 10, &odbm);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    double value = (double)odbm.mcr_val / pow(10.0, odbm.dec_val);
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"Number\":%ld,\"Value\":%.3f,\"DecVal\":%d,\"RawVal\":%ld},\"ErrorCode\":0}",
        macroNum, value, odbm.dec_val, odbm.mcr_val);
}

/* PUT /api/focas/macros/user */
static int api_write_user_macro(ushort h, const char *body, char *resp) {
    long number = 0; double value = 0.0;
    if (!json_num_val(body, "number", &number)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing number\"}");
    if (!json_dbl_val(body, "value", &value)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing value\"}");
    ODBM odbm;
    short ret = cnc_rdmacro(h, (short)number, 10, &odbm);
    int dec = (ret == EW_OK) ? odbm.dec_val : 0;
    long rawVal = (long)(value * pow(10.0, dec) + 0.5);
    ret = cnc_wrmacro(h, (short)number, 10, rawVal, (short)dec);
    if (ret == EW_OK) return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

/* GET /api/focas/macros/pcode/{macroNum} */
static int api_read_pcode_macro(ushort h, long macroNum, char *resp) {
    ODBPM odbpm;
    short ret = cnc_rdpmacro(h, macroNum, &odbpm);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    double value = (double)odbpm.mcr_val / pow(10.0, odbpm.dec_val);
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"Number\":%ld,\"Value\":%.3f,\"DecVal\":%d,\"RawVal\":%ld},\"ErrorCode\":0}",
        macroNum, value, odbpm.dec_val, odbpm.mcr_val);
}

/* PUT /api/focas/macros/pcode */
static int api_write_pcode_macro(ushort h, const char *body, char *resp) {
    long number = 0; double value = 0.0;
    if (!json_num_val(body, "number", &number)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing number\"}");
    if (!json_dbl_val(body, "value", &value)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing value\"}");
    ODBPM odbpm;
    short ret = cnc_rdpmacro(h, number, &odbpm);
    int dec = (ret == EW_OK) ? odbpm.dec_val : 0;
    long rawVal = (long)(value * pow(10.0, dec) + 0.5);
    ret = cnc_wrpmacro(h, number, rawVal, (short)dec);
    if (ret == EW_OK) return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

/* ********** PLC/PMC API ********** */

/* GET /api/focas/plc/{type}/{addr} */
static int api_read_plc(ushort h, int addrType, int addrNum, int bitIndex, char *resp) {
    IODBPMC pmc;
    memset(&pmc, 0, sizeof(pmc));
    int dataType = (bitIndex >= 0) ? 1 : 0;
    short ret = pmc_rdpmcrng(h, (short)addrType, (short)dataType, (unsigned short)addrNum, (unsigned short)addrNum, (short)sizeof(pmc), &pmc);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    int byteVal = (unsigned char)pmc.u.cdata[0];
    int bitVal = (bitIndex >= 0 && bitIndex <= 7) ? ((byteVal >> bitIndex) & 1) : -1;
    return sprintf(resp,
        "{\"Success\":true,\"Data\":{\"AddressType\":%d,\"AddressNum\":%d,\"ByteValue\":%d,\"HexValue\":\"0x%02X\",\"BitValue\":%d,\"BitIndex\":%d},\"ErrorCode\":0}",
        addrType, addrNum, byteVal, byteVal, bitVal, (bitIndex >= 0 ? bitIndex : -1));
}

/* PUT /api/focas/plc */
static int api_write_plc(ushort h, const char *body, char *resp) {
    long addrType = 0, addrNum = 0, value = 0, bitIndex = -1;
    if (!json_num_val(body, "addressType", &addrType)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing addressType\"}");
    if (!json_num_val(body, "addressNum", &addrNum)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing addressNum\"}");
    if (!json_num_val(body, "value", &value)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing value\"}");
    json_num_val(body, "bitIndex", &bitIndex);
    IODBPMC pmcW;
    memset(&pmcW, 0, sizeof(pmcW));
    pmcW.type_a = (short)addrType;
    pmcW.datano_s = (unsigned short)addrNum;
    pmcW.datano_e = (unsigned short)addrNum;
    if (bitIndex >= 0 && bitIndex <= 7) {
        IODBPMC pmcR;
        memset(&pmcR, 0, sizeof(pmcR));
        short retR = pmc_rdpmcrng(h, (short)addrType, 2, (unsigned short)addrNum, (unsigned short)addrNum, 1, &pmcR);
        int byteVal = (retR == EW_OK) ? (unsigned char)pmcR.u.cdata[0] : 0;
        if (value) byteVal |= (1 << (int)bitIndex);
        else byteVal &= ~(1 << (int)bitIndex);
        pmcW.type_d = 2;
        pmcW.u.cdata[0] = (char)byteVal;
    } else {
        pmcW.type_d = 2;
        pmcW.u.cdata[0] = (char)(value & 0xFF);
    }
    short ret = pmc_wrpmcrng(h, 1, &pmcW);
    if (ret == EW_OK) return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

/* ********** Parameters API ********** */

/* GET /api/focas/parameters/{paramNum} */
static int api_read_parameter(ushort h, long paramNum, char *resp) {
    IODBPSD psd;
    memset(&psd, 0, sizeof(psd));
    short ret = cnc_rdparam(h, 0, 0, (short)paramNum, &psd);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"ParamNum\":%ld,\"Type\":%d,\"LData\":%ld,\"IData\":%d,\"CData\":%d},\"ErrorCode\":0}",
        paramNum, psd.type, psd.u.ldata, psd.u.idata, (int)(unsigned char)psd.u.cdata);
}

/* PUT /api/focas/parameters */
static int api_write_parameter(ushort h, const char *body, char *resp) {
    long paramNum = 0, value = 0;
    if (!json_num_val(body, "paramNum", &paramNum)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing paramNum\"}");
    if (!json_num_val(body, "value", &value)) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Missing value\"}");
    IODBPSD psd;
    memset(&psd, 0, sizeof(psd));
    psd.datano = (short)paramNum;
    psd.type = 0;
    psd.u.ldata = value;
    short ret = cnc_wrparam(h, 0, &psd);
    if (ret == EW_OK) return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

/* ********** Override API ********** */

/* GET /api/focas/override */
static int api_override(ushort h, char *resp) {
    int len = 28 + 4 * 4 * MAX_AXIS;
    ODBDY2 dy;
    short ret = cnc_rddynamic2(h, -1, (short)len, &dy);
    if (ret != EW_OK)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
    return sprintf(resp, "{\"Success\":true,\"Data\":{\"FeedOverride\":%ld,\"SpindleOverride\":%ld,\"RapidOverride\":0},\"ErrorCode\":0}",
        dy.actf, dy.acts);
}

/* POST /api/focas/alarm/clear - alias for reset (cnc_almck not available in Linux lib) */
static int api_alarm_clear(ushort h, char *resp) {
    short ret = cnc_reset(h);
    if (ret == EW_OK) return sprintf(resp, "{\"Success\":true,\"Data\":null,\"ErrorCode\":0}");
    return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret));
}

/* GET /api/focas/macros - range read cnc_rdmacror2 with double array */
static int api_macros_range(ushort h, int start, int count, char *resp) {
    if (count <= 0 || count > 500) count = 1;
    int num = count;
    double *data = (double*)calloc(count, sizeof(double));
    if (!data) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"OOM\"}");
    short ret = cnc_rdmacror2(h, (unsigned long)start, (unsigned long*)&num, data);
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{");
    n += sprintf(resp+n, "\"Start\":%d,\"Count\":%d,\"ActualCount\":%d,\"Values\":{", start, count, num);
    int first = 1;
    for (int i = 0; i < num && i < count; i++) {
        if (!first) n += sprintf(resp+n, ",");
        first = 0;
        n += sprintf(resp+n, "\"%d\":%.6f", start + i, data[i]);
    }
    n += sprintf(resp+n, "}},\"ErrorCode\":0}");
    free(data);
    return n;
}

/* POST /api/focas/macros/batch - non-contiguous macro read from JSON array */
static int api_macros_batch(ushort h, const char *body, char *resp) {
    int start = 100, count = 1;
    json_num_val(body, "start", (long*)&start);
    json_num_val(body, "count", (long*)&count);
    return api_macros_range(h, start, count, resp);
}

/* PMC address type: convert letter to numeric code */
static short pmc_adr_type(char c) {
    switch(c) {
        case 'G': case 'g': return 0;
        case 'F': case 'f': return 1;
        case 'Y': case 'y': return 2;
        case 'X': case 'x': return 3;
        case 'A': case 'a': return 4;
        case 'R': case 'r': return 5;
        case 'T': case 't': return 6;
        case 'K': case 'k': return 7;
        case 'C': case 'c': return 8;
        case 'D': case 'd': return 9;
        case 'E': case 'e': return 10;
        default: return -1;
    }
}

/* GET /api/focas/plc?type=G&start=0&count=1&startBit=-1 - query-based range read */
static int api_plc_query(ushort h, const char *query, char *resp) {
    char type_s[4] = "G";
    char start_s[16] = "0", count_s[16] = "1", bit_s[16] = "-1";
    parse_query(query, "type", type_s, sizeof(type_s));
    parse_query(query, "start", start_s, sizeof(start_s));
    parse_query(query, "count", count_s, sizeof(count_s));
    parse_query(query, "startBit", bit_s, sizeof(bit_s));
    int start = atoi(start_s), cnt = atoi(count_s), startBit = atoi(bit_s);
    if (cnt <= 0 || cnt > 256) cnt = 1;
    short adrType = pmc_adr_type(type_s[0]);
    if (adrType < 0)
        return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"Invalid type\"}");
    
    /* Allocate buffer: header(8 bytes) + data bytes */
    int bufsize = 8 + cnt;
    char *buf = (char*)calloc(bufsize, 1);
    if (!buf) return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-99,\"ErrorMsg\":\"OOM\"}");
    
    short ret = pmc_rdpmcrng(h, adrType, 0, (short)start, (short)(start + cnt - 1), (short)bufsize, (IODBPMC*)buf);
    if (ret != EW_OK) { free(buf); return sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":%d,\"ErrorMsg\":\"%s\"}", ret, focas_error(ret)); }
    
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{");
    n += sprintf(resp+n, "\"Type\":\"%c\",\"Start\":%d,\"StartBit\":%d,\"Count\":%d,\"Values\":[", type_s[0], start, startBit, cnt);
    unsigned char *cdata = (unsigned char*)(buf + 8);
    int first = 1;
    if (startBit >= 0 && startBit <= 7) {
        int curBit = startBit, vi = 0;
        for (int i = 0; i < cnt && vi < cnt; i++) {
            while (curBit <= 7 && vi < cnt) {
                if (!first) n += sprintf(resp+n, ","); first = 0;
                n += sprintf(resp+n, "%d", (cdata[i] >> curBit) & 1);
                curBit++; vi++;
            }
            if (curBit > 7) curBit = 0;
        }
    } else {
        for (int i = 0; i < cnt; i++) {
            if (!first) n += sprintf(resp+n, ","); first = 0;
            n += sprintf(resp+n, "%d", (int)cdata[i]);
        }
    }
    n += sprintf(resp+n, "]},\"ErrorCode\":0}");
    free(buf);
    return n;
}

/* GET /api/focas/plc/batch - batch read from query params (simplified) */
/* POST /api/focas/plc/batch - batch read from JSON body */
static int api_plc_batch(ushort h, const char *body, char *resp) {
    return api_plc_query(h, body, resp); /* reuse query handler */
}

/* GET /api/focas/parameters/axis/{paramNum} - axis-specific parameter */
static int api_axis_parameter(ushort h, long paramNum, char *resp) {
    /* Read axis names first */
    ODBSYS si;
    short ret = cnc_sysinfo(h, &si);
    int axisCount = 3;
    if (ret == EW_OK) {
        char axes[4]; memcpy(axes, si.axes, 2); axes[2]=0;
        axisCount = atoi(axes);
        if (axisCount > MAX_AXIS) axisCount = MAX_AXIS;
    }
    int n = 0;
    n += sprintf(resp+n, "{\"Success\":true,\"Data\":{\"Values\":{");
    int first = 1;
    for (int idx = 0; idx < axisCount && idx < 8; idx++) {
        IODBPSD psd;
        memset(&psd, 0, sizeof(psd));
        short ret2 = cnc_rdparam3(h, (short)paramNum, (short)(idx + 1), 0, 1, &psd);
        if (!first) n += sprintf(resp+n, ","); first = 0;
        n += sprintf(resp+n, "\"%s\":%ld", default_axis_names[idx], psd.u.ldata);
    }
    n += sprintf(resp+n, "}},\"ErrorCode\":0}");
    return n;
}

/* ============ HTTP parsing ============ */

typedef struct {
    char method[8];
    char path[512];
    char query[512];
    char body[BUF_SIZE];
    int body_len;
} HttpRequest;

static void parse_query(const char *query, const char *key, char *val, int vlen) {
    char search[64]; snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(query, search);
    if (!p) return;
    p += strlen(search);
    int i = 0;
    while (*p && *p != '&' && i < vlen-1) val[i++] = *p++;
    val[i] = 0;
}

static int parse_request(const char *raw, int raw_len, HttpRequest *req) {
    memset(req, 0, sizeof(HttpRequest));
    const char *p = raw;
    int i = 0;
    while (*p && *p != ' ' && i < 7) req->method[i++] = *p++;
    if (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && *p != '?' && i < 511) req->path[i++] = *p++;
    if (*p == '?') {
        p++; i = 0;
        while (*p && *p != ' ' && i < 511) req->query[i++] = *p++;
    }
    const char *body = strstr(raw, "\r\n\r\n");
    if (body) {
        body += 4;
        req->body_len = raw_len - (body - raw);
        if (req->body_len > 0) {
            if (req->body_len >= BUF_SIZE) req->body_len = BUF_SIZE - 1;
            memcpy(req->body, body, req->body_len);
            req->body[req->body_len] = 0;
        }
    }
    return 1;
}

static int read_http_request(int fd, char *buf, int buf_size) {
    int total = 0, n;
    int header_end = 0;
    while (total < buf_size - 1) {
        n = READ(fd, buf + total, buf_size - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        if (!header_end && strstr(buf, "\r\n\r\n")) {
            header_end = 1;
            char *cl = strstr(buf, "Content-Length:");
            if (cl) {
                int content_len = atoi(cl + 16);
                char *body_start = strstr(buf, "\r\n\r\n") + 4;
                int body_so_far = total - (body_start - buf);
                while (body_so_far < content_len && total < buf_size - 1) {
                    n = READ(fd, buf + total, buf_size - 1 - total);
                    if (n <= 0) break;
                    total += n;
                    buf[total] = 0;
                    body_so_far += n;
                }
            }
            break;
        }
    }
    return total;
}

/* ============ HTTP response ============ */

static void send_response(int fd, int code, const char *status, const char *body, int body_len) {
    char header[512];
    int hlen = sprintf(header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        code, status, body_len);
    WRITE(fd, header, hlen);
    WRITE(fd, body, body_len);
}

static void send_file_response(int fd, const char *filepath, const char *content_type) {
    int file_fd = OPEN_FILE(filepath);
    if (file_fd < 0) {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        WRITE(fd, not_found, (int)strlen(not_found));
        return;
    }
    struct stat st;
    FSTAT_FD(file_fd, &st);
    char header[256];
    int hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        content_type, (long)st.st_size);
    WRITE(fd, header, hlen);
    char fbuf[8192];
    ssize_t n;
    while ((n = READ(file_fd, fbuf, sizeof(fbuf))) > 0) {
        WRITE(fd, fbuf, n);
    }
    CLOSE_FILE(file_fd);
}

/* ============ Route handler ============ */

static void handle_request(int fd, HttpRequest *req) {
    char resp[RESP_SIZE];
    int n = 0;

    char ip[64] = "192.168.0.47";
    char port_s[16] = "8193";
    char timeout_s[16] = "3";
    parse_query(req->query, "ip", ip, sizeof(ip));
    parse_query(req->query, "port", port_s, sizeof(port_s));
    parse_query(req->query, "timeout", timeout_s, sizeof(timeout_s));
    ushort port = (ushort)atoi(port_s);
    int timeout = atoi(timeout_s);

    if (strcmp(req->method, "OPTIONS") == 0) {
        const char *cors = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET,POST,PUT,DELETE,OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n\r\n";
        WRITE(fd, cors, (int)strlen(cors));
        return;
    }

    /* Static files */
    if (strcmp(req->path, "/") == 0) {
        send_file_response(fd, "index.html", "text/html; charset=utf-8"); return;
    }
    if (strcmp(req->path, "/swagger") == 0 || strcmp(req->path, "/swagger/") == 0) {
        send_file_response(fd, "index.html", "text/html; charset=utf-8"); return;
    }
    if (strcmp(req->path, "/swagger.json") == 0) {
        send_file_response(fd, "swagger.json", "application/json; charset=utf-8"); return;
    }
    if (strcmp(req->path, "/openapi.json") == 0) {
        send_file_response(fd, "openapi.json", "application/json; charset=utf-8"); return;
    }
    if (strcmp(req->path, "/docs") == 0 || strcmp(req->path, "/docs/") == 0) {
        send_file_response(fd, "docs.html", "text/html; charset=utf-8"); return;
    }

    /* Ping (no CNC handle needed) */
    if (strcmp(req->path, "/api/focas/ping") == 0) {
        n = api_ping(resp);
        send_response(fd, 200, "OK", resp, n);
        return;
    }

    /* All routes below need CNC handle */
    ushort h = get_handle(ip, port, timeout);
    if (!h) {
        n = sprintf(resp, "{\"Success\":false,\"Data\":null,\"ErrorCode\":-16,\"ErrorMsg\":\"Connect failed %s:%d\"}", ip, port);
        send_response(fd, 200, "OK", resp, n);
        return;
    }

    /* --- Basic monitor/control (existing, keep) --- */
    if (strcmp(req->path, "/api/focas/sysinfo") == 0) {
        n = api_sysinfo(h, resp);
    }
    else if (strcmp(req->path, "/api/focas/status") == 0) {
        n = api_status(h, resp);
    }
    else if (strcmp(req->path, "/api/focas/monitor") == 0) {
        n = api_monitor(h, resp);
    }
    else if (strcmp(req->path, "/api/focas/start") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_start(h, resp);
    }
    else if (strcmp(req->path, "/api/focas/reset") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_reset(h, resp);
    }
    else if (strcmp(req->path, "/api/focas/alarm") == 0 && strcmp(req->method, "GET") == 0) {
        n = api_alarm(h, resp);
    }
    else if (strcmp(req->path, "/api/focas/alarm/clear") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_alarm_clear(h, resp);
    }
    /* --- Old program routes (backward compat) --- */
    else if (strcmp(req->path, "/api/focas/program/upload") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_program_upload(h, req->body, req->body_len, resp);
    }
    else if (strcmp(req->path, "/api/focas/program/download") == 0) {
        char prg_s[16] = "1";
        parse_query(req->query, "program", prg_s, sizeof(prg_s));
        n = api_program_download(h, atol(prg_s), resp);
    }
    else if (strcmp(req->path, "/api/focas/program/run") == 0 && strcmp(req->method, "POST") == 0) {
        char prg_s[16] = "1";
        parse_query(req->query, "program", prg_s, sizeof(prg_s));
        n = api_program_run(h, atol(prg_s), resp);
    }
    /* --- Override --- */
    else if (strcmp(req->path, "/api/focas/override") == 0) {
        n = api_override(h, resp);
    }
    /* --- Programs API (new, matching README spec) --- */
    else if (strcmp(req->path, "/api/focas/programs") == 0 && strcmp(req->method, "GET") == 0) {
        n = api_program_list(h, resp);
    }
    else if (strcmp(req->path, "/api/focas/programs") == 0 && strcmp(req->method, "PUT") == 0) {
        n = api_program_upload_json(h, req->body, resp);
    }
    else if (path_prefix(req->path, "/api/focas/programs/")) {
        const char *sub = req->path + 20;
        if (strcmp(sub, "actpt") == 0 && strcmp(req->method, "GET") == 0) {
            n = api_active_pointer(h, resp);
        }
        else {
            /* Parse /api/focas/programs/{progNum} or /api/focas/programs/{progNum}/{action} */
            char subcopy[64];
            strncpy(subcopy, sub, sizeof(subcopy) - 1); subcopy[sizeof(subcopy)-1] = 0;
            char *slash = strchr(subcopy, '/');
            long progNum = 0;
            if (slash) {
                *slash = 0;
                progNum = atol(subcopy);
                const char *action = sub + (slash - subcopy) + 1;
                if (strcmp(action, "run") == 0 && strcmp(req->method, "POST") == 0) {
                    n = api_run_program(h, progNum, resp);
                } else if (strcmp(action, "autostart") == 0 && strcmp(req->method, "POST") == 0) {
                    n = api_autostart(h, progNum, resp);
                } else {
                    goto unknown_route;
                }
            } else {
                progNum = atol(subcopy);
                if (strcmp(req->method, "GET") == 0) {
                    n = api_program_content(h, progNum, resp);
                } else if (strcmp(req->method, "DELETE") == 0) {
                    n = api_program_delete(h, progNum, resp);
                } else {
                    goto unknown_route;
                }
            }
        }
    }
    /* --- Macros API --- */
    else if (strcmp(req->path, "/api/focas/macros") == 0 && strcmp(req->method, "GET") == 0) {
        char start_s[16]="100", count_s[16]="1";
        parse_query(req->query, "start", start_s, sizeof(start_s));
        parse_query(req->query, "count", count_s, sizeof(count_s));
        n = api_macros_range(h, atoi(start_s), atoi(count_s), resp);
    }
    else if (strcmp(req->path, "/api/focas/macros/batch") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_macros_batch(h, req->body, resp);
    }
    else if (path_prefix(req->path, "/api/focas/macros/user/")) {
        long macroNum = 0;
        path_int_after(req->path, "/api/focas/macros/user/", &macroNum);
        if (strcmp(req->method, "GET") == 0) n = api_read_user_macro(h, macroNum, resp);
        else goto unknown_route;
    }
    else if (strcmp(req->path, "/api/focas/macros/user") == 0 && strcmp(req->method, "PUT") == 0) {
        n = api_write_user_macro(h, req->body, resp);
    }
    else if (path_prefix(req->path, "/api/focas/macros/pcode/")) {
        long macroNum = 0;
        path_int_after(req->path, "/api/focas/macros/pcode/", &macroNum);
        if (strcmp(req->method, "GET") == 0) n = api_read_pcode_macro(h, macroNum, resp);
        else goto unknown_route;
    }
    else if (strcmp(req->path, "/api/focas/macros/pcode") == 0 && strcmp(req->method, "PUT") == 0) {
        n = api_write_pcode_macro(h, req->body, resp);
    }
    /* --- PLC API --- */
    else if (strcmp(req->path, "/api/focas/plc") == 0 && strcmp(req->method, "GET") == 0) {
        n = api_plc_query(h, req->query, resp);
    }
    else if (strcmp(req->path, "/api/focas/plc") == 0 && strcmp(req->method, "PUT") == 0) {
        n = api_write_plc(h, req->body, resp);
    }
    else if (strcmp(req->path, "/api/focas/plc/batch") == 0 && strcmp(req->method, "POST") == 0) {
        n = api_plc_batch(h, req->body, resp);
    }
    else if (path_prefix(req->path, "/api/focas/plc/")) {
        const char *sub = req->path + 15;
        if (strcmp(req->method, "PUT") == 0 && *sub == 0) {
            n = api_write_plc(h, req->body, resp);
        } else {
            int addrType = 0, addrNum = 0;
            if (sscanf(sub, "%d/%d", &addrType, &addrNum) >= 2) {
                char bit_s[16] = "-1";
                parse_query(req->query, "bit", bit_s, sizeof(bit_s));
                int bitIndex = atoi(bit_s);
                n = api_read_plc(h, addrType, addrNum, bitIndex, resp);
            } else {
                goto unknown_route;
            }
        }
    }
    /* --- Parameters API --- */
    else if (path_prefix(req->path, "/api/focas/parameters/axis/")) {
        long paramNum = 0;
        path_int_after(req->path, "/api/focas/parameters/axis/", &paramNum);
        n = api_axis_parameter(h, paramNum, resp);
    }
    else if (path_prefix(req->path, "/api/focas/parameters/")) {
        long paramNum = 0;
        path_int_after(req->path, "/api/focas/parameters/", &paramNum);
        if (strcmp(req->method, "GET") == 0) n = api_read_parameter(h, paramNum, resp);
        else if (strcmp(req->method, "PUT") == 0) n = api_write_parameter(h, req->body, resp);
        else goto unknown_route;
    }
    else if (strcmp(req->path, "/api/focas/parameters") == 0 && strcmp(req->method, "PUT") == 0) {
        n = api_write_parameter(h, req->body, resp);
    }
    else {
        unknown_route:
        n = sprintf(resp, "{\"Success\":false,\"ErrorCode\":-99,\"ErrorMsg\":\"Unknown route: %s %s\"}", req->method, req->path);
        send_response(fd, 404, "Not Found", resp, n);
        return;
    }

    send_response(fd, 200, "OK", resp, n);
}

/* ============ Main ============ */

int main(int argc, char *argv[]) {
    int port = PORT_DEFAULT;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);  /* ignore broken pipe */
#endif

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    short ret = cnc_startupprocess(0, g_logfile);
    if (ret != EW_OK) {
        fprintf(stderr, "FOCAS2 startup failed (ret=%d)\n", ret);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    init_conn_pool();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 32) < 0) { perror("listen"); return 1; }

    printf("\033[32m");
    printf("=======================================================\n");
    printf("  FOCAS2 Web API Server\n");
    printf("  Port:    %d\n", port);
    printf("  Ping:    http://localhost:%d/api/focas/ping\n", port);
    printf("  Monitor: http://localhost:%d/api/focas/monitor\n", port);
    printf("  Swagger: http://localhost:%d/swagger\n", port);
    printf("  Docs:    http://localhost:%d/docs\n", port);
    printf("=======================================================\n");
    printf("\033[0m");

    while (g_running) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int client_fd = accept(server_fd, (struct sockaddr*)&cli, &cli_len);
        if (client_fd < 0) continue;

        char buf[BUF_SIZE];
        int total = read_http_request(client_fd, buf, BUF_SIZE);
        buf[total] = 0;

        if (total > 0) {
            HttpRequest req;
            if (parse_request(buf, total, &req)) {
                handle_request(client_fd, &req);
            }
        }
        CLOSE_SOCKET(client_fd);
    }

    for (int i = 0; i < MAX_CONN; i++) {
        if (g_conn[i].handle) cnc_freelibhndl(g_conn[i].handle);
    }
    cnc_exitprocess();
    CLOSE_SOCKET(server_fd);

#ifdef _WIN32
    WSACleanup();
#endif

    printf("Server stopped.\n");
    return 0;
}

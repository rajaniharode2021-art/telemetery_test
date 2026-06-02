#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <time.h>
#include <errno.h>

#define SHM_PATH    "/dev/shm/data"
#define CFG_PATH    "/workspace/build/telemetry_config.json"
#define MQTT_HOST   "127.0.0.1"
#define MQTT_PORT   1883
#define TOPIC       "/vehicle/telemetry"
#define CLIENT_ID   "telemetry-forwarder"
#define LOG_HOST    "127.0.0.1"
#define LOG_PORT    8080
#define POLL_MS     50
#define MQ_CAP      256
#define LQ_CAP      512
#define BATCH_MAX   50
#define FLUSH_MS    2000
#define MAX_FIELDS  16

typedef enum { U16, U32, F32, STR } FType;

typedef struct {
    char name[64];
    uint32_t offset;
    uint32_t size;
    FType type;
} Field;

typedef struct {
    char shm[256];
    uint32_t pkt_size;
    uint16_t magic;
    Field fields[MAX_FIELDS];
    int nfields;
} Config;

typedef struct {
    char **buf;
    int cap, head, tail, count, dropped, done;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} Queue;

static volatile sig_atomic_t running = 1;
static Queue *mq, *lq;

/* ---- queue ---- */

static Queue *q_new(int cap) {
    Queue *q = calloc(1, sizeof(*q));
    q->buf = calloc(cap, sizeof(char *));
    q->cap = cap;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    return q;
}

static void q_push(Queue *q, char *s) {
    pthread_mutex_lock(&q->mu);
    if (q->count >= q->cap) 
    {   
        q->dropped++; 
        free(s); 
    }
    else { 
        q->buf[q->tail] = s; 
        q->tail = (q->tail+1) % q->cap; q->count++; 
        pthread_cond_signal(&q->cv); 
    }
    pthread_mutex_unlock(&q->mu);
}

static char *q_pop(Queue *q, int ms) {
    struct timespec ts;
    pthread_mutex_lock(&q->mu);
    if (!q->count && !q->done) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) 
        { 
            ts.tv_sec++; ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&q->cv, &q->mu, &ts);
    }
    char *s = NULL;
    if (q->count) 
    { 
        s = q->buf[q->head]; 
        q->head = (q->head+1) % q->cap; 
        q->count--; 
    }
    pthread_mutex_unlock(&q->mu);
    return s;
}

static void q_stop(Queue *q) {
    pthread_mutex_lock(&q->mu);
    q->done = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

/* ---- utils ---- */

static uint16_t u16(const uint8_t *p) { return p[0] | p[1]<<8; }
static uint32_t u32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static float    f32(const uint8_t *p) { uint32_t v=u32(p); float f; memcpy(&f,&v,4); return f; }

static void msleep(int ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }

static int64_t now_ms() {
    struct timespec t; 
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec*1000 + t.tv_nsec/1000000;
}

/* ---- tiny json config parser ---- */

static const char *jval(const char *j, const char *key) {
    char k[128]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(j, k);
     if (!p) return NULL;
    p += strlen(k); 
    while (*p==' '||*p==':'||*p=='\t') 
        p++; 
    return p;
}
static double jnum(const char *j, const char *k, double def) { const char *p=jval(j,k); return p ? strtod(p,NULL) : def; }
static void jstr(const char *j, const char *k, char *out, int n) {
    const char *p=jval(j,k); if (!p||*p!='"') return; p++;
    int i=0; while (*p&&*p!='"'&&i<n-1) out[i++]=*p++; out[i]=0;
}

static int load_config(const char *path, Config *c) {
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *b = malloc(sz+1); b[fread(b,1,sz,f)] = 0; fclose(f);

    jstr(b, "path", c->shm, sizeof c->shm);
    if (!c->shm[0]) strncpy(c->shm, SHM_PATH, sizeof(c->shm)-1);
    c->pkt_size = (uint32_t)jnum(b, "size_bytes", 26);
    c->magic    = (uint16_t)jnum(b, "value", 0xDEAD);

    const char *p = strstr(b, "\"fields\""); p = p ? strchr(p,'[') : NULL;
    if (!p) { free(b); return -1; } p++;

    while (*p && c->nfields < MAX_FIELDS) {
        p = strchr(p, '{'); if (!p) break;
        const char *e = strchr(p, '}'); if (!e) break;
        int bl = e-p+1; char *fb = malloc(bl+1); memcpy(fb,p,bl); fb[bl]=0;
        Field *fd = &c->fields[c->nfields++];
        jstr(fb, "name", fd->name, sizeof fd->name);
        fd->offset = (uint32_t)jnum(fb, "offset", 0);
        fd->size   = (uint32_t)jnum(fb, "size_bytes", 1);
        char t[32]={0}; jstr(fb, "type", t, sizeof t);
        fd->type = !strcmp(t,"uint32")?U32 : !strcmp(t,"float32")?F32 : !strcmp(t,"string")?STR : U16;
        free(fb); p = e+1;
    }
    free(b); return c->nfields > 0 ? 0 : -1;
}

/* ---- decode packet to json ---- */

static char *decode(const uint8_t *raw, const Config *c) {
    if (u16(raw) != c->magic) 
        return NULL;
    char *out = malloc(2048); 
    int n = 0;
    n += snprintf(out+n, 2048-n, "{");
    for (int i = 0; i < c->nfields; i++) {
        const Field *fd = &c->fields[i];
        const uint8_t *p = raw + fd->offset;
        if (i) n += snprintf(out+n, 2048-n, ",");
        n += snprintf(out+n, 2048-n, "\"%s\":", fd->name);
        switch (fd->type) {
            case U16: n += snprintf(out+n, 2048-n, "%u", u16(p)); break;
            case U32: n += snprintf(out+n, 2048-n, "%u", u32(p)); break;
            case F32: { float v=f32(p); n += isfinite(v) ? snprintf(out+n,2048-n,"%.4g",(double)v) : snprintf(out+n,2048-n,"null"); break; }
            case STR: { char s[64]={0}; memcpy(s,p,fd->size<63?fd->size:63); n+=snprintf(out+n,2048-n,"\"%s\"",s); break; }
        }
    }
    snprintf(out+n, 2048-n, "}"); return out;
}

/* ---- logging ---- */

static void log_ev(const char *fmt, ...) {
    char msg[512]; 
    va_list ap; 
    va_start(ap,fmt); 
    vsnprintf(msg,sizeof msg,fmt,ap); 
    va_end(ap);
    printf("[log] %s\n", msg); 
    fflush(stdout);
    char *e = malloc(600);
    snprintf(e, 600, "{\"message\":\"%s\",\"timestamp\":%lld}", msg, (long long)now_ms());
    q_push(lq, e);
}

/* ---- tcp / mqtt ---- */

static int tcp_connect(const char *host, int port) {
    struct addrinfo hints={0}, *res=NULL;
    hints.ai_family = AF_INET; 
    hints.ai_socktype = SOCK_STREAM;
    char ps[8]; 
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) 
        return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen)) 
    { 
        close(fd);
        freeaddrinfo(res); return -1; 
    }
    freeaddrinfo(res); return fd;
}

static int writen(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) { ssize_t r = write(fd,p,n); if (r<=0) return -1; p+=r; n-=r; }
    return 0;
}

static int enc_varlen(uint8_t *b, uint32_t v) {
    int i=0; do { b[i]=v&0x7F; v>>=7; if(v) b[i]|=0x80; i++; } while(v); return i;
}

static int mqtt_connect(int fd) {
    uint16_t cl = strlen(CLIENT_ID);
    uint32_t rem = 10 + 2 + cl;
    uint8_t p[128]; 
    int n=0;
    p[n++]=0x10; 
    n+=enc_varlen(p+n, rem);
    p[n++]=0; p[n++]=4; 
    memcpy(p+n,"MQTT",4); n+=4;
    p[n++]=4; 
    p[n++]=0x02; 
    p[n++]=0; 
    p[n++]=60;
    p[n++]=(cl>>8); 
    p[n++]=cl;
    memcpy(p+n,CLIENT_ID,cl); n+=cl;
    if (writen(fd, p, n))
         return -1;
    uint8_t ack[4]; 
    return (read(fd,ack,4)==4 && ack[0]==0x20 && ack[3]==0) ? 0 : -1;
}

static int mqtt_pub(int fd, const char *topic, const char *payload) {
    uint16_t tl = strlen(topic); uint32_t pl = strlen(payload);
    uint8_t h[8]; int n=0;
    h[n++]=0x30; n+=enc_varlen(h+n, 2+tl+pl);
    h[n++]=(tl>>8); h[n++]=tl;
    return (writen(fd,h,n)||writen(fd,topic,tl)||writen(fd,payload,pl)) ? -1 : 0;
}

static void *mqtt_thread(void *arg) {
    (void)arg; int fd=-1, tries=0;
retry:
    while (running && fd < 0) {
        fd = tcp_connect(MQTT_HOST, MQTT_PORT);
        if (fd>=0 && mqtt_connect(fd)) 
        {
             close(fd); 
             fd=-1; 
        }
        if (fd < 0) 
        { 
            log_ev("mqtt connect fail #%d", ++tries); 
            msleep(2000); 
        }
    }
    log_ev("mqtt connected after %d tries", tries);
    while (running) {
        char *msg = q_pop(mq, 500); if (!msg) continue;
        if (mqtt_pub(fd, TOPIC, msg)) {
            log_ev("mqtt publish failed, reconnecting");
            free(msg); close(fd); fd=-1; msleep(2000); goto retry;
        }
        free(msg);
    }
    if (fd>=0) close(fd);
    return NULL;
}

/*  http log thread ---- */

static int http_post(const char *body) {
    int fd = tcp_connect(LOG_HOST, LOG_PORT); if (fd<0) return -1;
    char req[65536]; int bl = strlen(body);
    int hl = snprintf(req, sizeof req,
        "POST /batch HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        LOG_HOST, LOG_PORT, bl);
    if (hl+bl >= (int)sizeof req) { close(fd); return -1; }
    memcpy(req+hl, body, bl);
    if (writen(fd, req, hl+bl)) { close(fd); return -1; }

    /* read full response until server closes connection */
    char resp[64]={0}; int ri=0; int st=0;
    char rbuf[512]; ssize_t n;
    while ((n = read(fd, rbuf, sizeof rbuf)) > 0) {
        if (!st && ri < (int)sizeof(resp)-1) {
            int copy = n < (int)sizeof(resp)-1-ri ? n : (int)sizeof(resp)-1-ri;
            memcpy(resp+ri, rbuf, copy); ri += copy;
        }
    }
    close(fd);
    sscanf(resp, "HTTP/%*s %d", &st);
    return (st==200||st==201) ? 0 : (st ? st : -1);
}


static void *log_thread(void *arg) {
    (void)arg;
    char *batch[BATCH_MAX]; int nb=0;
    int64_t last = now_ms();
    printf("[log] started -> %s:%d\n", LOG_HOST, LOG_PORT);

    while (running || lq->count > 0) {
        int wait = (int)(FLUSH_MS - (now_ms()-last));
        char *e = q_pop(lq, wait<0?0:wait);
        if (e) batch[nb++] = e;

        if (!nb) continue;
        if (nb < BATCH_MAX && (now_ms()-last) < FLUSH_MS && running) continue;

        char *body = malloc(nb*600+16); int pos=0;
        pos += snprintf(body+pos, nb*600+16, "[");
        for (int i=0; i<nb; i++) { if(i) pos+=snprintf(body+pos,nb*600+16-pos,","); pos+=snprintf(body+pos,nb*600+16-pos,"%s",batch[i]); }
        snprintf(body+pos, nb*600+16-pos, "]");

        int sent=0;
        for (int t=0; t<5&&!sent; t++) {
            int rc = http_post(body);
            if (rc==0) { sent=1; }
            else if (rc>=500) { printf("[log] server error %d, retry %d\n", rc, t+1); msleep(2000); }
            else { printf("[log] post failed rc=%d, dropping %d\n", rc, nb); break; }
        }
        if (!sent) printf("[log] batch dropped\n");

        free(body);
        for (int i=0; i<nb; i++) free(batch[i]);
        nb=0; last=now_ms();
    }
    printf("[log] stopped\n");
    return NULL;
}

/* ---- main ---- */

static void on_sig(int s) { (void)s; running=0; }

int main(int argc, char **argv) {
    signal(SIGINT,on_sig); 
    signal(SIGTERM,on_sig); 
    signal(SIGPIPE,SIG_IGN);

    Config cfg = {0};
    if (load_config(argc>1?argv[1]:CFG_PATH, &cfg)) 
    {   
        fprintf(stderr,"config failed\n"); 
        return 1; 
    }
    printf("fields=%d pkt=%u shm=%s\n", cfg.nfields, cfg.pkt_size, cfg.shm);

    int shm = -1;
    for (int i=0; i<30 && shm<0; i++) {
        shm = open(cfg.shm, O_RDONLY);
        if (shm<0) { 
            printf("waiting for shm...\n"); 
            msleep(500); 
        }
    }
    if (shm<0) 
    { 
        fprintf(stderr,"can't open shm\n"); 
        return 1; 
    }

    mq = q_new(MQ_CAP); 
    lq = q_new(LQ_CAP);
    
    pthread_t t1, t2;
    pthread_create(&t1, NULL, mqtt_thread, NULL);
    pthread_create(&t2, NULL, log_thread, NULL);
    log_ev("started pid=%d", (int)getpid());

    uint8_t prev[256]={0}, curr[256];
    uint32_t last_seq=0; int init=0; uint64_t count=0;
    printf("running...\n");

    while (running) {
        if (lseek(shm,0,SEEK_SET)<0 || read(shm,curr,cfg.pkt_size)!=(ssize_t)cfg.pkt_size) 
        { 
            msleep(POLL_MS); 
            continue; 
        }
        if (init && !memcmp(curr,prev,cfg.pkt_size)) 
        { 
            msleep(POLL_MS); 
            continue; 
        }

        memcpy(prev, curr, cfg.pkt_size);

        char *json = decode(curr, &cfg);
        if (!json) { log_ev("bad magic"); msleep(POLL_MS); continue; }

        uint32_t seq = u32(curr+2);
        if (init && seq!=last_seq+1 && seq!=last_seq)
            log_ev("seq gap: expected %u got %u", last_seq+1, seq);
        
        last_seq=seq; 
        init=1;

        q_push(mq, json);
        if (++count % 100 == 0)
            log_ev("count=%llu mq=%d lq=%d dropped=%d", (unsigned long long)count, mq->count, lq->count, mq->dropped);
        msleep(POLL_MS);
    }

    log_ev("shutting down");
    q_stop(mq); 
    msleep(500); 
    q_stop(lq);
    pthread_join(t1,NULL); 
    pthread_join(t2,NULL);
    close(shm);
    printf("done. count=%llu\n", (unsigned long long)count);
    return 0;
}
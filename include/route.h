#ifndef ROUTE_H
#define ROUTE_H

#include <stddef.h>
#include <stdint.h>

#define ROUTE_MAX_ROUTES   16
#define ROUTE_MAX_BACKENDS 8
#define ROUTE_MAX_WEIGHT   4
#define ROUTE_RING_POINTS  160                /* points per unit of weight */
#define ROUTE_RING_MAX (ROUTE_MAX_BACKENDS * ROUTE_RING_POINTS * ROUTE_MAX_WEIGHT)

#define HEALTH_FAILS_TO_DOWN 3
#define HEALTH_OKS_TO_UP     2

typedef struct {
    char host[256];
    char port[8];
    unsigned weight;                      /* 1..ROUTE_MAX_WEIGHT, "*N" in spec */
    /* Health state: read/written only with __atomic builtins. */
    int down;                             /* 0 = UP, 1 = DOWN */
    unsigned fails;                       /* consecutive failures */
    unsigned oks;                         /* consecutive successes */
    int64_t changed_ms;                   /* CLOCK_MONOTONIC ms of last UP/DOWN change (set at add) */
    unsigned long req_ok, req_err;        /* proxied request outcomes */
    /* Back-pressure counters: atomic only. */
    unsigned inflight;                    /* proxied requests currently in progress */
    unsigned long shed;                   /* times skipped because of the cap */
} backend;

typedef struct {
    char prefix[256];                     /* e.g. "/api/" */
    backend backends[ROUTE_MAX_BACKENDS];
    size_t backend_count;
    unsigned int next;                    /* round-robin cursor, use atomics */
    int ketama;                           /* "@ketama" after the prefix */
    /* Ketama ring, built and sorted once in route_table_add; read lock-free.
     * Split arrays keep route_table small (tests put two copies on the stack). */
    size_t ring_len;
    uint32_t ring_hash[ROUTE_RING_MAX];
    uint8_t ring_backend[ROUTE_RING_MAX];
} route;

typedef struct {
    route routes[ROUTE_MAX_ROUTES];
    size_t count;
} route_table;

void route_table_init(route_table *t);

/* Add a route from a spec string:
 *   "/api/=127.0.0.1:8001,127.0.0.1:8003"
 *   "/echo@ketama=127.0.0.1:8001*2,127.0.0.1:8002"   (*N = weight 1..4)
 * prefix must start with '/'. Returns 0 on success, -1 on malformed spec
 * or table full (table unchanged on failure). */
int route_table_add(route_table *t, const char *spec);

/* Longest-prefix match on a decoded request path. NULL if none. */
route *route_match(route_table *t, const char *path);

/* key의 시도 순서대로, DOWN이 아닌 서로 다른 백엔드를 최대 max개 out에 채운다.
 * ketama: 링을 시계방향으로 돌며 찾는다. rr: 커서 위치부터 찾는다(가중치 반영).
 * 반환값은 채운 개수. key may be NULL for rr. */
size_t route_pick(route *r, const char *key, backend **out, size_t max);

/* Fraction (0..1) of the key space owned by backend i: ring arc share for
 * ketama, weight share for rr. Ignores health. */
double route_share(const route *r, size_t i);

/* 성공/실패를 health 상태에 반영한다. passive 경로와 active 경로가 둘 다 호출한다.
 * FAILS_TO_DOWN consecutive failures -> DOWN; while DOWN, OKS_TO_UP
 * consecutive successes -> UP. Thread-safe. */
void backend_report(backend *b, int ok);
int  backend_is_up(const backend *b);

int backend_try_acquire(backend *b, unsigned cap);
void backend_release(backend *b);

#endif

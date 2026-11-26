/*
 * gen_trace.c - synthetic Score-P-like OTF2 trace for EduMPI_GUI performance tests
 *
 * Simulates a BSP stencil application (periodic 2D grid) with:
 *   - compute (load imbalance, slow nodes, OS noise)
 *   - halo exchange via MPI_Irecv/MPI_Isend/MPI_Waitall (4 neighbours)
 *   - MPI_Allreduce (WORLD) every iteration
 *   - MPI_Alltoall in row communicators every 10 iterations
 *   - MPI_Bcast (WORLD) + MPI_Barrier (node communicators) every 50 iterations
 *   - ring pipeline with blocking MPI_Send/MPI_Recv every 25 iterations
 * Times are causally consistent (a receive / Waitall never ends before the message arrives,
 * collectives end after the last rank entered) -> real late-sender / waiting times.
 *
 * Build:  gcc -O2 -o gen_trace gen_trace.c $(otf2-config --cflags --ldflags --libs) -lm
 *         (Ubuntu: apt install libotf2-trace-dev otf2-tools)
 * Usage:  ./gen_trace --out DIR [--ranks 1024] [--ranks-per-node 32]
 *                     [--iterations N | --target-gb 8] [--seed 1] [--rank-order row|random]
 *         --rank-order random places the ranks on the grid in random order (the same for the
 *         same seed): neighbours are no longer rank +-1 / +-PX, e.g. to test layouts that must
 *         find the grid without the numbering.
 * Output: DIR/traces.otf2 (+ traces.def, traces/); open it in the GUI as a trace.
 */
#include <otf2/otf2.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Parameter ---------------------------------------------------------------- */
static int P = 1024, RPN = 32, SEED = 1, RANDOM_ORDER = 0;
static long ITERS = 0;
static double TARGET_GB = 0;
static int ALLTOALL_EVERY = 10, BCAST_EVERY = 50, PIPE_EVERY = 25;
static const char *OUT = NULL;
/* measured with this generator (OTF2 3.0.3), used for --target-gb */
static double BYTES_PER_EVENT = 10.0;

/* ---- RNG (xorshift) ---------------------------------------------------------- */
static uint64_t rs = 88172645463325252ull;
static inline uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static inline double urand(void) { return (rnd() >> 11) * (1.0 / 9007199254740992.0); }
static inline double nrand(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }

/* ---- Definitionen ------------------------------------------------------------- */
enum { S_EMPTY, S_MACHINE_NAME, S_MACHINE, S_NODE, S_MASTER, S_MPI, S_PROG, S_WORLD, S_SELF,
       S_NODECOMM, S_ROWCOMM, S_FILE, S_FIRST_REGION };
enum { R_MAIN, R_TIMESTEP, R_COMPUTE, R_HALO, R_REDUCE, R_ROWX, R_PIPE, R_CHECKPOINT,
       R_MPI_INIT, R_MPI_FINALIZE, R_IRECV, R_ISEND, R_WAITALL, R_ALLREDUCE, R_ALLTOALL,
       R_BCAST, R_BARRIER, R_SEND, R_RECV, R_COUNT };
static const char *rname[R_COUNT] = { "main", "timestep", "compute_stencil", "halo_exchange",
    "global_reduce", "row_transpose", "pipeline_sweep", "checkpoint",
    "MPI_Init", "MPI_Finalize", "MPI_Irecv", "MPI_Isend", "MPI_Waitall", "MPI_Allreduce",
    "MPI_Alltoall", "MPI_Bcast", "MPI_Barrier", "MPI_Send", "MPI_Recv" };
#define IS_MPI(r) ((r) >= R_MPI_INIT)

enum { C_WORLD = 0, C_SELF = 1, C_NODE0 = 2 }; /* followed by the node comms, then the row comms */

/* ---- Zustand ------------------------------------------------------------------ */
static OTF2_EvtWriter **ew;
static uint64_t *nev;     /* events per rank */
static double *t;         /* current time per rank [ns] */
static double *slow;      /* load factor per rank */
static double *sendT;     /* send time per rank x direction */
static uint64_t *req;     /* request ids per rank */
static int PX, PY, NODES;
/* Grid position <-> rank (--rank-order random: ranks placed on the grid in random order). */
static int *rankAt;  /* grid position -> rank */
static int *posOf;   /* rank -> grid position */
static const uint64_t T0 = 1000000000ull; /* Global Offset */
static uint64_t tmax = 0;

static inline uint64_t ts(int r) { uint64_t v = T0 + (uint64_t)t[r]; if (v > tmax) tmax = v; return v; }
static inline void enter(int r, int reg) { OTF2_EvtWriter_Enter(ew[r], NULL, ts(r), reg); nev[r]++; }
static inline void leave(int r, int reg) { OTF2_EvtWriter_Leave(ew[r], NULL, ts(r), reg); nev[r]++; }
static inline int node_of(int r) { return r / RPN; }
static inline int nb(int r, int d) {
    int x = posOf[r] % PX, y = posOf[r] / PX;
    switch (d) { case 0: x = (x + 1) % PX; break; case 1: x = (x + PX - 1) % PX; break;
                 case 2: y = (y + 1) % PY; break; default: y = (y + PY - 1) % PY; }
    return rankAt[y * PX + x];
}
static inline double xfer(int a, int b, double bytes) { /* latency + bytes / bandwidth [ns] */
    return node_of(a) == node_of(b) ? 400 + bytes / 8.0 : 1800 + bytes / 2.5;
}

/* Collective on communicator comm with members m[0..n-1] (world ranks) */
static void collective(int user, int mpi, OTF2_CollectiveOp op, OTF2_CommRef comm,
                       const int *m, int n, uint32_t root, uint64_t bytes, double cost)
{
    double last = 0;
    for (int i = 0; i < n; ++i) {
        int r = m[i];
        if (user >= 0) { enter(r, user); t[r] += 200; }
        enter(r, mpi);
        OTF2_EvtWriter_MpiCollectiveBegin(ew[r], NULL, ts(r)); nev[r]++;
        if (t[r] > last) last = t[r];
    }
    double end = last + cost;
    for (int i = 0; i < n; ++i) {
        int r = m[i];
        t[r] = end + urand() * 300;
        uint64_t s = op == OTF2_COLLECTIVE_OP_BARRIER ? 0 : bytes, rcv = s;
        if (op == OTF2_COLLECTIVE_OP_BCAST) { s = (i == (int)root) ? bytes * (n - 1) : 0; rcv = (i == (int)root) ? 0 : bytes; }
        if (op == OTF2_COLLECTIVE_OP_ALLTOALL) { s = bytes * n; rcv = bytes * n; }
        OTF2_EvtWriter_MpiCollectiveEnd(ew[r], NULL, ts(r), op, comm, root, s, rcv); nev[r]++;
        leave(r, mpi);
        if (user >= 0) { t[r] += 150; leave(r, user); }
    }
}

static OTF2_FlushType pre_flush(void *u, OTF2_FileType ft, OTF2_LocationRef l, void *c, bool f)
{ (void)u; (void)ft; (void)l; (void)c; (void)f; return OTF2_FLUSH; }
static OTF2_TimeStamp post_flush(void *u, OTF2_FileType ft, OTF2_LocationRef l)
{ (void)u; (void)ft; (void)l; return 0; }
static OTF2_FlushCallbacks flush_cb = { pre_flush, post_flush };

static void usage(void)
{
    fprintf(stderr, "Usage: gen_trace --out DIR [--ranks N] [--ranks-per-node N] "
                    "[--iterations N | --target-gb X] [--seed N] [--bytes-per-event X] [--rank-order row|random]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i]; const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!v) usage();
        if (!strcmp(a, "--out")) OUT = v;
        else if (!strcmp(a, "--ranks")) P = atoi(v);
        else if (!strcmp(a, "--ranks-per-node")) RPN = atoi(v);
        else if (!strcmp(a, "--iterations")) ITERS = atol(v);
        else if (!strcmp(a, "--target-gb")) TARGET_GB = atof(v);
        else if (!strcmp(a, "--seed")) SEED = atoi(v);
        else if (!strcmp(a, "--rank-order")) RANDOM_ORDER = v && !strcmp(v, "random");
        else if (!strcmp(a, "--bytes-per-event")) BYTES_PER_EVENT = atof(v);
        else usage();
        ++i;
    }
    if (!OUT || P < 2 || RPN < 1) usage();
    rs ^= (uint64_t)SEED * 0x9E3779B97F4A7C15ull;
    NODES = (P + RPN - 1) / RPN;
    PX = (int)sqrt((double)P); while (P % PX) --PX; PY = P / PX;
    /* about 47 events per rank and iteration (incl. the share of collectives / pipeline) */
    const double evPerIter = 47.0;
    if (!ITERS) ITERS = TARGET_GB > 0 ? (long)(TARGET_GB * 1e9 / (BYTES_PER_EVENT * evPerIter * P)) : 200;
    if (ITERS < 1) ITERS = 1;
    rankAt = malloc(sizeof(int) * P);
    posOf = malloc(sizeof(int) * P);
    for (int i = 0; i < P; ++i) rankAt[i] = i;
    if (RANDOM_ORDER) /* Fisher-Yates with its own seed, the same for the same --seed */
        for (int i = P - 1; i > 0; --i) {
            int j = (int)(rnd() % (uint64_t)(i + 1)), tmp = rankAt[i];
            rankAt[i] = rankAt[j]; rankAt[j] = tmp;
        }
    for (int i = 0; i < P; ++i) posOf[rankAt[i]] = i;
    fprintf(stderr, "Ranks %d, nodes %d (%d/node), grid %dx%d, iterations %ld, rank order %s\n", P, NODES, RPN, PX, PY,
            ITERS, RANDOM_ORDER ? "random" : "row by row");

    ew = calloc(P, sizeof *ew); nev = calloc(P, sizeof *nev); t = calloc(P, sizeof *t);
    slow = calloc(P, sizeof *slow); sendT = calloc((size_t)P * 4, sizeof *sendT); req = calloc(P, sizeof *req);
    int *world = malloc(P * sizeof(int)); for (int r = 0; r < P; ++r) world[r] = r;

    /* Load profile: base noise, some slow nodes, gradient over the grid */
    for (int r = 0; r < P; ++r) {
        slow[r] = 1.0 + 0.04 * nrand() + 0.10 * ((double)(posOf[r] % PX) / PX);
        if (node_of(r) % 7 == 3) slow[r] += 0.18; /* "slow node" */
        if (r % 97 == 13) slow[r] += 0.35;        /* single hotspot ranks */
        if (slow[r] < 0.8) slow[r] = 0.8;
    }

    OTF2_Archive *ar = OTF2_Archive_Open(OUT, "traces", OTF2_FILEMODE_WRITE, 256 * 1024, 4 * 1024 * 1024,
                                         OTF2_SUBSTRATE_POSIX, OTF2_COMPRESSION_NONE);
    if (!ar) { fprintf(stderr, "Cannot create the archive: %s\n", OUT); return 1; }
    OTF2_Archive_SetFlushCallbacks(ar, &flush_cb, NULL);
    OTF2_Archive_SetSerialCollectiveCallbacks(ar);
    OTF2_Archive_SetCreator(ar, "edumpi synthetic trace generator");
    OTF2_Archive_OpenEvtFiles(ar);
    for (int r = 0; r < P; ++r) ew[r] = OTF2_Archive_GetEvtWriter(ar, r);

    /* ---- start: main + MPI_Init (collective CREATE_HANDLE) ---- */
    for (int r = 0; r < P; ++r) {
        t[r] = urand() * 2e6; /* staggered process start */
        OTF2_EvtWriter_ProgramBegin(ew[r], NULL, ts(r), S_PROG, 0, NULL); nev[r]++;
        t[r] += 500; enter(r, R_MAIN); t[r] += 300;
    }
    collective(-1, R_MPI_INIT, OTF2_COLLECTIVE_OP_CREATE_HANDLE, C_WORLD, world, P, OTF2_UNDEFINED_UINT32, 0, 5e6);

    int *mem = malloc(P * sizeof(int));
    const int rowComm0 = C_NODE0 + NODES;
    for (long it = 0; it < ITERS; ++it) {
        /* ---- phase 1: timestep, compute, halo posts ---- */
        double noiseRank = urand() < 0.05 ? (double)(rnd() % P) : -1; /* OS noise */
        double phase = 1.0 + 0.15 * sin(it * 2 * M_PI / 400.0);       /* slow drift */
        uint64_t haloBytes = 32768ull << (it / 1000 % 3);              /* 32/64/128 KiB */
        for (int r = 0; r < P; ++r) {
            enter(r, R_TIMESTEP); t[r] += 120;
            enter(r, R_COMPUTE);
            double c = 1800000.0 * slow[r] * phase * (1 + 0.03 * nrand());
            if (r == (int)noiseRank) c += 2000000 + urand() * 4000000;
            if (c < 200000) c = 200000;
            t[r] += c;
            leave(r, R_COMPUTE);
            t[r] += 80; enter(r, R_HALO);
            for (int d = 0; d < 4; ++d) {
                enter(r, R_IRECV); t[r] += 300 + urand() * 200;
                OTF2_EvtWriter_MpiIrecvRequest(ew[r], NULL, ts(r), req[r] + d); nev[r]++;
                t[r] += 100; leave(r, R_IRECV);
            }
            for (int d = 0; d < 4; ++d) {
                enter(r, R_ISEND); t[r] += 600 + urand() * 300;
                OTF2_EvtWriter_MpiIsend(ew[r], NULL, ts(r), nb(r, d), C_WORLD, 100 + d, haloBytes, req[r] + 4 + d); nev[r]++;
                sendT[r * 4 + d] = t[r];
                t[r] += 200; leave(r, R_ISEND);
            }
        }
        /* ---- phase 2: Waitall (ends after all halo messages arrived) ---- */
        for (int r = 0; r < P; ++r) {
            enter(r, R_WAITALL);
            double done = t[r] + 500;
            for (int d = 0; d < 4; ++d) {
                int s = nb(r, d ^ 1);          /* neighbour that sends to me in direction d */
                double arr = sendT[s * 4 + d] + xfer(s, r, (double)haloBytes);
                if (arr > done) done = arr;
            }
            t[r] = done;
            for (int d = 0; d < 4; ++d) { OTF2_EvtWriter_MpiIsendComplete(ew[r], NULL, ts(r), req[r] + 4 + d); nev[r]++; }
            for (int d = 0; d < 4; ++d) {
                int s = nb(r, d ^ 1);
                OTF2_EvtWriter_MpiIrecv(ew[r], NULL, ts(r), s, C_WORLD, 100 + d, haloBytes, req[r] + d); nev[r]++;
            }
            t[r] += 300; leave(r, R_WAITALL);
            t[r] += 100; leave(r, R_HALO);
            req[r] += 8;
        }
        /* ---- Allreduce (residual) ---- */
        collective(R_REDUCE, R_ALLREDUCE, OTF2_COLLECTIVE_OP_ALLREDUCE, C_WORLD, world, P,
                   OTF2_UNDEFINED_UINT32, 8, 2500.0 * log2((double)P));
        /* ---- Alltoall in the row communicators ---- */
        if (it % ALLTOALL_EVERY == ALLTOALL_EVERY - 1)
            for (int y = 0; y < PY; ++y) {
                for (int x = 0; x < PX; ++x) mem[x] = rankAt[y * PX + x];
                collective(R_ROWX, R_ALLTOALL, OTF2_COLLECTIVE_OP_ALLTOALL, rowComm0 + y, mem, PX,
                           OTF2_UNDEFINED_UINT32, 4096, 900.0 * PX + 20000);
            }
        /* ---- pipeline: blocking Send/Recv along a ring (late sender) ---- */
        if (it % PIPE_EVERY == PIPE_EVERY - 1) {
            /* rank 0 sends first; rank r receives from r-1, then sends to r+1 */
            double prevSend = 0;
            for (int r = 0; r < P; ++r) {
                enter(r, R_PIPE); t[r] += 100;
                if (r > 0) {
                    enter(r, R_RECV);
                    double arr = prevSend + xfer(r - 1, r, 16384);
                    if (arr > t[r]) t[r] = arr;
                    t[r] += 200;
                    OTF2_EvtWriter_MpiRecv(ew[r], NULL, ts(r), r - 1, C_WORLD, 7, 16384); nev[r]++;
                    leave(r, R_RECV);
                    t[r] += 3000 * slow[r];
                }
                enter(r, R_SEND); t[r] += 400;
                OTF2_EvtWriter_MpiSend(ew[r], NULL, ts(r), (r + 1) % P, C_WORLD, 7, 16384); nev[r]++;
                prevSend = t[r];
                t[r] += xfer(r, (r + 1) % P, 16384) * 0.3; /* eager: short */
                leave(r, R_SEND);
                if (r > 0) { t[r] += 100; leave(r, R_PIPE); }
            }
            /* rank 0 closes the ring */
            enter(0, R_RECV);
            double arr = prevSend + xfer(P - 1, 0, 16384);
            if (arr > t[0]) t[0] = arr;
            t[0] += 200;
            OTF2_EvtWriter_MpiRecv(ew[0], NULL, ts(0), P - 1, C_WORLD, 7, 16384); nev[0]++;
            leave(0, R_RECV); t[0] += 100; leave(0, R_PIPE);
        }
        /* ---- checkpoint: Bcast (WORLD) + barrier per node ---- */
        if (it % BCAST_EVERY == BCAST_EVERY - 1) {
            for (int r = 0; r < P; ++r) enter(r, R_CHECKPOINT);
            collective(-1, R_BCAST, OTF2_COLLECTIVE_OP_BCAST, C_WORLD, world, P, 0, 65536,
                       4000.0 * log2((double)P) + 65536 / 2.5);
            for (int n = 0; n < NODES; ++n) {
                int cnt = 0;
                for (int r = n * RPN; r < P && r < (n + 1) * RPN; ++r) { mem[cnt++] = r; t[r] += 150000 * (0.8 + 0.4 * urand()); }
                collective(-1, R_BARRIER, OTF2_COLLECTIVE_OP_BARRIER, C_NODE0 + n, mem, cnt, OTF2_UNDEFINED_UINT32, 0, 1500);
            }
            for (int r = 0; r < P; ++r) { t[r] += 100; leave(r, R_CHECKPOINT); }
        }
        for (int r = 0; r < P; ++r) { t[r] += 90; leave(r, R_TIMESTEP); }
        if (it % 500 == 0) {
            uint64_t e = 0; for (int r = 0; r < P; ++r) e += nev[r];
            fprintf(stderr, "\riteration %ld/%ld, %.0f M events, ~%.2f GB", it, ITERS, e / 1e6, e * BYTES_PER_EVENT / 1e9);
        }
    }
    collective(-1, R_MPI_FINALIZE, OTF2_COLLECTIVE_OP_DESTROY_HANDLE, C_WORLD, world, P, OTF2_UNDEFINED_UINT32, 0, 30000);
    for (int r = 0; r < P; ++r) {
        t[r] += 200; leave(r, R_MAIN);
        OTF2_EvtWriter_ProgramEnd(ew[r], NULL, ts(r), 0); nev[r]++;
    }

    uint64_t total = 0;
    for (int r = 0; r < P; ++r) { total += nev[r]; OTF2_Archive_CloseEvtWriter(ar, ew[r]); }
    OTF2_Archive_CloseEvtFiles(ar);

    /* empty local definitions (present in Score-P traces; the reader opens them optionally) */
    OTF2_Archive_OpenDefFiles(ar);
    for (int r = 0; r < P; ++r) OTF2_Archive_CloseDefWriter(ar, OTF2_Archive_GetDefWriter(ar, r));
    OTF2_Archive_CloseDefFiles(ar);

    /* ---- global definitions ---- */
    OTF2_GlobalDefWriter *g = OTF2_Archive_GetGlobalDefWriter(ar);
    OTF2_GlobalDefWriter_WriteClockProperties(g, 1000000000ull, T0, tmax - T0 + 1, 1764000000000000000ull);
    uint32_t sid = 0;
    const char *fixed[] = { "", "synthetic-cluster", "machine", "node", "Master thread", "MPI",
                            "/opt/edumpi/stencil2d", "MPI_COMM_WORLD", "MPI_COMM_SELF",
                            "node_comm", "row_comm", "/opt/edumpi/stencil2d.c" };
    for (; sid < S_FIRST_REGION; ++sid) OTF2_GlobalDefWriter_WriteString(g, sid, fixed[sid]);
    for (int i = 0; i < R_COUNT; ++i) OTF2_GlobalDefWriter_WriteString(g, S_FIRST_REGION + i, rname[i]);
    sid = S_FIRST_REGION + R_COUNT;
    OTF2_GlobalDefWriter_WriteParadigm(g, OTF2_PARADIGM_MPI, S_MPI, OTF2_PARADIGM_CLASS_PROCESS);
    for (int i = 0; i < R_COUNT; ++i)
        OTF2_GlobalDefWriter_WriteRegion(g, i, S_FIRST_REGION + i, S_FIRST_REGION + i, S_EMPTY,
            IS_MPI(i) ? OTF2_REGION_ROLE_FUNCTION : OTF2_REGION_ROLE_FUNCTION,
            IS_MPI(i) ? OTF2_PARADIGM_MPI : OTF2_PARADIGM_COMPILER, OTF2_REGION_FLAG_NONE,
            IS_MPI(i) ? S_EMPTY : S_FILE, IS_MPI(i) ? 0 : 10 + 20 * i, IS_MPI(i) ? 0 : 25 + 20 * i);
    OTF2_GlobalDefWriter_WriteSystemTreeNode(g, 0, S_MACHINE_NAME, S_MACHINE, OTF2_UNDEFINED_SYSTEM_TREE_NODE);
    char buf[64];
    for (int n = 0; n < NODES; ++n) {
        snprintf(buf, sizeof buf, "node%03d", n + 1);
        OTF2_GlobalDefWriter_WriteString(g, sid, buf);
        OTF2_GlobalDefWriter_WriteSystemTreeNode(g, 1 + n, sid++, S_NODE, 0);
    }
    for (int r = 0; r < P; ++r) {
        snprintf(buf, sizeof buf, "MPI Rank %d", r);
        OTF2_GlobalDefWriter_WriteString(g, sid, buf);
        OTF2_GlobalDefWriter_WriteLocationGroup(g, r, sid++, OTF2_LOCATION_GROUP_TYPE_PROCESS, 1 + node_of(r),
                                                OTF2_UNDEFINED_LOCATION_GROUP);
    }
    for (int r = 0; r < P; ++r)
        OTF2_GlobalDefWriter_WriteLocation(g, r, S_MASTER, OTF2_LOCATION_TYPE_CPU_THREAD, nev[r], r);

    uint64_t *m64 = malloc(P * sizeof(uint64_t));
    for (int r = 0; r < P; ++r) m64[r] = r;
    OTF2_GlobalDefWriter_WriteGroup(g, 0, S_EMPTY, OTF2_GROUP_TYPE_COMM_LOCATIONS, OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, P, m64);
    OTF2_GlobalDefWriter_WriteGroup(g, 1, S_EMPTY, OTF2_GROUP_TYPE_COMM_SELF, OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, 0, NULL);
    OTF2_GlobalDefWriter_WriteGroup(g, 2, S_EMPTY, OTF2_GROUP_TYPE_COMM_GROUP, OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, P, m64);
    OTF2_GlobalDefWriter_WriteComm(g, C_WORLD, S_WORLD, 2, OTF2_UNDEFINED_COMM, OTF2_COMM_FLAG_NONE);
    OTF2_GlobalDefWriter_WriteComm(g, C_SELF, S_SELF, 1, OTF2_UNDEFINED_COMM, OTF2_COMM_FLAG_NONE);
    uint32_t gid = 3;
    for (int n = 0; n < NODES; ++n) {
        int cnt = 0;
        for (int r = n * RPN; r < P && r < (n + 1) * RPN; ++r) m64[cnt++] = r;
        snprintf(buf, sizeof buf, "node_comm %d", n);
        OTF2_GlobalDefWriter_WriteString(g, sid, buf);
        OTF2_GlobalDefWriter_WriteGroup(g, gid, S_EMPTY, OTF2_GROUP_TYPE_COMM_GROUP, OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, cnt, m64);
        OTF2_GlobalDefWriter_WriteComm(g, C_NODE0 + n, sid++, gid++, C_WORLD, OTF2_COMM_FLAG_NONE);
    }
    for (int y = 0; y < PY; ++y) {
        for (int x = 0; x < PX; ++x) m64[x] = rankAt[y * PX + x];
        snprintf(buf, sizeof buf, "row_comm %d", y);
        OTF2_GlobalDefWriter_WriteString(g, sid, buf);
        OTF2_GlobalDefWriter_WriteGroup(g, gid, S_EMPTY, OTF2_GROUP_TYPE_COMM_GROUP, OTF2_PARADIGM_MPI, OTF2_GROUP_FLAG_NONE, PX, m64);
        OTF2_GlobalDefWriter_WriteComm(g, rowComm0 + y, sid++, gid++, C_WORLD, OTF2_COMM_FLAG_NONE);
    }
    OTF2_Archive_CloseGlobalDefWriter(ar, g);
    OTF2_Archive_Close(ar);
    fprintf(stderr, "\ndone: %d ranks, %ld iterations, %.1f M events, run time %.2f s\n",
            P, ITERS, total / 1e6, (tmax - T0) / 1e9);
    return 0;
}

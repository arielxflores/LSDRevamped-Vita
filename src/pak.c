/* Game data access: dreams.pak (archive made by tools/mkpak.py), moviesN.pak (the videos, stored
 * uncompressed, sent to the Vita separately because they rarely change) or, on the PC test build,
 * plain files in the data directory.
 *
 * Version 2 packs keep each compressed entry as independent LZ4 blocks of 256 KB (unpacked), with a
 * CRC of the packed bytes. A reader thread pulls the entry off the memory card while the game thread
 * unpacks (and, for dream.bin, sets up) the blocks that have arrived: reading, unpacking and checking
 * overlap instead of running one after the other. */
#include <zlib.h>
#include "common.h"
#include "lz4.h"
#ifdef __vita__
#include <psp2/io/fcntl.h>
#endif

#define PAK_STORED 0x80000000u /* packed size flags: the entry is not compressed, */
#define PAK_LZ4 0x40000000u    /* or is LZ4 (v1: one block; v2: blocks with a table), otherwise zlib (v1) */
#define PAK_FLAGS (PAK_STORED | PAK_LZ4)

#define READ_PIECE (256 * 1024) /* the reader thread's read size */

typedef struct {
    char name[48];
    uint32_t off, csize, size, crc;
} PakEntry;

typedef struct {
    FILE *f;
    PakEntry *ent;
    int nent;
    int version;
    char path[256];
#ifdef __vita__
    SceUID fd; /* entries are read straight from the file system (sceIoPread) */
#endif
} Pak;

static int g_check_crc = 1;
void vfs_check_crc(int on) { g_check_crc = on; }

#define MAX_PAKS 40 /* dreams 3, movies 6, intro 4, music 5 now */
static Pak g_paks[MAX_PAKS];
static int g_npak;
static char g_dir[256];

static int pak_open(Pak *p, const char *pak_path)
{
    memset(p, 0, sizeof(*p));
    FILE *f = fopen(pak_path, "rb");
    if (!f) {
        lsd_log("data pack %s not found", pak_path);
        return 0;
    }
    uint32_t hdr[4];
    if (fread(hdr, 4, 4, f) != 4 || memcmp(hdr, "LSDP", 4) != 0 || (hdr[1] != 1 && hdr[1] != 2) || hdr[2] == 0 ||
        hdr[2] > 4096) {
        lsd_log("data pack %s: bad header (version %u; this build reads 1 and 2)", pak_path, (unsigned)hdr[1]);
        fclose(f);
        return 0;
    }
    p->version = (int)hdr[1];
    p->nent = (int)hdr[2];
    p->ent = (PakEntry *)calloc((size_t)p->nent, sizeof(PakEntry));
    if (!p->ent || fread(p->ent, sizeof(PakEntry), (size_t)p->nent, f) != (size_t)p->nent) {
        lsd_log("data pack %s: truncated table", pak_path);
        fclose(f);
        free(p->ent);
        memset(p, 0, sizeof(*p));
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    for (int i = 0; i < p->nent; i++) {
        p->ent[i].name[47] = 0;
        uint32_t cs = p->ent[i].csize & ~PAK_FLAGS;
        if ((p->ent[i].csize & PAK_STORED) && cs != p->ent[i].size)
            fsize = -1;
        if ((long)p->ent[i].off + (long)cs > fsize) {
            lsd_log("data pack %s is incomplete (%ld bytes; entry %s needs %u). Upload it again.", pak_path, fsize,
                    p->ent[i].name, (unsigned)(p->ent[i].off + cs));
            fclose(f);
            free(p->ent);
            memset(p, 0, sizeof(*p));
            return 0;
        }
    }
    p->f = f;
#ifdef __vita__
    p->fd = sceIoOpen(pak_path, SCE_O_RDONLY, 0);
    if (p->fd >= 0) { /* (reads go through fd: one open file per pack is enough) */
        fclose(f);
        p->f = NULL;
    }
#endif
    snprintf(p->path, sizeof(p->path), "%s", pak_path);
    lsd_log("data pack %s: %d files, %ld bytes (v%d)", pak_path, p->nent, fsize, p->version);
    return 1;
}

int vfs_init(const char *pak_path, const char *dir)
{
    snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : "");
    if (!pak_path)
        return 1;
    if (!pak_open(&g_paks[0], pak_path))
        return 0;
    g_npak = 1;
    return 1;
}

int vfs_add_pak(const char *pak_path)
{
    if (!g_npak || g_npak >= MAX_PAKS) /* (plain files: the movies are in the data directory too) */
        return 0;
    if (!pak_open(&g_paks[g_npak], pak_path))
        return 0;
    g_npak++;
    return 1;
}

int vfs_add_pak_quiet(const char *pak_path)
{
    FILE *f = fopen(pak_path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return vfs_add_pak(pak_path);
}

static uint8_t *read_plain(const char *name, size_t *size, int quiet)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (!quiet) lsd_log("open failed: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        lsd_log("out of memory reading %s (%ld bytes)", path, n);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        lsd_log("short read on %s: %u of %ld", path, (unsigned)got, n);
        free(buf);
        return NULL;
    }
    *size = (size_t)n;
    return buf;
}

/* ------------------------------------------------------------------ streams */

enum { K_PLAIN, K_STORED, K_CHUNKED, K_LZ4_V1, K_ZLIB_V1 };

typedef struct {
    const Pak *p;
    uint32_t off, len;
    uint8_t *dst;
    volatile uint32_t got; /* bytes in dst so far (written by the reader) */
    volatile int err;
} ReadJob;

struct VfsStream {
    char name[48];
    const Pak *p;
    int kind;
    uint8_t *z;   /* packed bytes (as stored) */
    uint8_t *out; /* unpacked (== z when stored) */
    uint32_t cs, size;
    uint32_t avail; /* out[0, avail) is ready */
    /* chunked: u32 nchunk, u32 raw_chunk, u32 csize[nchunk], blocks */
    uint32_t nchunk, raw_chunk, next, zpos;
    const uint32_t *csz;
    uint32_t crc, crc_pos; /* crc32 of z[0, crc_pos) */
    uint32_t want_crc;
    ReadJob job;
    int task;
    int bad;
    double t_open, t_wait, t_unpack, t_check;
};

static uint32_t job_got(ReadJob *j) { return __atomic_load_n(&j->got, __ATOMIC_ACQUIRE); }

static void read_job(void *arg)
{
    ReadJob *j = (ReadJob *)arg;
    uint32_t pos = 0;
    while (pos < j->len) {
        uint32_t n = j->len - pos < READ_PIECE ? j->len - pos : READ_PIECE;
        int ok;
#ifdef __vita__
        if (j->p->fd >= 0)
            ok = sceIoPread(j->p->fd, j->dst + pos, n, (SceOff)(j->off + pos)) == (int)n;
        else
#endif
            ok = fseek(j->p->f, (long)(j->off + pos), SEEK_SET) == 0 && fread(j->dst + pos, 1, n, j->p->f) == n;
        if (!ok) {
            __atomic_store_n(&j->err, 1, __ATOMIC_RELEASE);
            return;
        }
        pos += n;
        __atomic_store_n(&j->got, pos, __ATOMIC_RELEASE);
    }
}

/* wait until the reader has n packed bytes in */
static int wait_packed(VfsStream *s, uint32_t n)
{
    if (job_got(&s->job) >= n)
        return 1;
    double t0 = plat_seconds();
    while (job_got(&s->job) < n) {
        if (__atomic_load_n(&s->job.err, __ATOMIC_ACQUIRE)) {
            lsd_log("read error in %s at %s", s->p->path, s->name);
            s->bad = 1;
            return 0;
        }
        plat_task_yield();
    }
    s->t_wait += plat_seconds() - t0;
    return 1;
}

static void check_upto(VfsStream *s, uint32_t n)
{
    if (n <= s->crc_pos)
        return;
    double t0 = plat_seconds();
    if (g_check_crc)
        s->crc = (uint32_t)crc32(s->crc, s->z + s->crc_pos, n - s->crc_pos);
    s->crc_pos = n;
    s->t_check += plat_seconds() - t0;
}

static VfsStream *stream_open(const Pak *p, const PakEntry *e, const char *name)
{
    VfsStream *s = (VfsStream *)calloc(1, sizeof(VfsStream));
    if (!s)
        return NULL;
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->p = p;
    s->cs = e->csize & ~PAK_FLAGS;
    s->size = e->size;
    s->want_crc = e->crc;
    s->t_open = plat_seconds();
    s->task = -1;
    if (e->csize & PAK_STORED)
        s->kind = K_STORED;
    else if (e->csize & PAK_LZ4)
        s->kind = p->version >= 2 ? K_CHUNKED : K_LZ4_V1;
    else
        s->kind = K_ZLIB_V1;
    s->z = (uint8_t *)malloc(s->cs ? s->cs : 1);
    s->out = s->kind == K_STORED ? s->z : (uint8_t *)malloc(s->size ? s->size : 1);
    if (!s->z || !s->out) {
        lsd_log("out of memory unpacking %s (%u bytes)", name, (unsigned)s->size);
        free(s->z);
        if (s->out != s->z) free(s->out);
        free(s);
        return NULL;
    }
    s->job.p = p;
    s->job.off = e->off;
    s->job.len = s->cs;
    s->job.dst = s->z;
    if (s->cs > 2 * READ_PIECE)
        s->task = plat_task_start(read_job, &s->job, 1); /* (big: read on the second core meanwhile) */
    else if (s->cs)
        read_job(&s->job); /* (small: a thread is not worth it) */
    if (s->kind == K_CHUNKED) {
        if (!wait_packed(s, 8))
            return s;
        memcpy(&s->nchunk, s->z, 4);
        memcpy(&s->raw_chunk, s->z + 4, 4);
        uint64_t hdr = 8 + 4ull * s->nchunk;
        if (s->raw_chunk == 0 || hdr > s->cs ||
            (uint64_t)s->nchunk != ((uint64_t)s->size + s->raw_chunk - 1) / s->raw_chunk) {
            lsd_log("%s in %s is damaged (block table). Upload the data pack again.", name, p->path);
            s->bad = 1;
            return s;
        }
        if (!wait_packed(s, (uint32_t)hdr))
            return s;
        s->csz = (const uint32_t *)(s->z + 8);
        s->zpos = (uint32_t)hdr;
    }
    return s;
}

const uint8_t *vfs_stream_need(VfsStream *s, size_t upto)
{
    if (s->bad || upto > s->size)
        return NULL;
    if (upto <= s->avail)
        return s->out;
    switch (s->kind) {
    case K_PLAIN:
        break;
    case K_STORED:
        if (!wait_packed(s, (uint32_t)upto))
            return NULL;
        check_upto(s, (uint32_t)upto);
        s->avail = (uint32_t)upto;
        break;
    case K_CHUNKED:
        while (s->avail < upto) {
            if (s->next >= s->nchunk) {
                s->bad = 1;
                return NULL;
            }
            uint32_t c = s->csz[s->next];
            uint32_t raw = s->size - s->avail < s->raw_chunk ? s->size - s->avail : s->raw_chunk;
            if (c > s->cs - s->zpos) {
                s->bad = 1;
                return NULL;
            }
            if (!wait_packed(s, s->zpos + c))
                return NULL;
            check_upto(s, s->zpos + c);
            double t0 = plat_seconds();
            int n = LZ4_decompress_safe((const char *)s->z + s->zpos, (char *)s->out + s->avail, (int)c, (int)raw);
            s->t_unpack += plat_seconds() - t0;
            if (n != (int)raw) {
                lsd_log("%s in %s is damaged (lz4 block %u: %d). Upload the data pack again.", s->name, s->p->path,
                        (unsigned)s->next, n);
                s->bad = 1;
                return NULL;
            }
            s->zpos += c;
            s->avail += raw;
            s->next++;
        }
        break;
    case K_LZ4_V1: {
        if (!wait_packed(s, s->cs))
            return NULL;
        double t0 = plat_seconds();
        int n = LZ4_decompress_safe((const char *)s->z, (char *)s->out, (int)s->cs, (int)s->size);
        s->t_unpack += plat_seconds() - t0;
        if (n != (int)s->size) {
            lsd_log("%s in %s is damaged (lz4 %d). Upload the data pack again.", s->name, s->p->path, n);
            s->bad = 1;
            return NULL;
        }
        s->avail = s->size;
        break;
    }
    case K_ZLIB_V1: {
        if (!wait_packed(s, s->cs))
            return NULL;
        uLongf n = s->size;
        double t0 = plat_seconds();
        int r = uncompress(s->out, &n, s->z, s->cs);
        s->t_unpack += plat_seconds() - t0;
        /* the zlib stream's own Adler-32 check catches damaged data */
        if (r != Z_OK || n != s->size) {
            lsd_log("%s in %s is damaged (zlib %d). Upload the data pack again.", s->name, s->p->path, r);
            s->bad = 1;
            return NULL;
        }
        s->avail = s->size;
        break;
    }
    }
    return s->out;
}

size_t vfs_stream_size(const VfsStream *s) { return s->size; }

int vfs_stream_check(VfsStream *s)
{
    int ok = vfs_stream_need(s, s->size) != NULL;
    if (s->task >= 0) {
        plat_task_join(s->task); /* (the reader is done, or stopped on an error) */
        s->task = -1;
    }
    if (ok && g_check_crc && s->kind != K_PLAIN && s->kind != K_ZLIB_V1) {
        /* stored and v2 entries: the CRC is of the packed bytes; v1 LZ4: of what came out */
        if (s->kind == K_LZ4_V1) {
            double t0 = plat_seconds();
            ok = (uint32_t)crc32(0L, s->out, s->size) == s->want_crc;
            s->t_check += plat_seconds() - t0;
        } else {
            check_upto(s, s->cs);
            ok = s->crc == s->want_crc;
        }
        if (!ok)
            lsd_log("%s in %s is damaged (checksum). Upload the data pack again.", s->name, s->p->path);
    }
    if (ok && s->size >= 2000000)
        lsd_log("read %s: %.1f MB packed, %.1f MB: %.0f ms (waiting for the card %.0f ms, unpacking %.0f ms, "
                "checking %.0f ms)",
                s->name, s->cs / 1e6, s->size / 1e6, (plat_seconds() - s->t_open) * 1000.0, s->t_wait * 1000.0,
                s->t_unpack * 1000.0, s->t_check * 1000.0);
    if (!ok)
        s->bad = 1;
    return ok;
}

uint8_t *vfs_stream_take(VfsStream *s, size_t *size)
{
    if (s->task >= 0)
        plat_task_join(s->task);
    uint8_t *out = s->out;
    if (s->out != s->z)
        free(s->z);
    if (size)
        *size = s->size;
    free(s);
    return out;
}

uint8_t *vfs_stream_finish(VfsStream *s, size_t *size)
{
    if (!s)
        return NULL;
    int ok = vfs_stream_check(s);
    uint8_t *out = vfs_stream_take(s, size);
    if (!ok) {
        free(out);
        return NULL;
    }
    return out;
}

void vfs_stream_abort(VfsStream *s)
{
    if (!s)
        return;
    s->bad = 1;
    if (s->task >= 0)
        plat_task_join(s->task);
    if (s->out != s->z)
        free(s->z);
    free(s->out);
    free(s);
}

static VfsStream *open_q(const char *name, int quiet)
{
    if (!g_npak) {
        size_t n = 0;
        double t0 = plat_seconds();
        uint8_t *b = read_plain(name, &n, quiet);
        if (!b)
            return NULL;
        VfsStream *s = (VfsStream *)calloc(1, sizeof(VfsStream));
        if (!s) {
            free(b);
            return NULL;
        }
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->kind = K_PLAIN;
        s->t_open = t0;
        s->z = s->out = b;
        s->cs = s->size = s->avail = (uint32_t)n;
        s->task = -1;
        return s;
    }
    for (int k = 0; k < g_npak; k++)
        for (int i = 0; i < g_paks[k].nent; i++)
            if (strcmp(g_paks[k].ent[i].name, name) == 0)
                return stream_open(&g_paks[k], &g_paks[k].ent[i], name);
    if (!quiet) lsd_log("%s is not in %s", name, g_paks[0].path);
    return NULL;
}

VfsStream *vfs_stream_open(const char *name) { return open_q(name, 0); }

uint8_t *vfs_read(const char *name, size_t *size)
{
    double t0 = plat_seconds();
    uint8_t *r = vfs_stream_finish(open_q(name, 0), size);
    prof_add(PROF_IO, plat_seconds() - t0);
    return r;
}

uint8_t *vfs_read_bg(const char *name, size_t *size) { return vfs_stream_finish(open_q(name, 0), size); }

uint8_t *vfs_read_quiet(const char *name, size_t *size)
{
    double t0 = plat_seconds();
    uint8_t *r = vfs_stream_finish(open_q(name, 1), size);
    prof_add(PROF_IO, plat_seconds() - t0);
    return r;
}

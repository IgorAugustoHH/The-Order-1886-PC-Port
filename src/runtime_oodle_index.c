/* BB_OODLE_INDEX=1 (research, off by default): after BB_OODLE_INDEX_DELAY seconds (default 60,
 * the game at its menu), walk /app0/_data, decompress every COMPRESS file in memory with the
 * game's own Oodle stream decoder, as its file streamer does (eboot 01.02 +0x804970: decoder state
 * in caller memory, a 0x20000 circular window; the prologues are checked first), and list
 * the OrbShdr shader footers found. Only metadata is written (hash, binary type, GCN length,
 * file and logical offset), to BB_OODLE_INDEX_FILE (default oodle_index.jsonl); no game bytes. */
#include "runtime.h"
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* +0xe804d0: decoder state in `memory` (type 0, as the streamer). +0xe80640: decode from comp into
   the circular window at window_pos; result[0] = bytes decoded, result[1] = bytes consumed
   (16 bytes written).
   Guest longs are 64-bit (host long is 32-bit on Windows). */
typedef void *(ABI *DecoderCreate)(int type,int64_t raw_size,void *memory);
typedef int (ABI *DecoderDecode)(void *decoder,void *window,int64_t window_pos,int64_t window_size,
                                 int64_t window_avail,const void *comp,int64_t comp_avail,int64_t a,int64_t b,
                                 int32_t result[4]);
/* +0xe80600: frees the scratch buffer the decoder allocates from the game's Oodle heap. */
typedef void (ABI *DecoderFree)(void *decoder);
/* +0xe81790: OodleLZ_Decompress, which the game uses for flat COMPRESS files (+0xc77d80): it reads
   the compressor from the stream, where the streamer above assumes type 0. Returns raw bytes. */
typedef int64_t (ABI *OodleDecompress)(const void *comp,int64_t comp_size,void *raw,int64_t raw_size,int64_t fuzz,
                                       int64_t crc,int64_t verbosity,int64_t dec_base,int64_t dec_size,void *callback);

enum { CREATE_OFFSET=0xe804d0, DECODE_OFFSET=0xe80640, FREE_OFFSET=0xe80600, WHOLE_OFFSET=0xe81790,
       ALLOCATOR_OFFSET=0x1679d90,
       BLOCK=0x40000, WINDOW=0x20000, STATE=0x2000, MAX_FLAT=512u<<20 };
static const unsigned char create_prologue[16]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
                                                0x41,0x55,0x41,0x54,0x53,0x50,0x48,0x89};
static const unsigned char free_prologue[16]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,
                                              0xfb,0x48,0x8b,0x7b,0x30,0x48,0x85,0xff};
static const unsigned char decode_prologue[16]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
                                                0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec};

static unsigned char *image;
static char root[1024];
static FILE *out;
static DecoderCreate create;
static DecoderDecode decode_step;
static DecoderFree release;
static OodleDecompress whole;
static unsigned long long opened, unresolved;
static int area_mode; /* package index for the area prefetch: no file */

static uint64_t package_id(const char *rel) {
    const char *name=strrchr(rel,'/');
    name=name ? name+1 : rel;
    char *end=NULL;
    const uint64_t id=strtoull(name,&end,16);
    return end && !*end ? id : 0;
}
static unsigned char *window, *state;

/* raw_size bytes from comp_size into dst, as +0x804970 does; 0 when the decoder stalls. */
static int decode_into(void *decoder,const unsigned char *comp,uint64_t comp_size,unsigned char *dst,uint64_t raw_size) {
    static int shown;
    if (!shown++) { fprintf(stderr,"Oodle index: state %p, decoder %p\n",(void *)state,decoder); fflush(stderr); }
    uint64_t done=0, used=0, pos=0;
    while (done<raw_size) {
        int32_t result[4]={0,0,0,0}; /* the decoder clears and writes 16 bytes */
        if (!decode_step(decoder,window,(int64_t)pos,WINDOW,(int64_t)(WINDOW-pos),comp+used,
                         (int64_t)(comp_size-used),0,0,result))
            return 0;
        const uint64_t got=(uint32_t)result[0], ate=(uint32_t)result[1];
        if (!got && !ate) return 0;
        if (got>raw_size-done || ate>comp_size-used) return 0;
        memcpy(dst+done,window+pos,got);
        done+=got; used+=ate;
        pos=pos+got==WINDOW ? 0 : pos+got;
    }
    return 1;
}

static int decode(const unsigned char *comp,uint64_t comp_size,unsigned char *dst,uint64_t raw_size) {
    void *decoder=create(0,(int64_t)raw_size,state);
    const int ok=decode_into(decoder,comp,comp_size,dst,raw_size);
    release(decoder);
    return ok;
}
static unsigned long long files, blocks, failures, shaders, skipped;

static uint64_t rd64(const unsigned char *p) { uint64_t v; memcpy(&v,p,8); return v; }
static uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }

/* BB_OODLE_INDEX_TRACE=1: the block about to be decoded, flushed, to find one that faults. */
static void trace(const char *rel,uint64_t logical,uint64_t csize,uint64_t raw) {
    static int on=-1;
    if (on<0) { const char *e=getenv("BB_OODLE_INDEX_TRACE"); on=e && e[0]=='1'; }
    if (!on || !out) return;
    fprintf(out,"{\"decode\":\"%s\",\"logical\":%llu,\"compressed\":%llu,\"raw\":%llu}\n",rel,
            (unsigned long long)logical,(unsigned long long)csize,(unsigned long long)raw);
    fflush(out);
}

/* The program that ends at the footer at data[i]. The packages keep no Shdr header, so: GCN code
   ends with s_endpgm (0xbf810000) at code + length - 4, and the tables between the code and the
   footer are small (at most 500 bytes in the 278 .sb files, where this rule finds every program).
   Returns the code offset in data, or -1. */
static long program_start(const unsigned char *data,size_t i,uint32_t length) {
    static const unsigned char endpgm[4]={0x00,0x00,0x81,0xbf};
    const size_t floor=i>4096 ? i-4096 : 0;
    for (size_t e=i;e>=floor+4;--e) {
        if (memcmp(data+e-4,endpgm,4)) continue;
        return e>=length ? (long)(e-length) : -1;
    }
    return -1;
}

/* FNV-1a 64 of the whole program, code through footer: equal digests mean equal programs. */
static uint64_t digest(const unsigned char *p,size_t n) {
    uint64_t v=0xcbf29ce484222325ull;
    for (size_t k=0;k<n;++k) v=(v^p[k])*0x100000001b3ull;
    return v;
}

/* Footers in data[from..size); `base` is data's logical offset in the file. A footer that starts
   in the last 32 bytes is left for the next window (the caller keeps that overlap). */
static void scan(const char *rel,const unsigned char *data,size_t size,uint64_t base,size_t from) {
    for (size_t i=from;i+32<=size;++i) {
        if (data[i]!='O' || memcmp(data+i,"OrbShdr",7)) continue;
        const uint32_t info=rd32(data+i+8);
        const uint64_t hash=rd64(data+i+0x10);
        const unsigned type=(info>>2)&0xf, length=info>>8;
        if (!length || type>9) continue;
        if (area_mode) {
            const uint64_t package=package_id(rel);
            if (package) bb_area_package_shader(package,hash);
            ++shaders;
            continue;
        }
        const long start=program_start(data,i,length);
        fprintf(out,"{\"file\":\"%s\",\"offset\":%llu,\"version\":%u,\"type\":%u,\"length\":%u,"
                "\"hash\":\"%016llx\",\"crc\":\"%08x\"",rel,(unsigned long long)(base+i),data[i+7],
                type,length,(unsigned long long)hash,rd32(data+i+0x18));
        if (start>=0) {
            const uint64_t sum=digest(data+start,i+28-(size_t)start);
            fprintf(out,",\"code\":%llu,\"size\":%llu,\"digest\":\"%016llx\"}\n",
                    (unsigned long long)(base+(uint64_t)start),(unsigned long long)(i+28-(size_t)start),
                    (unsigned long long)sum);
        } else { fprintf(out,",\"code\":null}\n"); ++unresolved; }
        ++shaders;
    }
}

static void index_file(const char *path,const char *rel) {
    if (++opened%1000==0 && out) { fprintf(out,"{\"opened\":%llu}\n",opened); fflush(out); }
    FILE *f=fopen(path,"rb");
    if (!f) return;
    unsigned char head[24];
    if (fread(head,1,24,f)!=24 || memcmp(head,"COMPRESS",8)) { fclose(f); return; }
    ++files;
    const uint64_t total=rd64(head+8), field=rd64(head+16);
    struct stat st;
    const int blocked=!stat(path,&st) && (uint64_t)st.st_size==field;
    if (!blocked) {
        /* Flat: one payload of `field` bytes right after the header. */
        if (total>MAX_FLAT || field>MAX_FLAT) { ++skipped; if (out) fprintf(out,"{\"file\":\"%s\",\"skipped\":%llu}\n",rel,(unsigned long long)total); fclose(f); return; }
        unsigned char *comp=malloc(field), *raw=malloc(total+16);
        if (comp && raw && fread(comp,1,field,f)==field) {
            ++blocks;
            trace(rel,0,field,total);
            if (whole(comp,(int64_t)field,raw,(int64_t)total,0,0,0,0,0,NULL)!=(int64_t)total) {
                ++failures; if (out) fprintf(out,"{\"file\":\"%s\",\"failed\":0}\n",rel);
            } else scan(rel,raw,total,0,0);
        }
        free(comp); free(raw); fclose(f); return;
    }
    /* Blocks: {u64 logical offset, u64 compressed size} + payload, 0x40000 logical bytes each.
       The window keeps the whole previous block, for programs that cross a boundary. */
    unsigned char *comp=malloc(BLOCK*2), *data=malloc(2*BLOCK);
    size_t carried=0;
    uint64_t carried_base=0;
    unsigned char block_head[16];
    while (comp && data && fread(block_head,1,16,f)==16) {
        const uint64_t logical=rd64(block_head), csize=rd64(block_head+8);
        if (logical>=total || csize>BLOCK*2 || fread(comp,1,csize,f)!=csize) { ++failures; break; }
        const uint64_t raw_size=total-logical<BLOCK ? total-logical : BLOCK;
        ++blocks;
        trace(rel,logical,csize,raw_size);
        if (!decode(comp,csize,data+carried,raw_size)) {
            ++failures; if (out) fprintf(out,"{\"file\":\"%s\",\"failed\":%llu}\n",rel,(unsigned long long)logical);
            carried=0; continue;
        }
        const uint64_t base=carried ? carried_base : logical;
        scan(rel,data,carried+raw_size,base,carried>=31 ? carried-31 : 0);
        const size_t keep=raw_size;
        memmove(data,data+carried+raw_size-keep,keep);
        carried=keep; carried_base=logical+raw_size-keep;
    }
    free(comp); free(data); fclose(f);
}

static void walk(const char *dir,const char *rel) {
    DIR *d=opendir(dir);
    if (!d) return;
    for (struct dirent *e;(e=readdir(d));) {
        if (e->d_name[0]=='.') continue;
        char path[2048], sub[1024];
        snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);
        snprintf(sub,sizeof(sub),"%s%s%s",rel,*rel ? "/" : "",e->d_name);
        struct stat st;
        if (stat(path,&st)) continue;
        if (S_ISDIR(st.st_mode)) walk(path,sub); else index_file(path,sub);
    }
    closedir(d);
}

static void *run(void *arg) {
    (void)arg;
    const char *e=getenv("BB_OODLE_INDEX_DELAY");
    sleep(e ? (unsigned)atoi(e) : 60);
    runtime_thread_attach_host("oodle-index");
    if (memcmp(image+CREATE_OFFSET,create_prologue,16) || memcmp(image+DECODE_OFFSET,decode_prologue,16) ||
        memcmp(image+FREE_OFFSET,free_prologue,16) || memcmp(image+WHOLE_OFFSET,decode_prologue,16)) {
        fprintf(stderr,"Oodle index: the decoder does not match eboot 01.02; nothing done\n");
        return NULL;
    }
    create=(DecoderCreate)(image+CREATE_OFFSET);
    decode_step=(DecoderDecode)(image+DECODE_OFFSET);
    release=(DecoderFree)(image+FREE_OFFSET);
    whole=(OodleDecompress)(image+WHOLE_OFFSET);
    window=malloc(WINDOW);
    state=malloc(STATE+16); /* the decoder aligns it to 16 */
    if (!window || !state) return NULL;
    uint64_t allocator;
    memcpy(&allocator,image+ALLOCATOR_OFFSET,8);
    fprintf(stderr,"Oodle index: Oodle malloc pointer %s (0x%llx)\n",allocator ? "set" : "NOT set (Oodle_Init not called)",
            (unsigned long long)allocator);
    const char *name=getenv("BB_OODLE_INDEX_FILE");
    out=fopen(name ? name : "oodle_index.jsonl","w");
    if (!out) { fprintf(stderr,"Oodle index: cannot write the index\n"); return NULL; }
    fprintf(stderr,"Oodle index: started on %s/_data\n",root);
    fflush(stderr);
    const uint64_t start=runtime_wait_begin();
    char data[1100];
    const char *sub=getenv("BB_OODLE_INDEX_DIR");   /* default: the folder that holds every shader */
    if (!sub) sub="1408209885/ps4/GPU";
    snprintf(data,sizeof(data),"%s/_data/%s",root,sub);
    char rel[600];
    snprintf(rel,sizeof(rel),"_data/%s",sub);
    walk(data,rel);
    fprintf(out,"{\"done\":%llu,\"shaders\":%llu,\"failures\":%llu,\"unresolved\":%llu}\n",files,shaders,
            failures,unresolved);
    fclose(out);
    fprintf(stderr,"Oodle index: done in %.1f s, %llu files, %llu blocks, %llu failures, %llu skipped, "
            "%llu shader footers\n",(runtime_wait_begin()-start)/1e9,files,blocks,failures,skipped,shaders);
    return NULL;
}

/* The package index the area prefetch needs, built once in the background when the GPU side
   has none: waits for the pipeline cache, then for the game to settle, at low priority. */
static void *run_area(void *arg) {
    (void)arg;
    int needed;
    while ((needed=bb_area_index_needed())<0) sleep(1);
    if (!needed) return NULL;
    sleep(20);
    runtime_thread_attach_host("area-index");
    if (memcmp(image+CREATE_OFFSET,create_prologue,16) || memcmp(image+DECODE_OFFSET,decode_prologue,16) ||
        memcmp(image+FREE_OFFSET,free_prologue,16)) {
        fprintf(stderr,"Area index: the decoder does not match eboot 01.02; no area prefetch\n");
        return NULL;
    }
    create=(DecoderCreate)(image+CREATE_OFFSET);
    decode_step=(DecoderDecode)(image+DECODE_OFFSET);
    release=(DecoderFree)(image+FREE_OFFSET);
    whole=(OodleDecompress)(image+WHOLE_OFFSET);
    window=malloc(WINDOW);
    state=malloc(STATE+16);
    if (!window || !state) return NULL;
    area_mode=1;
    const uint64_t start=runtime_wait_begin();
    char data[1100];
    snprintf(data,sizeof(data),"%s/_data/1408209885/ps4/GPU",root);
    walk(data,"_data/1408209885/ps4/GPU");
    bb_area_index_done();
    fprintf(stderr,"Area index: %llu packages, %llu shader footers, %llu failures in %.1f s\n",files,shaders,
            failures,(runtime_wait_begin()-start)/1e9);
    return NULL;
}

void runtime_oodle_index_start(unsigned char *loaded_image,const char *app0) {
    const char *e=getenv("BB_OODLE_INDEX");
    const char *area=getenv("BB_AREA_PREFETCH");
    if ((!e || e[0]!='1') && app0 && !(area && area[0]=='0')) {
        image=loaded_image;
        snprintf(root,sizeof(root),"%s",app0);
        pthread_t thread;
        if (!pthread_create(&thread,NULL,run_area,NULL)) pthread_detach(thread);
        return;
    }
    if (!e || e[0]!='1' || !app0) return;
    image=loaded_image;
    snprintf(root,sizeof(root),"%s",app0);
    pthread_t thread;
    if (!pthread_create(&thread,NULL,run,NULL)) pthread_detach(thread);
}

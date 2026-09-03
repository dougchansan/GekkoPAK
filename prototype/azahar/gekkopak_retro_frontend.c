#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

typedef bool (*retro_environment_t)(unsigned, void*);
typedef void (*retro_video_refresh_t)(const void*, unsigned, unsigned, size_t);
typedef void (*retro_audio_sample_t)(int16_t, int16_t);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t*, size_t);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned, unsigned, unsigned, unsigned);

struct retro_system_info { const char *library_name,*library_version,*valid_extensions; bool need_fullpath,block_extract; };
struct retro_game_geometry { unsigned base_width,base_height,max_width,max_height; float aspect_ratio; };
struct retro_system_timing { double fps,sample_rate; };
struct retro_system_av_info { struct retro_game_geometry geometry; struct retro_system_timing timing; };
struct retro_game_info { const char *path; const void *data; size_t size; const char *meta; };
struct retro_variable { const char *key,*value; };
struct retro_message { const char *msg; unsigned frames; };
enum retro_log_level { RETRO_LOG_DEBUG=0,RETRO_LOG_INFO,RETRO_LOG_WARN,RETRO_LOG_ERROR,RETRO_LOG_DUMMY=INT32_MAX };
typedef void (*retro_log_printf_t)(enum retro_log_level,const char*,...);
struct retro_log_callback { retro_log_printf_t log; };

struct retro_memory_descriptor {
    uint64_t flags;
    void *ptr;
    size_t offset,start,select,disconnect,len;
    const char *addrspace;
};
struct retro_memory_map { const struct retro_memory_descriptor *descriptors; unsigned num_descriptors; };

static const char *g_system_dir="./azahar_system";
static const char *g_save_dir="./azahar_saves";
static const char *g_assets_dir="./azahar_assets";
static const char *g_core_path="./azahar_libretro.so";
static const char *g_trace_path="./gekkopak_guest_trace.jsonl";

#define MAX_DESCS 64
static struct retro_memory_descriptor g_descs[MAX_DESCS];
static unsigned g_num_descs=0;
static FILE *g_trace=NULL;

#define GK_MAGIC 0x4B504B47u
#define GK_VERSION 0x00010000u
#define GK_MAILBOX 0x0FFFC000u
#define GK_PAYLOAD_OFF 0x40u
#define GK_SUMMARY_OFF 0x100u
#define GK_STATE_REQUEST 1u
#define GK_STATE_RESPONSE 2u
#define GK_OK 0u
#define GK_ERR_BAD_COMMAND 1u
#define GK_ERR_BAD_HANDLE 2u
#define GK_ERR_NO_MEMORY 3u
#define GK_ERR_NOT_READY 4u

#define GK_CMD_HELLO 1u
#define GK_CMD_GET_CAPS 2u
#define GK_CMD_ALLOC 3u
#define GK_CMD_UPLOAD 4u
#define GK_CMD_SUBMIT 5u
#define GK_CMD_POLL 6u
#define GK_CMD_COLLECT 7u
#define GK_CMD_FREE 8u
#define GK_CMD_COMPLETE 9u

#define GK_CAP_LOCAL_MEMORY (1u<<0)
#define GK_CAP_DSP_AUDIO (1u<<1)
#define GK_CAP_PAIRED_SINGLE (1u<<2)
#define GK_CAP_TEXTURE (1u<<3)
#define GK_LOCAL_MEMORY_BYTES (32u*1024u*1024u)

struct gk_mailbox {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;
    uint32_t command;
    uint32_t state;
    uint32_t result;
    uint32_t arg0,arg1,arg2,arg3;
    uint32_t out0,out1,out2,out3;
    uint32_t payload_len;
    uint32_t reserved;
};

struct gk_alloc { uint32_t handle; uint8_t *data; uint32_t size; uint32_t uploaded; };
struct gk_job {
    uint32_t handle;
    uint32_t alloc_handle;
    uint32_t kernel;
    uint32_t operations;
    uint32_t software_us;
    uint32_t modeled_us;
    uint32_t speedup_x1000;
    uint32_t checksum;
    int polls_remaining;
    bool ready;
};
static struct gk_alloc g_alloc={0};
static struct gk_job g_job={0};
static uint32_t g_last_seq=0;
static uint32_t g_next_alloc_handle=1;
static uint32_t g_next_job_handle=1;
static bool g_guest_done=false;

static void core_log(enum retro_log_level level,const char *fmt,...) {
    if (level < RETRO_LOG_INFO) return;
    static const char *names[]={"DEBUG","INFO","WARN","ERROR"};
    fprintf(stderr,"[AZAHAR/%s] ",(level>=0&&level<=3)?names[level]:"LOG");
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}

static void copy_memory_map(const struct retro_memory_map *m) {
    g_num_descs = m->num_descriptors < MAX_DESCS ? m->num_descriptors : MAX_DESCS;
    for (unsigned i=0;i<g_num_descs;i++) g_descs[i]=m->descriptors[i];
    fprintf(stderr,"[GekkoPAK/bridge] captured %u guest memory descriptors\n",g_num_descs);
    for (unsigned i=0;i<g_num_descs;i++) {
        fprintf(stderr,"[GekkoPAK/bridge] map[%u] start=0x%08zx len=0x%zx ptr=%p flags=0x%llx\n",
            i,g_descs[i].start,g_descs[i].len,g_descs[i].ptr,(unsigned long long)g_descs[i].flags);
    }
}

static void *guest_ptr(uint32_t addr,size_t size) {
    for (unsigned i=0;i<g_num_descs;i++) {
        const struct retro_memory_descriptor *d=&g_descs[i];
        if (!d->ptr || d->len==0) continue;
        uint64_t a=addr;
        if (a < d->start || a + size > d->start + d->len) continue;
        size_t translated=(size_t)((a & ~(uint64_t)d->disconnect)-d->start+d->offset);
        if (translated + size > d->offset + d->len) continue;
        return (uint8_t*)d->ptr + translated;
    }
    return NULL;
}

static uint32_t fnv1a(const uint8_t *p,size_t n) {
    uint32_t h=2166136261u;
    for (size_t i=0;i<n;i++) { h^=p[i]; h*=16777619u; }
    return h;
}

static const char *cmd_name(uint32_t cmd) {
    switch(cmd) {
        case GK_CMD_HELLO:return "HELLO";
        case GK_CMD_GET_CAPS:return "GET_CAPS";
        case GK_CMD_ALLOC:return "ALLOC";
        case GK_CMD_UPLOAD:return "UPLOAD";
        case GK_CMD_SUBMIT:return "SUBMIT";
        case GK_CMD_POLL:return "POLL";
        case GK_CMD_COLLECT:return "COLLECT";
        case GK_CMD_FREE:return "FREE";
        case GK_CMD_COMPLETE:return "COMPLETE";
        default:return "UNKNOWN";
    }
}

static uint32_t model_job_us(uint32_t tx_bytes,uint32_t rx_bytes,uint32_t ops) {
    double tx=((double)tx_bytes/(6.0*1024.0*1024.0))*1000000.0;
    double rx=((double)rx_bytes/(6.0*1024.0*1024.0))*1000000.0;
    double compute=((double)ops/100000000.0)*1000000.0;
    double total=25.0+tx+compute+rx;
    return (uint32_t)(total+0.5);
}

static void trace_cmd(const struct gk_mailbox *m) {
    if (!g_trace) return;
    fprintf(g_trace,
        "{\"seq\":%u,\"cmd\":\"%s\",\"arg\":[%u,%u,%u,%u],\"result\":%u,\"out\":[%u,%u,%u,%u],\"payload_len\":%u}\n",
        m->seq,cmd_name(m->command),m->arg0,m->arg1,m->arg2,m->arg3,m->result,
        m->out0,m->out1,m->out2,m->out3,m->payload_len);
    fflush(g_trace);
}

static void service_gekkopak(unsigned frame) {
    struct gk_mailbox *m=(struct gk_mailbox*)guest_ptr(GK_MAILBOX,sizeof(*m));
    if (!m || m->magic!=GK_MAGIC || m->state!=GK_STATE_REQUEST || m->seq==g_last_seq) return;
    g_last_seq=m->seq;
    m->result=GK_OK; m->out0=m->out1=m->out2=m->out3=0;

    fprintf(stderr,"[GekkoPAK/device] frame=%u seq=%u cmd=%s args=[%u,%u,%u,%u] payload=%u\n",
        frame,m->seq,cmd_name(m->command),m->arg0,m->arg1,m->arg2,m->arg3,m->payload_len);

    switch(m->command) {
        case GK_CMD_HELLO:
            m->out0=GK_VERSION;
            m->out1=GK_CAP_LOCAL_MEMORY|GK_CAP_DSP_AUDIO|GK_CAP_PAIRED_SINGLE|GK_CAP_TEXTURE;
            m->out2=GK_LOCAL_MEMORY_BYTES;
            m->out3=1;
            break;
        case GK_CMD_GET_CAPS:
            m->out0=GK_CAP_LOCAL_MEMORY|GK_CAP_DSP_AUDIO|GK_CAP_PAIRED_SINGLE|GK_CAP_TEXTURE;
            m->out1=GK_LOCAL_MEMORY_BYTES;
            m->out2=6*1024*1024;
            m->out3=25;
            break;
        case GK_CMD_ALLOC:
            if (m->arg0==0 || m->arg0>GK_LOCAL_MEMORY_BYTES || g_alloc.data) { m->result=GK_ERR_NO_MEMORY; break; }
            g_alloc.data=(uint8_t*)calloc(1,m->arg0);
            if (!g_alloc.data) { m->result=GK_ERR_NO_MEMORY; break; }
            g_alloc.handle=g_next_alloc_handle++; g_alloc.size=m->arg0; g_alloc.uploaded=0;
            m->out0=g_alloc.handle; m->out1=g_alloc.size;
            break;
        case GK_CMD_UPLOAD: {
            if (!g_alloc.data || m->arg0!=g_alloc.handle || m->arg1>g_alloc.size || m->payload_len>g_alloc.size-m->arg1) { m->result=GK_ERR_BAD_HANDLE; break; }
            uint8_t *payload=(uint8_t*)guest_ptr(GK_MAILBOX+GK_PAYLOAD_OFF,m->payload_len);
            if (!payload) { m->result=GK_ERR_BAD_HANDLE; break; }
            memcpy(g_alloc.data+m->arg1,payload,m->payload_len);
            if (m->arg1+m->payload_len>g_alloc.uploaded) g_alloc.uploaded=m->arg1+m->payload_len;
            m->out0=m->payload_len;
            m->out1=fnv1a(g_alloc.data,g_alloc.uploaded);
            break;
        }
        case GK_CMD_SUBMIT:
            if (!g_alloc.data || m->arg0!=g_alloc.handle || g_job.handle) { m->result=GK_ERR_BAD_HANDLE; break; }
            g_job.handle=g_next_job_handle++;
            g_job.alloc_handle=m->arg0;
            g_job.kernel=m->arg1;
            g_job.operations=m->arg2;
            g_job.software_us=m->arg3;
            g_job.checksum=fnv1a(g_alloc.data,g_alloc.uploaded);
            g_job.modeled_us=model_job_us(g_alloc.uploaded,16,g_job.operations);
            g_job.speedup_x1000=g_job.modeled_us?((uint64_t)g_job.software_us*1000u/g_job.modeled_us):0;
            g_job.polls_remaining=2;
            g_job.ready=false;
            m->out0=g_job.handle;
            m->out1=g_job.modeled_us;
            break;
        case GK_CMD_POLL:
            if (!g_job.handle || m->arg0!=g_job.handle) { m->result=GK_ERR_BAD_HANDLE; break; }
            if (!g_job.ready) {
                if (g_job.polls_remaining>0) g_job.polls_remaining--;
                if (g_job.polls_remaining==0) g_job.ready=true;
            }
            m->out0=g_job.ready?1u:0u;
            m->out1=(uint32_t)g_job.polls_remaining;
            break;
        case GK_CMD_COLLECT:
            if (!g_job.handle || m->arg0!=g_job.handle) { m->result=GK_ERR_BAD_HANDLE; break; }
            if (!g_job.ready) { m->result=GK_ERR_NOT_READY; break; }
            m->out0=g_job.modeled_us;
            m->out1=g_job.speedup_x1000;
            m->out2=g_job.checksum;
            m->out3=g_job.software_us;
            break;
        case GK_CMD_FREE:
            if (!g_alloc.data || m->arg0!=g_alloc.handle) { m->result=GK_ERR_BAD_HANDLE; break; }
            free(g_alloc.data); memset(&g_alloc,0,sizeof(g_alloc));
            break;
        case GK_CMD_COMPLETE:
            g_guest_done=true;
            m->out0=0x53534150u;
            break;
        default:
            m->result=GK_ERR_BAD_COMMAND;
            break;
    }

    trace_cmd(m);
    m->state=GK_STATE_RESPONSE;

    fprintf(stderr,"[GekkoPAK/device] -> result=%u out=[%u,%u,%u,%u]\n",
        m->result,m->out0,m->out1,m->out2,m->out3);

    if (m->command==GK_CMD_COMPLETE) {
        uint32_t *s=(uint32_t*)guest_ptr(GK_MAILBOX+GK_SUMMARY_OFF,9*sizeof(uint32_t));
        if (s) {
            fprintf(stderr,"\n=== GekkoPAK guest summary ===\n");
            fprintf(stderr,"status          : 0x%08x (%s)\n",s[0],s[0]==0x53534150u?"PASS":"FAIL");
            fprintf(stderr,"protocol        : %u.%u\n",s[1]>>16,s[1]&0xffffu);
            fprintf(stderr,"capabilities    : 0x%08x\n",s[2]);
            fprintf(stderr,"local RAM       : %u MiB\n",s[3]/(1024u*1024u));
            fprintf(stderr,"allocation      : handle %u\n",s[4]);
            fprintf(stderr,"job             : handle %u\n",s[5]);
            fprintf(stderr,"modeled offload : %u us\n",s[6]);
            fprintf(stderr,"speedup         : %u.%03ux\n",s[7]/1000u,s[7]%1000u);
            fprintf(stderr,"payload checksum: 0x%08x\n",s[8]);
            fprintf(stderr,"===============================\n\n");
        }
    }
}

static bool env_cb(unsigned cmd,void *data) {
    unsigned base=cmd&0xffffu;
    switch(base) {
        case 6: if(data){ const struct retro_message *m=data; fprintf(stderr,"[frontend/message] %s\n",m->msg?m->msg:"(null)"); } return true;
        case 9: if(data)*(const char**)data=g_system_dir; return true;
        case 10: return true;
        case 11: return true;
        case 15:
            if(data){ struct retro_variable *v=data; if(!v->key)return false;
                if(strcmp(v->key,"citra_graphics_api")==0)v->value="Software";
                else if(strcmp(v->key,"citra_is_new_3ds")==0)v->value="New 3DS";
                else if(strcmp(v->key,"citra_use_cpu_jit")==0)v->value="enabled";
                else if(strcmp(v->key,"citra_cpu_clock_percentage")==0)v->value="100";
                else return false;
                return true; }
            return false;
        case 19: if(data)*(const char**)data=g_core_path; return true;
        case 27: if(data)((struct retro_log_callback*)data)->log=core_log; return true;
        case 30: if(data)*(const char**)data=g_assets_dir; return true;
        case 31: if(data)*(const char**)data=g_save_dir; return true;
        case 36: if(data)copy_memory_map((const struct retro_memory_map*)data); return true;
        case 37:return true;
        case 52:return true;
        case 63:if(data)*(unsigned*)data=1;return true;
        default:return false;
    }
}
static void video_cb(const void *data,unsigned w,unsigned h,size_t pitch){(void)data;(void)pitch;static unsigned n=0;if(n++<2)fprintf(stderr,"[frontend] video %ux%u\n",w,h);}
static void audio_cb(int16_t l,int16_t r){(void)l;(void)r;}
static size_t audio_batch_cb(const int16_t *d,size_t frames){(void)d;return frames;}
static void input_poll_cb(void){}
static int16_t input_state_cb(unsigned p,unsigned d,unsigned i,unsigned id){(void)p;(void)d;(void)i;(void)id;return 0;}

#define LOADSYM(name) do{*(void**)(&name)=dlsym(h,#name);if(!name){fprintf(stderr,"missing %s: %s\n",#name,dlerror());return 2;}}while(0)

int main(int argc,char **argv) {
    const char *core=argc>1?argv[1]:g_core_path;
    const char *content=argc>2?argv[2]:NULL;
    const char *v;
    if ((v=getenv("GEKKOPAK_SYSTEM_DIR")) && *v) g_system_dir=v;
    if ((v=getenv("GEKKOPAK_SAVE_DIR")) && *v) g_save_dir=v;
    if ((v=getenv("GEKKOPAK_ASSETS_DIR")) && *v) g_assets_dir=v;
    if ((v=getenv("GEKKOPAK_TRACE")) && *v) g_trace_path=v;
    g_core_path=core;
    g_trace=fopen(g_trace_path,"w");
    if(!g_trace)perror("trace fopen");
    void *h=dlopen(core,RTLD_NOW|RTLD_LOCAL); if(!h){fprintf(stderr,"dlopen failed: %s\n",dlerror());return 2;}
    unsigned(*retro_api_version)(void)=NULL;void(*retro_set_environment)(retro_environment_t)=NULL;
    void(*retro_set_video_refresh)(retro_video_refresh_t)=NULL;void(*retro_set_audio_sample)(retro_audio_sample_t)=NULL;
    void(*retro_set_audio_sample_batch)(retro_audio_sample_batch_t)=NULL;void(*retro_set_input_poll)(retro_input_poll_t)=NULL;
    void(*retro_set_input_state)(retro_input_state_t)=NULL;void(*retro_init)(void)=NULL;void(*retro_deinit)(void)=NULL;
    void(*retro_get_system_info)(struct retro_system_info*)=NULL;void(*retro_get_system_av_info)(struct retro_system_av_info*)=NULL;
    bool(*retro_load_game)(const struct retro_game_info*)=NULL;void(*retro_unload_game)(void)=NULL;void(*retro_run)(void)=NULL;
    LOADSYM(retro_api_version);LOADSYM(retro_set_environment);LOADSYM(retro_set_video_refresh);LOADSYM(retro_set_audio_sample);
    LOADSYM(retro_set_audio_sample_batch);LOADSYM(retro_set_input_poll);LOADSYM(retro_set_input_state);LOADSYM(retro_init);
    LOADSYM(retro_deinit);LOADSYM(retro_get_system_info);LOADSYM(retro_get_system_av_info);LOADSYM(retro_load_game);
    LOADSYM(retro_unload_game);LOADSYM(retro_run);

    fprintf(stderr,"[frontend] libretro API=%u\n",retro_api_version());
    retro_set_environment(env_cb);retro_set_video_refresh(video_cb);retro_set_audio_sample(audio_cb);retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);retro_set_input_state(input_state_cb);retro_init();
    struct retro_system_info si={0};retro_get_system_info(&si);
    fprintf(stderr,"[frontend] core=%s version=%s\n",si.library_name?si.library_name:"?",si.library_version?si.library_version:"?");
    if(!content){fprintf(stderr,"content path required\n");return 2;}
    struct retro_game_info gi={content,NULL,0,NULL};
    bool ok=retro_load_game(&gi);fprintf(stderr,"[frontend] retro_load_game=%s\n",ok?"true":"false");
    if(ok){
        struct retro_system_av_info av={0};retro_get_system_av_info(&av);
        fprintf(stderr,"[frontend] New-3DS guest running: %ux%u @ %.3f Hz\n",av.geometry.base_width,av.geometry.base_height,av.timing.fps);
        for(unsigned f=0;f<90&&!g_guest_done;f++){retro_run();service_gekkopak(f);}
        if(!g_guest_done)fprintf(stderr,"[GekkoPAK/device] ERROR: guest transaction sequence did not complete\n");
        retro_unload_game();
    }
    retro_deinit();dlclose(h);if(g_trace)fclose(g_trace);if(g_alloc.data)free(g_alloc.data);
    fprintf(stderr,"[frontend] clean shutdown; guest_done=%s\n",g_guest_done?"true":"false");
    return g_guest_done?0:1;
}

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef bool (*retro_environment_t)(unsigned, void*);
typedef void (*retro_video_refresh_t)(const void*, unsigned, unsigned, size_t);
typedef void (*retro_audio_sample_t)(int16_t, int16_t);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t*, size_t);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned, unsigned, unsigned, unsigned);

struct retro_system_info {
    const char *library_name, *library_version, *valid_extensions;
    bool need_fullpath, block_extract;
};
struct retro_game_geometry {
    unsigned base_width, base_height, max_width, max_height;
    float aspect_ratio;
};
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info {
    struct retro_game_geometry geometry;
    struct retro_system_timing timing;
};
struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};
struct retro_variable { const char *key, *value; };
struct retro_message { const char *msg; unsigned frames; };
enum retro_log_level {
    RETRO_LOG_DEBUG = 0,
    RETRO_LOG_INFO,
    RETRO_LOG_WARN,
    RETRO_LOG_ERROR,
    RETRO_LOG_DUMMY = INT32_MAX
};
typedef void (*retro_log_printf_t)(enum retro_log_level, const char*, ...);
struct retro_log_callback { retro_log_printf_t log; };
struct retro_memory_descriptor {
    uint64_t flags;
    void *ptr;
    size_t offset, start, select, disconnect, len;
    const char *addrspace;
};
struct retro_memory_map {
    const struct retro_memory_descriptor *descriptors;
    unsigned num_descriptors;
};

#define MAX_DESCS 64
#define GK_SUMMARY_ADDR 0x0FFFC000u
#define GK_PASS 0x53534150u
#define GK_FAIL 0x4C494146u
#define GK_EXPECTED_PROTOCOL 0x00010000u
#define GK_EXPECTED_CAPS 0x0000000fu
#define GK_EXPECTED_LOCAL_RAM (32u * 1024u * 1024u)
#define GK_EXPECTED_OFFLOAD_US 2530u
#define GK_EXPECTED_SPEEDUP_X1000 1383u
#define GK_EXPECTED_CHECKSUM 0xf269b734u

static struct retro_memory_descriptor g_descs[MAX_DESCS];
static unsigned g_num_descs;
static const char *g_core_path;
static const char *g_system_dir = "./azahar_system";
static const char *g_save_dir = "./azahar_saves";
static const char *g_assets_dir = "./azahar_assets";
static unsigned g_video_frames;

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    if (level < RETRO_LOG_INFO)
        return;
    static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    fprintf(stderr, "[AZAHAR/%s] ", (level <= RETRO_LOG_ERROR) ? names[level] : "LOG");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void copy_memory_map(const struct retro_memory_map *map) {
    if (!map)
        return;
    g_num_descs = map->num_descriptors < MAX_DESCS ? map->num_descriptors : MAX_DESCS;
    for (unsigned i = 0; i < g_num_descs; ++i)
        g_descs[i] = map->descriptors[i];
    fprintf(stderr, "[frontend] captured %u guest memory descriptors for result validation only\n",
            g_num_descs);
}

static void *guest_ptr(uint32_t addr, size_t size) {
    for (unsigned i = 0; i < g_num_descs; ++i) {
        const struct retro_memory_descriptor *d = &g_descs[i];
        if (!d->ptr || !d->len)
            continue;
        const uint64_t a = addr;
        if (a < d->start || a + size > d->start + d->len)
            continue;
        const size_t translated =
            (size_t)((a & ~(uint64_t)d->disconnect) - d->start + d->offset);
        if (translated + size > d->offset + d->len)
            continue;
        return (uint8_t *)d->ptr + translated;
    }
    return NULL;
}

static bool env_cb(unsigned cmd, void *data) {
    const unsigned base = cmd & 0xffffu;
    switch (base) {
    case 6: /* SET_MESSAGE */
        if (data) {
            const struct retro_message *m = data;
            fprintf(stderr, "[frontend/message] %s\n", m->msg ? m->msg : "(null)");
        }
        return true;
    case 9: /* GET_SYSTEM_DIRECTORY */
        if (data)
            *(const char **)data = g_system_dir;
        return true;
    case 10: /* SET_PIXEL_FORMAT */
    case 11: /* SET_INPUT_DESCRIPTORS */
        return true;
    case 15: { /* GET_VARIABLE */
        if (!data)
            return false;
        struct retro_variable *v = data;
        if (!v->key)
            return false;
        if (strcmp(v->key, "citra_graphics_api") == 0)
            v->value = "Software";
        else if (strcmp(v->key, "citra_is_new_3ds") == 0)
            v->value = "New 3DS";
        else if (strcmp(v->key, "citra_use_cpu_jit") == 0)
            v->value = "enabled";
        else if (strcmp(v->key, "citra_cpu_clock_percentage") == 0)
            v->value = "100";
        else
            return false;
        return true;
    }
    case 19: /* GET_LIBRETRO_PATH */
        if (data)
            *(const char **)data = g_core_path;
        return true;
    case 27: /* GET_LOG_INTERFACE */
        if (data)
            ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case 30: /* GET_CORE_ASSETS_DIRECTORY */
        if (data)
            *(const char **)data = g_assets_dir;
        return true;
    case 31: /* GET_SAVE_DIRECTORY */
        if (data)
            *(const char **)data = g_save_dir;
        return true;
    case 36: /* SET_MEMORY_MAPS - used ONLY to inspect the final guest summary */
        if (data)
            copy_memory_map((const struct retro_memory_map *)data);
        return true;
    case 37: /* SET_GEOMETRY */
    case 52: /* SET_SERIALIZATION_QUIRKS */
        return true;
    case 63: /* GET_CORE_OPTIONS_VERSION */
        if (data)
            *(unsigned *)data = 1;
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    (void)data;
    (void)pitch;
    if (g_video_frames++ < 2)
        fprintf(stderr, "[frontend] video %ux%u\n", w, h);
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    (void)data;
    return frames;
}
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port; (void)device; (void)index; (void)id;
    return 0;
}

#define LOADSYM(name)                                                                    \
    do {                                                                                 \
        *(void **)(&name) = dlsym(handle, #name);                                        \
        if (!(name)) {                                                                   \
            fprintf(stderr, "missing %s: %s\n", #name, dlerror());                     \
            dlclose(handle);                                                              \
            return 2;                                                                     \
        }                                                                                 \
    } while (0)

static int validate_summary(const uint32_t *s) {
    fprintf(stderr, "\n=== GekkoPAK NTR guest summary ===\n");
    fprintf(stderr, "status          : 0x%08x (%s)\n", s[0], s[0] == GK_PASS ? "PASS" : "FAIL");
    fprintf(stderr, "protocol        : %u.%u\n", s[1] >> 16, s[1] & 0xffffu);
    fprintf(stderr, "capabilities    : 0x%08x\n", s[2]);
    fprintf(stderr, "local RAM       : %u MiB\n", s[3] / (1024u * 1024u));
    fprintf(stderr, "allocation      : handle %u\n", s[4]);
    fprintf(stderr, "job             : handle %u\n", s[5]);
    fprintf(stderr, "modeled offload : %u us\n", s[6]);
    fprintf(stderr, "speedup         : %u.%03ux\n", s[7] / 1000u, s[7] % 1000u);
    fprintf(stderr, "payload checksum: 0x%08x\n", s[8]);
    fprintf(stderr, "===================================\n\n");

    if (s[0] != GK_PASS || s[1] != GK_EXPECTED_PROTOCOL || s[2] != GK_EXPECTED_CAPS ||
        s[3] != GK_EXPECTED_LOCAL_RAM || s[4] == 0 || s[5] == 0 ||
        s[6] != GK_EXPECTED_OFFLOAD_US || s[7] != GK_EXPECTED_SPEEDUP_X1000 ||
        s[8] != GK_EXPECTED_CHECKSUM) {
        fprintf(stderr, "NTR E2E FAIL: summary did not match deterministic baseline\n");
        return 1;
    }

    fprintf(stderr, "NTR E2E PASS\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/azahar_libretro.so /path/to/gekkopak_guest_ntr.elf\n",
                argv[0]);
        return 2;
    }

    g_core_path = argv[1];
    const char *content_path = argv[2];
    const char *v;
    if ((v = getenv("GEKKOPAK_SYSTEM_DIR")) && *v) g_system_dir = v;
    if ((v = getenv("GEKKOPAK_SAVE_DIR")) && *v) g_save_dir = v;
    if ((v = getenv("GEKKOPAK_ASSETS_DIR")) && *v) g_assets_dir = v;
    (void)mkdir(g_system_dir, 0777);
    (void)mkdir(g_save_dir, 0777);
    (void)mkdir(g_assets_dir, 0777);

    void *handle = dlopen(g_core_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }

    unsigned (*retro_api_version)(void) = NULL;
    void (*retro_set_environment)(retro_environment_t) = NULL;
    void (*retro_set_video_refresh)(retro_video_refresh_t) = NULL;
    void (*retro_set_audio_sample)(retro_audio_sample_t) = NULL;
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t) = NULL;
    void (*retro_set_input_poll)(retro_input_poll_t) = NULL;
    void (*retro_set_input_state)(retro_input_state_t) = NULL;
    void (*retro_init)(void) = NULL;
    void (*retro_deinit)(void) = NULL;
    void (*retro_get_system_info)(struct retro_system_info *) = NULL;
    void (*retro_get_system_av_info)(struct retro_system_av_info *) = NULL;
    bool (*retro_load_game)(const struct retro_game_info *) = NULL;
    void (*retro_unload_game)(void) = NULL;
    void (*retro_run)(void) = NULL;

    LOADSYM(retro_api_version);
    LOADSYM(retro_set_environment);
    LOADSYM(retro_set_video_refresh);
    LOADSYM(retro_set_audio_sample);
    LOADSYM(retro_set_audio_sample_batch);
    LOADSYM(retro_set_input_poll);
    LOADSYM(retro_set_input_state);
    LOADSYM(retro_init);
    LOADSYM(retro_deinit);
    LOADSYM(retro_get_system_info);
    LOADSYM(retro_get_system_av_info);
    LOADSYM(retro_load_game);
    LOADSYM(retro_unload_game);
    LOADSYM(retro_run);

    fprintf(stderr, "[frontend] libretro API=%u\n", retro_api_version());
    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    struct retro_system_info info = {0};
    retro_get_system_info(&info);
    fprintf(stderr, "[frontend] core=%s version=%s\n",
            info.library_name ? info.library_name : "?",
            info.library_version ? info.library_version : "?");

    struct retro_game_info game = {content_path, NULL, 0, NULL};
    const bool loaded = retro_load_game(&game);
    fprintf(stderr, "[frontend] retro_load_game=%s\n", loaded ? "true" : "false");
    int result = 1;

    if (loaded) {
        struct retro_system_av_info av = {0};
        retro_get_system_av_info(&av);
        fprintf(stderr, "[frontend] New-3DS guest: %ux%u @ %.3f Hz\n",
                av.geometry.base_width, av.geometry.base_height, av.timing.fps);

        for (unsigned frame = 0; frame < 240; ++frame) {
            retro_run();
            uint32_t *summary = (uint32_t *)guest_ptr(GK_SUMMARY_ADDR, 9 * sizeof(uint32_t));
            if (!summary)
                continue;
            if (summary[0] == GK_PASS) {
                result = validate_summary(summary);
                break;
            }
            if (summary[0] == GK_FAIL) {
                fprintf(stderr, "NTR E2E FAIL: guest reported failure\n");
                result = 1;
                break;
            }
        }
        if (result != 0)
            fprintf(stderr, "NTR E2E FAIL: guest did not reach deterministic PASS state\n");
        retro_unload_game();
    }

    retro_deinit();
    dlclose(handle);
    fprintf(stderr, "[frontend] clean shutdown\n");
    return result;
}

/* SPDX-License-Identifier: MPL-2.0 */
/* MANIFOLD for the M-VAVE FM-1 (fm1-base). The engine (dsp/) runs in the audio interrupt; this file is the rest:
 * the panel, the screen, MIDI, patches and scenes, Bluetooth audio and the profiler. Part of the platform's unity
 * build (device/unity.c, host/plat_host.c): gfx.c, announce.c and touch.h are in scope.
 *
 * Sharing between the main loop and the audio interrupt:
 *   - the main loop never writes the engine. Every change goes through a small queue (cmd()) that app_render
 *     drains at the start of each block, so a parameter, a note, a scene or a whole patch lands between blocks.
 *   - the main loop reads the engine freely (values, node levels, the profiler's sums): a torn read of a float
 *     costs one frame of a wrong number on the screen, nothing more.
 *   - a rebuild (new scale, engine, surface...) is too long for the interrupt (~15 ms): the render goes silent and
 *     raises S.build_req, the main loop builds (synth_build_now), the render carries on. */
#include <stddef.h>                     /* offsetof (patch_up) */
#include "fastmath.h"
#include "dsp/synth.h"

/* ------------------------------------------------------------------ the engine --- */
static Synth S __attribute__((section(".pool")));

#define BUDGET_US ((uint32_t)(APP_BLOCK * 1000000u / 44100u))   /* one block's time: 2902 us at 128 frames */
#define BARRIER() __asm__ volatile("" ::: "memory")

/* ------------------------------------------------------------------ themes --- */
/* the screen's five steps: three neutral greys (rules, inactive, labels) and the theme's two accents */
typedef struct { const char *name; uint16_t a, b; } theme_t;
static const theme_t THEMES[] = {
    {"Glacier", RGB(64, 168, 232), RGB(168, 226, 255)},
    {"Ember", RGB(232, 128, 32), RGB(255, 196, 96)},
    {"Moss", RGB(88, 196, 108), RGB(176, 248, 184)},
    {"Rose", RGB(228, 88, 132), RGB(255, 168, 196)},
    {"Violet", RGB(148, 112, 240), RGB(208, 184, 255)},
    {"Sunset", RGB(240, 100, 64), RGB(255, 208, 128)},
    {"Lagoon", RGB(40, 168, 168), RGB(128, 236, 220)},
    {"Gold", RGB(204, 164, 40), RGB(255, 228, 128)},
    {"Mono", RGB(184, 184, 192), RGB(244, 244, 248)},
};
#define NTHEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

/* the engine's tick and render run from RAM, out of this code's direct call range: through a pointer */
static inline void *app_far(void *p) { void *volatile q = p; return q; }
static void theme_apply(int i)
{
    i = i < 0 ? 0 : i % NTHEMES;
    pal[0] = RGB(34, 34, 40);
    pal[1] = RGB(76, 76, 86);
    pal[2] = RGB(156, 156, 166);
    pal[3] = THEMES[i].a;
    pal[4] = THEMES[i].b;
}

/* ------------------------------------------------------------------ settings --- */
#define SET_MAGIC 0x4D4E4632u            /* "MNF2" */
typedef struct {
    uint32_t magic;
    uint32_t seed;                       /* the last roll: the same patch at the next power-on */
    int8_t oct;                          /* the keys' shift, in scale periods: -3 .. +3 */
    uint8_t style;
    uint8_t cap_pad, cap_uni, cap_nodes; /* the profiler's cuts: kept, so a choice made once stays made */
    uint8_t ws_os, rv_full, cpu2;     /* (ws_os, rv_full and cap_uni: unused since build F, the cuts are fixed) */
    uint8_t midi_ch;                     /* 0 = omni, 1..16 */
    int8_t last_patch;                   /* -1: none loaded; boot rolls set.seed */
    uint8_t theme;
    uint8_t bt_on, bt_auto;              /* Bluetooth radio at power-on; reconnect the last audio device */
    uint8_t midi_out;                    /* MIDI out's notes: 0 none, 1 the keys, 2 the keys and the layers (1.0.6) */
    uint8_t setup_sub;                   /* the setup page last shown (HOME twice lands there) */
    uint8_t cap_harm;                    /* partials per table (0 in older settings: all) */
    uint8_t keys_to;                     /* the keys play layer 1 (0), layer 2 (1), both (2) or the drums (3) */
    uint8_t ver;                         /* the settings' version (0 before 1.0.5): 5, the keys on both layers once; 6,
                                            MIDI out's (1.0.6) */
    /* (1.0.6) MIDI out */
    uint8_t mo_mpe;                      /* the notes as MPE (just pitches) */
    uint8_t mo_drums;                    /* 0 off, 1 the kit's notes, 2 pitched (the note each hit lands on) */
    uint8_t mo_sync;                     /* bit 0 the knobs (CC 21..24), bit 1 the clock */
    uint8_t mo_floor;                    /* dB under a layer's peak where its notes start: 12 .. 36 */
    uint8_t mo_rate;                     /* new notes a second a layer: 6 12 24 48 96, no cap (MO_RATE) */
    uint8_t mo_level;                    /* a note's level as it moves: pressure (MPE) or poly aftertouch */
    uint8_t mo_ch[4];                    /* Standard: the channels (0-based) of layer 1, layer 2, the keys, the drums */
    uint8_t mo_kit[NUM_DRUMS];           /* the kit's notes: kick, snare, hat, tom */
} settings_t;
#define SET_SIZE_5 ((offsetof(settings_t, ver) + 1u + 3u) & ~3u)   /* (1.0.5's size: up to ver, padded: 28) */
static settings_t set;
static uint8_t set_dirty;
int manifold_bt_on(void) { return set.bt_on; }

/* a patch: every parameter, the matrix, the macros, the locks and four scenes */
#define PATCH_MAGIC 0x4D4E5048u          /* "MNPH" (1.0.8): MNPG's layout, the patterns in 1.0.8's library */
#define PATCH_MAGIC_G 0x4D4E5047u        /* "MNPG" (1.0.6, 1.0.7): MNP9 with the patterns' 20, the colour's 12, the undertones' 4, Balance's 2
                                            and Dry after: read, its patterns moved to the new library */
#define PATCH_MAGIC_F 0x4D4E5046u        /* "MNPF" (1.0.5): read, wet */
#define PATCH_MAGIC_E 0x4D4E5045u        /* "MNPE": a 1.0.5 test build's, before Balance: read, it at its default */
#define PATCH_MAGIC_D 0x4D4E5044u        /* "MNPD": a 1.0.5 test build's, before the undertones: read, those at their defaults */
#define PATCH_MAGIC_C 0x4D4E5043u        /* "MNPC": a 1.0.5 test build's, before the hits' colour: read, those at their defaults */
#define PATCH_MAGIC_A 0x4D4E5041u        /* "MNPA": the 1.0.5 test builds' before the voices' colour: read, those at their defaults */
#define PATCH_MAGIC_9 0x4D4E5039u        /* "MNP9": 175 parameters, the tempo 10..180: read, the new ones at their defaults */
#define PATCH_MAGIC_8 0x4D4E5038u        /* "MNP8": the same, the tempo 60..180: read, its tempo moved to the new range */
#define PATCH_MAGIC_B 0x4D4E5042u        /* "MNPB": a withdrawn test build's (MNPA and 20 drum-hit parameters after it):
                                            read, those 20 dropped, so a patch saved there is neither lost nor erased */
#define NUM_PARAMS_B (NUM_PARAMS_A + 20)
#define NSCENE 4
typedef struct {
    uint32_t magic;
    uint16_t nparams, nslots;
    uint32_t seed;
    uint8_t style, scene_set, rsv[2];
    float p[NUM_PARAMS];
    ModSlot slot[MOD_SLOTS];
    int8_t kpath[NUM_PARAMS][KNOB_WP];
    float macro[NUM_MACRO];
    uint8_t locked[NUM_SECTIONS];
    uint16_t scene[NSCENE][NUM_PARAMS];  /* normalized, 0..65535 */
    int8_t scene_macro[NSCENE][NUM_MACRO];
} patch_t;
typedef char patch_fits[sizeof(patch_t) <= PLAT_STORE_MAX ? 1 : -1];
/* the older layouts, read and then laid out as patch_t (patch_up9, patch_upA): MNP9 and MNP8's (175 parameters),
   MNPA's (195) */
#define PATCH_OLD(T, NP) typedef struct { \
    uint32_t magic; uint16_t nparams, nslots; uint32_t seed; uint8_t style, scene_set, rsv[2]; \
    float p[NP]; ModSlot slot[MOD_SLOTS]; int8_t kpath[NP][KNOB_WP]; float macro[NUM_MACRO]; \
    uint8_t locked[NUM_SECTIONS]; uint16_t scene[NSCENE][NP]; int8_t scene_macro[NSCENE][NUM_MACRO]; } T
PATCH_OLD(patch9_t, NUM_PARAMS_9);
PATCH_OLD(patchA_t, NUM_PARAMS_A);
PATCH_OLD(patchC_t, NUM_PARAMS_C);
PATCH_OLD(patchD_t, NUM_PARAMS_D);
PATCH_OLD(patchE_t, NUM_PARAMS_E);
PATCH_OLD(patchF_t, NUM_PARAMS_F);
#undef PATCH_OLD
static patch_t pbuf __attribute__((section(".pool")));                     /* the patch in flight: a save's copy, or a load the render takes */
static patch_t undo_buf __attribute__((section(".pool")));                 /* the patch as it was before the last roll */
static uint8_t undo_ok;
static uint32_t rolls;                   /* rolls sent to the engine (the host's tests count them) */
static volatile uint8_t pbuf_busy;       /* 1: the render has a load queued from pbuf */
static uint64_t patch_here;              /* bit k: slot k holds a patch (slots 0..63: bank k / 16, key k % 16) */
static uint8_t bank;                     /* the active bank (0..3: A..D): SAVE + key and HOME + key use its 16 slots */
static uint32_t patch_obj(int slot)      /* a slot's store object: 1.0's 16 kept their numbers, Bluetooth's after them */
{
    return slot < 16 ? OBJ_P0 + (uint32_t)slot : OBJ_P16 + (uint32_t)slot - 16u;
}
static void int_text(int32_t v, char *b);
static void slot_name(int slot, char *t) /* "B5": the bank's letter, the key's number */
{
    t[0] = (char)('A' + slot / 16);
    int_text(slot % 16 + 1, t + 1);
}

/* the scenes of the patch playing (RAM; saved with the patch) */
static uint16_t scene[NSCENE][NUM_PARAMS] __attribute__((section(".pool")));
static int8_t scene_macro[NSCENE][NUM_MACRO];
static uint8_t scene_set, scene_cur = 0xFF;
static float scene_buf[NUM_PARAMS], scene_mbuf[NUM_MACRO] __attribute__((section(".pool")));
static volatile uint8_t scene_busy;

/* ------------------------------------------------------------------ the queue --- */
enum { C_PARAM, C_MACRO, C_ROLL, C_MUTATE, C_STYLE, C_NOTE, C_TRANSPORT, C_CAP, C_LOAD, C_LOCK, C_PANIC, C_SCENE,
       C_LAYER, C_MUTESEQ, C_KEYS, C_SLOT, C_VOCAB, C_VOCAB_DATA };
enum { CAP_PAD, CAP_UNI, CAP_NODES, CAP_WSOS, CAP_FBST, CAP_CPU2, CAP_HARM };
typedef struct { uint8_t op, a; uint16_t b; float f; uint32_t u; } cmd_t;
#define QN 64u
static cmd_t q[QN];
static volatile uint32_t q_w, q_r;

static int cmd(uint8_t op, uint32_t a, uint32_t b, float f, uint32_t u)
{
    uint32_t w = q_w;
    if (w - q_r >= QN)
        return 0;                        /* full: dropped (64 a block: only a stuck render fills it) */
    q[w % QN].op = op;
    q[w % QN].a = (uint8_t)a;
    q[w % QN].b = (uint16_t)b;
    q[w % QN].f = f;
    q[w % QN].u = u;
    BARRIER();
    q_w = w + 1u;
    return 1;
}

static void apply_cap(uint32_t which, uint32_t v)
{
    switch (which) {
    case CAP_PAD: S.cap_pad_voices = (uint8_t)v; break;
    case CAP_UNI: break;
    case CAP_NODES: S.cap_nodes = (uint8_t)v; break;
    case CAP_HARM: S.cap_harm = (uint8_t)v; break;    /* engine_update re-derives the matrix */
    case CAP_WSOS: break;
    case CAP_FBST: break;
    case CAP_CPU2: S.par_on = (int)v && plat_cpu2_on(); break;
    }
}

static void release_all(void)
{
    int i;
    for (i = 0; i < MAX_NODES; i++)
        if (S.held[i])
            synth_note(&S, i, 0.0f, 0);
}

static void drain(void)                  /* audio interrupt, start of a block */
{
    synth_roll_step(&S);                 /* a roll under way: its next part (a roll is spread over a few blocks) */
    while (q_r != q_w) {
        const cmd_t *c = &q[q_r % QN];
        switch (c->op) {
        case C_PARAM: synth_set_param(&S, c->b, c->f); break;
        case C_MACRO: synth_set_macro(&S, c->a, c->f); break;
        case C_ROLL: synth_roll_begin(&S, c->u, c->f, c->b); break;   /* (a part a block: synth_roll_step) */
        case C_MUTATE: synth_mutate(&S, c->f); break;
        case C_STYLE: synth_set_style(&S, c->a); break;
        case C_NOTE: synth_note(&S, c->b, c->f, c->a); break;
        case C_TRANSPORT: S.transport = c->a; break;
        case C_CAP: apply_cap(c->a, c->b); break;
        case C_LOCK: S.locked[c->a % NUM_SECTIONS] = (uint8_t)c->b; break;
        case C_LAYER: S.lay_t[c->a & 1u] = c->f; break;
        case C_MUTESEQ: S.mute_seq = c->a; break;
        case C_KEYS: S.keys_to = c->a; break;
        case C_VOCAB:
            if (c->a < NUM_LAYERS) {
                S.layer[c->a].vocab_mask = c->u;
                S.layer[c->a].vocab_count = c->b <= 16u ? (uint8_t)c->b : 0;
                S.layer[c->a].vocab_on = S.layer[c->a].vocab_count != 0;
                S.layer[c->a].vocab_revision++;
            }
            break;
        case C_VOCAB_DATA:
            if (c->a < NUM_LAYERS && c->b < 4u) {
                for (int i = 0; i < 4; i++)
                    S.layer[c->a].vocab_notes[c->b * 4u + (uint32_t)i] = (uint8_t)(c->u >> (i * 8));
            }
            break;
        case C_SLOT:                     /* a matrix route: a the route, b source << 8 | destination, f the depth */
            if (c->a < MOD_SLOTS) {
                ModSlot *m = &S.slot[c->a];
                m->src = (uint8_t)(c->b >> 8);
                m->dst = (uint8_t)c->b;
                m->depth = c->f;
            }
            break;
        case C_LOAD: {
            int i, structural = 0;
            if (S.base[P_SCALE] != pbuf.p[P_SCALE] || S.base[P_ROTATE] != pbuf.p[P_ROTATE] || S.base[P_OCTAVES] != pbuf.p[P_OCTAVES])
                synth_vocab_clear(&S);
            for (i = 0; i < NUM_PARAMS; i++) {
                if (g_param_meta[i].structural && S.base[i] != pbuf.p[i])
                    structural = 1;
                S.target[i] = pbuf.p[i];
                if (!g_param_meta[i].structural && g_param_meta[i].curve == CURVE_DISC)
                    S.base[i] = pbuf.p[i];
            }
            memcpy(S.slot, pbuf.slot, sizeof S.slot);
            memcpy(S.kpath, pbuf.kpath, sizeof S.kpath);
            memcpy(S.macro, pbuf.macro, sizeof S.macro);
            memcpy(S.locked, pbuf.locked, sizeof S.locked);
            S.style = pbuf.style;
            S.seed = pbuf.seed;
            S.morph_rate = 1.0f / (0.6f * SR / (float)SUB);   /* the continuous ones glide over 0.6 s */
            S.morphing = 1;
            if (c->a) {                  /* a whole patch (not an undo of a section roll): the groove restarts */
                synth_restart(&S, pbuf.seed);
            }
            if (structural) {
                S.rebuild_pending = 1;
                S.duck_tgt = 0.0f;
            }
            BARRIER();
            pbuf_busy = 0;
            break;
        }
        case C_SCENE: {                  /* the continuous parameters and the macros glide to the scene */
            int i;
            for (i = 0; i < NUM_PARAMS; i++) {
                if (g_param_meta[i].structural)
                    continue;
                S.target[i] = scene_buf[i];
                if (g_param_meta[i].curve == CURVE_DISC)
                    S.base[i] = scene_buf[i];
            }
            for (i = 0; i < NUM_MACRO; i++)
                S.macro[i] = scene_mbuf[i];
            S.morph_rate = 1.0f / (c->f * SR / (float)SUB);
            S.morphing = 1;
            BARRIER();
            scene_busy = 0;
            break;
        }
        case C_PANIC: {
            synth_silence(&S);
            release_all();
            break;
        }
        }
        BARRIER();
        q_r++;
    }
}

/* ------------------------------------------------------------------ notes --- */
/* the keys play the nodes in pitch order: key k is node rank (k + shift), shift = OCT in scale periods */
static uint8_t order[MAX_NODES];
static int n_order;
static uint32_t built_seen = 0xFFFFFFFFu;
static uint32_t builds;
static int8_t key_node[NKEYS];           /* the node each held key plays (its note off goes to the same) */
static uint32_t latched;                 /* keys held by LATCH (bit k: key k's node still sounding) */
static int8_t midi_node[128];

static void sort_nodes(void)
{
    int i, j, n = S.eng.n_nodes;
    if (n > MAX_NODES)
        n = MAX_NODES;
    for (i = 0; i < n; i++)
        order[i] = (uint8_t)i;
    for (i = 1; i < n; i++) {
        uint8_t v = order[i];
        float lv = S.eng.node[v].lr;
        for (j = i; j > 0 && S.eng.node[order[j - 1]].lr > lv; j--)
            order[j] = order[j - 1];
        order[j] = v;
    }
    n_order = n;
}

static int key_to_node(uint32_t k)
{
    int r, per = S.scale.n > 0 ? S.scale.n : 7;
    if (n_order <= 0)
        return -1;
    r = (int)k + set.oct * per + (n_order > NKEYS ? (n_order - NKEYS) / 2 : 0);   /* centred when the scale is wide */
    while (r < 0)
        r += per;
    while (r >= n_order)
        r -= per > 0 ? per : 1;
    if (r < 0)
        r = 0;
    return order[r];
}

#include "midi_out.h"                   /* MIDI out (1.0.6) */

static volatile uint32_t midi_count;
static void midi_in(void)                /* audio interrupt: notes play the nearest node, CCs move the macros */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24, ch = st & 15u;
        if (st >= 0xF0u)
            continue;
        if (set.midi_ch && ch + 1u != set.midi_ch)
            continue;
        midi_count++;
        if (S.eng.n_nodes == 0)
            continue;
        if ((st & 0xF0u) == 0x90u && d2) {
            float ratio = fm_exp2f(((float)d1 - S.val[P_ROOT]) * (1.0f / 12.0f));
            int nd = node_near(&S, ratio);
            midi_node[d1] = (int8_t)nd;
            synth_note(&S, nd, (float)d2 * (1.0f / 127.0f), 1);
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            if (midi_node[d1] >= 0)
                synth_note(&S, midi_node[d1], 0.0f, 0);
            midi_node[d1] = -1;
        } else if ((st & 0xF0u) == 0xB0u) {
            if (d1 == 1u)
                synth_set_macro(&S, 0, (float)d2 * (2.0f / 127.0f) - 1.0f);
            else if (d1 >= 21u && d1 <= 24u)
                synth_set_macro(&S, (int)(d1 - 21u), (float)d2 * (2.0f / 127.0f) - 1.0f);
            else if (d1 == 120u || d1 == 123u)
                release_all();
        }
    }
}

/* ------------------------------------------------------------------ render --- */
static float mix_l[APP_BLOCK], mix_r[APP_BLOCK];
static float gain_now;
static volatile uint32_t overloads;

/* A block that ran past its budget (the platform calls this from the audio interrupt). The answer is fewer notes, at
   once: one off the cap for every such block, down to 3 a layer; app_frame gives them back, one every 4 s without an
   overrun, up to the setting. A dropout becomes a thinner chord. */
static volatile uint32_t gov_last_ms;
void app_overload(void) { overloads++; if (S.cap_nodes > 3) S.cap_nodes--; gov_last_ms = plat_ms(); }

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    uint32_t i, done = 0;
    const float gain = (float)master_q12 * (8388000.0f / 4096.0f);
    if (!S.build_req) {                  /* (a rebuild in the main loop: commands and MIDI wait their turn) */
        drain();
        midi_in();
    }
    mo_blk_ms = plat_ms();               /* (MIDI out: this block plays a block from now; its hits go out then) */
    mo_blk_s = S.nsamp;
    while (done < n) {
        uint32_t m = n - done > APP_BLOCK ? APP_BLOCK : n - done;
        ((__typeof__(&synth_process))app_far((void *)&synth_process))(&S, mix_l, mix_r, (int)m);
        for (i = 0; i < m; i++) {
            float l, r;
            gain_now += (gain - gain_now) * 0.004f;
            l = mix_l[i] * gain_now;
            r = mix_r[i] * gain_now;
            l = l > 8388607.0f ? 8388607.0f : l < -8388607.0f ? -8388607.0f : l;
            r = r > 8388607.0f ? 8388607.0f : r < -8388607.0f ? -8388607.0f : r;
            lr[2u * (done + i)] = (int32_t)l;
            lr[2u * (done + i) + 1u] = (int32_t)r;
        }
        done += m;
    }
}

void app_panic(void)
{
    cmd(C_PANIC, 0, 0, 0.0f, 0);
    gain_now = 0.0f;
}

/* ------------------------------------------------------------------ Bluetooth audio --- */
/* the platform's A2DP source (fm1.toml bt_audio, own stack); the host simulator has none, so it gets a stand-in
 * with the same states, for the screens and the tests */
#if FM1B_BT_AUDIO && !defined(PLAT_HOST)
#define bta_on plat_bta_on
#define bta_scan plat_bta_scan
#define bta_found plat_bta_found
#define bta_known plat_bta_known
#define bta_pick plat_bta_pick
#define bta_reconnect plat_bta_reconnect
#define bta_hangup plat_bta_hangup
#define bta_state plat_bta_state
static void bta_sim_tick(void) {}
#else
static struct { int st, nk; uint32_t t; char known[4][17]; int up, pages; } sim;
static void sim_cpy(char *d, const char *s) { int i; for (i = 0; i < 16 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static void bta_on(int on) { sim.up = on; sim.st = on ? PLAT_BTA_READY : PLAT_BTA_OFF; }
static void bta_scan(int on) { if (sim.up && on) { sim.st = PLAT_BTA_SCANNING; sim.t = plat_ms(); } }
static int bta_found(int i, char *name)
{
    static const char *const F[] = {"Sim Speaker", "Sim Headphones"};
    if (sim.st != PLAT_BTA_FOUND || i < 0 || i > 1)
        return 0;
    sim_cpy(name, F[i]);
    return 1;
}
static int bta_known(int i, char *name)
{
    if (i < 0 || i >= sim.nk)
        return 0;
    sim_cpy(name, sim.known[i]);
    return 1;
}
static void bta_pick(int i)
{
    char n[17];
    if (!bta_found(i, n))
        return;
    if (sim.nk < 4)
        sim.nk++;
    {
        int j;
        for (j = 3; j > 0; j--)
            sim_cpy(sim.known[j], sim.known[j - 1]);
    }
    sim_cpy(sim.known[0], n);
    sim.pages++;
    sim.st = PLAT_BTA_PAGING;
    sim.t = plat_ms();
}
static int bta_reconnect(int i)
{
    if (!sim.up || i >= sim.nk)
        return -1;
    sim.pages++;
    sim.st = PLAT_BTA_PAGING;
    sim.t = plat_ms();
    return 0;
}
static void bta_hangup(void) { if (sim.up) sim.st = PLAT_BTA_READY; }
static int bta_state(void) { return sim.st; }
static void bta_sim_tick(void)
{
    if (sim.st == PLAT_BTA_SCANNING && plat_ms() - sim.t > 1500u)
        sim.st = PLAT_BTA_FOUND;
    if (sim.st == PLAT_BTA_PAGING && plat_ms() - sim.t > 600u)
        sim.st = PLAT_BTA_STREAMING;
}
#endif

static const char *bta_state_text(int st)
{
    static const char *const N[] = {"Off", "Scanning...", "Devices found", "None found", "Connecting...", "Linked",
                                    "Setting up...", "Streaming", "Refused", "No last device", "Failed", "Ready"};
    return st >= 0 && st < (int)(sizeof N / sizeof N[0]) ? N[st] : "?";
}

/* ------------------------------------------------------------------ init --- */
static void mo_defaults(void)            /* MIDI out (1.0.6): notes from the keys and the layers, Standard, the kit,
                                            the knobs and the clock; it all stays idle until something listens */
{
    static const uint8_t ch[4] = {0, 1, 2, 9}, kit[NUM_DRUMS] = {36, 38, 42, 45};
    set.midi_out = 2;
    set.mo_mpe = 0;
    set.mo_drums = 1;
    set.mo_sync = 3;
    set.mo_floor = 18;
    set.mo_rate = 2;                     /* 24 a second: 32nd notes at 180 BPM */
    set.mo_level = 0;
    memcpy(set.mo_ch, ch, sizeof ch);
    memcpy(set.mo_kit, kit, sizeof kit);
    set.ver = 6;
}
static void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    set.seed = 501u;
    set.style = STYLE_COSMIC;
    set.cap_pad = MAX_VOICES;
    set.cap_uni = 1;
    set.cap_nodes = GOV_MAX_ACTIVE;
    set.cap_harm = MAX_HARM;
    set.ws_os = 1;
    set.rv_full = 1;
    set.cpu2 = 1;
    set.last_patch = -1;
    set.bt_auto = 1;
    set.midi_out = 1;
    set.keys_to = 2;                     /* the keys play both layers (1.0.5) */
    set.ver = 5;
    mo_defaults();
}

/* an MNP9 (or MNP8) patch read into a patch_t's room, laid out as one in place: each field moved to its new place,
   the last first (every field only moves later), then the new parameters at their defaults (in the patch, its scenes
   and its knobs' paths) and the matrix's routes to the knobs moved past them */
static void mv_up(unsigned char *d, const unsigned char *s, size_t n)   /* a copy to a later place (d >= s): from the end */
{
    while (n--)
        d[n] = s[n];
}
#define PATCH_UP(FN, T, NP) \
static void FN(patch_t *p) \
{ \
    unsigned char *b = (unsigned char *)p; \
    int i, k; \
    mv_up(b + offsetof(patch_t, scene_macro), b + offsetof(T, scene_macro), sizeof p->scene_macro); \
    for (k = NSCENE - 1; k >= 0; k--) \
        mv_up(b + offsetof(patch_t, scene) + (size_t)k * NUM_PARAMS * 2u, b + offsetof(T, scene) + (size_t)k * (NP) * 2u, (size_t)(NP) * 2u); \
    mv_up(b + offsetof(patch_t, locked), b + offsetof(T, locked), sizeof p->locked); \
    mv_up(b + offsetof(patch_t, macro), b + offsetof(T, macro), sizeof p->macro); \
    mv_up(b + offsetof(patch_t, kpath), b + offsetof(T, kpath), (size_t)(NP) * KNOB_WP); \
    mv_up(b + offsetof(patch_t, slot), b + offsetof(T, slot), sizeof p->slot); \
    for (i = (NP); i < NUM_PARAMS; i++) { \
        p->p[i] = g_param_meta[i].def; \
        for (k = 0; k < KNOB_WP; k++) p->kpath[i][k] = 0; \
        for (k = 0; k < NSCENE; k++) p->scene[k][i] = (uint16_t)(g_param_meta[i].def * 65535.0f + 0.5f); \
    } \
    for (k = 0; k < MOD_SLOTS; k++) \
        if (p->slot[k].src && p->slot[k].dst >= (NP)) p->slot[k].dst = (uint8_t)(p->slot[k].dst + (NUM_PARAMS - (NP))); \
    p->nparams = NUM_PARAMS; \
}
PATCH_UP(patch_up9, patch9_t, NUM_PARAMS_9)
PATCH_UP(patch_upA, patchA_t, NUM_PARAMS_A)
PATCH_UP(patch_upC, patchC_t, NUM_PARAMS_C)
PATCH_UP(patch_upD, patchD_t, NUM_PARAMS_D)
PATCH_UP(patch_upE, patchE_t, NUM_PARAMS_E)
PATCH_UP(patch_upF, patchF_t, NUM_PARAMS_F)
#undef PATCH_UP

typedef struct {                         /* MNPB's layout */
    uint32_t magic; uint16_t nparams, nslots; uint32_t seed; uint8_t style, scene_set, rsv[2];
    float p[NUM_PARAMS_B]; ModSlot slot[MOD_SLOTS]; int8_t kpath[NUM_PARAMS_B][KNOB_WP]; float macro[NUM_MACRO];
    uint8_t locked[NUM_SECTIONS]; uint16_t scene[NSCENE][NUM_PARAMS_B]; int8_t scene_macro[NSCENE][NUM_MACRO];
} patchB_t;
typedef char patchB_fits[sizeof(patchB_t) <= PLAT_STORE_MAX ? 1 : -1];
/* (1.0.8) an older patch's drum patterns, from the 1.0.5..1.0.7 library to the same lines in this one: the patch,
   its scenes, its knobs' paths and routes to them (their steps scaled to the new library's) */
static void patch_pat_v107(patch_t *p)
{
    int d, k;
    for (d = 0; d < NUM_DRUMS; d++) {
        int id = P_D1_PAT + d;
        float sc = pat_v107_scale(d);
        p->p[id] = pat_v107(d, p->p[id]);
        for (k = 0; k < NSCENE; k++) {
            float u = pat_v107(d, (float)p->scene[k][id] * (1.0f / 65535.0f));
            p->scene[k][id] = (uint16_t)(u * 65535.0f + 0.5f);
        }
        for (k = 0; k < KNOB_WP; k++) {
            float w = (float)p->kpath[id][k] * sc;
            p->kpath[id][k] = (int8_t)(w < 0.0f ? w - 0.5f : w + 0.5f);
        }
        for (k = 0; k < MOD_SLOTS; k++)
            if (p->slot[k].src && p->slot[k].dst == id)
                p->slot[k].depth *= sc;
    }
}
static int patch_read_b(int slot, patch_t *p)   /* an MNPB patch, its drum-hit parameters dropped (and routes to them), the
                                                   voices' colour at its defaults */
{
    static patchB_t o __attribute__((section(".pool")));
    int i, k;
    if (plat_store_load(patch_obj(slot), &o, sizeof o) != (int)sizeof o || o.magic != PATCH_MAGIC_B
        || o.nparams != NUM_PARAMS_B || o.nslots != MOD_SLOTS)
        return 0;
    p->magic = PATCH_MAGIC_G; p->nparams = NUM_PARAMS; p->nslots = MOD_SLOTS; p->seed = o.seed; p->style = o.style;
    p->scene_set = o.scene_set; p->rsv[0] = o.rsv[0]; p->rsv[1] = o.rsv[1];
    for (i = 0; i < NUM_PARAMS; i++) {
        int in = i < NUM_PARAMS_A;
        p->p[i] = in ? o.p[i] : g_param_meta[i].def;
        for (k = 0; k < KNOB_WP; k++) p->kpath[i][k] = in ? o.kpath[i][k] : 0;
        for (k = 0; k < NSCENE; k++) p->scene[k][i] = in ? o.scene[k][i] : (uint16_t)(g_param_meta[i].def * 65535.0f + 0.5f);
    }
    for (k = 0; k < MOD_SLOTS; k++) {
        p->slot[k] = o.slot[k];
        if (o.slot[k].dst >= NUM_PARAMS_B) p->slot[k].dst = (uint8_t)(o.slot[k].dst - NUM_PARAMS_B + NUM_PARAMS);
        else if (o.slot[k].dst >= NUM_PARAMS_A) { p->slot[k].src = 0; p->slot[k].dst = 0; p->slot[k].depth = 0.f; }
    }
    for (k = 0; k < NUM_MACRO; k++) p->macro[k] = o.macro[k];
    for (k = 0; k < NUM_SECTIONS; k++) p->locked[k] = o.locked[k];
    for (k = 0; k < NSCENE; k++) for (i = 0; i < NUM_MACRO; i++) p->scene_macro[k][i] = o.scene_macro[k][i];
    return 1;
}
static int patch_read(int slot, patch_t *p)
{
    int n;
    if (slot < 0 || slot >= N_PATCH)
        return 0;
    if (plat_store_size(patch_obj(slot)) == (int)sizeof(patchB_t)) {   /* (the withdrawn MNPB's size: by its magic, as
                                                    MNPG has the same 215 parameters and the same size) */
        uint32_t mg = 0;
        if (plat_store_head(patch_obj(slot), &mg, 4) == 4 && mg == PATCH_MAGIC_B)
            return patch_read_b(slot, p);
    }
    n = plat_store_load(patch_obj(slot), p, sizeof *p);
    if (n == (int)sizeof(patch9_t) && (p->magic == PATCH_MAGIC_9 || p->magic == PATCH_MAGIC_8) && p->nparams == NUM_PARAMS_9
        && p->nslots == MOD_SLOTS) {
        patch_up9(p);                    /* (an older patch: the patterns' and the voices' new parameters at their defaults) */
        if (p->magic == PATCH_MAGIC_9)
            p->magic = PATCH_MAGIC_G;
    } else if (n == (int)sizeof(patchA_t) && p->magic == PATCH_MAGIC_A && p->nparams == NUM_PARAMS_A && p->nslots == MOD_SLOTS) {
        patch_upA(p);                    /* (an earlier 1.0.5 test build's: the voices' colour at its defaults) */
        p->magic = PATCH_MAGIC_G;
    } else if (n == (int)sizeof(patchC_t) && p->magic == PATCH_MAGIC_C && p->nparams == NUM_PARAMS_C && p->nslots == MOD_SLOTS) {
        patch_upC(p);                    /* (a later one's: the hits' colour at its defaults) */
        p->magic = PATCH_MAGIC_G;
    } else if (n == (int)sizeof(patchD_t) && p->magic == PATCH_MAGIC_D && p->nparams == NUM_PARAMS_D && p->nslots == MOD_SLOTS) {
        patch_upD(p);                    /* (a later one's still: the undertones at their defaults) */
        p->magic = PATCH_MAGIC_G;
    } else if (n == (int)sizeof(patchF_t) && p->magic == PATCH_MAGIC_F && p->nparams == NUM_PARAMS_F && p->nslots == MOD_SLOTS) {
        patch_upF(p);                    /* (1.0.5's: wet) */
        p->magic = PATCH_MAGIC_G;
    } else if (n == (int)sizeof(patchE_t) && p->magic == PATCH_MAGIC_E && p->nparams == NUM_PARAMS_E && p->nslots == MOD_SLOTS) {
        patch_upE(p);                    /* (the one before Balance: Balanced) */
        p->magic = PATCH_MAGIC_G;
    } else if (n != (int)sizeof *p)
        return 0;
    if (p->nparams != NUM_PARAMS || p->nslots != MOD_SLOTS)
        return 0;
    if (p->magic == PATCH_MAGIC_8) {     /* the same BPM in the wider range (the patch and its scenes) */
        const ParamMeta *tm = &g_param_meta[P_TEMPO];
        int k;
        p->p[P_TEMPO] = fm_clampf((60.0f + 120.0f * p->p[P_TEMPO] - tm->lo) / (tm->hi - tm->lo), 0.0f, 1.0f);
        for (k = 0; k < NSCENE; k++) {
            float u = (float)p->scene[k][P_TEMPO] * (1.0f / 65535.0f);
            u = fm_clampf((60.0f + 120.0f * u - tm->lo) / (tm->hi - tm->lo), 0.0f, 1.0f);
            p->scene[k][P_TEMPO] = (uint16_t)(u * 65535.0f + 0.5f);
        }
        p->magic = PATCH_MAGIC_G;
    }
    if (p->magic == PATCH_MAGIC_G) {     /* (1.0.8) a patch of 1.0.5..1.0.7: its patterns to the same lines in the new library */
        patch_pat_v107(p);
        p->magic = PATCH_MAGIC;
    }
    return p->magic == PATCH_MAGIC;
}

/* the power-on scale: the 1-3-5-9 hexany, unrotated, its ding on A1 (MIDI 33) */
static void start_set(int id, float v)
{
    S.target[id] = S.base[id] = S.mod[id] = v;
}
static int name_is(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static Scale scale_tmp __attribute__((section(".pool")));                 /* main loop only: a scale for the screen, or looked up by name */
static void start_scale(void)
{
    Scale *sc = &scale_tmp;
    int i, n = scale_count();
    for (i = 0; i < n; i++) {
        scale_get(i, sc);
        if (name_is(sc->name, "Hexany 1-3-5-9")) {
            start_set(P_SCALE, ((float)i + 0.5f) / g_param_meta[P_SCALE].hi);
            break;
        }
    }
    start_set(P_ROTATE, 0.5f / g_param_meta[P_ROTATE].hi);
    start_set(P_ROOT, (33.0f - g_param_meta[P_ROOT].lo) / (g_param_meta[P_ROOT].hi - g_param_meta[P_ROOT].lo));
}

static void scenes_from(const patch_t *p)
{
    memcpy(scene, p->scene, sizeof scene);
    memcpy(scene_macro, p->scene_macro, sizeof scene_macro);
    scene_set = p->scene_set;
    scene_cur = 0xFF;
}

void app_init(void)
{
    int k;
    uint32_t i;
    int sn = plat_store_load(OBJ_SETTINGS, &set, sizeof set);
    if ((sn != (int)sizeof set && sn != (int)SET_SIZE_5) || set.magic != SET_MAGIC ||
        set.oct < -3 || set.oct > 3 || set.style >= NUM_STYLES || set.cap_pad < 4 || set.cap_pad > MAX_VOICES ||
        set.keys_to > 3 ||
        set.midi_ch > 16 || set.last_patch >= N_PATCH || set.theme >= NTHEMES)
        settings_defaults();
    /* caps from older firmware (32 ringing nodes; no harmonics cap): bring them into range, keep the rest */
    set.cpu2 = 1;                        /* (the second core is always used: 1.0.4 and earlier let the Load page turn it
                                            off, and kept that) */
    if (set.ver < 5) {                   /* (settings from before 1.0.5: the keys on both layers, once) */
        set.keys_to = 2;
        set.ver = 5;
    }
    if (set.ver < 6 || sn != (int)sizeof set || set.midi_out > 2 || set.mo_drums > 2 || set.mo_sync > 3 ||
        set.mo_floor < 12 || set.mo_floor > 36 || set.mo_rate > 5 || set.mo_ch[0] > 15 || set.mo_ch[1] > 15 ||
        set.mo_ch[2] > 15 || set.mo_ch[3] > 15)
        mo_defaults();                   /* (settings from before 1.0.6: MIDI out's defaults) */
    if (set.cap_nodes < 4 || set.cap_nodes > GOV_MAX_ACTIVE)
        set.cap_nodes = GOV_MAX_ACTIVE;
    if (set.cap_harm < 4 || set.cap_harm > MAX_HARM)
        set.cap_harm = MAX_HARM;
    mo_reset();                          /* (MIDI out: nothing held) */
    for (k = 0; k < 128; k++)
        midi_node[k] = -1;
    for (k = 0; k < (int)NKEYS; k++)
        key_node[k] = -1;
    plat_cpu2_set(1);                    /* the second core: only now may it start (power-on, before the audio) */
    synth_init(&S, set.seed);
    S.clock = plat_ticks24;
    S.par_run = plat_cpu2_run;
    S.par_wait = plat_cpu2_wait;
    S.par_sync = plat_cpu2_sync;
    S.build_defer = 1;
    S.cap_pad_voices = set.cap_pad;
    S.cap_nodes = set.cap_nodes;
    S.cap_harm = set.cap_harm;
    S.par_on = set.cpu2 && plat_cpu2_on();
    S.keys_to = set.keys_to;
    S.transport = 0;                     /* silent at power-on: PLAY starts the generators (the keys play at once) */
    for (i = 0; i < N_PATCH; i++) {
        uint32_t magic;
        if (plat_store_head(patch_obj((int)i), &magic, 4) == 4 && (magic == PATCH_MAGIC || magic == PATCH_MAGIC_G || magic == PATCH_MAGIC_A || magic == PATCH_MAGIC_B || magic == PATCH_MAGIC_C || magic == PATCH_MAGIC_D || magic == PATCH_MAGIC_E || magic == PATCH_MAGIC_F || magic == PATCH_MAGIC_9 || magic == PATCH_MAGIC_8))
            patch_here |= 1ull << i;
    }
    if (set.last_patch >= 0)
        bank = (uint8_t)(set.last_patch / 16);   /* the bank of the patch coming back */
    synth_set_style(&S, set.style);
    if (set.last_patch >= 0 && patch_read(set.last_patch, &pbuf)) {
        for (i = 0; i < NUM_PARAMS; i++)
            S.base[i] = S.target[i] = S.mod[i] = pbuf.p[i];
        memcpy(S.slot, pbuf.slot, sizeof S.slot);
        memcpy(S.kpath, pbuf.kpath, sizeof S.kpath);
        memcpy(S.macro, pbuf.macro, sizeof S.macro);
        memcpy(S.locked, pbuf.locked, sizeof S.locked);
        S.style = pbuf.style;
        S.seed = pbuf.seed;
        synth_restart(&S, pbuf.seed);
        scenes_from(&pbuf);
    } else {                             /* power-on: a fresh Groove roll, on the 1-3-5-9 hexany at A1 */
        uint32_t sd = plat_ticks24();    /* (when the boot reached here, in 24 MHz ticks, with the last roll's seed) */
        sd ^= sd >> 16;
        sd *= 0x45D9F3Bu;
        sd ^= set.seed * 0x9E3779B1u;
        sd ^= sd >> 15;
        sd %= 100000u;
        set.last_patch = -1;
        set.style = STYLE_GROOVE;
        set.seed = sd;
        synth_set_style(&S, STYLE_GROOVE);
        synth_randomize(&S, sd, 0.0f);
        start_scale();
    }
    params_cache_all(&S);
    engine_build(&S);                    /* here, not in the render: the audio has not started */
    S.rebuild_pending = 0;
    S.duck = S.duck_tgt = 1.0f;
    builds++;
}

/* ------------------------------------------------------------------ text --- */
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void text_cpy(char *d, const char *s, int max)
{
    int i;
    for (i = 0; s[i] && i < max - 1; i++)
        d[i] = s[i];
    d[i] = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void fix_text(float v, int dec, char *b)
{
    int32_t sc = dec == 0 ? 1 : dec == 1 ? 10 : dec == 2 ? 100 : 1000, w, d;
    char t[12], *e;
    int neg = v < 0.0f;
    if (neg)
        v = -v;
    w = (int32_t)(v * (float)sc + 0.5f);
    *b = 0;
    if (neg && w)
        text_cat(b, "-");
    int_text(w / sc, t);
    text_cat(b, t);
    if (dec) {
        text_cat(b, ".");
        e = b;
        while (*e)
            e++;
        for (d = sc / 10; d >= 1; d /= 10)
            *e++ = (char)('0' + (w % sc / d) % 10);
        *e = 0;
    }
}

static void num_text(float v, char *b)
{
    float a = v < 0 ? -v : v;
    fix_text(v, a >= 100.0f ? 0 : a >= 10.0f ? 1 : a >= 1.0f ? 2 : 3, b);
}

static void time_text(float s, char *b)  /* seconds: ms below one */
{
    if (s < 1.0f) {
        int_text((int32_t)(s * 1000.0f + 0.5f), b);
        text_cat(b, " ms");
    } else {
        fix_text(s, s < 10.0f ? 2 : 1, b);
        text_cat(b, " s");
    }
}

static const char *const SURF_N[] = {"Plane", "Torus", "Sphere"};
static const char *const STYLE_N[] = {"Wild", "Hip-hop", "Groove", "Ambient", "Cosmic", "House", "Machine", "Breakbeat", "Funk", "Latin", "Indie"};
static const char *const KEYS_N[] = {"Layer 1", "Layer 2", "Both layers", "Drums"};
static const char *const REG_N[] = {"Octave down", "As tuned", "Octave up"};

static const char *const LFO_N[] = {"Sine", "Triangle", "Saw", "Square", "Step random", "Smooth random", "Chaos",
                                    "Rhythm", "Shape 9"};
static const char *const NOTE_N[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static const char *pick(const char *const *tab, int n, int i) { return i >= 0 && i < n ? tab[i] : "?"; }
#define PICK(tab, i) pick(tab, (int)(sizeof tab / sizeof tab[0]), i)

/* a parameter's full name, for the screen and a screen reader */
static void param_name(int id, char *b, int max)
{
    const char *n = g_param_meta[id].name;
    switch (id) {
    case P_FZ_SUSTAIN: n = "Fuzz sustain"; break;
    case P_FZ_TONE: n = "Fuzz tone"; break;
    case P_FZ_MIX: n = "Fuzz mix"; break;
    case P_TEMPER: n = "Temper"; break;
    case P_RH_ENGINE: n = "Rhythm"; break;
    case P_RH_DENS: n = "Density"; break;
    case P_RH_THREES: n = "Threes"; break;
    case P_RH_FINE: n = "Fine"; break;
    case P_RH_SWING: n = "Swing"; break;
    case P_RH_TURN: n = "Turn"; break;
    case P_RH_FORM: n = "Form"; break;
    case P_RH_MOTION: n = "Motion"; break;
    case P_RH_ASSIGN: n = "Drum choice"; break;
    case P_RH_SELRATE: n = "Selector"; break;
    case P_RH_FEED: n = "Feedback"; break;
    case P_RH_COUNT: n = "Kick every"; break;
    case P_RH_PULL: n = "Pull"; break;
    case P_RH_GRIP: n = "Grip"; break;
    case P_V1_CENTER: case P_V2_CENTER: n = "Center"; break;
    case P_V1_BAND: case P_V2_BAND: n = "Band"; break;
    case P_V1_TILT: case P_V2_TILT: n = "Tilt"; break;
    case P_V1_STRIDE: case P_V2_STRIDE: n = "Stride"; break;
    case P_V1_WAVE: case P_V2_WAVE: n = "Wave"; break;
    case P_V1_PHASE: case P_V2_PHASE: n = "Phase"; break;
    case P_V1_SPIN: case P_V2_SPIN: n = "Detune"; break;
    case P_V1_TONE: case P_V2_TONE: n = "Tone"; break;
    case P_V1_BLOOM: case P_V2_BLOOM: n = "Bloom"; break;
    case P_V1_WIDTH: case P_V2_WIDTH: n = "Width"; break;
    case P_D1_HOLD: case P_D2_HOLD: case P_D3_HOLD: case P_D4_HOLD: n = drum_name((id - P_D1_HOLD) / DR_STRIDE); break;
    case P_D1_VOICE: case P_D2_VOICE: case P_D3_VOICE: case P_D4_VOICE: n = "Voice"; break;
    case P_D1_ATTACK: case P_D2_ATTACK: case P_D3_ATTACK: case P_D4_ATTACK: n = "Attack"; break;
    case P_D1_CURVE: case P_D2_CURVE: case P_D3_CURVE: case P_D4_CURVE: n = "Curve"; break;
    case P_D1_BRIGHT: case P_D2_BRIGHT: case P_D3_BRIGHT: case P_D4_BRIGHT: n = "Bright"; break;
    case P_D1_SETTLE: case P_D2_SETTLE: case P_D3_SETTLE: case P_D4_SETTLE: n = "Settle"; break;
    case P_D1_SNAP: case P_D2_SNAP: case P_D3_SNAP: case P_D4_SNAP: n = "Snap"; break;
    case P_DL_TO_RV: n = "Delay into reverb"; break;
    case P_LFO_COUPLE: n = "LFO coupling"; break;
    case P_RV_FREEZE: n = "Reverb freeze"; break;
    case P_DRY: n = "Space knob"; break;
    case P_DR_NOTES: n = "Drum notes"; break;
    case P_FLT_MORPH: n = "Filter type"; break;
    case P_FLT_CUT: n = "Filter cutoff"; break;
    case P_FLT_RES: n = "Filter resonance"; break;
    case P_ROTATE: n = "Rotation (the ding)"; break;
    case P_V1_LEVEL: n = "Layer 1 level"; break;
    case P_V2_LEVEL: n = "Layer 2 level"; break;
    case P_G1_A: case P_G1_B: case P_G2_A: case P_G2_B: {
        int l = id >= P_V2_CENTER;
        n = gen_ab_name(pint(&S, LP(l, P_G1_FUNC)), id == LP(l, P_G1_B));
    } break;
    case P_DR_ON: n = "Drums"; break;
    case P_DR_LEVEL: n = "Drum level"; break;
    case P_DR_LAYER: n = "Drum layer"; break;
    case P_DR_SHAPE: n = "Shape by"; break;
    case P_DR_DEPTH: n = "Shape depth"; break;
    case P_DR_DUCK: n = "Kick duck"; break;
    case P_D1_LEVEL: case P_D2_LEVEL: case P_D3_LEVEL: case P_D4_LEVEL: n = drum_name((id - P_D1_LEVEL) / DR_STRIDE); break;
    case P_DR_ACCENT: n = "Accent"; break;
    case P_D1_DENS: case P_D2_DENS: case P_D3_DENS: case P_D4_DENS: n = drum_name((id - P_D1_DENS) / DR_STRIDE); break;
    case P_D1_SRC: case P_D2_SRC: case P_D3_SRC: case P_D4_SRC: n = drum_name((id - P_D1_SRC) / DR_STRIDE); break;
    case P_D1_PLACE: case P_D2_PLACE: case P_D3_PLACE: case P_D4_PLACE:
        n = drum_name((id - P_D1_PLACE) / DR_STRIDE); break;
    case P_D1_DECAY: case P_D2_DECAY: case P_D3_DECAY: case P_D4_DECAY: n = "Decay"; break;
    case P_G1_FUNC: case P_G2_FUNC: n = "Motion"; break;
    case P_G1_ENV: case P_G2_ENV: n = "Envelope"; break;
    case P_G1_ERATE: case P_G2_ERATE: n = "Envelope rate"; break;
    case P_G1_CONTOUR: case P_G2_CONTOUR: n = "Contour"; break;
    case P_G1_ROUTE: case P_G2_ROUTE: n = "Route"; break;
    case P_EVOLVE: n = "Evolve"; break;
    case P_D1_PAT: case P_D2_PAT: case P_D3_PAT: case P_D4_PAT: n = drum_name(id - P_D1_PAT); break;
    case P_D1_ROT: case P_D2_ROT: case P_D3_ROT: case P_D4_ROT: n = drum_name(id - P_D1_ROT); break;
    case P_D1_LEN: case P_D2_LEN: case P_D3_LEN: case P_D4_LEN: n = drum_name(id - P_D1_LEN); break;
    case P_D1_MUTE: case P_D2_MUTE: case P_D3_MUTE: case P_D4_MUTE: n = drum_name(id - P_D1_MUTE); break;
    case P_RH_GHOST: n = "Ghosts"; break;
    case P_RH_FILL: n = "Fills"; break;
    case P_RH_SWLEV: n = "Swing on"; break;
    case P_KEY_PULL: n = "Keys pull the points"; break;
    case P_V1_ODDEVEN: case P_V2_ODDEVEN: n = "Odd / even"; break;
    case P_V1_PLUCK: case P_V2_PLUCK: n = "Pluck"; break;
    case P_V1_DEPTH: case P_V2_DEPTH: n = "Depth (undertones)"; break;
    case P_V1_CLARITY: case P_V2_CLARITY: n = "Clarity"; break;
    case P_G1_BAL: case P_G2_BAL: n = "Balance"; break;
    case P_D1_HPLUCK: case P_D2_HPLUCK: case P_D3_HPLUCK: case P_D4_HPLUCK: n = "Pluck"; break;
    case P_D1_OE: case P_D2_OE: case P_D3_OE: case P_D4_OE: n = drum_name(id - P_D1_OE); break;
    }
    if (id >= P_LFO1_RATE && id <= P_LFO6_SYNC) {
        static const char *const W[] = {" rate", " shape", " sync"};
        char t[4] = {(char)('1' + (id - P_LFO1_RATE) / 3), 0, 0, 0};
        text_cpy(b, "LFO ", max);
        text_cat(b, t);
        text_cat(b, W[(id - P_LFO1_RATE) % 3]);
        return;
    }
    text_cpy(b, n, max);
    if (b[0] >= 'a' && b[0] <= 'z')
        b[0] = (char)(b[0] - 32);
}

static float real_of(int id, float n)
{
    const ParamMeta *m = &g_param_meta[id];
    if (n < 0.0f) n = 0.0f;
    if (n > 1.0f) n = 1.0f;
    if (m->curve == CURVE_DISC) {
        int c = (int)m->hi, i = (int)(n * (float)c);
        return (float)(i >= c ? c - 1 : i);
    }
    return m->curve == CURVE_LOG ? m->lo * fm_exp2f(n * fm_log2f(m->hi / m->lo)) : m->lo + n * (m->hi - m->lo);
}

static void value_text(int id, float n, char *b, int max)
{
    const ParamMeta *m = &g_param_meta[id];
    float v = real_of(id, n);
    int i = (int)v;
    char t[40];
    t[0] = 0;
    if (m->curve == CURVE_DISC) {
        switch (id) {
        case P_SCALE:
            scale_get(i, &scale_tmp);
            text_cpy(t, scale_tmp.name, sizeof t);
            break;
        case P_ROTATE:
            scale_get_rot(pint(&S, P_SCALE), i, &scale_tmp);
            text_cpy(t, scale_tmp.name, sizeof t);
            break;
        case P_SURFACE: text_cpy(t, PICK(SURF_N, i), sizeof t); break;
        case P_OCTAVES: int_text(i + 1, t); text_cat(t, i ? " octaves" : " octave"); break;
        case P_G1_FUNC: case P_G2_FUNC: text_cpy(t, gen_name(i), sizeof t); break;
        case P_G1_ENV: case P_G2_ENV: text_cpy(t, env_name(i), sizeof t); break;
        case P_G1_ERATE: case P_G2_ERATE: {
            text_cpy(t, erate_name(i), sizeof t);
        } break;
        case P_DR_ON: text_cpy(t, i ? "On" : "Off", sizeof t); break;
        case P_DR_LAYER: text_cpy(t, drum_layer_name(i), sizeof t); break;
        case P_DR_SHAPE: { static const char *const SN[] = {"None", "Layer 1", "Layer 2"}; text_cpy(t, PICK(SN, i), sizeof t); } break;
        case P_D1_SRC: case P_D2_SRC: case P_D3_SRC: case P_D4_SRC: text_cpy(t, drum_src_name(i), sizeof t); break;
        case P_G1_ROUTE: case P_G2_ROUTE: {
            int l = id >= P_V2_CENTER;
            static const char *const RS[NUM_ROUTES] = {"Plays", "Moves ", "Adds to ", "Multiplies "};
            text_cpy(t, RS[i < NUM_ROUTES ? i : 0], sizeof t);
            if (i != ROUTE_PLAY) text_cat(t, l ? "1" : "2");
            if (l == 0 && i != ROUTE_PLAY && pint(&S, P_G2_ROUTE) != ROUTE_PLAY) text_cpy(t, "Plays (2 routes)", sizeof t);
        } break;
        case P_G1_POINTS: case P_G2_POINTS: int_text(i + 1, t); text_cat(t, i ? " points" : " point"); break;
        case P_V1_REG: case P_V2_REG: text_cpy(t, PICK(REG_N, i), sizeof t); break;
        case P_RH_ENGINE: text_cpy(t, rh_engine_name(i), sizeof t); break;
        case P_RH_ASSIGN: text_cpy(t, rh_assign_name(i), sizeof t); break;
        case P_RH_SELRATE: text_cpy(t, selrate_name(i), sizeof t); break;
        case P_RH_COUNT: int_text(i + 2, t); text_cat(t, " events"); break;
        case P_RV_FREEZE: case P_RV_TUNED: text_cpy(t, i ? "On" : "Off", sizeof t); break;
        case P_DRY: text_cpy(t, i ? "Shape" : "Effects", sizeof t); break;
        case P_LFO1_SHAPE: case P_LFO2_SHAPE: case P_LFO3_SHAPE: case P_LFO4_SHAPE: case P_LFO5_SHAPE:
        case P_LFO6_SHAPE: text_cpy(t, PICK(LFO_N, i), sizeof t); break;
        case P_LFO1_SYNC: case P_LFO2_SYNC: case P_LFO3_SYNC: case P_LFO4_SYNC: case P_LFO5_SYNC:
        case P_LFO6_SYNC: text_cpy(t, i ? "Tempo" : "Free", sizeof t); break;
        case P_D1_PAT: case P_D2_PAT: case P_D3_PAT: case P_D4_PAT: {  /* (the strip under it shows the steps) */
            int d = id - P_D1_PAT, k;
            if (!i) { text_cpy(t, "Linear", sizeof t); break; }
            const PatLine *L = &PAT_LIB[PAT_OFF[d] + i - 1];
            int_text(i, t); text_cat(t, " of "); { char u[6]; int_text(PAT_CNT[d], u); text_cat(t, u); }
            for (k = 0; k < 1; k++) if (L->len == 32) text_cat(t, ", 2 bars");
        } break;
        case P_D1_ROT: case P_D2_ROT: case P_D3_ROT: case P_D4_ROT:
            if (!i) text_cpy(t, "On time", sizeof t);
            else { text_cpy(t, "+", sizeof t); { char u[6]; int_text(i, u); text_cat(t, u); } text_cat(t, i == 1 ? " 16th" : " 16ths"); }
            break;
        case P_D1_LEN: case P_D2_LEN: case P_D3_LEN: case P_D4_LEN:
            if (!i) text_cpy(t, "Its own", sizeof t);
            else { int_text(i + 1, t); text_cat(t, " steps"); }
            break;
        case P_D1_MUTE: case P_D2_MUTE: case P_D3_MUTE: case P_D4_MUTE: text_cpy(t, pmute_name(i), sizeof t); break;
        case P_RH_SWLEV: text_cpy(t, i ? "16ths" : "8ths", sizeof t); break;
        case P_KEY_PULL: text_cpy(t, i ? "On" : "Off: notes only", sizeof t); break;
        default: {
            char u[8];
            int_text(i + 1, t);
            {
                text_cat(t, " of ");
                int_text((int32_t)m->hi, u);
                text_cat(t, u);
            }
        }
        }
    } else if (id == P_G1_A || id == P_G1_B || id == P_G2_A || id == P_G2_B) {
        int l = id >= P_V2_CENTER;
        gen_ab_text(pint(&S, LP(l, P_G1_FUNC)), id == LP(l, P_G1_B), n, t, sizeof t);
    } else if (id == P_V1_CENTER || id == P_V2_CENTER) {          /* the partial the bank is centred on */
        float c = 1.0f + 11.0f * fm_powf(n, 1.6f);
        text_cpy(t, "Partial ", sizeof t); { char u[8]; fix_text(c, 1, u); text_cat(t, u); }
    } else if (id == P_V1_STRIDE || id == P_V2_STRIDE) {
        float st = 1.0f + 3.0f * n;
        if (st < 1.05f) text_cpy(t, "Every partial", sizeof t);
        else if (st > 1.95f && st < 2.05f) text_cpy(t, "Odd partials", sizeof t);
        else { text_cpy(t, "Every ", sizeof t); { char u[8]; fix_text(st, 1, u); text_cat(t, u); } }
    } else if (id == P_V1_TONE || id == P_V2_TONE) {              /* where the partials stop */
        float hz = 250.0f * fm_exp2f(6.0f * n);
        if (hz < 1000.0f) { int_text((int32_t)(hz + 0.5f), t); text_cat(t, " Hz"); }
        else { fix_text(hz * 0.001f, 1, t); text_cat(t, " kHz"); }
    } else if (id == P_V1_ODDEVEN || id == P_V2_ODDEVEN || (id >= P_D1_OE && id <= P_D4_OE)) {   /* hollow .. as is .. even */
        int pc = (int)((n < .5f ? .5f - n : n - .5f) * 200.0f + 0.5f);
        if (pc < 3) text_cpy(t, (id >= P_D1_OE) ? "None" : "As is", sizeof t);
        else { text_cpy(t, n < .5f ? "Hollow " : "Even ", sizeof t); { char u[6]; int_text(pc, u); text_cat(t, u); } text_cat(t, "%"); }
    } else if (id == P_V1_PLUCK || id == P_V2_PLUCK || (id >= P_D1_HPLUCK && id <= P_D4_HPLUCK)) {   /* its time */
        if (n <= .01f) text_cpy(t, (id >= P_D1_HPLUCK) ? "Off: the layer's" : "Off", sizeof t);
        else {
            float ms = 1500.0f * fm_exp2f(-6.0f * n);
            if (ms < 1000.0f) { int_text((int32_t)(ms + 0.5f), t); text_cat(t, " ms"); }
            else { fix_text(ms * 0.001f, 1, t); text_cat(t, " s"); }
        }
    } else if (id >= P_D1_DENS && id < P_D1_DENS + NUM_DRUMS * DR_STRIDE && (id - P_D1_DENS) % DR_STRIDE == P_D1_BRIGHT - P_D1_DENS) {
        int pc = (int)((n < .5f ? .5f - n : n - .5f) * 200.0f + 0.5f);   /* (dark .. as is .. bright) */
        if (pc < 3) text_cpy(t, "As is", sizeof t);
        else { text_cpy(t, n < .5f ? "Dark " : "Bright ", sizeof t); { char u[6]; int_text(pc, u); text_cat(t, u); } text_cat(t, "%"); }
    } else if (id == P_RV_DAMP) {                    /* how long the top rings, of the rest's decay */
        text_cpy(t, "Top ", sizeof t); { char u[6]; int_text((int32_t)((1.f - .75f * n) * 100.0f + 0.5f), u); text_cat(t, u); }
        text_cat(t, "% as long");
    } else if (id == P_DR_NOTES) {                  /* (1.0.6) Follow .. Fixed */
        int pc = (int)(n * 100.0f + 0.5f);
        if (pc <= 1) text_cpy(t, "Follow", sizeof t);
        else if (pc >= 99) text_cpy(t, "Fixed (a kit)", sizeof t);
        else { char u[6]; int_text(pc, u); text_cpy(t, u, sizeof t); text_cat(t, "% fixed"); }
    } else if (id == P_G1_BAL || id == P_G2_BAL) {  /* (dark .. balanced .. bright) */
        int pc = (int)((n < .5f ? .5f - n : n - .5f) * 200.0f + 0.5f);
        if (pc < 3) text_cpy(t, "Balanced", sizeof t);
        else { text_cpy(t, n < .5f ? "Dark " : "Bright ", sizeof t); { char u[6]; int_text(pc, u); text_cat(t, u); } text_cat(t, "%"); }
    } else if (id == P_V1_CLARITY || id == P_V2_CLARITY) {
        if (n <= .001f) text_cpy(t, "Open", sizeof t);
        else { int_text((int32_t)(n * 100.0f + 0.5f), t); text_cat(t, "%"); }
    } else if (id == P_RH_SWING) {
        int pc = (int)(n * 100.0f + 0.5f);
        int_text(pc, t);
        text_cat(t, pc < 8 ? "%  straight" : pc > 58 && pc < 75 ? "%  triplet" : pc > 92 ? "%  hard" : "%");
    } else if (id == P_RH_THREES) {
        int pc = (int)(n * 100.0f + 0.5f);
        int_text(pc, t);
        text_cat(t, pc < 20 ? "%  twos" : pc > 80 ? "%  threes" : pc > 40 && pc < 60 ? "%  3 against 4" : "%");
    } else if (id == P_TEMPER) {
        int pc = (int)(n * 100.0f + 0.5f);
        if (pc < 3) text_cpy(t, "Just", sizeof t);
        else if (pc > 97) text_cpy(t, "Equal (12-tone)", sizeof t);
        else { int_text(pc, t); text_cat(t, "% toward equal"); }
    } else if (id == P_FLT_MORPH) {
        text_cpy(t, n < 0.17f ? "Low-pass" : n < 0.4f ? "Low > band" : n < 0.6f ? "Band-pass" : n < 0.83f ? "Band > high" : "High-pass", sizeof t);
    } else if (id >= P_D1_DENS && id <= P_D4_SNAP && (id - P_D1_DENS) % DR_STRIDE == P_D1_LEVEL - P_D1_DENS) {
        if (v < 0.001f) text_cpy(t, "Off", sizeof t);
        else { int db = (int)(6.0206f * fm_log2f(v) - 0.5f); int_text(db, t); text_cat(t, " dB"); }
    } else if (id >= P_D1_DENS && id <= P_D4_SNAP && (id - P_D1_DENS) % DR_STRIDE == P_D1_PLACE - P_D1_DENS) {
        int d = (id - P_D1_DENS) / DR_STRIDE;
        if (pint(&S, DP(d, P_D1_SRC)) >= DSRC_L1) {      /* following a layer: which point */
            const GenSt *g = &S.layer[pint(&S, DP(d, P_D1_SRC)) - DSRC_L1].g;
            int np = g->npts > 0 ? g->npts : 1;
            text_cpy(t, "Point ", sizeof t); { char u[6]; int_text((int)(v * (float)(np - 1) + .5f) + 1, u); text_cat(t, u); }
        } else {                                          /* the level of the hierarchy it prefers */
            static const char *const PL[] = {"Strongest", "Strong", "Middling", "Weak", "Weakest"};
            int k = (int)(v * 4.0f + 0.5f);
            text_cpy(t, PL[k < 0 ? 0 : k > 4 ? 4 : k], sizeof t);
        }
    } else if (id >= P_D1_DENS && id <= P_D4_SNAP && (id - P_D1_DENS) % DR_STRIDE == P_D1_VOICE - P_D1_DENS) {
        if (n < 0.04f) text_cpy(t, "Lowest note", sizeof t);       /* which of the layer's sounding notes it rides */
        else if (n > 0.96f) text_cpy(t, "Highest note", sizeof t);
        else { int_text((int)(n * 100.0f + 0.5f), t); text_cat(t, "% up the notes"); }
    } else if (id >= P_D1_DENS && id <= P_D4_SNAP && (id - P_D1_DENS) % DR_STRIDE == P_D1_CURVE - P_D1_DENS) {
        if (n < 0.04f) text_cpy(t, "Exponential", sizeof t);
        else if (n > 0.96f) text_cpy(t, "Straight", sizeof t);
        else { int_text((int)(n * 100.0f + 0.5f), t); text_cat(t, "% straight"); }
    } else if (id >= P_D1_DENS && id <= P_D4_SNAP && ((id - P_D1_DENS) % DR_STRIDE == P_D1_DECAY - P_D1_DENS || (id - P_D1_DENS) % DR_STRIDE == P_D1_ATTACK - P_D1_DENS
                                                      || (id - P_D1_DENS) % DR_STRIDE == P_D1_HOLD - P_D1_DENS)) {
        if (v < 0.01f && v >= 0.001f) { fix_text(v * 1000.0f, 1, t); text_cat(t, " ms"); }   /* (attacks: tenths of a ms) */
        else time_text(v, t);
    } else if (id == P_G1_RATE || id == P_G2_RATE) {
        num_text(v, t);
        text_cat(t, " a bar");
    } else if (id == P_ROOT) {
        int nn = (int)(v + 0.5f);
        char o[6];
        text_cpy(t, NOTE_N[nn % 12], sizeof t);
        int_text(nn / 12 - 1, o);
        text_cat(t, o);
    } else if (id == P_TEMPO) {
        int_text((int32_t)(v + 0.5f), t);
        text_cat(t, " BPM");
    } else if (m->lo == 0.0f && (m->hi == 1.0f || m->hi == 1.5f || m->hi == 1.1f)) {
        int_text((int32_t)(v * 100.0f + 0.5f), t);
        text_cat(t, "%");
    } else if (id == P_RV_DECAY || id == P_RV_PRE) {
        time_text(v, t);
    } else {
        num_text(v, t);
        if (id == P_FLT_CUT || (id >= P_LFO1_RATE && id <= P_LFO6_SYNC))
            text_cat(t, " Hz");
        else if (id == P_CMP_THRESH)
            text_cat(t, " dB");
        else if (id == P_CMP_ATT || id == P_CMP_REL)
            text_cat(t, " ms");
        else if (id == P_CMP_RATIO)
            text_cat(t, ":1");
    }
    if (t[0] >= 'a' && t[0] <= 'z')
        t[0] = (char)(t[0] - 32);        /* names from the engine are lower case */
    text_cpy(b, t, max);
}

/* ------------------------------------------------------------------ pages --- */
/* An item on a page is a parameter (an id), a setting (V_*) or nothing (-1). A page is a few sub-pages of four
 * items each, one item a knob: K1..K4, top to bottom. SELECT moves between the sub-pages. */
enum { V_THEME = 1000, V_OCT, V_MIDI_CH, V_MIDI_OUT, V_BT_ON, V_BT_DEV, V_BT_LINK, V_BT_AUTO, V_CAP_PAD, V_CAP_UNI,
       V_CAP_NODES, V_WS_OS, V_FB_ST, V_CPU2, V_PEAK, V_STYLE, V_CAP_HARM, V_KEYS,
       V_MX_ROUTE, V_MX_SRC, V_MX_DST, V_MX_DEPTH,
       V_MO_TUNE, V_MO_DRUMS, V_MO_SYNC, V_MO_FLOOR, V_MO_RATE, V_MO_LEVEL, V_MO_CH1, V_MO_CH2, V_MO_CHK, V_MO_CHD,
       V_MO_KIT1, V_MO_KIT2, V_MO_KIT3, V_MO_KIT4 };
typedef struct { const char *name; int16_t it[4]; } sub_t;
#define _ (-1)
static int sub_item(const sub_t *sb, int k)
{
    int it = sb->it[k];
    if ((it == P_G1_A || it == P_G1_B) && pint(&S, P_G1_FUNC) == GEN_SEQUENCE) return -1;
    if ((it == P_G2_A || it == P_G2_B) && pint(&S, P_G2_FUNC) == GEN_SEQUENCE) return -1;
    return it;
}
static const sub_t SUB_SCALE[] = {
    {"Tuning", {P_SCALE, P_ROTATE, P_ROOT, P_OCTAVES}},
    {"Temper & surface", {P_TEMPER, P_SURFACE, _, _}},
};
static const sub_t SUB_VOICE1[] = {
    {"Tone", {P_V1_TONE, P_V1_TILT, P_V1_SPIN, P_V1_WAVE}},
    {"Spectrum", {P_V1_CENTER, P_V1_BAND, P_V1_STRIDE, P_V1_PHASE}},
    {"Voice", {P_V1_BLOOM, P_V1_WIDTH, P_V1_REG, P_V1_LEVEL}},
    {"Colour", {P_V1_ODDEVEN, P_V1_PLUCK, P_V1_DEPTH, P_V1_CLARITY}},       /* (1.0.5) */
};
static const sub_t SUB_GEN1[] = {
    {"Motion", {P_G1_FUNC, P_G1_A, P_G1_B, P_G1_RATE}},
    {"Points", {P_G1_POINTS, P_G1_SPREAD, P_G1_REACH, P_G1_ENERGY}},
    {"Envelope", {P_G1_ENV, P_G1_ERATE, P_G1_CONTOUR, P_G1_ROUTE}},
    {"Path view", {P_PATH_SCALE, P_PATH_ROT, P_PATH_X, P_PATH_Y}},
    {"Notes", {P_G1_BAL, P_KEY_PULL, _, _}},     /* (1.0.5: Balance; the keys' pull) */
};
static const sub_t SUB_METER[] = {           /* the rhythm and the drums it plays */
    {"Groove", {P_RH_DENS, P_RH_SWING, P_RH_THREES, P_RH_FINE}},
    {"Patterns", {P_D1_PAT, P_D2_PAT, P_D3_PAT, P_D4_PAT}},       /* (1.0.5: each drum's line, or Linear) */
    {"Pattern feel", {P_RH_GHOST, P_RH_FILL, P_RH_SWLEV, _}},
    {"Drums", {P_D1_DENS, P_D2_DENS, P_D3_DENS, P_D4_DENS}},
    {"Levels", {P_D1_LEVEL, P_D2_LEVEL, P_D3_LEVEL, P_D4_LEVEL}},
    {"Mix", {P_DR_ON, P_DR_LAYER, P_DR_LEVEL, P_DR_ACCENT}},
    {"Kick", {P_D1_VOICE, P_D1_ATTACK, P_D1_DECAY, P_D1_CURVE}},
    {"Snare", {P_D2_VOICE, P_D2_ATTACK, P_D2_DECAY, P_D2_CURVE}},
    {"Hat", {P_D3_VOICE, P_D3_ATTACK, P_D3_DECAY, P_D3_CURVE}},
    {"Tom", {P_D4_VOICE, P_D4_ATTACK, P_D4_DECAY, P_D4_CURVE}},
    {"Rhythm", {P_TEMPO, P_RH_ENGINE, P_RH_FORM, P_RH_TURN}},
    {"Drum choice", {P_RH_ASSIGN, P_RH_SELRATE, P_RH_FEED, P_RH_COUNT}},
    {"Lock & motion", {P_RH_PULL, P_RH_MOTION, P_EVOLVE, P_RH_GRIP}},
    {"Hold", {P_D1_HOLD, P_D2_HOLD, P_D3_HOLD, P_D4_HOLD}},
    {"Place", {P_D1_PLACE, P_D2_PLACE, P_D3_PLACE, P_D4_PLACE}},
    {"Shape, duck, notes", {P_DR_SHAPE, P_DR_DEPTH, P_DR_DUCK, P_DR_NOTES}},
    {"Follow", {P_D1_SRC, P_D2_SRC, P_D3_SRC, P_D4_SRC}},
    {"Rotate", {P_D1_ROT, P_D2_ROT, P_D3_ROT, P_D4_ROT}},
    {"Length", {P_D1_LEN, P_D2_LEN, P_D3_LEN, P_D4_LEN}},
    {"Mutes", {P_D1_MUTE, P_D2_MUTE, P_D3_MUTE, P_D4_MUTE}},
    {"Kick sound", {P_D1_BRIGHT, P_D1_SETTLE, P_D1_SNAP, P_D1_HPLUCK}},
    {"Snare sound", {P_D2_BRIGHT, P_D2_SETTLE, P_D2_SNAP, P_D2_HPLUCK}},
    {"Hat sound", {P_D3_BRIGHT, P_D3_SETTLE, P_D3_SNAP, P_D3_HPLUCK}},
    {"Tom sound", {P_D4_BRIGHT, P_D4_SETTLE, P_D4_SNAP, P_D4_HPLUCK}},
    {"Odd / even", {P_D1_OE, P_D2_OE, P_D3_OE, P_D4_OE}},          /* (1.0.5: the hit's push on its note's colour) */
};
static const sub_t SUB_GEN2[] = {
    {"Motion", {P_G2_FUNC, P_G2_A, P_G2_B, P_G2_RATE}},
    {"Points", {P_G2_POINTS, P_G2_SPREAD, P_G2_REACH, P_G2_ENERGY}},
    {"Envelope", {P_G2_ENV, P_G2_ERATE, P_G2_CONTOUR, P_G2_ROUTE}},
    {"Notes", {P_G2_BAL, _, _, _}},             /* (1.0.5) */
};
static const sub_t SUB_LFO[] = {             /* the matrix first: a route at a time (K1), what (K2), where (K3), how much (K4) */
    {"Matrix", {V_MX_ROUTE, V_MX_SRC, V_MX_DST, V_MX_DEPTH}},
    {"LFO 1", {P_LFO1_RATE, P_LFO1_SHAPE, P_LFO1_SYNC, P_LFO_COUPLE}},
    {"LFO 2", {P_LFO2_RATE, P_LFO2_SHAPE, P_LFO2_SYNC, P_LFO_COUPLE}},
    {"LFO 3", {P_LFO3_RATE, P_LFO3_SHAPE, P_LFO3_SYNC, P_LFO_COUPLE}},
    {"LFO 4", {P_LFO4_RATE, P_LFO4_SHAPE, P_LFO4_SYNC, P_LFO_COUPLE}},
    {"LFO 5", {P_LFO5_RATE, P_LFO5_SHAPE, P_LFO5_SYNC, P_LFO_COUPLE}},
    {"LFO 6", {P_LFO6_RATE, P_LFO6_SHAPE, P_LFO6_SYNC, P_LFO_COUPLE}},
};
static const sub_t SUB_FX[] = {
    {"Fuzz", {P_FZ_MIX, P_FZ_SUSTAIN, P_FZ_TONE, _}},
    {"Filter", {P_FLT_CUT, P_FLT_RES, P_FLT_MORPH, _}},
    {"Delay", {P_DL_SEND, P_DL_TIME, P_DL_FB, P_DL_TONE}},
    {"Delay 2", {P_DL_TAP, P_DL_TO_RV, _, _}},
    {"Reverb", {P_RV_SEND, P_RV_SIZE, P_RV_DECAY, P_RV_DAMP}},
    {"Space", {P_RV_PRE, P_RV_MOD, P_RV_TUNED, P_RV_FREEZE}},
    {"Compressor", {P_CMP_THRESH, P_CMP_RATIO, P_CMP_ATT, P_CMP_REL}},
    {"Output", {P_CMP_MIX, P_CMP_PUMP, P_MASTER, P_DRY}},
};
static const sub_t SUB_VOICE2[] = {
    {"Tone", {P_V2_TONE, P_V2_TILT, P_V2_SPIN, P_V2_WAVE}},
    {"Spectrum", {P_V2_CENTER, P_V2_BAND, P_V2_STRIDE, P_V2_PHASE}},
    {"Voice", {P_V2_BLOOM, P_V2_WIDTH, P_V2_REG, P_V2_LEVEL}},
    {"Colour", {P_V2_ODDEVEN, P_V2_PLUCK, P_V2_DEPTH, P_V2_CLARITY}},       /* (1.0.5) */
};
enum { SS_GENERAL, SS_BT, SS_LOAD, SS_LOAD2, SS_KEYS };
static const sub_t SUB_SETUP[] = {
    {"General", {V_THEME, V_OCT, V_MIDI_CH, _}},
    {"Bluetooth audio", {V_BT_ON, V_BT_DEV, V_BT_LINK, V_BT_AUTO}},
    {"Load", {V_CAP_NODES, V_CAP_PAD, V_CAP_HARM, _}},
    {"Load cuts 2", {V_CPU2, V_PEAK, _, _}},
    {"Keys", {V_KEYS, _, _, _}},
    {"MIDI out", {V_MIDI_OUT, V_MO_TUNE, V_MO_DRUMS, V_MO_SYNC}},           /* (1.0.6) */
    {"MIDI out notes", {V_MO_FLOOR, V_MO_RATE, V_MO_LEVEL, _}},
    {"MIDI out channels", {V_MO_CH1, V_MO_CH2, V_MO_CHK, V_MO_CHD}},
    {"MIDI out drum notes", {V_MO_KIT1, V_MO_KIT2, V_MO_KIT3, V_MO_KIT4}},
};
#undef _

enum { PG_HOME, PG_SCALE, PG_ENGINE, PG_EXCITE, PG_SEQ, PG_ARP, PG_LFO, PG_FX, PG_PAD, PG_SETUP, PG_PATCH, NPG };
typedef struct {
    const char *name;
    int btn;
    uint32_t secs;                       /* the sections REC + its button rolls and SAVE + its button locks */
    const sub_t *sub;
    int nsub;
} page_t;
#define NS(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const page_t PAGES[NPG] = {
    {"Perform", B_HOME, ROLL_KNOBS, 0, 0},
    {"Scale", B_SEL, 1u << SEC_SCALE, SUB_SCALE, NS(SUB_SCALE)},
    {"Voice 1", B_ENV, 1u << SEC_VOICE1, SUB_VOICE1, NS(SUB_VOICE1)},
    {"Layer 1", B_EDIT, 1u << SEC_GEN1, SUB_GEN1, NS(SUB_GEN1)},
    {"Drums", B_SEQ, 1u << SEC_DRUMS | 1u << SEC_METER, SUB_METER, NS(SUB_METER)},
    {"Layer 2", B_ARP, 1u << SEC_GEN2, SUB_GEN2, NS(SUB_GEN2)},
    {"Modulation", B_LFO, 1u << SEC_LFO, SUB_LFO, NS(SUB_LFO)},
    {"Effects", B_FX, 1u << SEC_FX, SUB_FX, NS(SUB_FX)},
    {"Voice 2", B_GLO, 1u << SEC_VOICE2, SUB_VOICE2, NS(SUB_VOICE2)},
    {"Setup", B_HOME, 0, SUB_SETUP, NS(SUB_SETUP)},
    {"Patches", B_SAVE, 0, 0, 0},        /* a SAVE tap: the banks, the slots, the settings (draw_patches) */
};

/* the performance keys, on the white keys while PLAY is held */
enum { PK_S1, PK_S2, PK_S3, PK_S4, PK_TAP, PK_FILL, PK_FREEZE, PK_LATCH, PK_MBANK, PK_MPAD, PK_MSEQ, PK_MDRUM, PK_UNDO,
       PK_PANIC, PK_DKEYS, PK_NONE };
static const char *const PK_N[] = {"Scene 1", "Scene 2", "Scene 3", "Scene 4", "Tap", "Fill", "Freeze",
                                   "Latch", "Layer 1", "Layer 2", "Generators", "Drums", "Undo", "Panic", "Drum keys"};
static const uint8_t WK[16] = PLAT_WHITE_KEYS;

static struct {
    int page, sub[NPG], prev_page;      /* prev_page: where a SAVE tap on the Patches page goes back to */
    uint32_t btn, keys;
    uint32_t rec_t, home_t, save_t, play_t, rec_tap;   /* rec_tap: a REC tap waiting to see if a second follows */
    uint8_t rec_used, home_used, save_used, play_used;
    uint8_t oct_pending, oct_combo;     /* defer OCT +/- until release; both together form a gesture */
    char msg[2][40];
    uint32_t msg_until;
    uint8_t redraw;
    int pend_id;                         /* a structural edit waits for the knob to rest: each one is a rebuild */
    float pend_v;
    uint32_t pend_t;
    uint32_t edit_t[4];
    /* performance */
    uint8_t latch, mute[4], fill, keys_prev;
    uint8_t vocab_recording, vocab_targets, vocab_full;
    uint8_t roll_pend;                  /* 1: a style roll waits for PRESET to rest; 2: a seed for ALGO */
    uint32_t roll_t, seed_pend;
    float fill_dens, fill_rat;
    uint32_t tap_t[4];
    int ntap;
    uint32_t scene_down_t[NSCENE];       /* a scene key held a second stores the scene */
    uint8_t scene_stored[NSCENE];
    /* Bluetooth */
    int bt_sel;
    /* the matrix page: the route shown */
    int mx_sel;
    /* profiler */
    uint32_t pf_last[PROF_N], pf_blocks_last, pf_t;
    uint32_t pf_us[PROF_N], pf_peak_us, pf_peak_hold, pf_cpu;
} ui;

#define VOCAB_MAX 16
static uint8_t vocab_notes[NUM_LAYERS][VOCAB_MAX];
static uint8_t vocab_count[NUM_LAYERS];

static uint32_t rng_ui = 0x1234567u;
static uint32_t rnd(void)
{
    rng_ui ^= rng_ui << 13;
    rng_ui ^= rng_ui >> 17;
    rng_ui ^= rng_ui << 5;
    return rng_ui;
}

void app_say(const char *a, const char *b)
{
    char t[84];
    text_cpy(ui.msg[0], a ? a : "", sizeof ui.msg[0]);
    text_cpy(ui.msg[1], b ? b : "", sizeof ui.msg[1]);
    ui.msg_until = plat_ms() + 1400u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void vocab_note(int nd)
{
    if (!ui.vocab_recording || nd < 0 || nd >= MAX_NODES)
        return;
    for (int l = 0; l < NUM_LAYERS; l++) {
        uint8_t bit = (uint8_t)(1u << l);
        if (!(ui.vocab_targets & bit))
            continue;
        if (vocab_count[l] < VOCAB_MAX) {
            vocab_notes[l][vocab_count[l]++] = (uint8_t)nd;
        } else if (!(ui.vocab_full & bit)) {
            ui.vocab_full |= bit;
            app_say("Vocabulary full", l ? "Layer 2: 16 notes" : "Layer 1: 16 notes");
        }
    }
}

static void vocab_toggle(void)
{
    if (!ui.vocab_recording) {
        uint8_t targets = set.keys_to == 3 ? 0 :
                          set.keys_to == 2 ? 3 :
                          set.keys_to < NUM_LAYERS ? (uint8_t)(1u << set.keys_to) : 0;
        if (!targets) {
            app_say("Pitch vocabulary", "Select a synth layer, not drums");
            return;
        }
        if (q_w - q_r > QN - ((targets & 1u ? 1u : 0u) + (targets & 2u ? 1u : 0u))) {
            app_say("Vocabulary", "Command queue busy");
            return;
        }
        ui.vocab_targets = targets;
        ui.vocab_full = 0;
        for (int l = 0; l < NUM_LAYERS; l++) {
            if (targets & (1u << l)) {
                vocab_count[l] = 0;
                if (!cmd(C_VOCAB, (uint32_t)l, 0, 0.0f, 0)) {
                    app_say("Vocabulary", "Command queue busy");
                    return;
                }
            }
        }
        ui.vocab_recording = 1;
        app_say("Pitch vocabulary", targets == 3 ? "Recording both layers" :
                targets == 1 ? "Recording Layer 1" : "Recording Layer 2");
    } else {
        int any_notes = 0;
        uint32_t needed = 5u * ((ui.vocab_targets & 1u ? 1u : 0u) + (ui.vocab_targets & 2u ? 1u : 0u));
        if (q_w - q_r > QN - needed) {
            app_say("Vocabulary", "Command queue busy");
            return;
        }
        for (int l = 0; l < NUM_LAYERS; l++) {
            if (ui.vocab_targets & (1u << l)) {
                if (vocab_count[l]) any_notes = 1;
                for (int chunk = 0; chunk < 4; chunk++) {
                    uint32_t packed = 0;
                    for (int i = 0; i < 4; i++) {
                        uint32_t ix = (uint32_t)chunk * 4u + (uint32_t)i;
                        if (ix < vocab_count[l])
                            packed |= (uint32_t)vocab_notes[l][ix] << (i * 8);
                    }
                    if (!cmd(C_VOCAB_DATA, (uint32_t)l, (uint32_t)chunk, 0.0f, packed)) {
                        app_say("Vocabulary", "Command queue busy");
                        return;
                    }
                }
                uint32_t mask = 0;
                for (int i = 0; i < vocab_count[l]; i++)
                    mask |= 1u << vocab_notes[l][i];
                if (!cmd(C_VOCAB, (uint32_t)l, vocab_count[l], 0.0f, mask)) {
                    app_say("Vocabulary", "Command queue busy");
                    return;
                }
            }
        }
        ui.vocab_recording = 0;
        app_say("Pitch vocabulary", any_notes ? "Vocabulary active" : "No notes recorded");
    }
}

static float norm_of(int id)
{
    return ui.pend_id == id ? ui.pend_v : S.target[id];
}

/* ------------------------------------------------------------------ settings items --- */
static int bt_list(int i, char *name)    /* the device list: what a scan found, else the ones remembered */
{
    int st = bta_state();
    if (st == PLAT_BTA_FOUND || st == PLAT_BTA_SCANNING)
        return bta_found(i, name);
    return bta_known(i, name);
}

static int bt_count(void)
{
    char n[17];
    int i = 0;
    while (i < 8 && bt_list(i, n))
        i++;
    return i;
}

/* ------------------------------------------------------------------ the matrix page --- */
/* 24 routes, a source to a destination at a depth (-100..+100% of the destination's range; on a knob, of its turn).
 * The sources in the engine's order; the destinations: the four knobs, then the LFOs, the layers' generators, the
 * voices, the drums, the rhythm, the effects (every parameter the matrix may move but the master level). */
static const char *const MXS_N[] = {"None", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "Timbre knob",
                                    "Groove knob", "Space knob", "Motion knob", "Output level", "Layer 1 onsets",
                                    "Random (a bar)", "Path X", "Path Y", "Velocity", "Layer 1 motion", "Layer 2 motion"};
typedef char mxs_n_check[sizeof MXS_N / sizeof MXS_N[0] == NUM_SRC ? 1 : -1];
static uint8_t mx_dl[MOD_NUM_DST];
static int mx_ndl;

static void mx_list(void)
{
    static const uint8_t ORD[] = {SEC_LFO, SEC_GEN1, SEC_GEN2, SEC_VOICE1, SEC_VOICE2, SEC_DRUMS, SEC_METER, SEC_FX, SEC_SCALE};
    int k, i, n = 0;
    if (mx_ndl)
        return;
    for (k = 0; k < NUM_MACRO; k++)
        mx_dl[n++] = (uint8_t)(MOD_DST_KNOB + k);
    for (k = 0; k < (int)sizeof ORD; k++)
        for (i = 0; i < NUM_PARAMS; i++)
            if (g_param_meta[i].section == ORD[k] && g_param_meta[i].moddable && i != P_MASTER)
                mx_dl[n++] = (uint8_t)i;
    mx_ndl = n;
}

static int mx_index(int dst)
{
    int i;
    mx_list();
    for (i = 0; i < mx_ndl; i++)
        if (mx_dl[i] == dst)
            return i;
    return -1;
}

static void cat_lower(char *b, const char *t)
{
    char u[32];
    text_cpy(u, t, sizeof u);
    if (u[0] >= 'A' && u[0] <= 'Z')
        u[0] = (char)(u[0] + 32);
    text_cat(b, u);
}

/* a destination's name with its place: "Kick decay", "LFO 3 rate", "Layer 2 rate", "Space knob" */
static void mx_dst_name(int d, char *b, int max)
{
    char t[32];
    const char *pre = 0;
    if (d >= MOD_DST_KNOB && d < MOD_NUM_DST) {
        text_cpy(b, knob_name(d - MOD_DST_KNOB), max);
        text_cat(b, " knob");
        return;
    }
    if (d < 0 || d >= NUM_PARAMS) {
        text_cpy(b, "-", max);
        return;
    }
    param_name(d, t, sizeof t);
    switch (g_param_meta[d].section) {
    case SEC_VOICE1: pre = d == P_V1_LEVEL ? 0 : "Voice 1 "; break;
    case SEC_VOICE2: pre = d == P_V2_LEVEL ? 0 : "Voice 2 "; break;
    case SEC_GEN1: pre = d >= P_PATH_SCALE && d <= P_PATH_Y ? 0 : "Layer 1 "; break;
    case SEC_GEN2: pre = "Layer 2 "; break;
    case SEC_METER: pre = "Rhythm "; break;
    case SEC_DRUMS:
        {   /* a drum's own: its name, then what (the per-drum blocks, the patterns' and the hits' colour by fours) */
            int dn = d >= P_D1_DENS && d < P_D1_DENS + NUM_DRUMS * DR_STRIDE ? (d - P_D1_DENS) / DR_STRIDE
                   : d >= P_D1_PAT && d <= P_D4_MUTE ? (d - P_D1_PAT) % NUM_DRUMS
                   : d >= P_D1_OE && d <= P_D4_HPLUCK ? (d - P_D1_OE) % NUM_DRUMS : -1;
            if (dn >= 0) {
                text_cpy(b, drum_name(dn), max);
                text_cat(b, " ");
                cat_lower(b, g_param_meta[d].name);
                return;
            }
        }
        pre = d == P_DR_ACCENT ? "Drum " : 0;
        break;
    case SEC_LFO:
        if (d != P_LFO_COUPLE) {
            char u[4];
            int_text((d - P_LFO1_RATE) / (P_LFO2_RATE - P_LFO1_RATE) + 1, u);
            text_cpy(b, "LFO ", max);
            text_cat(b, u);
            text_cat(b, " rate");
            return;
        }
        break;
    }
    if (pre) {
        text_cpy(b, pre, max);
        cat_lower(b, t);
    } else
        text_cpy(b, t, max);
}

/* an edit to the route shown: sent to the engine, and written here so the screen (and the next turn) sees it at once
 * (depth, then destination, then source: an interrupt between them sees, for a tick, a route that could have been) */
static void mx_edit(int it, int32_t det)
{
    ModSlot m = S.slot[ui.mx_sel];
    int d = det > 0 ? 1 : -1;
    mx_list();
    if (it == V_MX_SRC) {
        int was = m.src;
        m.src = (uint8_t)clamp((int)m.src % NUM_SRC + d, 0, NUM_SRC - 1);
        if (!was && m.src) {             /* a new route: somewhere to go, and enough to hear */
            if (mx_index(m.dst) < 0)
                m.dst = mx_dl[0];
            if (m.depth == 0.0f)
                m.depth = 0.25f;
        }
    } else if (!m.src) {
        return;
    } else if (it == V_MX_DST) {
        int i = mx_index(m.dst);
        i = i < 0 ? 0 : clamp(i + (det > 1 || det < -1 ? 4 * d : d), 0, mx_ndl - 1);
        m.dst = mx_dl[i];
    } else {
        float v = m.depth + (float)det * (det > 1 || det < -1 ? 0.05f : 0.01f);
        v = fm_clampf(v, -1.0f, 1.0f);
        m.depth = (float)(int32_t)(v * 100.0f + (v < 0.0f ? -0.5f : 0.5f)) * 0.01f;   /* whole percents */
    }
    cmd(C_SLOT, (uint32_t)ui.mx_sel, (uint32_t)m.src << 8 | m.dst, m.depth, 0);
    S.slot[ui.mx_sel].depth = m.depth;
    S.slot[ui.mx_sel].dst = m.dst;
    BARRIER();
    S.slot[ui.mx_sel].src = m.src;
}

static void item_name(int it, char *b, int max)
{
    static const char *const N[] = {"Color theme", "Key octave", "MIDI channel", "Notes", "Bluetooth",
                                    "Device", "Connection", "Reconnect at power-on", "Layer 2 voices", "Oscillators a layer",
                                    "Layer 1 voices", "Fuzz 2x oversampling", "Reverb rate", "Second core",
                                    "Peak", "Style", "Partials", "Keys play", "Route", "Source", "Destination",
                                    "Depth", "Tuning", "Drums", "Knobs and clock", "Note floor", "Notes a second",
                                    "Level", "Layer 1 channel", "Layer 2 channel", "Keys channel", "Drums channel",
                                    "Kick note", "Snare note", "Hat note", "Tom note"};
    if (it >= V_THEME)
        text_cpy(b, N[it - V_THEME], max);
    else if (it >= 0)
        param_name(it, b, max);
    else
        b[0] = 0;
}

/* the item's value as text, and where its bar sits (0..1; -1: no bar) */
static float item_value(int it, char *b, int max)
{
    char t[24];
    b[0] = 0;
    if (it >= 0 && it < NUM_PARAMS) {
        value_text(it, norm_of(it), b, max);
        return norm_of(it);
    }
    switch (it) {
    case V_THEME: text_cpy(b, THEMES[set.theme % NTHEMES].name, max); return (float)set.theme / (float)(NTHEMES - 1);
    case V_OCT: int_text(set.oct, t); text_cpy(b, set.oct > 0 ? "+" : "", max); text_cat(b, t); return (float)(set.oct + 3) / 6.0f;
    case V_MIDI_CH:
        if (!set.midi_ch)
            text_cpy(b, "Omni", max);
        else
            int_text(set.midi_ch, b);
        return (float)set.midi_ch / 16.0f;
    case V_MIDI_OUT: text_cpy(b, set.midi_out == 2 ? "Keys + layers" : set.midi_out ? "Keys" : "Off", max); return -1.0f;
    case V_MO_TUNE: text_cpy(b, set.mo_mpe ? "MPE (just)" : "Standard", max); return -1.0f;
    case V_MO_DRUMS: text_cpy(b, set.mo_drums == 2 ? "Pitched" : set.mo_drums ? "Kit" : "Off", max); return -1.0f;
    case V_MO_SYNC: {
        static const char *const SY[] = {"Off", "Knobs", "Clock", "Knobs + clock"};
        text_cpy(b, SY[set.mo_sync & 3u], max);
        return -1.0f;
    }
    case V_MO_FLOOR: text_cpy(b, "-", max); int_text(set.mo_floor, t); text_cat(b, t); text_cat(b, " dB");
        return (float)(set.mo_floor - 12) / 24.0f;
    case V_MO_RATE:
        if (set.mo_rate >= 5)
            text_cpy(b, "No cap", max);
        else {
            int_text((int32_t)MO_RATE[set.mo_rate], b);
            text_cat(b, " a layer");
        }
        return (float)set.mo_rate / 5.0f;
    case V_MO_LEVEL: text_cpy(b, set.mo_level ? (set.mo_mpe ? "On: pressure" : "On: aftertouch") : "Off", max); return -1.0f;
    case V_MO_CH1: case V_MO_CH2: case V_MO_CHK: case V_MO_CHD:
        if (set.mo_mpe && it != V_MO_CHD) {
            text_cpy(b, "MPE: 2-15", max);
            return -1.0f;
        }
        if (set.mo_mpe) {
            text_cpy(b, set.mo_drums == 2 ? "MPE: 2-15" : "16 (MPE)", max);
            return -1.0f;
        }
        int_text(set.mo_ch[it - V_MO_CH1] + 1, b);
        return (float)set.mo_ch[it - V_MO_CH1] / 15.0f;
    case V_MO_KIT1: case V_MO_KIT2: case V_MO_KIT3: case V_MO_KIT4: {
        static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        int n = set.mo_kit[it - V_MO_KIT1];
        int_text(n, b);
        text_cat(b, " (");
        text_cat(b, NN[n % 12]);
        int_text(n / 12 - 1, t);
        text_cat(b, t);
        text_cat(b, ")");
        return (float)n / 127.0f;
    }
    case V_BT_ON: text_cpy(b, set.bt_on ? "On" : "Off", max); return -1.0f;
    case V_BT_DEV: {
        char n[17];
        if (bt_list(ui.bt_sel, n))
            text_cpy(b, n, max);
        else
            text_cpy(b, set.bt_on ? "None: REC scans" : "Bluetooth is off", max);
        return -1.0f;
    }
    case V_BT_LINK: text_cpy(b, bta_state_text(bta_state()), max); return -1.0f;
    case V_BT_AUTO: text_cpy(b, set.bt_auto ? "On" : "Off", max); return -1.0f;
    case V_CAP_PAD: int_text(set.cap_pad, b); text_cat(b, " of 8"); return (float)set.cap_pad / (float)MAX_VOICES;
    case V_CAP_UNI: {                       /* the oscillator cap: 0 off, 1 the default (64), else 8 x the value */
        int c = set.cap_uni == 0 ? 0 : set.cap_uni == 1 ? 64 : 8 * set.cap_uni;
        if (!c) { text_cpy(b, "No cap", max); return 1.0f; }
        int_text(c, b); text_cat(b, c == 64 ? " (default)" : ""); return (float)(c - 48) / 56.0f; }
    case V_CAP_NODES: int_text(set.cap_nodes, b); text_cat(b, " of 8"); return (float)set.cap_nodes / (float)MAX_VOICES;
    case V_KEYS: text_cpy(b, PICK(KEYS_N, set.keys_to), max); return -1.0f;
    case V_CAP_HARM: int_text(set.cap_harm, b); text_cat(b, " of 16"); return (float)set.cap_harm / (float)MAX_HARM;
    case V_WS_OS: text_cpy(b, set.ws_os ? "On" : "Off (cheaper)", max); return -1.0f;
    case V_FB_ST: text_cpy(b, set.rv_full ? "Full" : "Half (cheaper)", max); return -1.0f;
    case V_CPU2:                         /* (1.0.5: a status, not a control: the second core is always used) */
        text_cpy(b, plat_cpu2_barred() ? "Given up: one core" : !plat_cpu2_on() ? "Unavailable" : "Layer 2 on core 2", max);
        return -1.0f;
    case V_PEAK: {
        int_text((int32_t)(ui.pf_peak_hold * 100u / BUDGET_US), b);
        text_cat(b, "%  (turn: reset)");
        return -1.0f;
    }
    case V_STYLE: text_cpy(b, STYLE_N[S.style % NUM_STYLES], max); return (float)S.style / (float)(NUM_STYLES - 1);
    case V_MX_ROUTE:
        int_text(ui.mx_sel + 1, b);
        text_cat(b, S.slot[ui.mx_sel].src ? " of 24" : " of 24, empty");
        return (float)ui.mx_sel / (float)(MOD_SLOTS - 1);
    case V_MX_SRC: text_cpy(b, MXS_N[S.slot[ui.mx_sel].src % NUM_SRC], max); return -1.0f;
    case V_MX_DST:
        if (!S.slot[ui.mx_sel].src)
            text_cpy(b, "-", max);
        else
            mx_dst_name(S.slot[ui.mx_sel].dst, b, max);
        return -1.0f;
    case V_MX_DEPTH: {
        float v = S.slot[ui.mx_sel].depth;
        int32_t pc = (int32_t)(v * 100.0f + (v < 0.0f ? -0.5f : 0.5f));
        if (!S.slot[ui.mx_sel].src) {
            text_cpy(b, "-", max);
            return -1.0f;
        }
        text_cpy(b, pc > 0 ? "+" : "", max);
        int_text(pc, t);
        text_cat(b, t);
        text_cat(b, "%");
        return fm_clampf((v + 1.0f) * 0.5f, 0.0f, 1.0f);
    }
    }
    return -1.0f;
}

static void item_say(int it)
{
    char n[40], v[40];
    item_name(it, n, sizeof n);
    item_value(it, v, sizeof v);
    app_say(n, v);
}

static void save_settings(void)
{
    if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0)
        set_dirty = 0;
}

void app_say(const char *a, const char *b);
static void keep_settings(void)          /* SAVE + OCT+, or K4 on the Patches page */
{
    save_settings();
    app_say(set_dirty ? "Save failed" : "Settings kept", set_dirty ? "settings" : "theme, MIDI, Bluetooth, keys, load");
}

static void set_cap(uint32_t which, int32_t v)
{
    cmd(C_CAP, which, (uint32_t)v, 0.0f, 0);
    switch (which) {
    case CAP_PAD: set.cap_pad = (uint8_t)v; break;
    case CAP_UNI: set.cap_uni = (uint8_t)v; break;
    case CAP_NODES: set.cap_nodes = (uint8_t)v; break;
    case CAP_HARM: set.cap_harm = (uint8_t)v; break;
    case CAP_WSOS: set.ws_os = (uint8_t)v; break;
    case CAP_FBST: set.rv_full = (uint8_t)v; break;
    case CAP_CPU2: set.cpu2 = (uint8_t)v; break;
    }
    set_dirty = 1;
}

static void fill_patch(patch_t *p);
static void roll(uint32_t mask, const char *what);

static void bt_power(int on)
{
    set.bt_on = (uint8_t)on;
    set_dirty = 1;
    bta_on(on);
    if (!on)
        ui.bt_sel = 0;
}

/* a link being made (or made): another connect now would page again and end it */
static int bt_busy(void)
{
    int st = bta_state();
    return st == PLAT_BTA_PAGING || st == PLAT_BTA_LINKED || st == PLAT_BTA_SETUP || st == PLAT_BTA_STREAMING;
}

static void bt_connect(void)             /* K3 right: once (the knob's further detents do nothing while it connects) */
{
    int st = bta_state();
    if (!set.bt_on)
        bt_power(1);
    if (bt_busy())
        return;
    if (st == PLAT_BTA_FOUND || st == PLAT_BTA_SCANNING) {
        bta_pick(ui.bt_sel);
        ui.bt_sel = 0;                   /* (the device picked is now the first remembered one, the list shown from here) */
    } else
        bta_reconnect(ui.bt_sel);
}

static void edit_param(int id, int32_t det)
{
    const ParamMeta *m = &g_param_meta[id];
    float cur = norm_of(id), nv;
    if (m->curve == CURVE_DISC) {
        int c = (int)m->hi, k = (int)(cur * (float)c);
        if (k >= c)
            k = c - 1;
        k = clamp(k + det, 0, c - 1);
        nv = ((float)k + 0.5f) / (float)c;
    } else if (id == P_TEMPO) {          /* whole BPM: 1 a detent, 2 in a fast turn */
        float span = m->hi - m->lo, bpm = m->lo + cur * span;
        bpm = (float)(int32_t)(bpm + 0.5f) + (float)det * (det > 1 || det < -1 ? 2.0f : 1.0f);
        nv = fm_clampf((bpm - m->lo) / span, 0.0f, 1.0f);
    } else {
        float step = det > 1 || det < -1 ? 0.03f : 0.01f;   /* a fast turn goes further */
        nv = fm_clampf(cur + (float)det * step, 0.0f, 1.0f);
    }
    if (m->structural) {
        if (ui.pend_id >= 0 && ui.pend_id != id)
            cmd(C_PARAM, 0, (uint32_t)ui.pend_id, ui.pend_v, 0);
        ui.pend_id = id;
        ui.pend_v = nv;
        ui.pend_t = plat_ms();
    } else {
        cmd(C_PARAM, 0, (uint32_t)id, nv, 0);
        S.target[id] = nv;               /* (the render writes the same: the screen shows it at once) */
    }
    scene_cur = 0xFF;
}

static void edit_item(int it, int32_t det)
{
    int d = det > 0 ? 1 : -1;
    if (it < 0)
        return;
    if (it < NUM_PARAMS) {
        edit_param(it, det);
        item_say(it);
        return;
    }
    switch (it) {
    case V_THEME:
        set.theme = (uint8_t)((set.theme + NTHEMES + d) % NTHEMES);
        theme_apply(set.theme);
        set_dirty = 1;
        break;
    case V_OCT: set.oct = (int8_t)clamp(set.oct + d, -3, 3); set_dirty = 1; break;
    case V_MIDI_CH: set.midi_ch = (uint8_t)clamp(set.midi_ch + d, 0, 16); set_dirty = 1; break;
    case V_MIDI_OUT: mo_settings_changed(); set.midi_out = (uint8_t)clamp(set.midi_out + d, 0, 2); set_dirty = 1; break;
    case V_MO_TUNE:
        if ((d > 0) != set.mo_mpe) {
            mo_settings_changed();
            set.mo_mpe = d > 0;
            mo_mode_set(set.mo_mpe);
            set_dirty = 1;
        }
        break;
    case V_MO_DRUMS: mo_settings_changed(); set.mo_drums = (uint8_t)clamp(set.mo_drums + d, 0, 2); set_dirty = 1; break;
    case V_MO_SYNC: set.mo_sync = (uint8_t)clamp(set.mo_sync + d, 0, 3); set_dirty = 1; break;
    case V_MO_FLOOR: set.mo_floor = (uint8_t)clamp(set.mo_floor + 3 * d, 12, 36); set_dirty = 1; break;
    case V_MO_RATE: set.mo_rate = (uint8_t)clamp(set.mo_rate + d, 0, 5); set_dirty = 1; break;
    case V_MO_LEVEL: set.mo_level = d > 0; set_dirty = 1; break;
    case V_MO_CH1: case V_MO_CH2: case V_MO_CHK: case V_MO_CHD:
        if (set.mo_mpe)
            break;
        mo_settings_changed();
        set.mo_ch[it - V_MO_CH1] = (uint8_t)clamp(set.mo_ch[it - V_MO_CH1] + d, 0, 15);
        set_dirty = 1;
        break;
    case V_MO_KIT1: case V_MO_KIT2: case V_MO_KIT3: case V_MO_KIT4:
        mo_settings_changed();
        set.mo_kit[it - V_MO_KIT1] = (uint8_t)clamp(set.mo_kit[it - V_MO_KIT1] + d, 0, 127);
        set_dirty = 1;
        break;
    case V_BT_ON: bt_power(d > 0); break;
    case V_BT_DEV: ui.bt_sel = clamp(ui.bt_sel + d, 0, bt_count() > 0 ? bt_count() - 1 : 0); break;
    case V_BT_LINK:
        if (d > 0)
            bt_connect();
        else
            bta_hangup();
        break;
    case V_BT_AUTO: set.bt_auto = d > 0; set_dirty = 1; break;
    case V_CAP_PAD: set_cap(CAP_PAD, clamp(set.cap_pad + 2 * d, 4, MAX_VOICES)); break;
    case V_CAP_UNI: {
        int p = set.cap_uni == 0 ? 13 : set.cap_uni == 1 ? 8 : set.cap_uni;   /* 48 .. 96, then no cap */
        p = clamp(p + d, 6, 13);
        set_cap(CAP_UNI, p == 13 ? 0 : p);
        break; }
    case V_CAP_NODES: set_cap(CAP_NODES, clamp(set.cap_nodes + 2 * d, 4, MAX_VOICES)); break;
    case V_KEYS: set.keys_to = (uint8_t)clamp(set.keys_to + d, 0, 3); cmd(C_KEYS, set.keys_to, 0, 0.0f, 0); set_dirty = 1; break;
    case V_CAP_HARM: set_cap(CAP_HARM, clamp(set.cap_harm + 2 * d, 4, MAX_HARM)); break;
    case V_WS_OS: set_cap(CAP_WSOS, d > 0); break;
    case V_FB_ST: set_cap(CAP_FBST, d > 0); break;
    case V_CPU2: break;                  /* (a status: turning it changes nothing. Off, the first core carried both layers:
                                            twice its load, crackling) */
    case V_PEAK: ui.pf_peak_hold = 0; break;
    case V_MX_ROUTE: ui.mx_sel = clamp(ui.mx_sel + d, 0, MOD_SLOTS - 1); break;
    case V_MX_SRC: case V_MX_DST: case V_MX_DEPTH: mx_edit(it, det); break;
    case V_STYLE:
        set.style = (uint8_t)clamp(set.style + d, 0, NUM_STYLES - 1);
        cmd(C_STYLE, set.style, 0, 0.0f, 0);
        set_dirty = 1;
        break;
    }
    item_say(it);
}

/* ------------------------------------------------------------------ patches, scenes, undo --- */
static void fill_patch(patch_t *p)
{
    memset(p, 0, sizeof *p);
    p->magic = PATCH_MAGIC;
    p->nparams = NUM_PARAMS;
    p->nslots = MOD_SLOTS;
    p->seed = S.seed;
    p->style = (uint8_t)S.style;
    memcpy(p->p, S.target, sizeof p->p);    /* the values set (a glide in progress, at its end) */
    if (ui.pend_id >= 0)
        p->p[ui.pend_id] = ui.pend_v;
    memcpy(p->slot, S.slot, sizeof p->slot);
    memcpy(p->kpath, S.kpath, sizeof p->kpath);
    memcpy(p->macro, S.macro, sizeof p->macro);
    memcpy(p->locked, S.locked, sizeof p->locked);
    memcpy(p->scene, scene, sizeof p->scene);
    memcpy(p->scene_macro, scene_macro, sizeof p->scene_macro);
    p->scene_set = scene_set;
}

static int load_from(const patch_t *src, int whole)
{
    if (pbuf_busy)
        return 0;
    if (src != &pbuf)
        memcpy(&pbuf, src, sizeof pbuf);
    pbuf_busy = 1;
    BARRIER();
    if (!cmd(C_LOAD, (uint32_t)whole, 0, 0.0f, 0)) {
        pbuf_busy = 0;
        return 0;
    }
    return 1;
}

static void patch_save(int slot)
{
    char t[24] = "Slot ", n[8];
    if (pbuf_busy) {
        app_say("Busy", "try again");
        return;
    }
    fill_patch(&pbuf);
    slot_name(slot, n);
    text_cat(t, n);
    if (plat_store_save(patch_obj(slot), &pbuf, sizeof pbuf) == 0) {
        patch_here |= 1ull << slot;
        set.last_patch = (int8_t)slot;
        save_settings();
        app_say("Patch saved", t);
    } else {
        app_say("Save failed", t);
    }
}

static void patch_load(int slot)
{
    char t[24] = "Slot ", n[8];
    slot_name(slot, n);
    text_cat(t, n);
    if (pbuf_busy) {
        app_say("Busy", "try again");
        return;
    }
    if (!patch_read(slot, &pbuf)) {
        app_say("Empty", t);
        return;
    }
    scenes_from(&pbuf);
    if (!load_from(&pbuf, 1))
        return;
    set.last_patch = (int8_t)slot;
    set_dirty = 1;
    app_say("Patch loaded", t);
}

static void undo_snapshot(void)
{
    fill_patch(&undo_buf);
    undo_ok = 1;
}

static void undo(void)
{
    patch_t *t = &pbuf;
    if (!undo_ok) {
        app_say("Nothing to undo", "");
        return;
    }
    if (pbuf_busy)
        return;
    {   /* swap: a second undo is a redo */
        static patch_t cur __attribute__((section(".pool")));
        fill_patch(&cur);
        memcpy(t, &undo_buf, sizeof *t);
        memcpy(&undo_buf, &cur, sizeof cur);
    }
    load_from(&pbuf, 0);
    app_say("Undo", "the patch before the last roll");
}

static void scene_store(int k)
{
    int i;
    char t[8];
    for (i = 0; i < NUM_PARAMS; i++)
        scene[k][i] = (uint16_t)(fm_clampf(S.target[i], 0.0f, 1.0f) * 65535.0f + 0.5f);
    for (i = 0; i < NUM_MACRO; i++)
        scene_macro[k][i] = (int8_t)(S.macro[i] * 127.0f);
    scene_set |= (uint8_t)(1u << k);
    scene_cur = (uint8_t)k;
    ui.scene_stored[k] = 1;
    int_text(k + 1, t);
    app_say("Scene stored", t);
}

static void scene_recall(int k)
{
    int i;
    char t[8];
    int_text(k + 1, t);
    if (!(scene_set >> k & 1u)) {
        app_say("Scene empty", "hold its key to store");
        return;
    }
    if (scene_busy)
        return;
    for (i = 0; i < NUM_PARAMS; i++)
        scene_buf[i] = (float)scene[k][i] * (1.0f / 65535.0f);
    for (i = 0; i < NUM_MACRO; i++)
        scene_mbuf[i] = (float)scene_macro[k][i] * (1.0f / 127.0f);
    scene_busy = 1;
    BARRIER();
    if (!cmd(C_SCENE, 0, 0, 1.2f, 0)) {
        scene_busy = 0;
        return;
    }
    scene_cur = (uint8_t)k;
    app_say("Scene", t);
}

static void roll(uint32_t mask, const char *what)
{
    uint32_t seed = rnd() % 100000u;
    float morph = S.style == STYLE_WILD ? 0.3f : 1.5f;
    char t[16] = "#";
    undo_snapshot();
    ui.fill = 0;                         /* (a fill held through a roll ends without restoring the old pattern) */
    cmd(C_ROLL, 0, mask, morph, seed);
    rolls++;
    if ((mask & ROLL_SOUND) == ROLL_SOUND) {
        set.seed = seed;
        set.last_patch = -1;
        set_dirty = 1;
    }
    scene_cur = 0xFF;
    int_text((int32_t)seed, t + 1);
    app_say(what, t);
}

static void tap_tempo(void)
{
    uint32_t now = plat_ms();
    if (ui.ntap && now - ui.tap_t[(ui.ntap - 1) & 3] > 2000u)
        ui.ntap = 0;                     /* a pause starts a new count */
    ui.tap_t[ui.ntap & 3] = now;
    ui.ntap++;
    if (ui.ntap >= 2) {
        int n = ui.ntap > 4 ? 4 : ui.ntap;
        uint32_t span = now - ui.tap_t[(ui.ntap - n) & 3];
        float bpm = 60000.0f * (float)(n - 1) / (float)(span ? span : 1u);
        char t[16];
        {   const ParamMeta *tm = &g_param_meta[P_TEMPO];   /* (the tempo's own range) */
            bpm = fm_clampf(bpm, tm->lo, tm->hi);
            cmd(C_PARAM, 0, P_TEMPO, (bpm - tm->lo) / (tm->hi - tm->lo), 0);
            S.target[P_TEMPO] = (bpm - tm->lo) / (tm->hi - tm->lo);
        }
        int_text((int32_t)(bpm + 0.5f), t);
        text_cat(t, " BPM");
        app_say("Tap tempo", t);
    } else {
        app_say("Tap tempo", "keep tapping");
    }
}

static void fill(int on)
{
    if (on && !ui.fill) {
        ui.fill_dens = S.target[P_G1_ENERGY];
        ui.fill_rat = S.target[P_G2_ENERGY];
        cmd(C_PARAM, 0, P_G1_ENERGY, fm_minf(1.0f, ui.fill_dens + 0.4f), 0);
        cmd(C_PARAM, 0, P_G2_ENERGY, fm_minf(1.0f, ui.fill_rat + 0.3f), 0);
        ui.fill = 1;
    } else if (!on && ui.fill) {
        cmd(C_PARAM, 0, P_G1_ENERGY, ui.fill_dens, 0);
        cmd(C_PARAM, 0, P_G2_ENERGY, ui.fill_rat, 0);
        ui.fill = 0;
    }
}

static void perf_key(int pk, int down)
{
    if (pk >= PK_S1 && pk <= PK_S4) {
        if (down) {
            ui.scene_down_t[pk] = plat_ms() | 1u;
            ui.scene_stored[pk] = 0;
        } else {
            if (!ui.scene_stored[pk])
                scene_recall(pk);
            ui.scene_down_t[pk] = 0;
        }
        return;
    }
    if (pk == PK_FILL) {
        fill(down);
        if (down)
            app_say("Fill", "while held");
        return;
    }
    if (!down)
        return;
    switch (pk) {
    case PK_TAP: tap_tempo(); break;
    case PK_FREEZE: {
        int on = S.target[P_RV_FREEZE] < 0.5f;
        cmd(C_PARAM, 0, P_RV_FREEZE, on ? 0.75f : 0.25f, 0);
        S.target[P_RV_FREEZE] = on ? 0.75f : 0.25f;
        app_say("Freeze", on ? "On: the reverb holds" : "Off");
        break;
    }
    case PK_LATCH: {
        uint32_t k;
        ui.latch = !ui.latch;
        if (!ui.latch)                   /* latch off: what it held lets go */
            for (k = 0; k < NKEYS; k++)
                if ((latched >> k & 1u) && key_node[k] >= 0) {
                    cmd(C_NOTE, 0, (uint32_t)key_node[k], 0.0f, 0);
                    mo_key((int)k, -1, 0, 0);
                    key_node[k] = -1;
                }
        latched = 0;
        app_say("Latch", ui.latch ? "On: keys hold" : "Off");
        break;
    }
    case PK_MBANK: case PK_MPAD:
        ui.mute[pk - PK_MBANK] = !ui.mute[pk - PK_MBANK];
        cmd(C_LAYER, (uint32_t)(pk - PK_MBANK), 0, ui.mute[pk - PK_MBANK] ? 0.0f : 1.0f, 0);
        app_say(pk == PK_MBANK ? "Layer 1" : "Layer 2", ui.mute[pk - PK_MBANK] ? "Muted" : "On");
        break;
    case PK_MSEQ:
        ui.mute[2] = !ui.mute[2];
        cmd(C_MUTESEQ, ui.mute[2], 0, 0.0f, 0);
        app_say("Generators", ui.mute[2] ? "Muted (keys still play)" : "On");
        break;
    case PK_MDRUM: {                     /* the drums on and off (their parameter: the patch keeps it) */
        int on = !pint(&S, P_DR_ON);
        cmd(C_PARAM, 0, P_DR_ON, on ? 0.75f : 0.25f, 0);
        S.target[P_DR_ON] = on ? 0.75f : 0.25f;
        app_say("Drums", on ? "On" : "Off");
        break;
    }
    case PK_DKEYS:                       /* the keys play the drums, and back */
        if (set.keys_to == 3) set.keys_to = ui.keys_prev;
        else { ui.keys_prev = set.keys_to; set.keys_to = 3; }
        cmd(C_KEYS, set.keys_to, 0, 0.0f, 0);
        app_say("Keys", set.keys_to == 3 ? "Play the drums" : KEYS_N[set.keys_to]);
        break;
    case PK_UNDO: undo(); break;
    case PK_PANIC: cmd(C_PANIC, 0, 0, 0.0f, 0); latched = 0; mo_panic(); app_say("Panic", "all notes off"); break;
    }
}

static int perf_key_of(uint32_t k)       /* key k's performance role (white keys, left to right), PK_NONE if none */
{
    int i;
    for (i = 0; i < 16; i++)
        if (WK[i] == k)
            return i < PK_NONE ? i : PK_NONE;
    return PK_NONE;
}

static int page_of_button(uint32_t b)
{
    int p;
    for (p = PG_SCALE; p <= PG_PAD; p++)
        if (PAGES[p].btn == (int)b)
            return p;
    return -1;
}

/* ------------------------------------------------------------------ input --- */
static void keys_in(uint32_t keys)
{
    uint32_t k, btn = ui.btn;
    int mod_save = (btn >> B_SAVE) & 1u, mod_home = (btn >> B_HOME) & 1u, mod_play = (btn >> B_PLAY) & 1u;
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {
            if (mod_play) {                         /* PLAY held: the performance keys */
                ui.play_used = 1;
                perf_key(perf_key_of(k), 1);
                continue;
            }
            if (mod_save && k < 16u) {             /* SAVE + key: the active bank's slot */
                ui.save_used = 1;
                patch_save(bank * 16 + (int)k);
                continue;
            }
            if (mod_home && k < 16u) {
                ui.home_used = 1;
                patch_load(bank * 16 + (int)k);
                continue;
            }
            if (ui.latch && (latched & bit) && key_node[k] >= 0) {   /* latched: a second press lets it go */
                cmd(C_NOTE, 0, (uint32_t)key_node[k], 0.0f, 0);
                mo_key((int)k, -1, 0, 0);
                key_node[k] = -1;
                latched &= ~bit;
                continue;
            }
            {
                int nd = key_to_node(k);
                uint32_t v = touch_vel(k);
                if (nd < 0 || S.build_req)
                    continue;
                key_node[k] = (int8_t)nd;
                if (cmd(C_NOTE, 1, (uint32_t)nd, (float)(v ? v : 100u) * (1.0f / 127.0f), 0))
                    vocab_note(nd);
                mo_key((int)k, nd, (int)(v ? v : 100u), 1);
                if (ui.latch)
                    latched |= bit;
            }
        } else if (!(keys & bit) && (ui.keys & bit)) {
            int pk = perf_key_of(k);
            if (pk == PK_FILL || (pk >= PK_S1 && pk <= PK_S4 && ui.scene_down_t[pk]))
                perf_key(pk, 0);          /* (the fill and the scenes act on release, wherever PLAY is) */
            if (key_node[k] >= 0 && !(latched & bit)) {
                cmd(C_NOTE, 0, (uint32_t)key_node[k], 0.0f, 0);
                mo_key((int)k, -1, 0, 0);
                key_node[k] = -1;
            }
        }
    }
    for (k = 0; k < NSCENE; k++)                 /* a scene key held a second: store */
        if (ui.scene_down_t[k] && !ui.scene_stored[k] && plat_ms() - ui.scene_down_t[k] > 1000u)
            scene_store((int)k);
    ui.keys = keys;
}

static const char *const BANK_N[N_BANK] = {"Bank A", "Bank B", "Bank C", "Bank D"};

static void go_page(int pg)
{
    ui.page = pg;
    ui.redraw = 1;
    if (pg == PG_SETUP)
        ui.sub[PG_SETUP] = set.setup_sub % NS(SUB_SETUP);
    if (pg == PG_PATCH)
        app_say(PAGES[pg].name, BANK_N[bank]);
    else if (pg != PG_HOME && PAGES[pg].sub)
        app_say(PAGES[pg].name, PAGES[pg].sub[ui.sub[pg]].name);
    else
        app_say(PAGES[pg].name, "");
}

static void bank_step(int d)             /* OCT - / + or K1 on the Patches page */
{
    int b = clamp((int)bank + d, 0, N_BANK - 1);
    if (b != (int)bank) {
        bank = (uint8_t)b;
        ui.redraw = 1;
    }
    app_say(BANK_N[bank], "SAVE + key: save   /   HOME + key: load");
}

static void oct_step(int b)             /* a single OCT press, applied on release */
{
    char t[8];
    if (ui.page == PG_PATCH) { bank_step(b == B_OCTUP ? 1 : -1); return; }
    set.oct = (int8_t)clamp(set.oct + (b == B_OCTUP ? 1 : -1), -3, 3);
    set_dirty = 1;
    int_text(set.oct, t);
    app_say("Key octave", t);
}

static void buttons_in(uint32_t btn)
{
    uint32_t b, now = plat_ms() | 1u;
    uint32_t down = btn & ~ui.btn, up = ui.btn & ~btn;
    uint32_t oct_mask = (1u << B_OCTDN) | (1u << B_OCTUP);
    /* Recognise the pair before processing either individual button. */
    if (!ui.oct_combo && !(btn & (1u << B_SAVE)) &&
        (btn & oct_mask) == oct_mask) {
        ui.oct_combo = 1;
        ui.oct_pending = 0;
        vocab_toggle();
    }
    ui.btn = btn;
    for (b = 0; b < NB; b++) {
        int pg;
        if (!(down >> b & 1u))
            continue;
        switch (b) {
        case B_REC: ui.rec_t = now; ui.rec_used = 0; break;
        case B_HOME:
            ui.home_t = now; ui.home_used = 0;
            if (ui.rec_t) { ui.rec_used = 1; ui.home_used = 1; roll(ROLL_KNOBS, "Roll the knobs"); }   /* REC + HOME: the knobs */
            else if (ui.save_t) {                    /* SAVE + HOME: lock the knobs */
                int on = !S.locked[SEC_KNOBS];
                ui.save_used = 1; ui.home_used = 1;
                cmd(C_LOCK, SEC_KNOBS, on, 0.0f, 0);
                app_say("Knobs", on ? "Locked: rolls leave them" : "Unlocked");
            }
            break;
        case B_SAVE: ui.save_t = now; ui.save_used = 0; break;
        case B_PLAY: ui.play_t = now; ui.play_used = 0; ui.redraw = 1; break;
        case B_OCTDN: case B_OCTUP:
            if (ui.oct_combo) break;
            if (ui.save_t || (btn & (1u << B_SAVE))) {
                ui.save_used = 1;
                if (b == B_OCTUP) keep_settings();
                break;
            }
            ui.oct_pending = b == B_OCTUP ? 2 : 1;
            break;
        default:
            pg = page_of_button(b);
            if (pg < 0)
                break;
            if (ui.rec_t) {                          /* REC + page: roll that page's sections; FX or LFO: motion and effects */
                ui.rec_used = 1;
                if (PAGES[pg].secs & ROLL_MOTION) roll(ROLL_MOTION, "Roll motion & effects");
                else roll(PAGES[pg].secs, PAGES[pg].name);
            } else if (ui.save_t) {                  /* SAVE + page: lock / unlock its sections */
                uint32_t s, on = !S.locked[__builtin_ctz(PAGES[pg].secs)];
                ui.save_used = 1;
                for (s = 0; s < NUM_SECTIONS; s++)
                    if (PAGES[pg].secs >> s & 1u)
                        cmd(C_LOCK, s, on, 0.0f, 0);
                app_say(PAGES[pg].name, on ? "Locked: rolls leave it" : "Unlocked");
            } else {
                go_page(pg);
            }
            break;
        }
    }
    /* REC: a tap rolls everything (on the Bluetooth page, it scans); held 0.7 s, a mutation */
    if (ui.rec_t && (btn >> B_REC & 1u) && !ui.rec_used && now - ui.rec_t >= 700u &&
        !(ui.page == PG_SETUP && ui.sub[PG_SETUP] == SS_BT)) {
        ui.rec_used = 1;
        undo_snapshot();
        cmd(C_MUTATE, 0, 0, 0.35f, 0);
        app_say("Mutate", "a nudge to everything");
    }
    if (up >> B_REC & 1u) {
        if (!ui.rec_used) {
            if (ui.page == PG_SETUP && ui.sub[PG_SETUP] == SS_BT) {
                if (!set.bt_on)
                    bt_power(1);
                bta_scan(1);
                ui.bt_sel = 0;
                app_say("Bluetooth", "scanning for devices");
            } else if (ui.rec_tap && now - ui.rec_tap < 400u) {   /* a double tap: everything */
                ui.rec_tap = 0;
                roll(ROLL_ALL, "Roll everything");
            } else ui.rec_tap = now ? now : 1u;           /* a tap: the sound, once no second tap follows */
        }
        ui.rec_t = 0;
    }
    if (up >> B_HOME & 1u) {                         /* HOME: Perform; from Perform, Setup */
        if (!ui.home_used)
            go_page(ui.page == PG_HOME ? PG_SETUP : PG_HOME);
        ui.home_t = 0;
    }
    if (up >> B_SAVE & 1u) {                         /* a SAVE tap: the Patches page, and back */
        if (!ui.save_used) {
            if (ui.page == PG_PATCH)
                go_page(ui.prev_page);
            else {
                ui.prev_page = ui.page;
                go_page(PG_PATCH);
            }
        }
        ui.save_t = 0;
    }
    if (up >> B_PLAY & 1u) {                         /* PLAY: a tap runs / stops; held, the performance keys */
        if (!ui.play_used && now - ui.play_t < 400u) {   /* (held longer to look at the map: nothing) */
            cmd(C_TRANSPORT, !S.transport, 0, 0.0f, 0);
            app_say("Sequencer", S.transport ? "Stopped" : "Running");
        }
        ui.play_t = 0;
        ui.redraw = 1;
    }
    /* A pair ends on the first release, but stays latched until both are up. */
    if (ui.oct_combo && (up & oct_mask)) {
        if (!(btn & oct_mask)) ui.oct_combo = 0;
    } else if (!ui.oct_combo && (up & oct_mask) && ui.oct_pending) {
        int pending = ui.oct_pending;
        ui.oct_pending = 0;
        if (ui.save_t || (btn & (1u << B_SAVE))) {
            ui.save_used = 1;
            if (pending == 2) keep_settings();
        } else oct_step(pending == 2 ? B_OCTUP : B_OCTDN);
    }
}

static int strlen_(const char *s) { int n = 0; while (s[n]) n++; return n; }
/* what a knob moves: its part of the instrument, and how many parameters ride its path */
static void macro_targets(int m, char *b, int max)
{
    int i, n = 0;
    char u[8];
    for (i = 0; i < NUM_PARAMS; i++)
        if (knob_of(i) == m && (S.kpath[i][0] || S.kpath[i][1] || S.kpath[i][2] || S.kpath[i][3]))
            n++;
    text_cpy(b, knob_name(m), max);
    text_cat(b, ": ");
    int_text(n, u);
    text_cat(b, u);
    text_cat(b, " parameters");
}

static void encoders_in(void)
{
    int32_t e[NE];
    uint32_t k;
    for (k = 0; k < NE; k++)
        e[k] = plat_enc((int)k);
    /* PRESET (the style) and ALGO (the seed) show their choice at once and roll once the knob rests: a fast turn
       through five seeds is one roll, not five rebuilds */
    if (e[EN_PRESET]) {
        set.style = (uint8_t)((set.style + NUM_STYLES + (e[EN_PRESET] > 0 ? 1 : -1)) % NUM_STYLES);
        set_dirty = 1;
        ui.roll_pend = 1;
        ui.roll_t = plat_ms();
        app_say("Style", STYLE_N[set.style]);
    }
    if (e[EN_ALGO]) {
        char t[12] = "#";
        if (ui.roll_pend != 2)
            ui.seed_pend = S.seed;
        ui.seed_pend = (uint32_t)((int32_t)ui.seed_pend + e[EN_ALGO]);
        if (ui.roll_pend != 1)
            ui.roll_pend = 2;
        ui.roll_t = plat_ms();
        int_text((int32_t)ui.seed_pend, t + 1);
        app_say("Seed", t);
    }
    if (ui.page == PG_HOME) {
        for (k = 0; k < 4u; k++) {
            int32_t d = e[EN_K1 + k];
            if (d) {
                char t[48];
                float v = fm_clampf(S.macro[k] + (float)d * 0.03f, -1.0f, 1.0f);
                cmd(C_MACRO, k, 0, v, 0);
                S.macro[k] = v;
                mo.knob_dirty |= (uint8_t)(1u << k);   /* (MIDI out: a knob you turned) */
                int_text((int32_t)(v * 100.0f), t);
                text_cat(t, "  ");
                macro_targets((int)k, t + strlen_(t), (int)sizeof t - (int)strlen_(t));
                app_say(knob_name((int)k), t);
                ui.edit_t[k] = plat_ms();
            }
        }
        if (e[EN_SELECT]) {
            edit_param(P_TEMPO, e[EN_SELECT]);
            item_say(P_TEMPO);
        }
        return;
    }
    if (ui.page == PG_PATCH) {                       /* K1: the bank; K4 right: keep the settings */
        if (e[EN_K1]) {
            bank_step(e[EN_K1] > 0 ? 1 : -1);
            ui.edit_t[0] = plat_ms();
        }
        if (e[EN_K4] > 0) {
            keep_settings();
            ui.edit_t[3] = plat_ms();
            ui.redraw = 1;
        }
        return;
    }
    {
        const page_t *pg = &PAGES[ui.page];
        if (e[EN_SELECT]) {
            int s = clamp(ui.sub[ui.page] + e[EN_SELECT], 0, pg->nsub - 1);
            if (s != ui.sub[ui.page]) {
                ui.sub[ui.page] = s;
                if (ui.page == PG_SETUP) {
                    set.setup_sub = (uint8_t)s;
                    set_dirty = 1;
                }
                app_say(pg->name, pg->sub[s].name);
            }
        }
        for (k = 0; k < 4u; k++)
            if (e[EN_K1 + k]) {
                int it = sub_item(&pg->sub[ui.sub[ui.page]], k);
                if (it >= 0) edit_item(it, e[EN_K1 + k]);
                ui.edit_t[k] = plat_ms();
            }
    }
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons();
    touch_poll();
    keys = touch_keys();
    buttons_in(btn);
    keys_in(keys);
    encoders_in();
    if (ui.rec_tap && plat_ms() - ui.rec_tap >= 400u) {   /* REC tapped once: roll the sound */
        ui.rec_tap = 0;
        roll(ROLL_SOUND, "Roll sound");
    }
    if (ui.roll_pend && plat_ms() - ui.roll_t > 350u) {
        if (ui.roll_pend == 1) {
            cmd(C_STYLE, set.style, 0, 0.0f, 0);
            S.style = set.style;             /* (the roll below reads it for its morph time) */
            roll(ROLL_ALL, STYLE_N[set.style]);
        } else {
            char t[12] = "#";
            undo_snapshot();
            ui.fill = 0;
            cmd(C_ROLL, 0, SEC_ALL, 1.0f, ui.seed_pend);
            rolls++;
            set.seed = ui.seed_pend;
            set.last_patch = -1;
            set_dirty = 1;
            scene_cur = 0xFF;
            int_text((int32_t)ui.seed_pend, t + 1);
            app_say("Seed", t);
        }
        ui.roll_pend = 0;
    }
    if (ui.pend_id >= 0 && plat_ms() - ui.pend_t > 300u) {   /* the structural edit, once its knob has rested */
        cmd(C_PARAM, 0, (uint32_t)ui.pend_id, ui.pend_v, 0);
        ui.pend_id = -1;
    }
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ drawing --- */
static void clip_text(int32_t x, int32_t y, int32_t w, const felucca_font_t *f, const char *s, uint16_t c)
{
    char t[48];
    int n = 0;
    while (s[n] && n < 47) {
        t[n] = s[n];
        n++;
    }
    t[n] = 0;
    if (text_w(f, t) > w) {              /* too long: cut, with an ellipsis */
        while (n > 0 && text_w(f, t) + text_w(f, "...") > w)
            t[--n] = 0;
        text_cat(t, "...");
    }
    cv_text(x, y, f, t, c);
}

static void rtext(int32_t xr, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)   /* right-aligned */
{
    cv_text(xr - text_w(f, s), y, f, s, c);
}

/* the battery beside the tempo (Perform): 0..3 bars in an outline, amber at one bar or none; on USB power, "USB" */
static void draw_battery(int32_t xr, int b)
{
    int32_t x0 = xr - 16, y = 12, i;
    uint16_t c = b <= 1 ? C_AMB : C_HI;
    if (b < 0)
        return;
    if (b > 3) {
        rtext(xr, 10, &FONT_T, "USB", C_GRAY);
        return;
    }
    cv_rect(x0, y, 14, 1, C_GRAY);
    cv_rect(x0, y + 8, 14, 1, C_GRAY);
    cv_rect(x0, y, 1, 9, C_GRAY);
    cv_rect(x0 + 13, y, 1, 9, C_GRAY);
    cv_rect(x0 + 14, y + 2, 2, 5, C_GRAY);
    for (i = 0; i < b; i++)
        cv_rect(x0 + 2 + i * 4, y + 2, 3, 5, c);
}

static void draw_header(void)
{
    uint32_t now = plat_ms();
    char t[24];
    int32_t x;
    cv_begin(240, 32, C_BLACK);
    if (now < ui.msg_until) {            /* what just changed, for a moment */
        x = cv_text(8, 7, &FONT_B, ui.msg[0], C_WHITE);
        clip_text(x + 8, 7, 232 - x - 8, &FONT_S, ui.msg[1], C_HI);
    } else {
        const page_t *pg = &PAGES[ui.page];
        x = cv_text(8, 7, &FONT_B, pg->name, C_WHITE);
        if (ui.page != PG_HOME && pg->sub)
            x = cv_text(x + 8, 7, &FONT_S, pg->sub[ui.sub[ui.page]].name, C_HI);
        if (ui.page == PG_PATCH)
            x = cv_text(x + 8, 7, &FONT_S, BANK_N[bank], C_HI);
        if (pg->secs && pg->secs != SEC_ALL && S.locked[__builtin_ctz(pg->secs)])
            cv_text(x + 8, 10, &FONT_T, "LOCKED", C_AMB);
        if (ui.page == PG_HOME && overloads) {               /* the audio ran out of time: how often (Setup > Load has the rest) */
            char o[16] = "OVER ";
            int_text((int32_t)(overloads > 9999u ? 9999u : overloads), o + 5);
            cv_text(x + 8, 10, &FONT_T, o, C_AMB);
        }
        /* right: tempo, the transport, Bluetooth */
        value_text(P_TEMPO, norm_of(P_TEMPO), t, sizeof t);
        t[3] = 0;                        /* the number alone */
        if (t[2] == ' ')
            t[2] = 0;
        x = 232;
        if (bta_state() == PLAT_BTA_STREAMING) {
            rtext(x, 10, &FONT_T, "BT", C_HI);
            x -= 20;
        }
        if (S.transport) {               /* a play triangle */
            int32_t i;
            for (i = 0; i < 5; i++)
                cv_rect(x - 9 + i, 11 + i, 1, 11 - 2 * i, C_HI);
        } else {
            cv_rect(x - 9, 11, 9, 9, C_DIM);
        }
        rtext(x - 14, 7, &FONT_B, t, S.transport ? C_WHITE : C_GRAY);
        if (ui.page == PG_HOME)
            draw_battery(x - 14 - text_w(&FONT_B, t) - 8, plat_battery());
    }
    cv_rect(0, 31, 240, 1, C_LINE);
    cv_blit(0, 0);
}

static void bar(int32_t x, int32_t y, int32_t w, float v, int bipolar, uint16_t c)
{
    cv_rect(x, y, w, 3, C_LINE);
    if (bipolar) {
        int32_t mid = x + w / 2, e = x + (int32_t)((v + 1.0f) * 0.5f * (float)w);
        if (e >= mid)
            cv_rect(mid, y, e - mid + 1, 3, c);
        else
            cv_rect(e, y, mid - e, 3, c);
        cv_rect(mid, y - 2, 1, 7, C_GRAY);
    } else {
        cv_rect(x, y, (int32_t)(fm_clampf(v, 0.0f, 1.0f) * (float)w), 3, c);
    }
}

/* four rows, one a knob: name and value, a bar under */
/* a pattern's steps under its item: a hit lit, a ghost dim, the beats' cells a little taller, the step playing now
   white (a two-bar line: both bars, the one playing brighter) */
static void pat_strip(int d, int32_t x, int32_t y, int hot)
{
    const PatLine *L = pat_line(&S, d);
    int len = pint(&S, P_D1_LEN + d), rot = pint(&S, P_D1_ROT + d), k, n;
    if (!L)
        return;
    len = len > 0 ? len + 1 : L->len;
    n = L->len;
    {
        int32_t cw = n == 32 ? 6 : 12, step = (int32_t)(S.meter.tau * 16.0f) & 15;
        uint32_t g = S.meter.bar * 16u + (uint32_t)step;
        int now = (int)(((int32_t)(g % 65536u) - rot) % len);
        if (now < 0) now += len;
        now %= n;
        for (k = 0; k < n; k++) {
            int32_t cx = x + k * cw + (n == 32 && k >= 16 ? 4 : 0), h = (k & 3) == 0 ? 8 : 6, cy = y + 8 - h;
            int on = (int)(L->hit >> k & 1u), gh = (int)(L->ghost >> k & 1u);
            uint16_t c = k == now && S.transport ? C_WHITE : on ? (gh ? C_GRAY : (hot ? C_HI : C_AMB)) : C_LINE;
            cv_rect(cx, on || k == now ? cy : y + 6, cw - 2, on || k == now ? h : 2, c);
        }
    }
}

static void draw_items(const sub_t *sb)
{
    int k;
    char n[40], v[40];
    uint32_t now = plat_ms();
    cv_begin(240, 144, C_BLACK);
    for (k = 0; k < 4; k++) {
        int it = sub_item(sb, k);
        int32_t y = k * 36 + 2;
        int hot = now - ui.edit_t[k] < 600u;
        float pos;
        char tag[3] = {'K', (char)('1' + k), 0};
        cv_rect(6, y + 3, 18, 13, hot ? C_HI : C_LINE);
        cv_text(9, y + 3, &FONT_T, tag, hot ? C_BLACK : C_GRAY);
        if (it < 0) {
            cv_text(32, y + 1, &FONT_XS, "-", C_DIM);
            continue;
        }
        item_name(it, n, sizeof n);
        pos = item_value(it, v, sizeof v);
        clip_text(32, y + 1, 96, &FONT_XS, n, hot ? C_WHITE : C_GRAY);
        {
            int dl = -1;                  /* the drum this item belongs to: its hit, a light that fades */
            if (it >= P_D1_DENS && it <= P_D4_SNAP && ((it - P_D1_DENS) % DR_STRIDE == 0 || (it - P_D1_DENS) % DR_STRIDE == P_D1_SRC - P_D1_DENS ||
                                                     (it - P_D1_DENS) % DR_STRIDE == P_D1_LEVEL - P_D1_DENS))
                dl = (it - P_D1_DENS) / DR_STRIDE;
            else if (it >= P_D1_PAT && it <= P_D4_MUTE)
                dl = (it - P_D1_PAT) % NUM_DRUMS;
            else if (it >= P_D1_OE && it <= P_D4_HPLUCK)
                dl = (it - P_D1_OE) % NUM_DRUMS;
            if (dl >= 0) {
                float fl = S.dr.flash[dl];
                if (fl > 0.15f) cv_rect(32 + text_w(&FONT_XS, n) + 6, y + 6, 6, 6, fl > 0.6f ? C_HI : C_AMB);
            }
        }
        {
            int32_t w = text_w(&FONT_B, v), x = 234 - w;
            if (x < 132) {                /* a long value: smaller, and cut if need be */
                clip_text(132, y + 1, 102, &FONT_S, v, hot ? C_WHITE : C_HI);
            } else {
                cv_text(x, y, &FONT_B, v, hot ? C_WHITE : C_HI);
            }
        }
        if (it >= P_D1_PAT && it <= P_D4_PAT && pint(&S, it) > 0)
            pat_strip(it - P_D1_PAT, 32, y + 20, hot);
        else if (pos >= 0.0f)
            bar(32, y + 22, 202, it == V_MX_DEPTH ? pos * 2.0f - 1.0f : pos, it == V_MX_DEPTH, hot ? C_HI : C_AMB);
        else
            cv_rect(32, y + 23, 202, 1, C_LINE);
    }
    cv_blit(0, 36);
}

/* the Patches page: the banks (K1), the active bank's 16 slots on their keys, the settings (K4) */
static void draw_patches(void)
{
    uint32_t now = plat_ms();
    int i, hot1 = now - ui.edit_t[0] < 600u, hot4 = now - ui.edit_t[3] < 600u;
    char t[16];
    cv_begin(240, 144, C_BLACK);
    cv_rect(6, 5, 18, 13, hot1 ? C_HI : C_LINE);
    cv_text(9, 5, &FONT_T, "K1", hot1 ? C_BLACK : C_GRAY);
    cv_text(32, 3, &FONT_XS, "Bank", hot1 ? C_WHITE : C_GRAY);
    for (i = 0; i < N_BANK; i++) {                   /* A..D, each with how many of its slots hold a patch */
        int32_t x = 92 + i * 36, n = __builtin_popcountll(patch_here >> (i * 16) & 0xFFFFull);
        int on = i == (int)bank;
        if (on)
            cv_rect(x, 2, 34, 20, C_HI);
        else {
            cv_rect(x, 2, 34, 1, C_LINE); cv_rect(x, 21, 34, 1, C_LINE);
            cv_rect(x, 2, 1, 20, C_LINE); cv_rect(x + 33, 2, 1, 20, C_LINE);
        }
        t[0] = (char)('A' + i);
        t[1] = 0;
        cv_text(x + 5, 3, &FONT_XS, t, on ? C_BLACK : C_GRAY);
        int_text(n, t);
        rtext(x + 30, 7, &FONT_T, t, on ? C_BLACK : C_DIM);
    }
    for (i = 0; i < 16; i++) {                       /* the slots: a patch lit, the one playing bright */
        int slot = bank * 16 + i, here = (int)(patch_here >> slot & 1u), cur = slot == set.last_patch;
        int32_t x = 4 + (i & 3) * 59, y = 28 + (i >> 2) * 23;
        uint16_t bg = cur ? C_HI : here ? C_LINE : C_BLACK, fg = cur ? C_BLACK : here ? C_HI : C_DIM;
        cv_rect(x, y, 56, 20, bg);
        if (!here && !cur) {
            cv_rect(x, y, 56, 1, C_LINE); cv_rect(x, y + 19, 56, 1, C_LINE);
            cv_rect(x, y, 1, 20, C_LINE); cv_rect(x + 55, y, 1, 20, C_LINE);
        }
        slot_name(slot, t);
        cv_text(x + 6, y + 1, &FONT_XS, t, fg);
    }
    cv_rect(6, 126, 18, 13, hot4 ? C_HI : C_LINE);
    cv_text(9, 126, &FONT_T, "K4", hot4 ? C_BLACK : C_GRAY);
    cv_text(32, 124, &FONT_XS, "Keep settings", hot4 ? C_WHITE : C_GRAY);
    rtext(234, 124, &FONT_XS, set_dirty ? "unsaved: turn right" : "kept", set_dirty ? C_AMB : C_DIM);
    cv_blit(0, 36);
}

static void draw_home(void)
{
    int i;
    char t[48];
    const int32_t X0 = 4, Y0 = 2, W = 104, H = 104;
    cv_begin(240, 144, C_BLACK);
    /* the surface: each node where it sits, lit by how much it rings; the path's place a cross */
    cv_rect(X0 - 1, Y0 - 1, W + 2, 1, C_LINE);
    cv_rect(X0 - 1, Y0 + H, W + 2, 1, C_LINE);
    cv_rect(X0 - 1, Y0, 1, H, C_LINE);
    cv_rect(X0 + W, Y0, 1, H, C_LINE);
    for (i = 0; i < S.eng.n_nodes && i < MAX_NODES; i++) {
        const Node *nd = &S.eng.node[i];
        int32_t x = X0 + (int32_t)(nd->x * (float)(W - 5)) + 1, y = Y0 + (int32_t)(nd->y * (float)(H - 5)) + 1;
        float lv = nd->active ? fm_sqrtf(nd->level) * 40.0f : 0.0f;
        uint16_t c = S.held[i] ? C_WHITE : lv > 0.5f ? C_HI : lv > 0.12f ? C_AMB : lv > 0.02f ? C_GRAY : C_DIM;
        int32_t sz = lv > 0.5f || S.held[i] ? 5 : 3;
        cv_rect(x - (sz - 3) / 2, y - (sz - 3) / 2, sz, sz, c);
    }
    {   /* the generators' points: layer 1 as crosses, layer 2 as dots, each brighter while its envelope is up */
        int l, j;
        for (l = 0; l < NUM_LAYERS; l++)
            for (j = 0; j < S.layer[l].g.npts && j < MAX_PTS; j++) {
                const GenSt *g = &S.layer[l].g;
                int32_t x = X0 + (int32_t)(fm_clampf(g->qx[j], 0.0f, 1.0f) * (float)(W - 1));
                int32_t y = Y0 + (int32_t)(fm_clampf(g->qy[j], 0.0f, 1.0f) * (float)(H - 1));
                uint16_t c = g->e[j] > 0.3f ? C_WHITE : g->e[j] > 0.05f ? C_HI : C_GRAY;
                if (l == 0) {
                    cv_rect(x - 3, y, 7, 1, c);
                    cv_rect(x, y - 3, 1, 7, c);
                } else
                    cv_rect(x - 1, y - 1, 3, 3, c);
            }
    }
    /* what is playing */
    value_text(P_SCALE, norm_of(P_SCALE), t, sizeof t);
    clip_text(116, 0, 122, &FONT_B, t, C_HI);
    text_cpy(t, rh_engine_name(pint(&S, P_RH_ENGINE)), sizeof t);   /* the rhythm's engine and the surface */
    text_cat(t, "  /  ");
    {
        char u[16];
        value_text(P_SURFACE, S.base[P_SURFACE], u, sizeof u);
        text_cat(t, u);
    }
    clip_text(116, 19, 122, &FONT_XS, t, C_GRAY);
    /* the two layers' functions */
    {   /* both playing: "Orbit / Drift"; routed: the carrier, then how the modulator acts on it, e.g. "Lorenz x Euclid"
           (Multiplies), "Lorenz + Euclid" (Adds), "Lorenz on Orbit" (Moves) */
        int mi = gen_modulator(&S);
        if (mi < 0) { text_cpy(t, gen_name(pint(&S, P_G1_FUNC)), sizeof t); text_cat(t, " / "); text_cat(t, gen_name(pint(&S, P_G2_FUNC))); }
        else {
            int ro = pint(&S, LP(mi, P_G1_ROUTE));
            text_cpy(t, gen_name(pint(&S, LP(1 - mi, P_G1_FUNC))), sizeof t);
            text_cat(t, ro == ROUTE_MOVE ? " on " : ro == ROUTE_ADD ? " + " : " x ");
            text_cat(t, ro == ROUTE_MOVE ? gen_name(pint(&S, LP(mi, P_G1_FUNC))) : env_name(pint(&S, LP(mi, P_G1_ENV))));
        }
    }
    clip_text(116, 35, 122, &FONT_XS, t, ui.mute[2] ? C_DIM : C_AMB);
    t[0] = 0;
    text_cat(t, STYLE_N[S.style % NUM_STYLES]);
    text_cat(t, "  ");
    if (set.last_patch >= 0) {
        char u[8];
        text_cat(t, "patch ");
        slot_name(set.last_patch, u);
        text_cat(t, u);
    } else {
        char u[12];
        text_cat(t, "#");
        int_text((int32_t)S.seed, u);
        text_cat(t, u);
    }
    clip_text(116, 51, 122, &FONT_XS, t, C_GRAY);
    /* the state of the performance keys, as chips */
    {
        static const char *const CH[] = {"LATCH", "FREEZE", "FILL"};
        const uint8_t on[3] = {ui.latch, (S.target[P_RV_FREEZE] >= 0.5f), ui.fill};
        int32_t x = 116;
        for (i = 0; i < 3; i++) {
            int32_t w = text_w(&FONT_T, CH[i]) + 8;
            cv_rect(x, 72, w, 14, on[i] ? C_HI : C_LINE);
            cv_text(x + 4, 73, &FONT_T, CH[i], on[i] ? C_BLACK : C_DIM);
            x += w + 4;
        }
        x = 116;
        for (i = 0; i < 4; i++) {
            static const char *const M[] = {"L1", "L2", "GEN", "DRM"};
            int off = i < 3 ? ui.mute[i] : !pint(&S, P_DR_ON);
            float fl = 0.0f;
            int32_t w = text_w(&FONT_T, M[i]) + 8;
            if (i == 3) { int d; for (d = 0; d < NUM_DRUMS; d++) if (S.dr.flash[d] > fl) fl = S.dr.flash[d]; }
            int recording = i < NUM_LAYERS && ui.vocab_recording && (ui.vocab_targets & (1u << i));
            int constrained = i < NUM_LAYERS && !recording && S.layer[i].vocab_on;
            uint16_t colour = off ? C_LINE : recording ? ((plat_ms() / 250u) & 1u ? C_WHITE : C_HI) : constrained ? C_WHITE : fl > 0.5f ? C_HI : C_AMB;
            cv_rect(x, 90, w, 14, colour);
            cv_text(x + 4, 91, &FONT_T, M[i], off ? C_DIM : C_BLACK);
            x += w + 4;
        }
        if (scene_cur < NSCENE) {
            char sc[8] = "SCN ";
            sc[4] = (char)('1' + scene_cur);
            sc[5] = 0;
            cv_text(x + 2, 91, &FONT_T, sc, C_HI);
        }
    }
    /* the four knobs, bipolar: each rides a path through its part of the instrument */
    for (i = 0; i < 4; i++) {
        int32_t x = 4 + (i & 1) * 120, y = 112 + (i >> 1) * 16;
        int hot = plat_ms() - ui.edit_t[i] < 600u;
        cv_text(x, y, &FONT_T, knob_name(i), hot ? C_WHITE : C_GRAY);   /* Timbre, Groove, Space, Motion */
        bar(x + 36, y + 5, 74, S.macro[i], 1, hot ? C_HI : C_AMB);
        {   /* where the matrix has it now (a route to the knob): a white tick riding the bar */
            float km = S.kmod[i], dv = km - S.macro[i];
            if (dv > 0.01f || dv < -0.01f)
                cv_rect(x + 36 + (int32_t)((fm_clampf(km, -1.0f, 1.0f) + 1.0f) * 0.5f * 73.0f), y + 3, 1, 7, C_WHITE);
        }
    }
    cv_blit(0, 36);
}

static void draw_perf_map(void)          /* PLAY held: what each white key does, in key order */
{
    int i;
    cv_begin(240, 144, C_BLACK);
    for (i = 0; i < PK_NONE; i++) {
        int32_t x = 2 + (i % 4) * 60, y = (i / 4) * 36, on = 0;
        int mute = i >= PK_MBANK && i <= PK_MDRUM;
        char k[4];
        if (i <= PK_S4)
            on = scene_cur == i;
        else if (i == PK_FILL) on = ui.fill;
        else if (i == PK_FREEZE) on = (S.target[P_RV_FREEZE] >= 0.5f);
        else if (i == PK_LATCH) on = ui.latch;
        else if (i == PK_MDRUM) on = !pint(&S, P_DR_ON);
        else if (i == PK_DKEYS) on = set.keys_to == 3;
        else if (mute) on = ui.mute[i - PK_MBANK];
        cv_rect(x, y, 56, 33, on ? C_HI : C_LINE);
        int_text(i + 1, k);
        cv_text(x + 4, y + 2, &FONT_T, k, on ? C_BLACK : C_DIM);
        clip_text(x + 4, y + 14, 50, &FONT_XS, PK_N[i], on ? C_BLACK : C_WHITE);
        if (mute)
            rtext(x + 52, y + 2, &FONT_T, "MUTE", on ? C_BLACK : C_DIM);
        if (i <= PK_S4 && (scene_set >> i & 1u))
            rtext(x + 52, y + 2, &FONT_T, "SET", on ? C_BLACK : C_HI);
    }
    cv_text(184, 110, &FONT_T, "Hold a", C_DIM);
    cv_text(184, 122, &FONT_T, "scene:", C_DIM);
    cv_text(184, 134, &FONT_T, "store", C_DIM);
    cv_blit(0, 36);
}

static void prof_refresh(void)
{
    uint32_t i, now = plat_ms(), blocks = S.prof_blocks, db;
    if (now - ui.pf_t < 500u)
        return;
    ui.pf_t = now;
    db = blocks - ui.pf_blocks_last;
    ui.pf_blocks_last = blocks;
    for (i = 0; i < PROF_N; i++) {
        uint32_t v = S.prof[i], d = v - ui.pf_last[i];
        ui.pf_last[i] = v;
        ui.pf_us[i] = db ? d / db / PLAT_TICKS_PER_US : 0u;
    }
    ui.pf_peak_us = S.prof_peak / PLAT_TICKS_PER_US;
    S.prof_peak = 0;
    if (ui.pf_peak_us > ui.pf_peak_hold)
        ui.pf_peak_hold = ui.pf_peak_us;
    ui.pf_cpu = plat_cpu_pct();
    ui.redraw = 1;
}

static void draw_load(void)              /* the profiler: where each block's time goes */
{
    /* (Generators is a part of Control, not beside it: rhythm, motions, harmony, drum triggers, timed inside the tick;
       the drums' sound is timed with the effects) */
    static const char *const ROW_N[] = {" of it, gens", "Control", "Layer 2 wait", "Layer 1", "Drums + FX", "Total", "Layer 2"};
    static const uint8_t ROW_ORDER[] = {PROF_CTRL, PROF_SEQ, PROF_BANK, PROF_PADWORK, PROF_PAD, PROF_FX, PROF_TOTAL};
    char t[48], n[12];
    uint32_t i, tot = ui.pf_us[PROF_TOTAL] * 100u / BUDGET_US, plat = ui.pf_cpu > tot ? ui.pf_cpu - tot : 0u;
    int32_t y = 0;
    cv_begin(240, 144, C_BLACK);
    for (i = 0; i < sizeof ROW_ORDER; i++, y += 15) {
        uint32_t id = ROW_ORDER[i], us = ui.pf_us[id], w = us * 100u / BUDGET_US;
        int total = id == PROF_TOTAL;
        const char *nm = id == PROF_PADWORK && S.par_on ? "Layer 2 (core 2)" : ROW_N[id];
        cv_text(6, y, &FONT_XS, nm, total ? C_WHITE : C_GRAY);
        cv_rect(84, y + 6, 100, 4, C_LINE);
        cv_rect(84, y + 6, (int32_t)(w > 100u ? 100u : w), 4, w > 85u ? C_WHITE : total ? C_HI : C_AMB);
        int_text((int32_t)w, t);
        text_cat(t, "%");
        rtext(234, y, &FONT_XS, t, total ? C_WHITE : C_HI);
    }
    y += 3;
    cv_rect(0, y - 2, 240, 1, C_LINE);
    t[0] = 0;
    text_cat(t, "Peak ");
    int_text((int32_t)(ui.pf_peak_hold * 100u / BUDGET_US), n);
    text_cat(t, n);
    text_cat(t, "%   Platform (USB, BT) ");
    int_text((int32_t)plat, n);
    text_cat(t, n);
    text_cat(t, "%");
    cv_text(6, y, &FONT_XS, t, ui.pf_peak_hold * 100u / BUDGET_US > 90u ? C_WHITE : C_AMB);
    t[0] = 0;
    text_cat(t, "Xruns ");
    int_text((int32_t)plat_xruns(), n);
    text_cat(t, n);
    text_cat(t, "   over 90% ");
    int_text((int32_t)overloads, n);
    text_cat(t, n);
    text_cat(t, "   voices ");
    int_text(S.eng.n_active, n);
    text_cat(t, n);
    text_cat(t, "/");
    int_text(S.cap_nodes, n);
    text_cat(t, n);
    cv_text(6, y + 15, &FONT_XS, t, C_GRAY);
    cv_blit(0, 36);
}

static void draw_footer(void)
{
    cv_begin(240, 60, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    if (ui.page == PG_SETUP && (ui.sub[PG_SETUP] == SS_LOAD || ui.sub[PG_SETUP] == SS_LOAD2)) {
        /* the cuts, compact: each on its knob */
        const sub_t *sb = &SUB_SETUP[ui.sub[PG_SETUP]];
        int k;
        for (k = 0; k < 4; k++) {
            char n[40], v[40];
            int32_t x = 4 + (k & 1) * 118, y = 4 + (k >> 1) * 26;
            char tag[3] = {'K', (char)('1' + k), 0};
            if (sb->it[k] < 0)
                continue;
            item_name(sb->it[k], n, sizeof n);
            item_value(sb->it[k], v, sizeof v);
            cv_text(x, y, &FONT_T, tag, C_DIM);
            clip_text(x + 18, y - 2, 96, &FONT_XS, n, C_GRAY);
            clip_text(x + 18, y + 10, 96, &FONT_XS, v, C_HI);
        }
        cv_blit(0, 180);
        return;
    }
    if (ui.page == PG_PATCH) {
        char t[48] = "SAVE + key:  save to ";
        text_cat(t, BANK_N[bank]);
        cv_text(6, 4, &FONT_XS, t, C_GRAY);
        text_cpy(t, "HOME + key:  load from ", sizeof t);
        text_cat(t, BANK_N[bank]);
        cv_text(6, 20, &FONT_XS, t, C_GRAY);
        cv_text(6, 36, &FONT_XS, "OCT - / +: bank   /   SAVE + OCT+: settings", C_DIM);
        cv_blit(0, 180);
        return;
    }
    if (ui.page == PG_HOME) {
        cv_text(6, 4, &FONT_XS, "Hold PLAY: performance keys", C_GRAY);
        cv_text(6, 20, &FONT_XS, "REC roll   /   hold REC mutate   /   ALGO seed", C_DIM);
        cv_text(6, 36, &FONT_XS, "SELECT tempo   /   PRESET style   /   HOME setup", C_DIM);
    } else {
        /* the sub-pages as tabs; the one shown lit */
        const page_t *pg = &PAGES[ui.page];
        int s, cur = ui.sub[ui.page], first = cur > 1 ? cur - 1 : 0;
        int32_t x = 6;
        if (first > 0)
            x = cv_text(x, 4, &FONT_XS, "< ", C_DIM);
        for (s = first; s < pg->nsub && x < 230; s++) {
            int32_t w = text_w(&FONT_XS, pg->sub[s].name);
            if (x + w > 232) {
                cv_text(x, 4, &FONT_XS, ">", C_DIM);
                break;
            }
            cv_text(x, 4, &FONT_XS, pg->sub[s].name, s == cur ? C_HI : C_DIM);
            if (s == cur)
                cv_rect(x, 20, w, 2, C_HI);
            x += w + 12;
        }
        if (ui.page == PG_SETUP && ui.sub[PG_SETUP] == SS_BT)
            cv_text(6, 30, &FONT_XS, "REC: scan   /   K3 right: connect, left: hang up", C_DIM);
        else if (ui.page == PG_SETUP)
            cv_text(6, 30, &FONT_XS, "SELECT: pages   /   SAVE + OCT+: keep settings", C_DIM);
        else if (ui.page == PG_LFO && ui.sub[PG_LFO] == 0)
            cv_text(6, 30, &FONT_XS, "None clears   /   SAVE + LFO keeps routes", C_DIM);
        else
            cv_text(6, 30, &FONT_XS, "REC + button: roll   /   SAVE + button: lock", C_DIM);
    }
    cv_blit(0, 180);
}

static int perf_map_shown(void)
{
    return ui.page == PG_HOME && (ui.btn >> B_PLAY & 1u) && ui.play_t && plat_ms() - ui.play_t > 150u;
}

static void draw_main(void)
{
    if (perf_map_shown())
        draw_perf_map();
    else if (ui.page == PG_HOME)
        draw_home();
    else if (ui.page == PG_SETUP && (ui.sub[PG_SETUP] == SS_LOAD || ui.sub[PG_SETUP] == SS_LOAD2))
        draw_load();
    else if (ui.page == PG_PATCH)
        draw_patches();
    else
        draw_items(&PAGES[ui.page].sub[ui.sub[ui.page]]);
}

static void draw(void)
{
    draw_header();
    draw_main();
    draw_footer();
}

static void lights(void)
{
    uint32_t b = 0, k, keys = ui.keys, dimk = 0;
    if (ui.page >= PG_SCALE && ui.page <= PG_PAD)
        b |= 1u << PAGES[ui.page].btn;
    if (ui.page == PG_HOME || ui.page == PG_SETUP)
        b |= 1u << B_HOME;
    if (S.transport)
        b |= 1u << B_PLAY;
    if (ui.rec_t)
        b |= 1u << B_REC;
    if (set_dirty)
        b |= 1u << B_SAVE;
    if (ui.btn >> B_PLAY & 1u) {         /* the performance keys: the ones that do something, the ones on */
        for (k = 0; k < PK_NONE; k++) {
            int on = k <= PK_S4 ? scene_cur == k : k == PK_FILL ? ui.fill : k == PK_FREEZE ? (S.target[P_RV_FREEZE] >= 0.5f) :
                     k == PK_LATCH ? ui.latch : (k >= PK_MBANK && k <= PK_MSEQ) ? ui.mute[k - PK_MBANK] :
                     k == PK_MDRUM ? !pint(&S, P_DR_ON) : k == PK_DKEYS ? set.keys_to == 3 : 0;
            if (on)
                keys |= 1u << WK[k];
            else
                dimk |= 1u << WK[k];
        }
    } else if (ui.btn >> B_SAVE & 1u || ui.btn >> B_HOME & 1u || ui.page == PG_PATCH) {
        keys |= (uint32_t)(patch_here >> (bank * 16) & 0xFFFFu);   /* the active bank's slots that hold a patch */
    } else {
        for (k = 0; k < NKEYS; k++) {
            int nd = key_to_node(k);
            if (nd >= 0 && S.eng.node[nd].active && S.eng.node[nd].level > 2e-4f)
                keys |= 1u << k;
        }
        keys |= latched;
    }
    plat_lights(b, keys, 0, dimk, 1);
}

void app_ui_init(void)
{
    theme_apply(set.theme);
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.pend_id = -1;
    ui.page = PG_HOME;
    ui.redraw = 1;
    rng_ui ^= plat_ticks24();
    if (set.bt_on) {                     /* Bluetooth audio: on as it was left, the last device called again */
        bta_on(1);
        if (set.bt_auto)
            bta_reconnect(0);
    }
}

void app_frame(void)
{
    {   /* the load governor's way back up */
        static uint32_t gov_step;
        uint32_t t = plat_ms();
        if (S.cap_nodes < (int)set.cap_nodes && t - gov_last_ms > 4000u && t - gov_step > 4000u) { S.cap_nodes++; gov_step = t; }
    }
    static uint32_t last_draw, last_bt;
    uint32_t now = plat_ms();
    static uint32_t vocab_seen;
    if (vocab_seen != S.vocab_generation) {
        vocab_seen = S.vocab_generation;
        memset(vocab_count, 0, sizeof vocab_count);
        ui.vocab_recording = 0;
        ui.vocab_targets = 0;
        ui.vocab_full = 0;
    }
    if (S.build_req) {                   /* the render is waiting: the rebuild, here */
        synth_build_now(&S);
        builds++;
    }
    if (builds != built_seen) {
        built_seen = builds;
        sort_nodes();
    }
    if (!S.build_req)
        mo_pitches();                    /* (MIDI out: the notes' pitches, when they change) */
    mo_knobs();
    bta_sim_tick();
    {   /* the battery's bars: the header shows them */
        static int last_batt = -2;
        int b = plat_battery();
        if (b != last_batt) {
            last_batt = b;
            ui.redraw = 1;
        }
    }
    if ((uint32_t)bta_state() != last_bt) {
        last_bt = (uint32_t)bta_state();
        if (last_bt == PLAT_BTA_STREAMING)
            app_say("Bluetooth audio", "streaming (the speaker is muted)");
        ui.redraw = 1;
    }
    input();
    ann_tick(now);
    if (ui.page == PG_SETUP)
        prof_refresh();
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    /* the screen: everything when something changed; between, only what moves (the surface on Perform while it plays, 10 times a
       second) and a knob's highlight going out */
    {
        static uint8_t map_was;
        uint32_t k, hot = 0;
        uint8_t map = (uint8_t)perf_map_shown();
        for (k = 0; k < 4u; k++)
            if (ui.edit_t[k] && now - ui.edit_t[k] >= 600u && now - ui.edit_t[k] < 700u)
                hot = 1;
        if (map != map_was) {
            map_was = map;
            ui.redraw = 1;
        }
        /* while a Bluetooth link is being made, the screen goes out a part at a time (header, middle, footer: one a
           frame) and the surface 4 times a second, so the main loop is never long between its Bluetooth polls: a reply
           the device sends meanwhile waits in the radio's queue (BTI_RING), and a full queue would lose it */
        static uint8_t parts;
        int slow = bt_busy() && bta_state() != PLAT_BTA_STREAMING;
        if (ui.redraw) {
            ui.redraw = 0;
            last_draw = now;
            if (slow)
                parts = 7;
            else {
                parts = 0;
                draw();
            }
        } else if (parts) {
            if (parts & 1u) draw_header();
            else if (parts & 2u) draw_main();
            else draw_footer();
            parts &= (uint8_t)(parts - 1u);
        } else if ((ui.page == PG_HOME && !map && S.transport && now - last_draw >= (slow ? 250u : 100u)) ||
                   (hot && now - last_draw >= 100u)) {
            last_draw = now;
            draw_main();
        }
    }
    lights();
}

#ifdef PLAT_HOST
#include <stdio.h>
static uint32_t dr_last;
static float g_kept[NUM_PARAMS];
/* (the tests'): this sound saved in an older layout of NP parameters in slot n: the parameters past NP dropped (and
   routes to them), with a route from LFO 1 to the Groove knob in its last matrix slot and, past FIRST, one from LFO 1
   to parameter FIRST (a withdrawn one) in its first, at that layout's numbering */
#define MAKE_OLD(FN, T, NP, MAGIC, FIRST) \
static int FN(int sl) \
{ \
    static T o; \
    int i, k; \
    fill_patch(&pbuf); \
    memset(&o, 0, sizeof o); \
    o.magic = MAGIC; o.nparams = (NP); o.nslots = MOD_SLOTS; o.seed = pbuf.seed; o.style = pbuf.style; \
    o.scene_set = pbuf.scene_set; \
    for (i = 0; i < (FIRST); i++) { \
        o.p[i] = pbuf.p[i]; \
        for (k = 0; k < KNOB_WP; k++) o.kpath[i][k] = pbuf.kpath[i][k]; \
        for (k = 0; k < NSCENE; k++) o.scene[k][i] = pbuf.scene[k][i]; \
    } \
    memcpy(o.slot, pbuf.slot, sizeof o.slot); \
    for (k = 0; k < MOD_SLOTS; k++) \
        if (o.slot[k].dst >= NUM_PARAMS) o.slot[k].dst = (uint8_t)(o.slot[k].dst - NUM_PARAMS + (NP)); \
        else if (o.slot[k].dst >= (FIRST)) o.slot[k].src = 0; \
    if ((FIRST) < (NP)) { o.slot[0].src = SRC_LFO1; o.slot[0].dst = (uint8_t)(FIRST); o.slot[0].depth = .5f; } \
    o.slot[MOD_SLOTS - 1].src = SRC_LFO1; o.slot[MOD_SLOTS - 1].dst = (uint8_t)((NP) + 1); o.slot[MOD_SLOTS - 1].depth = .5f; \
    memcpy(o.macro, pbuf.macro, sizeof o.macro); memcpy(o.locked, pbuf.locked, sizeof o.locked); \
    memcpy(o.scene_macro, pbuf.scene_macro, sizeof o.scene_macro); \
    if (plat_store_save(patch_obj(sl), &o, sizeof o) != 0) return 0; \
    patch_here |= 1ull << sl; \
    return 1; \
}
MAKE_OLD(make_old9, patch9_t, NUM_PARAMS_9, PATCH_MAGIC_9, NUM_PARAMS_9)
MAKE_OLD(make_oldA, patchA_t, NUM_PARAMS_A, PATCH_MAGIC_A, NUM_PARAMS_A)
MAKE_OLD(make_oldB, patchB_t, NUM_PARAMS_B, PATCH_MAGIC_B, NUM_PARAMS_A)
MAKE_OLD(make_oldC, patchC_t, NUM_PARAMS_C, PATCH_MAGIC_C, NUM_PARAMS_C)
MAKE_OLD(make_oldD, patchD_t, NUM_PARAMS_D, PATCH_MAGIC_D, NUM_PARAMS_D)
MAKE_OLD(make_oldE, patchE_t, NUM_PARAMS_E, PATCH_MAGIC_E, NUM_PARAMS_E)
MAKE_OLD(make_oldF, patchF_t, NUM_PARAMS_F, PATCH_MAGIC_F, NUM_PARAMS_F)
#undef MAKE_OLD
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "sub"))
        *got = ui.sub[ui.page];
    else if (!strcmp(what, "transport"))
        *got = S.transport;
    else if (!strcmp(what, "mo_ons"))               /* MIDI out (1.0.6): note-ons and note-offs sent, notes out now */
        *got = (int)mo.ons;
    else if (!strcmp(what, "mo_offs"))
        *got = (int)mo.offs;
    else if (!strcmp(what, "mo_open")) {
        int i, c = 0;
        for (i = 0; i < MO_V; i++)
            c += mo_v[i].used && mo_v[i].dests;
        *got = c;
    } else if (!strcmp(what, "mo_ons_min"))         /* (at least so many note-ons: 4 when so) */
        *got = (int)mo.ons >= 4 ? 4 : (int)mo.ons;
    else if (!strcmp(what, "mo_open_max")) {         /* (notes out but the drums') */
        int i, c = 0;
        for (i = 0; i < MO_V; i++)
            c += mo_v[i].used && mo_v[i].dests && mo_v[i].src != MS_DRUM;
        *got = c;
    } else if (!strcmp(what, "mo_up_usb"))
        *got = mo.up[0];
    else if (!strcmp(what, "mo_up_ble"))
        *got = mo.up[1];
    else if (!strcmp(what, "mo_notes"))
        *got = set.midi_out;
    else if (!strcmp(what, "mo_ccs"))               /* knob CCs sent */
        *got = (int)mo.ccs;
    else if (!strcmp(what, "mo_clk_run"))           /* the clock runs to USB */
        *got = mo.clk_run[0];
    else if (!strcmp(what, "mo_mpe"))
        *got = set.mo_mpe;
    else if (!strcmp(what, "mo_panic"))
        { mo_panic(); *got = 1; }
    else if (!strcmp(what, "mo_chans_ok")) {        /* MPE: every note out on its own member channel (2..15) */
        int i, j, ok = 1;
        for (i = 0; i < MO_V; i++)
            if (mo_v[i].used && mo_v[i].dests && mo_v[i].src != MS_DRUM) {
                if (set.mo_mpe && (mo_v[i].ch < 1 || mo_v[i].ch > 14))
                    ok = 0;
                for (j = i + 1; j < MO_V; j++)
                    if (set.mo_mpe && mo_v[j].used && mo_v[j].dests && mo_v[j].ch == mo_v[i].ch)
                        ok = 0;
            }
        *got = ok;
    }
    else if (!strcmp(what, "nodes"))
        *got = S.eng.n_nodes;
    else if (!strncmp(what, "keep_", 5))            /* (the tests') remember a parameter's value now */
        { g_kept[atoi(what + 5) % NUM_PARAMS] = S.base[atoi(what + 5) % NUM_PARAMS]; *got = 1; }
    else if (!strncmp(what, "same_", 5))            /* ... and whether it is that again (within a glide's end) */
        { int id = atoi(what + 5) % NUM_PARAMS; float d = S.base[id] - g_kept[id]; *got = d < .01f && d > -.01f; }
    else if (!strncmp(what, "onknob_", 7))          /* a parameter is on a performance knob's path */
        { int id = atoi(what + 7) % NUM_PARAMS, k, on = 0; for (k = 0; k < KNOB_WP; k++) on |= S.kpath[id][k] != 0; *got = on; }
    else if (!strcmp(what, "seed"))
        *got = (int)S.seed;
    else if (!strcmp(what, "style"))
        *got = S.style;
    else if (!strcmp(what, "builds"))
        *got = (int)builds;
    else if (!strcmp(what, "patches"))               /* the active bank's slots (bit = key) */
        *got = (int)(patch_here >> (bank * 16) & 0xFFFFu);
    else if (!strncmp(what, "pat", 3) && what[3] >= '0' && what[3] <= '3' && !what[4])   /* a drum's pattern (0 linear) */
        *got = pint(&S, P_D1_PAT + (what[3] - '0'));
    else if (!strncmp(what, "mute", 4) && what[4] >= '0' && what[4] <= '3' && !what[5])
        *got = pint(&S, P_D1_MUTE + (what[4] - '0'));
    else if (!strcmp(what, "cpu2_used"))             /* layer 2 on the second core (where there is one) */
        *got = S.par_on == plat_cpu2_on();
    else if (!strcmp(what, "kick_pat_on"))
        *got = pint(&S, P_D1_PAT) > 0;
    else if (!strncmp(what, "makeB_", 6)) {          /* (a test's): this sound saved as a withdrawn build's MNPB patch in
                                                        slot n, a route to its first drum-hit parameter in slot 0, one from
                                                        LFO 1 to the Groove knob in the last (at MNPB's numbering) */
        *got = make_oldB(atoi(what + 6));
    } else if (!strcmp(what, "first_route"))       /* the first matrix slot's source (0: cleared) */
        *got = S.slot[0].src;
    else if (!strcmp(what, "bal1"))                  /* layer 1's Balance, 0 (dark) .. 100 (bright) */
        *got = (int)(S.base[P_G1_BAL] * 100.0f + 0.5f);
    else if (!strcmp(what, "drnotes"))              /* (1.0.6) the drums' notes: 0 Follow, 1 Fixed */
        *got = (int)(S.target[P_DR_NOTES] * 100.0f + .5f);
    else if (!strncmp(what, "drhz_", 5)) {           /* drum n's last note, Hz */
        int d = atoi(what + 5) & 3; const DrumV *v = &S.dr.v[d];
        *got = v->node >= 0 ? (int)(S.eng.node[v->node].freq * fm_exp2f((float)(pint(&S, v->layer ? P_V2_REG : P_V1_REG) - 1)) + .5f) : -1;
    } else if (!strcmp(what, "dry"))                  /* (1.0.6) the patch is Dry */
        *got = pint(&S, P_DRY);
    else if (!strcmp(what, "fxw"))                  /* the effects' share now, % */
        *got = (int)(S.fx.fxw * 100.f + .5f);
    else if (!strncmp(what, "knobdry_", 8))          /* the knob a parameter rides on a Dry patch */
        *got = knob_of_dry(atoi(what + 8), 1);
    else if (!strcmp(what, "keys_pull"))
        *got = pint(&S, P_KEY_PULL);
    else if (!strcmp(what, "swlev"))
        *got = pint(&S, P_RH_SWLEV);
    else if (!strncmp(what, "make9_", 6))           /* (a test's): this sound saved as a 1.0.4 patch (MNP9) in slot n, */
        *got = make_old9(atoi(what + 6));            /* with a route from LFO 1 to the Groove knob in its last matrix slot */
    else if (!strncmp(what, "makeA_", 6))           /* the same as an earlier 1.0.5 test build's (MNPA) */
        *got = make_oldA(atoi(what + 6));
    else if (!strncmp(what, "makeC_", 6))           /* and a later one's (MNPC) */
        *got = make_oldC(atoi(what + 6));
    else if (!strncmp(what, "makeD_", 6))           /* and the one before the undertones (MNPD) */
        *got = make_oldD(atoi(what + 6));
    else if (!strncmp(what, "makeF_", 6))           /* and 1.0.5's (MNPF) */
        *got = make_oldF(atoi(what + 6));
    else if (!strncmp(what, "makeE_", 6))           /* and the one before Balance (MNPE) */
        *got = make_oldE(atoi(what + 6));
    else if (!strncmp(what, "pmask_", 6))           /* layer 1's Clarity mask on note n */
        *got = S.layer[0].pmask[atoi(what + 6)];
    else if (!strcmp(what, "nodes"))                 /* how many notes */
        *got = S.eng.n_nodes;
    else if (!strcmp(what, "hitcolour"))             /* the hits' colour at its defaults (odd/even in the middle, no pluck) */
    {
        int d, ok = 1;
        for (d = 0; d < NUM_DRUMS; d++)
            if (S.base[P_D1_OE + d] < .49f || S.base[P_D1_OE + d] > .51f || S.base[P_D1_HPLUCK + d] > .005f) ok = 0;
        *got = ok;
    } else if (!strncmp(what, "dstname_", 8)) {     /* a matrix destination's name starts with a drum's (1) or "?" (0) */
        char nb[40];
        mx_dst_name(atoi(what + 8), nb, sizeof nb);
        *got = nb[0] != '?' && nb[0] != '-';
    } else if (!strncmp(what, "knobof_", 7))         /* the performance knob a parameter is on (-1 none) */
        *got = knob_of(atoi(what + 7));
    else if (!strcmp(what, "colour"))                /* the voices' colour at its defaults (odd/even in the middle, no pluck) */
        *got = S.base[P_V1_ODDEVEN] > .49f && S.base[P_V1_ODDEVEN] < .51f && S.base[P_V2_ODDEVEN] > .49f && S.base[P_V2_ODDEVEN] < .51f
               && S.base[P_V1_PLUCK] < .005f && S.base[P_V2_PLUCK] < .005f;
    else if (!strcmp(what, "last_route"))           /* the last matrix slot's destination */
        *got = S.slot[MOD_SLOTS - 1].src ? S.slot[MOD_SLOTS - 1].dst : -1;
    else if (!strcmp(what, "patches_all"))           /* how many of the 64 hold a patch */
        *got = __builtin_popcountll(patch_here);
    else if (!strcmp(what, "bank"))
        *got = bank;
    else if (!strcmp(what, "cap_pad"))
        *got = S.cap_pad_voices;
    else if (!strcmp(what, "cap_uni"))
        *got = 64;
    else if (!strcmp(what, "ws_os"))
        *got = 0;
    else if (!strcmp(what, "cap_harm"))
        *got = S.cap_harm;
    else if (!strcmp(what, "cap_nodes"))
        *got = S.cap_nodes;
    else if (!strcmp(what, "locked_fx"))
        *got = S.locked[SEC_FX];
    else if (!strcmp(what, "oct"))
        *got = set.oct;
    else if (!strcmp(what, "theme"))
        *got = set.theme;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "build_req"))
        *got = S.build_req;
    else if (!strcmp(what, "scenes"))
        *got = scene_set;
    else if (!strcmp(what, "scene"))
        *got = scene_cur == 0xFF ? -1 : scene_cur;
    else if (!strcmp(what, "freeze"))
        *got = S.ival[P_RV_FREEZE];
    else if (!strcmp(what, "latch"))
        *got = ui.latch;
    else if (!strcmp(what, "mute_bank"))
        *got = S.lay_t[0] == 0.0f;
    else if (!strcmp(what, "mute_seq"))
        *got = S.mute_seq;
    else if (!strcmp(what, "gen1"))
        *got = (int)(S.target[P_G1_FUNC] * (float)NUM_GENS);
    else if (!strcmp(what, "gen2"))
        *got = (int)(S.target[P_G2_FUNC] * (float)NUM_GENS);
    else if (!strcmp(what, "drums_on"))
        *got = pint(&S, P_DR_ON);
    else if (!strcmp(what, "drum_hits"))            /* the drums hit since the last ask: 1 if any */
        { *got = S.dr.hits != dr_last; dr_last = S.dr.hits; }
    else if (!strcmp(what, "drum_clear"))           /* forget the hits so far (always 0) */
        { *got = 0; dr_last = S.dr.hits; }
    else if (!strcmp(what, "drum_layer"))
        *got = pint(&S, P_DR_LAYER);
    else if (!strcmp(what, "drum_shape"))
        *got = pint(&S, P_DR_SHAPE);
    else if (!strcmp(what, "keys_drums"))
        *got = S.keys_to == 3;
    else if (!strcmp(what, "env2"))
        *got = (int)(S.target[P_G2_ENV] * (float)NUM_ENVS);
    else if (!strcmp(what, "route2"))
        *got = (int)(S.target[P_G2_ROUTE] * (float)NUM_ROUTES);
    else if (!strcmp(what, "modulator"))           /* the layer that modulates (1, 2), or 0 */
        *got = gen_modulator(&S) + 1;
    else if (!strcmp(what, "rh_engine"))
        *got = pint(&S, P_RH_ENGINE);
    else if (!strcmp(what, "rh_events"))             /* the stream made events since the last ask: 1 if any */
        { static uint32_t last; *got = S.meter.nev != last; last = S.meter.nev; }
    else if (!strcmp(what, "knob_params"))           /* parameters riding the four knobs' paths */
        { int i, c = 0; for (i = 0; i < NUM_PARAMS; i++) if (S.kpath[i][0] || S.kpath[i][3]) c++; *got = c; }
    else if (!strcmp(what, "oscillators"))           /* oscillators layer 1 rendered in its last block */
        *got = S.layer[0].n_osc;
    else if (!strcmp(what, "scales"))
        *got = scale_count();
    else if (!strcmp(what, "keys_to"))
        *got = S.keys_to;
    else if (!strcmp(what, "root"))                  /* the ding's MIDI note */
        *got = (int)(real_of(P_ROOT, S.target[P_ROOT]) + 0.5f);
    else if (!strcmp(what, "hexany")) {               /* 1: the scale is the 1-3-5-9 hexany, unrotated */
        scale_get(pint(&S, P_SCALE), &scale_tmp);
        *got = name_is(scale_tmp.name, "Hexany 1-3-5-9") && pint(&S, P_ROTATE) == 0;
    } else if (!strcmp(what, "battery"))
        *got = plat_battery();
    else if (!strcmp(what, "dump_params")) {        /* (docs) every page's rows: name, range, defaults, choices -> params.tsv */
        FILE *f = fopen("params.tsv", "w");
        int pg, sb, r, k;
        char nm[48], v0[48], v1[48], vd[48];
        for (pg = 1; f && pg < NPG; pg++)
            for (sb = 0; sb < PAGES[pg].nsub; sb++)
                for (r = 0; r < 4; r++) {
                    int id = PAGES[pg].sub[sb].it[r];
                    if (id < 0 || id >= NUM_PARAMS) {
                        if (id >= 0) { item_name(id, nm, sizeof nm); fprintf(f, "%s\t%s\t%d\tV\t%s\n", PAGES[pg].name, PAGES[pg].sub[sb].name, r + 1, nm); }
                        continue;
                    }
                    const ParamMeta *m = &g_param_meta[id];
                    param_name(id, nm, sizeof nm);
                    value_text(id, 0.0f, v0, sizeof v0);
                    value_text(id, 1.0f, v1, sizeof v1);
                    value_text(id, m->def, vd, sizeof vd);
                    fprintf(f, "%s\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\tmod=%d", PAGES[pg].name, PAGES[pg].sub[sb].name, r + 1, id,
                            nm, m->name, v0, vd, v1, m->moddable);
                    if (m->curve == CURVE_DISC && (int)m->hi <= 40)
                        for (k = 0; k < (int)m->hi; k++) { value_text(id, ((float)k + 0.5f) / m->hi, v0, sizeof v0); fprintf(f, "\t[%s]", v0); }
                    fprintf(f, "\n");
                }
        if (f) fclose(f);
        *got = 1;
    }
#if !(FM1B_BT_AUDIO && !defined(PLAT_HOST))
    else if (!strcmp(what, "bt_pages"))              /* (the simulator's stand-in) the pages started: one a connect */
        *got = sim.pages;
#endif
    else if (!strcmp(what, "bt_sel"))
        *got = ui.bt_sel;
    else if (!strcmp(what, "mx_route"))              /* the matrix page: the route shown (1..24), its source, destination, depth % */
        *got = ui.mx_sel + 1;
    else if (!strcmp(what, "mx_src"))
        *got = S.slot[ui.mx_sel].src;
    else if (!strcmp(what, "mx_dst"))
        *got = S.slot[ui.mx_sel].dst;
    else if (!strcmp(what, "mx_depth"))
        *got = (int)(S.slot[ui.mx_sel].depth * 100.0f + (S.slot[ui.mx_sel].depth < 0.0f ? -0.5f : 0.5f));
    else if (!strncmp(what, "knob_moved", 10))      /* how far the matrix moves a knob from the hand (x 1000) */
        { float v = S.kmod[(what[10] - '0') & 3] - S.macro[(what[10] - '0') & 3]; *got = (int)((v < 0.0f ? -v : v) * 1000.0f); }
    else if (!strcmp(what, "rises"))                /* the generators made rises since the last ask: 1 if any */
        { static uint32_t last; *got = S.n_rises != last; last = S.n_rises; }
    else if (!strcmp(what, "clips"))                /* samples the safety clip touched so far */
        *got = (int)S.fx.clips;
    else if (!strcmp(what, "tempo"))
        *got = (int)(real_of(P_TEMPO, S.target[P_TEMPO]) + 0.5f);
    else if (!strcmp(what, "rolls"))
        *got = (int)rolls;
    else if (!strcmp(what, "changed_scale") || !strcmp(what, "changed_sound") || !strcmp(what, "changed_motion") ||
             !strcmp(what, "changed_knobs")) {         /* 1 if that group changed since the last ask of the same */
        static uint32_t last[4];
        uint32_t h = 2166136261u; int g, i, k;
        g = what[8] == 's' && what[9] == 'c' ? 0 : what[8] == 's' ? 1 : what[8] == 'm' ? 2 : 3;
        if (g == 0) { for (i = 0; i < NUM_PARAMS; i++) if (g_param_meta[i].section == SEC_SCALE) h = (h ^ (uint32_t)(S.target[i] * 1e6f)) * 16777619u; }
        else if (g == 1) { for (i = 0; i < NUM_PARAMS; i++) if (ROLL_SOUND >> g_param_meta[i].section & 1u) h = (h ^ (uint32_t)(S.target[i] * 1e6f)) * 16777619u; }
        else if (g == 2) { for (i = 0; i < NUM_PARAMS; i++) if (ROLL_MOTION >> g_param_meta[i].section & 1u) h = (h ^ (uint32_t)(S.target[i] * 1e6f)) * 16777619u;
                           for (k = 0; k < MOD_SLOTS; k++) h = (h ^ (uint32_t)(S.slot[k].dst * 977 + S.slot[k].src)) * 16777619u; }
        else for (i = 0; i < NUM_PARAMS; i++) for (k = 0; k < KNOB_WP; k++) h = (h ^ (uint32_t)(uint8_t)S.kpath[i][k]) * 16777619u;
        *got = h != last[g]; last[g] = h;
    }
    else if (!strcmp(what, "bt"))
        *got = bta_state();
    else if (!strncmp(what, "param", 5))
        *got = (int)(S.target[atoi(what + 5) % NUM_PARAMS] * 1000.0f + 0.5f);
    else
        return 0;
    return 1;
}
#endif

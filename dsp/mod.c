/*
 * Six LFOs (with loose Kuramoto coupling), the modulation matrix and the performance knobs' paths.
 * Everything is evaluated once per control tick; LFO -> LFO routes use
 * the previous tick's values, which keeps feedback loops stable.
 */
#define MANIFOLD_DSP_UNIT
#include "synth.h"

static const float SYNC_CPB[] = {1.f/8, 1.f/6, 1.f/4, 1.f/3, 1.f/2, 2.f/3, 3.f/4, 4.f/5,
                                 1.f, 5.f/4, 4.f/3, 3.f/2, 2.f, 3.f, 4.f};
#define N_SYNC ((int)(sizeof SYNC_CPB / sizeof SYNC_CPB[0]))

#include "ctl_on.h"
DSP_CTL void lfo_tick(Synth *s, float dt) {
    float K = pval(s, P_LFO_COUPLE), tempo = pval(s, P_TEMPO);
    float ph[NUM_LFO];
    for (int j = 0; j < NUM_LFO; j++) ph[j] = s->lfo[j].phase;
    for (int j = 0; j < NUM_LFO; j++) {
        Lfo *l = &s->lfo[j];
        int base = P_LFO1_RATE + 3 * j;
        float rate = pval(s, base);
        if (pint(s, base + 2)) {                      /* tempo sync incl. just ratios */
            float cpb = rate * 60.f / tempo; int bi = 0; float bd = 1e9f;
            for (int k = 0; k < N_SYNC; k++) { float d = fabsf(flog2(cpb / SYNC_CPB[k])); if (d < bd) { bd = d; bi = k; } }
            rate = SYNC_CPB[bi] * tempo / 60.f;
        }
        float dph = rate * dt;
        if (K > 0.f) {                                /* Kuramoto: drift toward the others */
            float c = 0; for (int k = 0; k < NUM_LFO; k++) if (k != j) c += fsin(ph[k] - ph[j]);
            dph += K * 0.05f * c * dt;
        }
        float np = l->phase + dph; int wrap = np >= 1.f;
        np -= floorf(np); l->phase = np;
        if (wrap) {
            l->prev = l->target;
            l->target = frand(&s->rng) * 2.f - 1.f;
            if (l->logistic <= 0.f || l->logistic >= 1.f) l->logistic = 0.37f;
            l->logistic = 3.91f * l->logistic * (1.f - l->logistic);
            l->step++;
        }
        float v;
        switch (pint(s, base + 1)) {
        case 0: v = fsin(np); break;
        case 1: v = np < .5f ? 4.f * np - 1.f : 3.f - 4.f * np; break;
        case 2: v = 2.f * np - 1.f; break;
        case 3: v = np < .5f ? 1.f : -1.f; break;
        case 4: v = l->target; break;
        case 5: { float f = np * np * (3.f - 2.f * np); v = l->prev + (l->target - l->prev) * f; } break;
        case 6: v = 2.f * l->logistic - 1.f; break;
        case 7: { int st = (int)(np * 8.f); v = ((st * 5) % 8) < 5 ? 1.f : -0.3f; } break;
        default: { float e = s->eng.env_follow * 4.f; v = (e > 1.f ? 1.f : e) * 2.f - 1.f; } break;
        }
        l->value = v;
    }
}

/* The performance knobs. Each rides a path through every parameter of its part of the instrument: a parameter's
   offset is 0 with the knob at its centre and follows four rolled waypoints (at -1, -1/2, +1/2, +1), straight lines
   between them, so a full turn passes through several different settings of all of them at once.
     Timbre   both voices (the oscillator banks)        Groove   the rhythm and the drums
     Space    the effects                               Motion   both generators, the path, the LFOs */
static const char *const KNOB_N[NUM_MACRO] = {"Timbre", "Groove", "Space", "Motion"};
uint8_t g_knob_dry;                          /* the patch is Dry (1.0.6): Space is Shape (mod_apply keeps it) */
const char *knob_name(int k) { return k == 2 && g_knob_dry ? "Shape" : k >= 0 && k < NUM_MACRO ? KNOB_N[k] : "?"; }
/* A Dry patch (1.0.6): the effects off every knob but the compressor's; Space, shown as Shape, has the envelopes and
   the room the instrument makes itself: each drum's attack, hold, decay, curve, settle, snap and pluck, the duck, the
   layers' contour, the voices' pluck, width and spin, and the compressor's attack; Pump (its rhythm) goes to Groove */
static int knob_dry(int id) {
    if (id == P_CMP_PUMP) return 1;
    if (id == P_CMP_ATT) return 2;            /* (the compressor's attack only: its depth, release and mix moved the loudness) */
    if (id >= P_CMP_THRESH && id <= P_CMP_MIX) return -1;
    if (g_param_meta[id].section == SEC_FX) return -1;
    if ((id >= P_D1_HPLUCK && id <= P_D4_HPLUCK) || id == P_DR_DUCK || id == P_G1_CONTOUR || id == P_G2_CONTOUR ||
        id == P_V1_PLUCK || id == P_V2_PLUCK || id == P_V1_WIDTH || id == P_V2_WIDTH || id == P_V1_SPIN || id == P_V2_SPIN)
        return 2;
    if (id >= P_D1_DENS && id < P_D1_DENS + NUM_DRUMS * DR_STRIDE) {
        int k = (id - P_D1_DENS) % DR_STRIDE + P_D1_DENS;
        if (k == P_D1_ATTACK || k == P_D1_HOLD || k == P_D1_DECAY || k == P_D1_CURVE || k == P_D1_SETTLE || k == P_D1_SNAP)
            return 2;
    }
    return 9;                                /* (as when wet) */
}
int knob_of_rd(int id, int dry) {
    if (id < 0 || id >= NUM_PARAMS || !g_param_meta[id].moddable) return -1;
    if (dry) { int k = knob_dry(id); if (k != 9) return k; }
    if (id == P_V1_LEVEL || id == P_V2_LEVEL) return 3;                    /* (1.0.6) the layers' levels: Motion (+-3 dB) */
    if (id == P_MASTER || id == P_DR_LEVEL || id == P_TEMPER) return -1;
    if ((id >= P_D1_ROT && id <= P_D4_ROT) || id == P_RH_FILL) return 3;   /* the patterns' rotations, the fills: Motion */
    if (id >= P_D1_LEN && id <= P_D4_LEN) return -1;                       /* (a loop's length: by hand) */
    if (id >= P_V1_ODDEVEN && id <= P_V2_PLUCK) return 0;                  /* the voices' colour: Timbre */
    if (id >= P_D1_OE && id <= P_D4_HPLUCK) return 1;                      /* the hits' colour and pluck: Groove */
    if (id == P_V1_DEPTH || id == P_V2_DEPTH) return 0;                    /* the voices' depth: Timbre */
    if (id >= P_D1_DENS && id < P_D1_DENS + NUM_DRUMS * DR_STRIDE && (id - P_D1_DENS) % DR_STRIDE == P_D1_BRIGHT - P_D1_DENS)
        return 0;                                                          /* a drum's Bright (dark .. bright): Timbre */
    if (id == P_V1_CLARITY || id == P_V2_CLARITY) return -1;               /* (Clarity: by hand; it switches partials) */
    if (id == P_G1_BAL || id == P_G2_BAL) return 0;                        /* the notes' balance (dark .. bright): Timbre */
    switch (g_param_meta[id].section) {
    case SEC_VOICE1: case SEC_VOICE2: return 0;
    case SEC_METER: case SEC_DRUMS: return 1;                              /* (1.0.6: the drums' levels too, +-4 dB) */
    case SEC_FX: return 2;
    case SEC_GEN1: case SEC_GEN2: case SEC_LFO: return 3;
    default: return -1;
    }
}
/* the tick reads which knob from a table (worked out in the flash when the patch turns Dry or wet: the rules are long,
   and the RAM for code is short) */
static int8_t g_knob[NUM_PARAMS];
static uint8_t g_knob_made = 2;              /* (the table's Dry; 2: not made yet) */
static void knob_table(int dry) {
    for (int i = 0; i < NUM_PARAMS; i++) g_knob[i] = (int8_t)knob_of_rd(i, dry);
    g_knob_made = (uint8_t)dry;
}
DSP_CTL int knob_of_r(int id) {
    if (g_knob_made != g_knob_dry) DSP_FAR(knob_table)(g_knob_dry);
    return id >= 0 && id < NUM_PARAMS ? g_knob[id] : -1;
}
int knob_of(int id) { return DSP_FAR(knob_of_r)(id); }
int knob_of_dry(int id, int dry) { return knob_of_rd(id, dry); }   /* (the roll's: the patch it makes) */      /* (the interface's and the roll's call, from the flash) */
DSP_CTL float knob_offset(const Synth *s, int id) {
    int k = knob_of_r(id); if (k < 0) return 0.f;
    float m = s->kmod[k]; if (m == 0.f) return 0.f;
    const int8_t *w = s->kpath[id];
    float a = m < 0.f ? -m : m, near = (float)(m < 0.f ? w[1] : w[2]), far = (float)(m < 0.f ? w[0] : w[3]);
    float v = a < .5f ? near * (a * 2.f) : near + (far - near) * (a * 2.f - 1.f);
    return v * (k == 2 && g_knob_dry ? 2.f / 127.f : 1.f / 127.f);   /* (Shape, on a Dry patch: twice as far) */
}

DSP_CTL void mod_apply(Synth *s) {
    float *src = s->src;
    g_knob_dry = (uint8_t)(s->target[P_DRY] > .5f);
    for (int j = 0; j < NUM_LFO; j++) src[SRC_LFO1 + j] = s->lfo[j].value;
    for (int m = 0; m < NUM_MACRO; m++) src[SRC_MACRO1 + m] = s->macro[m];
    src[SRC_ENV] = s->eng.env_follow * 4.f > 1.f ? 1.f : s->eng.env_follow * 4.f;
    src[SRC_TRIG] = s->trig_env;
    src[SRC_RAND] = s->rand_hold;
    src[SRC_PATHX] = s->px * 2.f - 1.f; src[SRC_PATHY] = s->py * 2.f - 1.f;
    src[SRC_VEL] = s->last_vel;
    for (int l = 0; l < NUM_LAYERS; l++) {                    /* each motion: its loudest point's envelope */
        const GenSt *g = &s->layer[l].g; float e = 0.f;
        for (int j = 0; j < g->npts; j++) if (g->e[j] > e) e = g->e[j];
        src[SRC_MOT1 + l] = e > 1.f ? 1.f : e;
    }

    float km[NUM_MACRO];
    for (int m = 0; m < NUM_MACRO; m++) km[m] = s->macro[m];
    for (int i = 0; i < NUM_PARAMS; i++) s->mod[i] = s->base[i];
    for (int k = 0; k < MOD_SLOTS; k++) {
        const ModSlot *ms = &s->slot[k];
        if (!ms->src || ms->src >= NUM_SRC) continue;
        float v = ms->depth * src[ms->src];
        if (ms->dst >= MOD_DST_KNOB) { if (ms->dst < MOD_NUM_DST) km[ms->dst - MOD_DST_KNOB] += v; continue; }
        if (!g_param_meta[ms->dst].moddable) continue;
        s->mod[ms->dst] += v;
    }
    int any = 0;
    for (int m = 0; m < NUM_MACRO; m++) {
        float v = km[m]; v = v < -1.f ? -1.f : v > 1.f ? 1.f : v;
        if (!(v == v)) v = 0.f;                                  /* (never a NaN into the paths) */
        s->kmod[m] = v; if (v != 0.f) any = 1;
    }
    /* (1.0.7) the knobs' table read here, once, and each parameter's offset worked out in line (as knob_offset): two
       calls a parameter, 215 of them a tick, were most of this loop's time */
    if (any && g_knob_made != g_knob_dry) DSP_FAR(knob_table)(g_knob_dry);
    const float ksc2 = g_knob_dry ? 2.f / 127.f : 1.f / 127.f;
    for (int i = 0; i < NUM_PARAMS; i++) {
        float ko = 0.f;
        if (any) {
            int k = g_knob[i];
            float m = k >= 0 ? s->kmod[k] : 0.f;
            if (m != 0.f) {
                const int8_t *w = s->kpath[i];
                float a = m < 0.f ? -m : m, near = (float)(m < 0.f ? w[1] : w[2]), far = (float)(m < 0.f ? w[0] : w[3]);
                float kv = a < .5f ? near * (a * 2.f) : near + (far - near) * (a * 2.f - 1.f);
                ko = kv * (k == 2 ? ksc2 : 1.f / 127.f);
            }
        }
        int seq_b = (i == P_G1_B && pint(s, P_G1_FUNC) == GEN_SEQUENCE)
                 || (i == P_G2_B && pint(s, P_G2_FUNC) == GEN_SEQUENCE);
        float v = seq_b ? s->base[i] : (g_param_meta[i].moddable ? s->mod[i] + ko : s->base[i]);
        v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
        s->mod[i] = v;
        if (v != s->last_mod[i]) param_cache(s, i);   /* only what moved pays for its mapping */
    }
}

/*
 * MANIFOLD v8 - portable engine core
 *
 * Everything is a reading of continuous functions. Two layers, each a generator (points moving on the Tonnetz, each
 * with an envelope) and a voice engine (a bank of 16 oscillators a note, at the note's harmonics 1..16, every level
 * and phase set by one continuous function). The rhythm (rhythm.c) is a sum of pulse functions at whole-number ratios
 * to the bar; an event is a peak of that sum over a threshold, and a second layer decides which drum it plays. The
 * same harmonic series is the timbre (partials), the scales (scales.c: the series itself, just intonation, equal
 * temperament, and anything between: Temper) and the time (the pulse functions' ratios).
 *
 * Device: M-VAVE FM-1 (JieLi AC79, pi32v2, single-precision FPU, no libm). float32 only, no malloc, 44.1 kHz,
 * control work every 64 samples.
 */
#ifndef MANIFOLD_SYNTH_H
#define MANIFOLD_SYNTH_H

#include <stdint.h>
#include "fmath.h"

#ifndef SR
#define SR            44100.0f    /* the platform's rate (fm1-base main_fm1.c FS) */
#endif
/* big zero-initialised buffers: the device's .pool RAM (fm1-base plat.h PLAT_POOL), plain statics on a host */
#if defined(PLAT_HOST) || !defined(MANIFOLD_FW)
#define DSP_POOL
#else
#define DSP_POOL __attribute__((section(".pool")))
#endif
/* the per-sample code runs from RAM on the device (fm1-base device/app.ld: .ram_text, "an app's audio code, out of
   the XIP cache's way"; .c1_text is the part of it the second core may run, and where calls to flash code are
   allowed): the voices, the effects and the drums, with their helpers inlined into them. Everything else of the engine
   runs in place from the flash, through the cache both cores and the screen's code share. */
#if defined(PLAT_HOST) || !defined(MANIFOLD_FW) || defined(MANIFOLD_FLASH_ONLY)   /* (a build with everything in the flash: fm1.toml o2_cflags) */
#define DSP_RAM
#define DSP_CTL
#define DSP_INL static inline
#else
#define DSP_RAM __attribute__((section(".c1_text")))
/* the control tick (rhythm, generators, modulation, coefficients): in RAM as well, built for size. From the flash it
   ran through the cache the screen and the radio share, and took most of the block on the device */
#define DSP_CTL __attribute__((section(".c1_text"), minsize))
#define DSP_INL static inline __attribute__((always_inline))
#endif
/* a call from flash code into the RAM code is out of a direct jump's reach: through a pointer the compiler cannot fold
   back (fm1-base bsp/chip/fm1_flash.h FL_FAR does the same) */
#if defined(PLAT_HOST) || !defined(MANIFOLD_FW) || defined(MANIFOLD_FLASH_ONLY)
#define DSP_FAR(fn) fn
#else
static inline __attribute__((always_inline)) void *dsp_far(void *p) { void *volatile q = p; return q; }
#define DSP_FAR(fn) ((__typeof__(&fn))dsp_far((void *)&fn))
#endif
#ifndef SUB
#define SUB           64          /* control sub-block, samples (~1.45 ms) */
#endif
#define MAX_NODES     32          /* the notes of the drum: the series' partials, or a scale over 1 to 3 octaves */
#define MAX_DEGREES   12          /* notes per octave */
#define NUM_LAYERS    2
#define MAX_VOICES    8           /* the most nodes one layer renders at once (the profiler's cap runs 4..this) */
#define GOV_MAX_ACTIVE MAX_VOICES
#define NUM_LFO       6
#define NUM_MACRO     4
#define LIM_LA        64          /* the output limiter's look-ahead, samples (1.45 ms) */
#define MOD_SLOTS     24
#define MAX_PTS_D     8           /* = MAX_PTS (the drums follow a layer's points) */
#define MAX_SEQ_NOTES 16          /* Sequence motion: the notes recorded for one layer */
#define KNOB_WP       4           /* a knob's path: its offsets at -1, -1/2, +1/2 and +1 (0 at the centre) */

#ifndef PI_F
#define PI_F    3.14159265358979f
#endif
#define TWO_PI  6.28318530717959f

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    SEC_SCALE, SEC_VOICE1, SEC_GEN1, SEC_METER, SEC_GEN2, SEC_LFO, SEC_FX, SEC_VOICE2, SEC_DRUMS,
    SEC_KNOBS,                 /* no parameters: the performance knobs' paths */
    NUM_SECTIONS
} Section;
#define SEC_ALL ((1u << NUM_SECTIONS) - 1u)   /* every section (the rolls' mask) */
/* the three rolls, and all of them (the scale is rolled only from its page) */
#define ROLL_SOUND  (1u << SEC_VOICE1 | 1u << SEC_VOICE2 | 1u << SEC_GEN1 | 1u << SEC_GEN2 | 1u << SEC_METER | 1u << SEC_DRUMS)
#define ROLL_MOTION (1u << SEC_LFO | 1u << SEC_FX)
#define ROLL_KNOBS  (1u << SEC_KNOBS)
#define ROLL_ALL    (ROLL_SOUND | ROLL_MOTION | ROLL_KNOBS)

typedef enum {
    /* scale & surface */
    P_SCALE, P_ROOT, P_OCTAVES, P_SURFACE, P_ROTATE, P_TEMPER,
    /* layer 1: the voice (the oscillator bank), then the generator. Layer 2 repeats the block (LP()). */
    P_V1_CENTER, P_V1_BAND, P_V1_TILT, P_V1_STRIDE, P_V1_WAVE, P_V1_PHASE, P_V1_SPIN, P_V1_TONE, P_V1_BLOOM, P_V1_WIDTH, P_V1_LEVEL, P_V1_REG,
    P_G1_FUNC, P_G1_A, P_G1_B, P_G1_RATE, P_G1_POINTS, P_G1_SPREAD, P_G1_ENV, P_G1_ERATE, P_G1_CONTOUR, P_G1_REACH, P_G1_ENERGY, P_G1_ROUTE,
    P_V2_CENTER, P_V2_BAND, P_V2_TILT, P_V2_STRIDE, P_V2_WAVE, P_V2_PHASE, P_V2_SPIN, P_V2_TONE, P_V2_BLOOM, P_V2_WIDTH, P_V2_LEVEL, P_V2_REG,
    P_G2_FUNC, P_G2_A, P_G2_B, P_G2_RATE, P_G2_POINTS, P_G2_SPREAD, P_G2_ENV, P_G2_ERATE, P_G2_CONTOUR, P_G2_REACH, P_G2_ENERGY, P_G2_ROUTE,
    /* the path's view (layer 1's projection of its own state) */
    P_PATH_SCALE, P_PATH_ROT, P_PATH_X, P_PATH_Y,
    /* the rhythm: the stream (which pulse functions, how many of its peaks pass), its long form, what picks the drum */
    P_TEMPO, P_RH_ENGINE, P_RH_DENS, P_RH_THREES, P_RH_FINE, P_RH_SWING, P_RH_TURN, P_RH_FORM, P_RH_MOTION,
    P_RH_ASSIGN, P_RH_SELRATE, P_RH_FEED, P_RH_COUNT, P_RH_PULL, P_RH_GRIP, P_EVOLVE,
    /* the drums: on, the layer they play on, level, accent, shape by, depth, duck; then each drum's block (DP()):
       its triggers (density, place, follow), the note it rides (voice), its envelope (attack, hold, decay, curve),
       level, its character on the voice (bright, settle, snap). Never its pitch: the scale is the synth's */
    P_DR_ON, P_DR_LAYER, P_DR_LEVEL, P_DR_ACCENT, P_DR_SHAPE, P_DR_DEPTH, P_DR_DUCK,
#define DRUM_IDS(D) D##_DENS, D##_PLACE, D##_SRC, D##_VOICE, D##_ATTACK, D##_HOLD, D##_DECAY, D##_CURVE, D##_LEVEL, \
                    D##_BRIGHT, D##_SETTLE, D##_SNAP
    DRUM_IDS(P_D1), DRUM_IDS(P_D2), DRUM_IDS(P_D3), DRUM_IDS(P_D4),
#undef DRUM_IDS
    /* LFOs */
    P_LFO1_RATE, P_LFO1_SHAPE, P_LFO1_SYNC,
    P_LFO2_RATE, P_LFO2_SHAPE, P_LFO2_SYNC,
    P_LFO3_RATE, P_LFO3_SHAPE, P_LFO3_SYNC,
    P_LFO4_RATE, P_LFO4_SHAPE, P_LFO4_SYNC,
    P_LFO5_RATE, P_LFO5_SHAPE, P_LFO5_SYNC,
    P_LFO6_RATE, P_LFO6_SHAPE, P_LFO6_SYNC,
    P_LFO_COUPLE,
    /* effects */
    P_FZ_SUSTAIN, P_FZ_TONE, P_FZ_MIX,
    P_FLT_CUT, P_FLT_RES, P_FLT_MORPH,
    P_DL_SEND, P_DL_TIME, P_DL_FB, P_DL_TONE, P_DL_TAP,
    P_RV_SEND, P_RV_SIZE, P_RV_DECAY, P_RV_DAMP, P_RV_PRE, P_RV_MOD,
    P_RV_TUNED, P_RV_FREEZE, P_DL_TO_RV,
    P_CMP_THRESH, P_CMP_RATIO, P_CMP_ATT, P_CMP_REL, P_CMP_MIX, P_CMP_PUMP,
    P_MASTER,
    /* the patterns (1.0.5), after everything older so a patch's parameter numbers keep their meaning: each drum's
       line from the trained library (0: linear, the stream's own choice), its rotation in steps, its loop's length
       (0: the line's own), the drum it mutes on the steps it plays; how much the patterned drums also take the
       linear layer's events (ghosts), the phrase fills, the level the swing bends (8ths or 16ths); the keys' pull */
    P_D1_PAT, P_D2_PAT, P_D3_PAT, P_D4_PAT,
    P_D1_ROT, P_D2_ROT, P_D3_ROT, P_D4_ROT,
    P_D1_LEN, P_D2_LEN, P_D3_LEN, P_D4_LEN,
    P_D1_MUTE, P_D2_MUTE, P_D3_MUTE, P_D4_MUTE,
    P_RH_GHOST, P_RH_FILL, P_RH_SWLEV, P_KEY_PULL,
    /* each voice's colour (1.0.5): Odd/even, the balance of its odd and even partials (the middle as before), and
       Pluck, its upper partials fading faster than the lower after it rises (0: off) */
    P_V1_ODDEVEN, P_V2_ODDEVEN, P_V1_PLUCK, P_V2_PLUCK,
    /* and each drum's on the note it hits: Odd/even, the note's colour pushed at the hit and settling with it (as its
       Bright does; the middle: none), and Pluck, the note plucked by the hit (0: the layer's own) */
    P_D1_OE, P_D2_OE, P_D3_OE, P_D4_OE,
    P_D1_HPLUCK, P_D2_HPLUCK, P_D3_HPLUCK, P_D4_HPLUCK,
    /* each voice's reach below its pitch: Depth, how much of its energy goes to its undertones (an octave, two, three
       below: in tune), and Clarity, how many partials of all the notes each octave may hold (0: any) */
    P_V1_DEPTH, P_V2_DEPTH, P_V1_CLARITY, P_V2_CLARITY,
    /* each layer's Balance: how its motion's energy is shared among the notes, Dark (toward the bass) .. Balanced
       (every note alike, whatever the motion favours) .. Bright (toward the middle) */
    P_G1_BAL, P_G2_BAL,
    P_DRY,                     /* (1.0.6) the effects out: fuzz, filter, delay, reverb; the Space knob is Shape */
    P_DR_NOTES,                /* (1.0.6) the drums' notes, Follow (0: the motion's sounding ones) .. Fixed (1: a kit at
                                  set pitches); between, a drum rides a sounding note near its pitch, else its own */
    NUM_PARAMS
} ParamId;
#define NUM_PARAMS_F (P_G2_BAL + 1)                      /* 1.0.5's ("MNPF") */
#define NUM_PARAMS_E (P_V2_CLARITY + 1)                  /* the 1.0.5 test build's before Balance ("MNPE") */
#define NUM_PARAMS_D (P_D4_HPLUCK + 1)                   /* the 1.0.5 test build's before the undertones ("MNPD") */
#define NUM_PARAMS_C (P_V2_PLUCK + 1)                    /* the 1.0.5 test build's with the voices' colour ("MNPC") */
#define NUM_PARAMS_A (P_KEY_PULL + 1)                    /* the 1.0.5 test builds' patches before the voices' colour ("MNPA") */
#define NUM_PARAMS_9 (P_MASTER + 1)                      /* the parameters of the patches before 1.0.5 ("MNP9") */

#define LAYER_STRIDE (P_V2_CENTER - P_V1_CENTER)
#define LP(l, p) ((p) + (l) * LAYER_STRIDE)             /* layer l's copy of a layer-1 parameter */
#define DR_STRIDE (P_D2_DENS - P_D1_DENS)
#define DP(d, p) ((p) + (d) * DR_STRIDE)                 /* drum d's copy of a drum-1 parameter */

typedef enum { CURVE_LIN, CURVE_LOG, CURVE_DISC } Curve;

typedef struct {
    const char *name;
    uint8_t  section;
    uint8_t  curve;
    uint8_t  moddable;    /* can the matrix / macros reach it */
    uint8_t  structural;  /* change needs a duck + rebuild */
    float    lo, hi;      /* real-unit range (DISC: count in hi) */
    float    def;         /* normalized default */
    float    rlo, rhi;    /* normalized randomization window */
    uint8_t  tri;         /* 1 = triangular (centre-weighted) roll */
} ParamMeta;

extern ParamMeta g_param_meta[NUM_PARAMS];

/* ------------------------------------------------------------------ */
/* Modulation                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    SRC_NONE,
    SRC_LFO1, SRC_LFO2, SRC_LFO3, SRC_LFO4, SRC_LFO5, SRC_LFO6,
    SRC_MACRO1, SRC_MACRO2, SRC_MACRO3, SRC_MACRO4,
    SRC_ENV,      /* the output's level */
    SRC_TRIG,     /* layer 1's rises (how much is starting now) */
    SRC_RAND,     /* a random value held for a bar */
    SRC_PATHX, SRC_PATHY,
    SRC_VEL,      /* the last key's velocity */
    SRC_MOT1, SRC_MOT2,   /* each layer's motion: its loudest point's envelope (so the generators can shape the drums) */
    NUM_SRC
} ModSrc;

typedef struct { uint8_t src, dst; float depth; } ModSlot;
/* destinations past the parameters: the four performance knobs (MOD_DST_KNOB + k). A route there moves the knob's
   position (and so its whole path) as if the hand turned it; the knob's own source still reads the hand alone. */
#define MOD_DST_KNOB  NUM_PARAMS
#define MOD_NUM_DST   (NUM_PARAMS + NUM_MACRO)

typedef struct {
    float phase, value, prev, target;  /* target: smooth random */
    float logistic;
    uint32_t step;
} Lfo;

/* ------------------------------------------------------------------ */
/* Scales (scales.c, scale_data.h)                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    int   n;                  /* notes: per octave, or (series) in all */
    float ratio[MAX_NODES];   /* just ratio to the ding: in [1, 2) ascending, ratio[0] = 1; (series) the partial number */
    float et[MAX_NODES];      /* the same note in 12-tone equal temperament, as a ratio */
    int   num[MAX_NODES], den[MAX_NODES];
    float wt[MAX_NODES];      /* tonal weight, 0.45..1: the note's harmonicity to the ding (1/1 1, 3/2 0.9, 5/4 0.7 ...) */
    int   series;             /* the notes are partials of the ding (not repeated by octave): 1 all, 2 the odd ones */
    float period;             /* 2 */
    int   family, rot;
    char  name[40];
} Scale;

int  scale_count(void);
int  scale_group_start(int g);                /* the first scale of group 0..3 (harmonic, pentatonic, modes, other); 4: the count */
void scale_get(int index, Scale *out);        /* rotation 0 */
void scale_get_rot(int index, int rot, Scale *out);
void scale_series_n(Scale *sc, int n);        /* a series scale's partials, n of them */

/* ------------------------------------------------------------------ */
/* The surface                                                         */
/* ------------------------------------------------------------------ */
enum { SURF_PLANE, SURF_TORUS, SURF_SPHERE, NUM_SURFACES };
/* (1.0.5: Hip-hop took Tongue drum's place, House Drum circle's; Breaks became Breakbeat) */
enum { STYLE_WILD, STYLE_HIPHOP, STYLE_GROOVE, STYLE_AMBIENT, STYLE_COSMIC, STYLE_HOUSE, STYLE_MACHINE, STYLE_BREAKS, NUM_STYLES };

typedef struct {
    float x, y;            /* surface coords, [0,1)^2 */
    float freq;            /* fundamental, Hz */
    float lr;              /* log2 of ratio */
    float ratio;           /* ratio to the ding */
    float gl, gr;          /* pan gains (Width) */
    float level;           /* how loud it is now (both layers): the screen, the keys' lights */
    int   active;          /* a voice sounds it */
    uint8_t nbr[4];        /* four nearest nodes on the surface */
    int8_t lat[3];         /* lattice coordinates: the exponents of 3, 5 and 7 (the Tonnetz the surface is laid out on) */
    float lx, ly;          /* its place on the lattice (11, 13 and the higher primes each have a direction too) */
    float lrj, lre;        /* log2 of its ratio to the ding: just, and equal-tempered (Temper moves between them) */
    float wt;              /* tonal weight x the bass voicing: what a generator's drive is scaled by */
    uint8_t pc;            /* pitch class: its degree in the scale (series: its odd part's index) */
    uint8_t oct;           /* register: octaves above the ding */
    float sx, sy, sz;      /* (the sphere) its place in 3-D, halved: surf_d2 reads it */
} Node;

typedef struct {
    int   n_nodes;
    Node  node[MAX_NODES];
    int   surface;
    float env_follow;
    int   n_active;
    float fcx, fcy, fext;  /* the frame the motions are drawn in: the notes' centre and extent */
    float spacing;         /* the mean distance from a node to its nearest neighbour (proximity is measured in it) */
    float temper;          /* the Temper the nodes' pitches were last set for */
    int   root;            /* the ding's node (the strongest tonal weight, the lowest) */
} Engine;

/* ------------------------------------------------------------------ */
/* The rhythm (rhythm.c)                                               */
/* ------------------------------------------------------------------ */
/* The stream: R(x) = sum over k of w_k b(k x - p_k), x in bars, b a periodic pulse function (period 1, peak 1 at 0),
   k whole numbers: 1 2 4 8 16 32 (the twos), 3 6 12 24 (the threes), 18, and a second function at 4 whose phase is the
   swing. An event is a peak of R over the threshold: when, and how strongly. Slower functions at 1/2 .. 1/64 of the
   bar move the threshold, two weights and the selector (Form). Nothing is on a grid. */
#define RH_NH 12
enum { RH_THRESHOLD, RH_LOCK, NUM_RH_ENGINES };
enum { RH_SELECT, RH_COUNT, NUM_RH_ASSIGN };
#define NUM_SELRATES 5
typedef struct {
    float tau;                     /* now, as a fraction of the bar */
    uint32_t bar;                  /* bars since the start */
    float bar_sec;                 /* a bar's length, seconds (4 beats) */
    float beats;                   /* beats in a bar: 4 (Tempo counts beats) */
    float w[RH_NH], ph[RH_NH];     /* the pulse functions' weights and phases now */
    float top;                     /* the weights' sum (the most R can be) */
    float th;                      /* the threshold now */
    float R, R1, R2;               /* R now and at the two ticks before (a peak is found between them) */
    float Rn;                      /* R as 0..1 of top (the layers' accent, the screen) */
    float sel;                     /* the selector now, -1..1 */
    float tomb;                    /* the tom's bias now */
    float tired[4];                /* Select's feedback: how lately each drum played */
    uint32_t count;                /* Count: events since the strongest */
    uint32_t nev;                  /* events so far */
    float ev_s;                    /* the last event's strength (the screen) */
    float ev_flash;                /* ... and its light */
    float lk[4], lkm;              /* Lock: each drum's oscillator phase (cycles), the master's */
    float w0[RH_NH], top0;         /* the patterns: the stream's weights without swing (a step's place in the metre) */
    float lkq[4];                  /* ... and how well it is locked, 0..1 (the screen) */
} Rhythm;
const char *rh_engine_name(int e);
const char *rh_assign_name(int a);
const char *selrate_name(int r);
struct Synth;
void  rhythm_tick(struct Synth *s, float dt);              /* the clock, the weights, the stream, the events */
float rhythm_R(const struct Synth *s, float tau);          /* the stream at bar fraction tau (this bar), 0..1 of top */
float rhythm_slow(const struct Synth *s, int q, float ph); /* cos 2 pi (x / q - ph): a function at 1/q of the bar rate */

/* ------------------------------------------------------------------ */
/* Generators (gen.c)                                                  */
/* ------------------------------------------------------------------ */
/* A generator is 1..MAX_PTS points on the Tonnetz, each moving along a continuous function of time, each with an
   envelope that is a continuous function too. A note's loudness is the sum over the points of the point's envelope
   times a smooth function of its distance to the note. Most motions are continuous paths; Sequence is the recorded-note path. */
enum { GEN_ORBIT, GEN_LADDER, GEN_SWARM, GEN_LORENZ, GEN_MOTIF, GEN_DRIFT, GEN_BILLIARD, GEN_GRAVITY, GEN_STRIDE,
       GEN_LOOPS, GEN_ERODE, GEN_CHORDS, GEN_SEQUENCE, NUM_GENS };
enum { ENV_PULSE, ENV_SWELL, ENV_SPEED, ENV_PASS, ENV_HEIGHT, ENV_HOLD, ENV_STREAM, ENV_DRUMS, NUM_ENVS };
#define NUM_GRATES 16          /* Rate: whole-number ratios to the bar */
/* a layer's route: it plays, or it modulates the other layer (and is silent): its path moves the other's points, its
   envelopes add to the other's, or multiply them. Its Energy is then the depth */
enum { ROUTE_PLAY, ROUTE_MOVE, ROUTE_ADD, ROUTE_MUL, NUM_ROUTES };
/* the drums: four voices (kick, snare, hat, tom). The rhythm's events are theirs (Select or Count picks which drum
   each one plays), or a drum follows a layer's point (Follow) */
#define NUM_DRUMS 4
enum { DSRC_STREAM, DSRC_L1, DSRC_L2, NUM_DSRC };
/* the layer the drums play on: one, or Split (kick and tom on the layer sounding lower, snare and hat the higher) */
enum { DLAYER_L1, DLAYER_L2, DLAYER_SPLIT, NUM_DLAYERS };
#define DQ 8                    /* a drum's queue of hits to come (a roll, late microtiming) */
enum { DSHAPE_NONE, DSHAPE_L1, DSHAPE_L2, NUM_DSHAPES };
/* a drum's hit on a voice for one control tick: the voice (layer, node), its level at the tick's start and end (the
   end reached from sample off: where a new hit lands), and the character it puts on the voice at the tick's middle:
   the partials' tilt (log2 a partial), the Wave push, and the kick's lift of the 90 Hz fade. (Never the pitch) */
#define MAX_HARM   16         /* oscillators a note: its harmonics 1..13, then its undertones 1/2, 1/4, 1/8 */
#define NH         13         /* (the harmonics' slots: 0..12; the undertones': 13, 14, 15) */
typedef struct { int8_t layer, node; uint8_t off, d; float h0, h1, tilt, snap, w, oe, pl; } DrumHit;   /* (oe: its Odd/even push, -1..1; pl: its Pluck) */   /* w: the character's strength, by the hit's envelope */
typedef struct {
    float env;                 /* the envelope, 0..1 (Shape by's Drums motion envelope and the duck read it) */
    float lc;                  /* (1.0.6) Dry: the level Shape's lengths give back (dr_lcomp) */
    float g;                   /* this hit's velocity */
    float t;                   /* samples since the hit (where the envelope is) */
    float ls;                  /* the level a hit started from (a retrigger rises from where it is, no click) */
    int   on;                  /* a hit is sounding */
    int   node, layer;         /* the voice it rides */
    DrumHit cur, old;          /* this tick's hit, and the one it replaced on another voice, fading out */
    int   delay, pend;         /* a hit pending: it lands this many samples into the coming block */
    uint8_t armed[MAX_PTS_D];  /* Follow: a point's envelope fell back below the threshold */
    int32_t qt[DQ]; float qv[DQ]; int16_t qn[DQ]; int nq;   /* the queue: samples from now, velocity, note (unused) */
} DrumV;
typedef struct {
    DrumV v[NUM_DRUMS];
    uint32_t hits;             /* hits so far (the tests watch it) */
    float flash[NUM_DRUMS];    /* the screen: a hit's light */
    float shape, shape_tgt;    /* Shape by: the layer's envelope, glided */
    float shape_e;             /* ... and the envelope itself (it lengthens the decay) */
    float duck, dk0, dk1;      /* the kick's sidechain: its envelope (0 none .. 1 full), the gain on the other voices at
                                  this tick's start and end */
    float oa[NUM_DRUMS][MAX_HARM];   /* each drum's Odd/even push at full, as its layer's bank coloured (drums_tick) */
    float of[NUM_DRUMS];             /* ... and that push (-1..1): a note takes the share its hit gives it */
    uint8_t hn[NUM_DRUMS];           /* each drum's hits, counted (MIDI out watches them) */
    uint32_t hs[NUM_DRUMS];          /* ... and where the last one lands (Synth.nsamp's count) */
    uint8_t pf[NUM_DRUMS];           /* (1.0.6) the last hit had Fixed in it: oa holds its colour toward its drum's
                                        profile (Body, Crack, Sizzle), by how far toward Fixed */
} Drums;
#define MAX_PTS 8
#define NUM_ERATES 13
typedef struct {
    int   func, n, npts;
    float t;                       /* the motion's clock, in cycles of Rate */
    float mgn[MAX_PTS];            /* a motion's own gain on each point (Loops, Erode: 1 otherwise) */
    float x[MAX_PTS], y[MAX_PTS];  /* each point, generator coordinates [-0.5, 0.5]^2 (before the path view) */
    float qx[MAX_PTS], qy[MAX_PTS];/* ... on the surface (after it) */
    float sx[MAX_PTS], sy[MAX_PTS], sz[MAX_PTS], sw[MAX_PTS];   /* flow state per point (ODEs, phases, drift) */
    float lx[MAX_PTS], ly[MAX_PTS], spd[MAX_PTS];               /* last position, speed (smoothed) */
    float ht[MAX_PTS];             /* the flow's third coordinate, 0..1 (the Height envelope) */
    float hm[MAX_PTS], hv[MAX_PTS];/* its running mean and variance (2 s): the envelope reads it relative to them */
    float rx[MAX_PTS], ry[MAX_PTS], rh[MAX_PTS], jx[MAX_PTS], jy[MAX_PTS], jh[MAX_PTS]; int key;  /* continuity across formula changes */
    uint8_t seq[MAX_SEQ_NOTES];    /* Sequence: recorded engine-node indices, in playing order */
    uint8_t seq_len, seq_recording;
    float avg_age; uint32_t rng0;  /* the averages' age; the motion's fixed seed */
    float sm[MAX_PTS];             /* the speed's running mean (2 s) */
    float ox[MAX_PTS], oy[MAX_PTS];/* the keys' pull on each point (continuous) */
    float eph[MAX_PTS];            /* envelope phase, cycles */
    float e[MAX_PTS];              /* envelope, 0..1 */
    float u[MAX_NODES];            /* drive: sum over points of envelope x proximity */
    float a[MAX_NODES];            /* loudness: the drive through the consonance gain, the voices' target */
    float upv[MAX_NODES];          /* last tick's drive (its slope ranks rising notes; rises are counted) */
    float kage[MAX_NODES][4], kvel[MAX_NODES][4];  /* the keys' pushes: age (s) and velocity */
    float gw[MAX_NODES];           /* Balance: each note's share of the drive (gen.c, balance_tab.h) */
    float gwkey[4]; uint8_t gwtick;                /* (what it was worked out for) */
    float kin[MAX_NODES];          /* the keys' input signal: the pushes' kernels plus what is held */
    uint8_t held[MAX_NODES]; float held_v[MAX_NODES], hlev[MAX_NODES];
    float px, py, pxs, pys;        /* the path shown: point 0, generator coordinates */
    float evo_a, evo_b, evo_va, evo_vb;
    float la, lb;                  /* A and B at the last tick (Billiard turns by the change) */
    float vw[MAX_NODES];           /* (unused) */
    float onset;
    uint32_t rises;
    uint32_t rng;
} GenSt;
const char *gen_name(int g);
const char *env_name(int e);
const char *erate_name(int r);
int grate_of(float cycles);                                       /* Rate's nearest whole-number ratio */
const char *grate_name(int r);
float grate_value(int r);
const char *gen_ab_name(int g, int which);                        /* what A (0) and B (1) mean for g */
void gen_ab_text(int g, int which, float norm, char *buf, int len); /* their value, in g's terms */
struct Synth;
void gen_reset(GenSt *g, uint32_t seed);
void gen_tick(struct Synth *s, int layer, float dt);              /* the drive u_n(t) of every node, now */
void gen_harmony(struct Synth *s, float dt);                      /* a_n = u_n, smoothed (3 ms), both layers */
void gen_key(struct Synth *s, int layer, int node, float vel, int on);
void gen_seq_record(struct Synth *s, int layer_mask, int on);
void gen_seq_note(struct Synth *s, int layer_mask, int node);

/* ------------------------------------------------------------------ */
/* Effects                                                             */
/* ------------------------------------------------------------------ */
#define RV_BUF  2300          /* reverb line buffers, in half-rate steps: lines up to 4500 samples (102 ms) */
#define RV_PRE  2300          /* the pre-delay, half-rate steps: up to 0.1 s */
typedef struct { float ic1, ic2; } Svf;

typedef struct {
    /* fuzz, 2x oversampled: two soft-clipping stages, each with its own filters, then the tone */
    float dc_x[2], dc_y[2];
    float fz_hp[2], fz_l1[2], fz_h1[2], fz_l2[2], fz_tl[2], fz_th[2], fz_thx[2];
    Svf   morph[2];
    /* BBD delay */
    int16_t dbuf[16384];      /* (1.0.7: 16-bit floats, fx.c to16 / fr16) */
    int    dw; float dacc, dD, dlp1, dlp2, dout_lp[2], dfb_hp;
    /* FDN reverb */
    int16_t rbuf[8][RV_BUF];  /* (16-bit floats) */
    int    rw[8]; float rlen[8], rlen_tgt[8], rlp[8];
    float  ap_buf[4][200]; int ap_w[4];
    int16_t pre[RV_PRE]; int pre_w;   /* (16-bit floats) */
    float  rmod_ph;
    /* compressor */
    float cenv, cgain_db, cgain; int ccount;
    uint32_t clips;           /* samples past the limiter's ceiling that the final soft clip shaped (rare: pushed hard) */
    float lim_d[2][LIM_LA];   /* the limiter's look-ahead delay */
    float lim_seg[LIM_LA / 8], lim_pk, lim_g, lim_tgt; int lim_w, lim_n;
    float lim_gr;             /* the limiter's gain reduction now, dB (<= 0; the screen's meter) */
    float odc_x[2], odc_y[2];
    float rv_h[4], rv_o[2]; int rv_ph;   /* the half-rate reverb: its input low-pass's state (two biquads), its last
                                            output, which sample */
    float fxw;                /* (1.0.6) the filter in: 1, or 0 set wide open (gliding ~30 ms between); at 0 it is not run */
} Fx;

/* ------------------------------------------------------------------ */
/* Voices: a bank of oscillators per note (voice.c)                    */
/* ------------------------------------------------------------------ */
#define OSC_N      4096       /* the oscillators' wave tables */
typedef struct {
    uint32_t ph2;             /* the detuned copy's phase (the note's first partials, doubled a few cents away) */
    uint32_t ph;              /* the note's phase at an eighth of its pitch (2^32 a cycle): every partial is a whole
                                 multiple of it (harmonic k: 8k; the undertones 4, 2, 1), plus its offset */
    float a;                  /* loudness at the start of this block */
    float gv;                 /* the voice cap's crossfade: 1 rendered, 0 not (ramps, never jumps) */
    uint8_t on;               /* it was rendered last block (else its phases start aligned) */
    float age;                /* Pluck: seconds since its level last rose */
    float pk;                 /* and that level (let down slowly, so a new rise after a fall starts it again) */
    float hp;                 /* the Pluck a drum's hit gave it: held until the note rises by itself (or 2 s without hits) */
    float hq;                 /* seconds since a hit last plucked it */
    float pq;                 /* Pluck's step between partials, as it was last block (it opens over a few blocks, not in one) */
    float oe;                 /* a hit's Odd/even push on it, smoothed the same way */
    int8_t hd;                /* the drum whose push it is (its colour: Drums.oa) */
} VoiceSt;
typedef struct {
    GenSt   g;
    VoiceSt vs[MAX_NODES];
    float   amp[MAX_HARM];         /* the bank's function now: each oscillator's level (unit energy over the 16), glided */
    float   amc[MAX_HARM];         /* and with the layer's Odd/even (voices_tick) */
    float   amp_t[MAX_HARM];       /* ... its target */
    float   pho[MAX_HARM];         /* each oscillator's phase offset now, cycles (Phase's law, turned by Spin) */
    uint16_t pmask[MAX_NODES];     /* Clarity: which of each note's partials may play (a bit a slot; voices_tick) */
    float   mkey[4];               /* (what the mask was worked out for) */
    float   spin;                  /* Spin's angle, cycles */
    float   level_g;               /* the layer's output gain (Level, gain staging), smoothed */
    float   eavg;                  /* the layer's loudness energy (sum of a_n^2), averaged */
    int     n_voices;              /* rendered this block */
    int     n_osc;                 /* oscillators rendered this block (the profiler, the tests) */
} Layer;

/* ------------------------------------------------------------------ */
/* Top level                                                           */
/* ------------------------------------------------------------------ */
typedef struct Synth {
    float base[NUM_PARAMS];    /* rolled / edited values (normalized) */
    float target[NUM_PARAMS];  /* morph target */
    float mod[NUM_PARAMS];     /* base + modulation, clamped */
    float val[NUM_PARAMS];     /* mod in real units, cached once per tick (pval) */
    int32_t ival[NUM_PARAMS];  /* discrete index, cached (pint): 32-bit, so reading one is not a 16-bit load beside the
                                  multiply that indexes with it (fm1-base bsp/docs/CPU.md) */
    float last_mod[NUM_PARAMS];
    float px, py;              /* path position (layer 1), once per tick, surface coordinates */
    float morph_rate;          /* per control tick */
    int   morphing;
    float macro[NUM_MACRO];    /* encoder positions, -1..1 */
    float kmod[NUM_MACRO];     /* the knobs as the paths read them: hand + matrix routes, -1..1 (mod_apply) */
    ModSlot slot[MOD_SLOTS];
    int8_t  kpath[NUM_PARAMS][KNOB_WP];   /* each parameter's offsets along its knob's path (x 1/127 of its range) */
    float src[NUM_SRC];
    Lfo   lfo[NUM_LFO];
    uint8_t locked[NUM_SECTIONS];

    Scale scale;
    Engine eng;
    Rhythm meter;              /* the rhythm (rhythm.c); the bar's clock is in it */
    Layer layer[NUM_LAYERS];
    int   style;
    uint8_t held[MAX_NODES];   /* nodes held by a key or MIDI */
    uint8_t keys_to;           /* where the keys play: 0 layer 1, 1 layer 2, 2 both, 3 the drums */
    Fx    fx;
    float l2_l[SUB], l2_r[SUB];/* layer 2's sub-block (it may be rendered on the second core) */

    float duck, duck_tgt; int rebuild_pending;
    /* runtime caps the profiler page sets */
    uint8_t cap_pad_voices;    /* layer 2 voices, 4..MAX_VOICES */
    uint8_t cap_nodes;         /* layer 1 voices, 4..MAX_VOICES */
    uint8_t cap_harm;          /* oscillators per note, 4..MAX_HARM */
    uint8_t transport;         /* the meter runs */
    uint32_t nsamp;            /* samples rendered so far (MIDI out times the drums' hits by it) */
    uint8_t mute_seq;          /* performance: the generators keep time but drive nothing (keys still play) */
    float lay_t[2], lay_g[2];  /* performance: layer gains, target and glide (0 muted, 1 open) */
    float trig_env, last_vel, rand_hold, rand_a, rand_b;   /* rand_hold: smooth between a per-bar a and b */
    /* profiler: ticks (plat_ticks24 on the device) spent per section, summed since the last read */
    uint32_t (*clock)(void);
    uint32_t prof[8], prof_blocks;
    uint32_t prof_peak;        /* the longest synth_process call since the last read (ticks) */
    uint8_t build_defer;
    volatile uint8_t build_req;
    /* second core: layer 2 on core 2 while core 1 runs layer 1 (0: one core) */
    int  (*par_run)(void (*fn)(uint32_t), uint32_t arg);
    int  (*par_wait)(void);
    void (*par_sync)(void);
    int   par_on;
    float sub_l[SUB], sub_r[SUB];
    Drums dr;
    uint32_t rng;
    uint32_t seed;
    int   tick;
    uint32_t n_rises;          /* rises the generators made (the tests watch it) */
} Synth;

/* API */
void synth_init(Synth *s, uint32_t seed);
void synth_process(Synth *s, float *outL, float *outR, int n);
void synth_randomize(Synth *s, uint32_t seed, float morph_seconds);
void synth_mutate(Synth *s, float amount);
void synth_roll_sections(Synth *s, uint32_t seed, float morph_seconds, uint32_t mask);   /* bit i: Section i */
void synth_set_param(Synth *s, int id, float norm);
void synth_set_macro(Synth *s, int m, float v);
void synth_describe(Synth *s, char *buf, int len);
void synth_set_style(Synth *s, int style);
void synth_strike(Synth *s, int node, float velocity);              /* a tap: a push that is let go at once */
void synth_note(Synth *s, int node, float velocity, int on);        /* a held push: key down / up */
void synth_restart(Synth *s, uint32_t seed);                        /* the generators and the meter from the top */
enum { PROF_SEQ, PROF_CTRL, PROF_PAD, PROF_BANK, PROF_FX, PROF_TOTAL, PROF_PADWORK, PROF_N };   /* SEQ: generators; BANK: layer 1; PAD: layer 2 */
void synth_build_now(Synth *s);
void synth_silence(Synth *s);               /* everything silent at once (panic) */

/* voices (voice.c) */
void voices_init(void);
void voices_build(Synth *s);                /* the bank's function, at once (a rebuild) */
void bank_fn(float c, float w, float te, float st, int kmax, float *a);   /* the bank's function (unit energy) */
void hit_colour(const Synth *s, int l, float o, float *out);           /* layer l's bank under Odd/even o */
void hit_profile(const Synth *s, int d, int l, int node, float o, float *out);   /* (1.0.6) Fixed: drum d's spectrum on that note */
int  drum_fixed(const Synth *s, int d, float x, int *layer);           /* (1.0.6) drum d's note at Follow..Fixed x (its layer), -1 none */
void voices_tick(Synth *s, int layer);      /* per control tick: the bank's function (levels, phases) from the controls */
void layer_render(Synth *s, int layer, float *L, float *R, int n);
int  knob_of_r(int id);                     /* (knob_of, in RAM: the tick's) */
int  knob_of_rd(int id, int dry);
int  knob_of_dry(int id, int dry);          /* knob_of for a patch Dry or not (the roll's) */
extern uint8_t g_knob_dry;                  /* the patch is Dry: Space is Shape */
int  knob_of(int id);                       /* the performance knob a parameter rides: 0 Timbre, 1 Groove, 2 Space, 3 Motion; -1 none */
const char *knob_name(int k);
float knob_offset(const Synth *s, int id);  /* the parameter's offset now, from its knob's position along its path */
void params_roll_knobs(Synth *s);
void params_roll_knobs_part(Synth *s, int i0, int i1);

/* internal, shared between files */
DSP_INL float pval(const Synth *s, int id) { return s->val[id]; }   /* modulated, real units (cached) */
DSP_INL int   pint(const Synth *s, int id) { return s->ival[id]; }  /* discrete index (cached) */
DSP_INL float pnorm(const Synth *s, int id) { return s->mod[id]; }  /* modulated, normalized */
void  param_cache(Synth *s, int id);        /* refresh val / ival of one param */
void  params_cache_all(Synth *s);
DSP_INL uint32_t xs32(uint32_t *st) {
    uint32_t x = *st ? *st : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *st = x;
}
DSP_INL float frand(uint32_t *st) { return (float)(xs32(st) >> 8) * (1.0f / 16777216.0f); }
/* fast math, inline in every unit (the DSP files are separate units on the device: no cross-file inlining) */
extern float g_sin_tab[1025];
DSP_INL float fsin(float t) {                    /* t in turns */
    t -= (float)(int)t; if (t < 0) t += 1.f;
    float f = t * 1024.f; int i = (int)f; if (i > 1023) i = 1023;
    float fr = f - (float)i;
    return g_sin_tab[i] + (g_sin_tab[i + 1] - g_sin_tab[i]) * fr;
}
DSP_INL float ftanh(float x) {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    float x2 = x * x; return x * (27.f + x2) / (27.f + 9.f * x2);
}
DSP_INL float fexp2(float x) {
    if (x < -126.f) return 0.f;
    if (x > 126.f) x = 126.f;
    int32_t fl = (int32_t)x; if ((float)fl > x) fl--;
    float f = x - (float)fl;
    /* 2^f on [0, 1): the coefficients sum to exactly 1, so 2^x is continuous across whole numbers and a decay factor
       2^(-tiny) is right (a cubic that ended 0.0003 short of 2 made every long per-sample decay last 80 ms) */
    union { float f; int32_t i; } u = { 1.f + f * (0.6930321f + f * (0.2413698f + f * (0.0520372f + f * 0.0135609f))) };
    u.i += fl * (1 << 23);
    return u.f;
}
DSP_INL float flog2(float x) {
    union { float f; int32_t i; } u = { x };
    float e = (float)((u.i >> 23) & 255) - 128.f;
    u.i = (u.i & ~(255 << 23)) + (127 << 23);
    float m = u.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}
float dsp_sin(float t); float dsp_exp2(float x); float dsp_log2(float x);   /* (ctl_on.h) */
DSP_INL float frsqrt(float x) {                  /* 1/sqrt(x), two Newton steps */
    union { float f; int32_t i; } u = { x };
    u.i = 0x5f3759df - (u.i >> 1);
    float y = u.f; y = y * (1.5f - 0.5f * x * y * y); return y * (1.5f - 0.5f * x * y * y);
}
DSP_INL float ftan_pi(float x) {                 /* tan(pi * x), x in [0, 0.43]: the SVF prewarp */
    float a = PI_F * x; if (a > 1.35f) a = 1.35f;
    float a2 = a * a;
    return a * (135135.f - a2 * (17325.f - a2 * (378.f - a2))) / (135135.f - a2 * (62370.f - a2 * (3150.f - 28.f * a2)));
}
DSP_INL float fabs_(float x) { return x < 0.f ? -x : x; }
DSP_INL float floor_(float x) { float f = (float)(int)x; return f > x ? f - 1.f : f; }   /* (|x| < 2^31) */
DSP_INL float fclamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

void engine_build(Synth *s);
void engine_update(Synth *s);               /* per control tick: pan, the path point on the surface */
float surf_dist(int surf, float x1, float y1, float x2, float y2);
/* the squared distance from one point to many nodes (surf_dist squared, without its root): the point once (surf_pt),
   then each node (surf_d2). On the sphere the node's place in 3-D is kept (engine.c layout) */
typedef struct { float x, y, ax, ay, az; } SurfPt;
DSP_CTL void surf_pt(int surf, float x, float y, SurfPt *p);   /* (engine.c: the tick's, once a point) */
DSP_INL float surf_d2(int surf, const SurfPt *p, const Node *nd) {
    float dx, dy;
    if (surf == SURF_SPHERE) { float dz; dx = p->ax - nd->sx; dy = p->ay - nd->sy; dz = p->az - nd->sz; return dx * dx + dy * dy + dz * dz; }
    dx = p->x - nd->x; dy = p->y - nd->y;
    if (surf == SURF_TORUS) { dx -= floor_(dx + .5f); dy -= floor_(dy + .5f); }
    return dx * dx + dy * dy;
}
int   node_near(const Synth *s, float ratio);            /* the node nearest a pitch (MIDI) */
void path_unview(const Synth *s, float dX, float dY, float *px, float *py);
int  gen_modulator(const Synth *s);
int  gen_modulator_r(const Synth *s);   /* (the same, in RAM: the tick's) */   /* the layer that modulates the other, or -1 */
const char *route_name(int r);
void path_view(const Synth *s, float px, float py, float *x, float *y);   /* a projection -> surface coords */

/* the patterns (patterns.c, trained from tools/patterns/corpus.py; rhythm.c plays them, params.c rolls them) */
typedef struct { uint32_t hit, ghost; uint8_t len; } PatLine;   /* a line: its steps (16ths, 16 or 32), the ghosts among them */
typedef struct { uint8_t genre, line[4], bpm_lo, bpm_hi, sw_lo, sw_hi, lev, w; } PatEntry;   /* a corpus entry */
enum { PG_HOUSE, PG_BREAKS, PG_DNB, PG_HIPHOP };
enum { PMUTE_NONE, PMUTE_KICK, PMUTE_SNARE, PMUTE_HAT, PMUTE_TOM, PMUTE_LINEAR, NUM_PMUTE };
extern const PatLine PAT_LIB[];
extern const uint8_t PAT_OFF[], PAT_CNT[];
extern const PatEntry PAT_ENT[];
extern const int PAT_NENT;
const PatLine *pat_line(const struct Synth *s, int d);   /* drum d's line now, or 0 (linear) */
const char *pmute_name(int m);

/* drums (drums.c) */
void drums_tick(Synth *s, float dt);        /* per control tick: the triggers */
float layer_wave(const Synth *s, int l, float ph, float f);   /* layer l's waveform (its bright table) at phase ph, for pitch f */
void drums_hit(Synth *s, int d, int node, float vel);   /* a hit now (the keys in drum mode; node unused: the drum
                                                           rides what its layer is playing) */
const char *drum_name(int d);
const char *drum_src_name(int v);
const char *drum_layer_name(int v);
void lfo_tick(Synth *s, float dt);
void mod_apply(Synth *s);

int  fx_tails_on(void);         /* (1.0.6) the delay and the reverb are running */
void fx_init(Fx *f);
void fx_update(Synth *s);
void fx_process(Synth *s, float *L, float *R, int n, float trig_env);

void params_init_meta(void);
void params_roll(Synth *s, uint32_t seed);
#define ROLL_PARTS 4
void params_roll_part(Synth *s, int part, uint32_t seed);
void synth_roll_begin(Synth *s, uint32_t seed, float morph_seconds, uint32_t mask);   /* a roll, a part a block: */
int synth_roll_step(Synth *s);                                                        /* (call at each block's start) */
void params_roll_mod(Synth *s);

#endif

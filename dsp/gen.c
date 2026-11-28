/*
 * The generators. A generator is one to eight points on the Tonnetz, each moving along a continuous function of
 * time, each carrying an envelope that is a continuous function too. Every node's drive is
 *
 *     u_n(t) = wt_n * sum over points j of  e_j(t) * w(|q_j(t) - node_n| / Reach)
 *
 * with w a smooth bump of the point's distance to the node on the surface (the Tonnetz: neighbours are fifths, thirds,
 * sixths) and wt_n the note's tonal weight (engine.c). So pitch is where a point is, and loudness is
 * its envelope where it is: there is no grid, no sequence and no event anywhere. Polyphony is the number of points
 * (Points), spread along the same motion (Spread).
 *
 * Motions: nine, each a different kind of behaviour (A and B are each one's two controls; Rate its speed, a
 * whole-number ratio to the bar, so a periodic motion repeats with the rhythm):
 *   Orbit      a closed figure at a frequency ratio (a Lissajous figure): a loop, an ostinato
 *   Ladder     a run through the notes in order of pitch, up, up and down, or down, resting on each: scales and
 *              arpeggios by step (every other motion moves on the lattice, by fifths and thirds)
 *   Swarm      the points are coupled oscillators (Kuramoto): they gather into a chord and scatter
 *   Lorenz     chaos with two lobes: it circles one group of notes, then jumps to the other
 *   Motif      a small figure stated at the ding, again, then answered at the fifth or the third, then at home again
 *              (A A B A): a phrase and its answer
 *   Drift      a smooth random walk: slow, aimless, never repeating
 *   Billiard   straight runs and reflections, round a bumper: arpeggios that fold back; the bumper makes it chaotic
 *   Gravity    the point orbits the key's own notes (wells at the ding, then its fifth, then its third): it falls
 *              toward the tonic, swings round it and away
 *   Stride     it moves only as the rhythm's stream peaks, and turns while it rests: a melody that steps on the beat
 * Envelopes (each point's, Contour shapes them):
 *   Pulse      a periodic peak, at Env rate in exact ratio to the bar and locked to it
 *   Swell      a periodic swell, the same
 *   Speed      the point's speed: loud while it moves
 *   Pass       its nearness to a note: it sounds as it passes through the lattice
 *   Height     the motion's third coordinate (the flows' z, the distance from the centre, the speed round a well)
 *   Hold       steady
 *   Stream     the rhythm's own stream: it sounds where the drums' events are (Contour: only the strongest)
 *   Drums      a drum's envelope
 * The stream also accents every envelope (Grip); the keys are continuous inputs that sound and pull the nearest point
 * toward their note.
 */
#define MANIFOLD_DSP_UNIT
#include "synth.h"
#include "mstring.h"

static const char *const GEN_N[NUM_GENS] = {"Orbit", "Ladder", "Swarm", "Lorenz", "Motif", "Drift", "Billiard", "Gravity", "Stride", "Loops", "Erode", "Chords"};
const char *gen_name(int g) { return g >= 0 && g < NUM_GENS ? GEN_N[g] : "?"; }
static const char *const ENV_N[NUM_ENVS] = {"Pulse", "Swell", "Speed", "Pass", "Height", "Hold", "Stream", "Drums"};
const char *env_name(int e) { return e >= 0 && e < NUM_ENVS ? ENV_N[e] : "?"; }
static const char *const ROUTE_N[NUM_ROUTES] = {"Plays", "Moves the other", "Adds to the other", "Multiplies the other"};
const char *route_name(int r) { return r >= 0 && r < NUM_ROUTES ? ROUTE_N[r] : "?"; }

/* which layer modulates the other: layer 2's route first (so with both set, layer 2 modulates and layer 1 plays) */
DSP_CTL int gen_modulator_r(const Synth *s) {
    if (pint(s, P_G2_ROUTE) != ROUTE_PLAY) return 1;
    if (pint(s, P_G1_ROUTE) != ROUTE_PLAY) return 0;
    return -1;
}

int gen_modulator(const Synth *s) { return DSP_FAR(gen_modulator_r)(s); }   /* (the interface's call, from the flash) */

static const float ERATE_V[NUM_ERATES] = {1.f / 64, 1.f / 32, 1.f / 16, 1.f / 8, .25f, 1.f / 3, .5f, 2.f / 3, 1.f, 1.5f, 2.f, 3.f, 4.f};   /* cycles a beat */
static const char *const ERATE_N[NUM_ERATES] = {"1 per 16 bars", "1 per 8 bars", "1 per 4 bars", "1 per 2 bars", "1 per 4 beats", "1 per 3 beats", "1 per 2 beats", "2 per 3 beats",
                                                "1 a beat", "3 per 2 beats", "2 a beat", "3 a beat", "4 a beat"};
const char *erate_name(int r) { return r >= 0 && r < NUM_ERATES ? ERATE_N[r] : "?"; }
/* Rate: cycles a bar, whole-number ratios only (the control is continuous; the motion takes the nearest) */
static const float GRATE_V[NUM_GRATES] = {1.f / 64, 1.f / 32, 1.f / 16, 1.f / 12, 1.f / 8, 1.f / 6, .25f, 1.f / 3, .5f, 2.f / 3, .75f,
                                          1.f, 1.5f, 2.f, 3.f, 4.f};
static const char *const GRATE_N[NUM_GRATES] = {"1 per 64 bars", "1 per 32 bars", "1 per 16 bars", "1 per 12 bars", "1 per 8 bars",
    "1 per 6 bars", "1 per 4 bars", "1 per 3 bars", "1 per 2 bars", "2 per 3 bars", "3 per 4 bars", "1 a bar", "3 per 2 bars",
    "2 a bar", "3 a bar", "4 a bar"};
static float GRATE_L[NUM_GRATES];          /* flog2 of each, once */
DSP_CTL int grate_of(float cycles) {
    if (GRATE_L[0] == 0.f) for (int i = 0; i < NUM_GRATES; i++) GRATE_L[i] = flog2(GRATE_V[i]);
    float l = flog2(cycles > 1e-4f ? cycles : 1e-4f), bd = 1e9f; int bi = 0;
    for (int i = 0; i < NUM_GRATES; i++) { float d = GRATE_L[i] - l; if (d < 0.f) d = -d; if (d < bd) { bd = d; bi = i; } }
    return bi;
}
const char *grate_name(int r) { return r >= 0 && r < NUM_GRATES ? GRATE_N[r] : "?"; }
float grate_value(int r) { return GRATE_V[r < 0 ? 0 : r >= NUM_GRATES ? NUM_GRATES - 1 : r]; }

static const char *const AB_N[NUM_GENS][2] = {
    {"Ratio", "Phase"}, {"Span", "Contour"}, {"Coupling", "Spread"}, {"Chaos", "Zoom"}, {"Figure", "Answer"},
    {"Detail", "Density"}, {"Angle", "Bumper"}, {"Wells", "Energy"}, {"Turn", "Stride"},
    {"Notes", "Swell"}, {"Life", "Wear"}, {"Progression", "Voicing"}};
const char *gen_ab_name(int g, int which) { return g >= 0 && g < NUM_GENS ? AB_N[g][which & 1] : "?"; }

static const float ORBIT_R[8] = {1.f, 2.f, 1.5f, 4.f / 3, 1.25f, 5.f / 3, 3.f, 2.5f};
static const char *const ORBIT_N[8] = {"1:1", "2:1", "3:2", "4:3", "5:4", "5:3", "3:1", "5:2"};

DSP_INL int shape_idx(float x, int n) { int i = (int)(x * (float)n); return i < 0 ? 0 : i >= n ? n - 1 : i; }

void gen_ab_text(int g, int which, float x, char *b, int len) {
    int pc = (int)(x * 100.f + .5f);
    switch (g) {
    case GEN_ORBIT: if (!which) { snprintf(b, len, "%s", ORBIT_N[shape_idx(x, 8)]); return; } break;
    case GEN_LOOPS: if (!which) { snprintf(b, len, "%d notes", 2 + (int)(x * 4.f + .5f)); return; } break;
    case GEN_ERODE: if (!which) { snprintf(b, len, "%d passes", (int)(4.f * fexp2(4.f * x) + .5f)); return; } break;
    case GEN_CHORDS: if (!which) { static const char *const PN[6] = {"1 4 5 1", "1 6 4 5", "1 3 4 2", "1 5 6 4", "1 1 4 1", "1 7 6 5"}; snprintf(b, len, "%s", PN[shape_idx(x, 6)]); return; }
                     snprintf(b, len, x < .25f ? "Thirds" : x < .75f ? "Fourths" : "Fifths"); return;
    case GEN_LADDER: if (which) { snprintf(b, len, x < .33f ? "Up" : x < .67f ? "Up and down" : "Down"); return; }
                     snprintf(b, len, "%d notes", 3 + (int)(x * 9.f + .5f)); return;
    case GEN_MOTIF: if (!which) { snprintf(b, len, "%s", ORBIT_N[1 + shape_idx(x, 6)]); return; }
                    snprintf(b, len, x < .34f ? "At the fifth" : x < .67f ? "At the third" : "Fifth, then third"); return;
    case GEN_LORENZ: if (!which) { int r = (int)(20.f + 30.f * x + .5f); snprintf(b, len, "rho %d", r); return; } break;
    case GEN_GRAVITY: if (!which) { int w = 1 + shape_idx(x, 3); snprintf(b, len, w == 1 ? "The ding" : w == 2 ? "Ding and fifth" : "Ding, fifth, third"); return; } break;
    case GEN_BILLIARD: if (!which) { snprintf(b, len, "%d deg", (int)(x * 180.f + .5f)); return; } break;
    default: break;
    }
    snprintf(b, len, "%d%%", pc);
}

DSP_INL float noise_hash(uint32_t n) { uint32_t x = n * 2654435761u + 0x9E3779B9u; x ^= x >> 15; x *= 0x2C1B3C6Du; x ^= x >> 12; return (float)(x >> 8) * (1.f / 16777216.f); }
#include "ctl_on.h"
DSP_INL float smooth01(float x) { x = fclamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }
DSP_INL float cs(float t) { return fsin(t + .25f); }   /* cos(2 pi t) */
DSP_INL float sn(float t) { return fsin(t); }          /* sin(2 pi t) */
DSP_INL float powf_(float x, float e) { return x <= 1e-9f ? 0.f : fexp2(e * flog2(x)); }

void gen_reset(GenSt *g, uint32_t seed) {
    memset(g, 0, sizeof *g);
    g->func = -1; g->key = -1; g->rng = seed * 747796405u + 1u;
}

/* set a function up; what is sounding and the keys carry over */
#include "ctl_off.h"   /* (setup runs from the flash, when the motion changes) */
#include "balance_tab.h"
/* Balance (1.0.5): each note's share of this layer's drive. Each motion favours some notes over a long run (Drift
   and Lorenz the frame's centre, Orbit and Billiard its edge, Gravity and Motif the bass, Chords and Loops the
   upper notes: BAL_EXP, measured from rolls). The gain divides that out, so every note gets alike (Balanced, 0), or
   then leans the share toward the bass (Dark, -1: +4 dB an octave down) or the middle of the notes' span (Bright, 1:
   up to +3 dB there, the ends down). The layer's whole energy is kept; no note goes past -9 / +9 dB. A table, worked
   out when the motion, the notes or Balance move, in the flash; the drive reads a gain a note */
static void balance_gains(Synth *s, int l, const float *key) {
    GenSt *g = &s->layer[l].g; const Engine *e = &s->eng;
    int N = e->n_nodes, func = (int)key[0]; float b = key[1];
    for (int k = 0; k < 4; k++) g->gwkey[k] = key[k];
    if (N <= 0) return;
    if (func < 0 || func >= NUM_GENS) func = 0;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < N; i++) { float x = e->node[i].lr; if (x < lo) lo = x; if (x > hi) hi = x; }
    float span = hi - lo > .1f ? hi - lo : 1.f, ex[MAX_NODES], se = 0.f, sg = 0.f;
    for (int i = 0; i < N; i++) {
        const Node *nd = &e->node[i];
        float p = (nd->lr - lo) / span, dx = nd->x - e->fcx, dy = nd->y - e->fcy;
        float rd = sqrtf(dx * dx + dy * dy) / (e->fext > 1e-4f ? e->fext : 1.f);
        int pb = (int)(p * 4.f); if (pb > 3) pb = 3; if (pb < 0) pb = 0;
        int rb = (int)(rd * 5.f); if (rb > 4) rb = 4;
        float E = BAL_EXP[func][rb][pb]; if (E < .05f) E = .05f;
        float db;                                                  /* the share wanted, in dB */
        if (b < 0.f) db = -b * 4.f * (.5f * span - (nd->lr - lo));
        else { float z = (p - .55f) / .45f; db = b * (3.f - 12.f * z * z); }
        if (db > 9.f) db = 9.f; if (db < -9.f) db = -9.f;
        float gw = fexp2(db * .16610f) / sqrtf(E);                 /* 10^(dB/20) (energy goes as the drive squared) */
        ex[i] = E; g->gw[i] = gw; se += E; sg += E * gw * gw;
    }
    float c = sg > 1e-9f ? sqrtf(se / sg) : 1.f;                  /* the layer's energy as it was */
    for (int i = 0; i < N; i++) g->gw[i] = fclamp(g->gw[i] * c, .35f, 2.8f);
    for (int i = N; i < MAX_NODES; i++) g->gw[i] = 1.f;
    (void)ex;
}
/* again only when the motion, Balance or the notes moved: the notes or the motion now; Balance alone every fourth
   tick at most (a fast LFO on it costs a quarter as much) */
static void balance_check(Synth *s, int l, int func, int N) {
    GenSt *g = &s->layer[l].g;
    float bk = pval(s, P_G1_BAL + l);
    float key[4] = {(float)func, (float)(int)(bk * 50.f) * .02f, (float)(N + 1000), N > 0 ? s->eng.node[0].lr + 7.f * s->eng.node[N - 1].lr + 3.f * s->eng.fext : 0.f};
    if (key[0] != g->gwkey[0] || key[2] != g->gwkey[2] || key[3] != g->gwkey[3]
        || (key[1] != g->gwkey[1] && !(++g->gwtick & 3))) balance_gains(s, l, key);
}
static void setup(GenSt *g, int func, int n) {
    GenSt k = *g;
    memset(g, 0, sizeof *g);
    memcpy(g->a, k.a, sizeof g->a); memcpy(g->kage, k.kage, sizeof g->kage); memcpy(g->kvel, k.kvel, sizeof g->kvel);
    memcpy(g->held, k.held, sizeof g->held); memcpy(g->held_v, k.held_v, sizeof g->held_v); memcpy(g->hlev, k.hlev, sizeof g->hlev);
    memcpy(g->eph, k.eph, sizeof g->eph); memcpy(g->e, k.e, sizeof g->e);
    memcpy(g->rx, k.rx, sizeof g->rx); memcpy(g->ry, k.ry, sizeof g->ry); memcpy(g->rh, k.rh, sizeof g->rh);
    memcpy(g->jx, k.jx, sizeof g->jx); memcpy(g->jy, k.jy, sizeof g->jy); memcpy(g->jh, k.jh, sizeof g->jh);
    g->key = k.key; memcpy(g->ox, k.ox, sizeof g->ox); memcpy(g->oy, k.oy, sizeof g->oy);
    g->rng = k.rng; g->rng0 = k.rng0 ? k.rng0 : k.rng | 1u; g->func = func; g->n = n; g->evo_a = k.evo_a; g->evo_b = k.evo_b; g->t = k.t;
    g->la = -1.f;
    for (int j = 0; j < MAX_PTS; j++) {                    /* each point's flow from its own place */
        float h1 = noise_hash((uint32_t)j * 17u + k.rng), h2 = noise_hash((uint32_t)j * 29u + k.rng + 3u);
        g->sx[j] = (h1 - .5f) * 2.f; g->sy[j] = (h2 - .5f) * 2.f; g->sz[j] = .5f + h1; g->sw[j] = h2;
        if (func == GEN_LORENZ) { g->sx[j] = 1.f + h1; g->sy[j] = 1.f + h2; g->sz[j] = 20.f + 5.f * h1; }
        if (func == GEN_BILLIARD || func == GEN_STRIDE) { g->sx[j] = (h1 - .5f) * .7f; g->sy[j] = (h2 - .5f) * .7f; g->sz[j] = .07f + .381966f * (float)j + .3f * h1; }
        if (func == GEN_GRAVITY) { g->sx[j] = .3f * fsin(h1 + .37f * (float)j + .25f); g->sy[j] = .3f * fsin(h1 + .37f * (float)j);
                                   g->sz[j] = -.9f * fsin(h1 + .37f * (float)j); g->sw[j] = .9f * fsin(h1 + .37f * (float)j + .25f); }
    }
}

/* a point in a box of half-width .45: a wall turns it back (position folded, heading mirrored) */
#include "ctl_on.h"
DSP_CTL static void walls(float *x, float *y, float *th) {
    if (*x > .45f) { *x = .9f - *x; *th = .5f - *th; } else if (*x < -.45f) { *x = -.9f - *x; *th = .5f - *th; }
    if (*y > .45f) { *y = .9f - *y; *th = -*th; } else if (*y < -.45f) { *y = -.9f - *y; *th = -*th; }
    *x = fclamp(*x, -.45f, .45f); *y = fclamp(*y, -.45f, .45f);
    *th -= floorf(*th);
}

/* a node's place in the motions' coordinates */
DSP_CTL static void node_gc(const Synth *s, int i, float *x, float *y) {
    const Engine *e = &s->eng;
    float ox = (pval(s, P_PATH_X) - .5f) * 2.f * e->fext, oy = (pval(s, P_PATH_Y) - .5f) * 2.f * e->fext;
    float dX = e->node[i].x - e->fcx - ox, dY = e->node[i].y - e->fcy - oy;
    if (e->surface != SURF_PLANE) dX -= floorf(dX + .5f);
    if (e->surface == SURF_TORUS) dY -= floorf(dY + .5f);
    path_unview(s, dX, dY, x, y);
    *x = fclamp(*x, -.45f, .45f); *y = fclamp(*y, -.45f, .45f);
}
/* the notes that hold the key, strongest first, one a pitch class (the ding, its fifth, its third ...): up to W */
DSP_CTL static int key_notes(const Synth *s, int W, int *out) {
    const Engine *e = &s->eng; int nw = 0;
    for (int k = 0; k < W; k++) {
        int bi = -1; float bw = -1.f;
        for (int i = 0; i < e->n_nodes; i++) {
            int dup = 0; for (int q = 0; q < nw; q++) if (e->node[out[q]].pc == e->node[i].pc) dup = 1;
            if (dup) continue;
            float w = e->node[i].wt - .01f * (float)e->node[i].oct; if (w > bw) { bw = w; bi = i; }
        }
        if (bi < 0) break;
        out[nw++] = bi;
    }
    return nw;
}
/* the notes in order of pitch (Ladder), kept for the tick: built when point 0 asks */
static uint8_t g_ord[NUM_LAYERS][MAX_NODES]; static int g_nord[NUM_LAYERS];
DSP_CTL static void pitch_order(const Synth *s, int l) {
    const Engine *e = &s->eng; int N = e->n_nodes; uint8_t *o = g_ord[l];
    for (int i = 0; i < N; i++) {
        int k = i; while (k > 0 && e->node[o[k - 1]].lr > e->node[i].lr) { o[k] = o[k - 1]; k--; }
        o[k] = (uint8_t)i;
    }
    g_nord[l] = N;
}

/* once a tick and layer (when point 0 asks): the notes in order of pitch, every note's place in the motions'
   coordinates, and the key's notes: the motions that walk the notes read these */
static float g_nx[NUM_LAYERS][MAX_NODES], g_ny[NUM_LAYERS][MAX_NODES]; static int g_kn[NUM_LAYERS][6], g_nk[NUM_LAYERS];
DSP_CTL static void prep(const Synth *s, int l, int j) {
    static uint8_t age[NUM_LAYERS];                          /* the notes do not move between ticks unless the path view is turned:
                                                               every 8th tick (93 ms) is often enough */
    if (j != 0 && g_nord[l] == s->eng.n_nodes) return;
    if (j == 0 && g_nord[l] == s->eng.n_nodes && (++age[l] & 7)) return;
    pitch_order(s, l);
    for (int i = 0; i < s->eng.n_nodes; i++) node_gc(s, i, &g_nx[l][i], &g_ny[l][i]);
    g_nk[l] = key_notes(s, 6, g_kn[l]);
}
DSP_INL void ncache(int l, int i, float *x, float *y) { *x = g_nx[l][i]; *y = g_ny[l][i]; }

/* ---------------- the motions ---------------- */
/* point j's position (generator coordinates) and the motion's third coordinate (0..1), now. tc: the motion's clock in
   cycles; off: the point's offset along the motion (Spread); dtc: cycles this tick (for the flows) */
DSP_CTL static void motion(Synth *s, GenSt *g, int func, int j, float tc, float off, float dtc, float A, float B, float *x, float *y, float *h) {
    float t = tc + off;
    switch (func) {
    case GEN_ORBIT: {                                         /* x = sin 2pi t, y = sin 2pi (rho t + phase) */
        float r = ORBIT_R[shape_idx(A, 8)];
        *x = .45f * sn(t); *y = .45f * sn(r * t + .25f * B);
        *h = .5f + .5f * sn(t * r * .5f);
    } break;
    case GEN_LADDER: {                                        /* through the notes in order of pitch, resting on each */
        int l = g == &s->layer[1].g; prep(s, l, j);
        int N = g_nord[l]; if (N < 2) { *x = *y = 0.f; *h = .5f; break; }
        int M = 3 + (int)(A * 9.f + .5f); if (M > N) M = N;
        int lo = (N - M) / 3 + 2 * j; if (lo + M > N) lo = N - M; if (lo < 0) lo = 0;   /* (each point starts a third higher) */
        float ph = t - floorf(t), u;                          /* 0..M-1 along the run */
        if (B < .33f) u = ph * (float)M;                      /* up, and round again */
        else if (B < .67f) u = (ph < .5f ? ph * 2.f : 2.f - ph * 2.f) * (float)(M - 1);
        else u = (1.f - ph) * (float)M;
        int i0 = (int)u; float f = u - (float)i0; i0 %= M; int i1 = B >= .33f && B < .67f ? (i0 + 1 < M ? i0 + 1 : i0) : (i0 + 1) % M;
        f = smooth01((f - .55f) / .45f);                      /* rest on the note, then go */
        float x0, y0, x1, y1; ncache(l, g_ord[l][lo + i0], &x0, &y0); ncache(l, g_ord[l][lo + i1], &x1, &y1);
        *x = x0 + (x1 - x0) * f; *y = y0 + (y1 - y0) * f;
        *h = ((float)i0 + f) / (float)(M > 1 ? M - 1 : 1); if (*h > 1.f) *h = 1.f;
    } break;
    case GEN_SWARM: {                                         /* Kuramoto over the points */
        int M = g->npts; float zr = 0.f, zi = 0.f;
        for (int q = 0; q < M; q++) { zr += cs(g->sx[q]); zi += sn(g->sx[q]); }
        zr /= (float)M; zi /= (float)M;
        float Kc = A * 3.f, om = 1.f + B * .5f * (noise_hash((uint32_t)j * 31u) * 2.f - 1.f);
        float d = dtc * (om + Kc * (zi * cs(g->sx[j]) - zr * sn(g->sx[j])));
        g->sw[j] = g->sx[j] + d;                              /* (applied after every point has read the others) */
        float r = .2f + .25f * (float)j / (float)(M > 1 ? M - 1 : 1);
        *x = r * cs(g->sx[j]); *y = r * sn(g->sx[j]);
        *h = sqrtf(zr * zr + zi * zi);
    } break;
    case GEN_MOTIF: {                                         /* a figure at home, again, away, home: A A B A */
        int l = g == &s->layer[1].g; prep(s, l, j);
        const int *kn = g_kn[l]; int nk = g_nk[l] < 3 ? g_nk[l] : 3; float cx[3] = {0, 0, 0}, cy[3] = {0, 0, 0};
        for (int k = 0; k < nk; k++) ncache(l, kn[k], &cx[k], &cy[k]);
        for (int k = nk; k < 3; k++) { cx[k] = cx[0]; cy[k] = cy[0]; }
        float ph = t - floorf(t), q4 = ph * 4.f; int seg = (int)q4; float fs = q4 - (float)seg; if (seg > 3) seg = 3;
        /* where the figure sits in each quarter: home, home, the answer, home (the answer: the fifth, the third, or
           the fifth one cycle and the third the next) */
        int cyc = (int)floorf(t), ans = B < .34f ? 1 : B < .67f ? 2 : 1 + (cyc & 1);
        int at = seg == 2 ? ans : 0, nx = seg == 1 ? ans : 0;
        float gl = smooth01((fs - .85f) / .15f);              /* the last of each quarter: on the way to the next */
        float X0 = cx[at] + (cx[nx] - cx[at]) * gl, Y0 = cy[at] + (cy[nx] - cy[at]) * gl;
        float r = ORBIT_R[1 + shape_idx(A, 6)], rad = .09f + .03f * (float)j;
        *x = fclamp(X0 + rad * sn(fs), -.5f, .5f); *y = fclamp(Y0 + rad * sn(r * fs + .125f), -.5f, .5f);
        *h = seg == 2 ? 1.f - .5f * gl : .5f * (seg == 1 ? 1.f + gl : 1.f);
    } break;
    case GEN_LORENZ: {
        float rho = 20.f + 30.f * A, dtau = dtc * .9f;
        int sub = (int)(dtau / .004f) + 1; float hh = dtau / (float)sub;
        for (int it = 0; it < sub; it++) {
            float X = g->sx[j], Y = g->sy[j], Z = g->sz[j];
            float dx = 10.f * (Y - X), dy = X * (rho - Z) - Y, dz = X * Y - 2.6666667f * Z;
            float x2 = X + hh * dx, y2 = Y + hh * dy, z2 = Z + hh * dz;
            g->sx[j] = X + .5f * hh * (dx + 10.f * (y2 - x2)); g->sy[j] = Y + .5f * hh * (dy + x2 * (rho - z2) - y2);
            g->sz[j] = Z + .5f * hh * (dz + x2 * y2 - 2.6666667f * z2);
            if (!(g->sx[j] == g->sx[j]) || g->sz[j] > 200.f) { g->sx[j] = 1.f; g->sy[j] = 1.f; g->sz[j] = 20.f; }
        }
        float z = 1.f / (35.f + 30.f * (1.f - B));
        *x = fclamp(g->sx[j] * z, -.5f, .5f); *y = fclamp((g->sz[j] - rho + 1.f) * z, -.5f, .5f);
        *h = fclamp(g->sz[j] / (rho * 1.6f), 0.f, 1.f);
    } break;
    case GEN_DRIFT: {                                         /* smooth value noise: random knots, joined by a quintic */
        /* B sets the knots a cycle (1..4), A how much a second, finer layer (three times as dense) wanders on it */
        float dens = 1.f + 3.f * B, fa = .6f * A, X = 0.f, Y = 0.f;
        for (int o = 0; o < 2; o++) {
            float tt = t * dens * (o ? 3.f : 1.f) + 37.f * (float)j, fl = floorf(tt), f = tt - fl;
            uint32_t n = (uint32_t)(int32_t)fl, sd = g->rng0 + (uint32_t)j * 7919u + (uint32_t)o * 104729u;
            float w = f * f * f * (f * (f * 6.f - 15.f) + 10.f), amp = o ? fa : 1.f;
            X += amp * ((1.f - w) * noise_hash(n * 2u + sd) + w * noise_hash(n * 2u + 2u + sd) - .5f);
            Y += amp * ((1.f - w) * noise_hash(n * 2u + 1u + sd * 3u) + w * noise_hash(n * 2u + 3u + sd * 3u) - .5f);
        }
        X *= .9f / (1.f + fa); Y *= .9f / (1.f + fa);
        *x = X; *y = Y; *h = smooth01(2.2f * sqrtf(X * X + Y * Y));
    } break;
    case GEN_BILLIARD: {                                      /* straight runs in a box, round a bumper of radius B */
        /* Angle turns the heading (by the change, so it is live); the bumper scatters the paths (Sinai's billiard:
           with it the runs never repeat, without it they close into figures) */
        if (g->la < 0.f) g->la = A;
        if (j == 0) g->lb = (A - g->la) * .5f;
        float th = g->sz[j] + g->lb, X = g->sx[j], Y = g->sy[j], rb = .28f * B, step = 1.8f * dtc;
        int sub = (int)(step / .02f) + 1; float hh = step / (float)sub;
        for (int it = 0; it < sub; it++) {
            X += hh * cs(th); Y += hh * sn(th);
            walls(&X, &Y, &th);
            float r2 = X * X + Y * Y;
            if (rb > .01f && r2 < rb * rb && r2 > 1e-8f) {    /* off the bumper: mirror the heading in its tangent */
                float r = sqrtf(r2), nx = X / r, ny = Y / r, vx = cs(th), vy = sn(th), dn = vx * nx + vy * ny;
                if (dn < 0.f) {
                    vx -= 2.f * dn * nx; vy -= 2.f * dn * ny;
                    float a2 = vy < 0.f ? -1.f : 1.f, c = fclamp(vx, -1.f, 1.f);   /* the angle of (vx, vy), turns */
                    th = a2 * (.25f - asinf(c) * .15915494f);
                }
                X = nx * rb; Y = ny * rb;
            }
        }
        g->sx[j] = X; g->sy[j] = Y; g->sz[j] = th - floorf(th);
        *x = X; *y = Y; *h = smooth01(2.2f * sqrtf(X * X + Y * Y));
    } break;
    case GEN_GRAVITY: {                                       /* it orbits the key's own notes */
        /* the wells: the ding, then the note of another pitch class that holds the key most (its fifth), then the
           next (its third), where they sit in this motion's coordinates */
        const Engine *e = &s->eng; int W = 1 + shape_idx(A, 3), nw = 0; float wx[3], wy[3]; int used[3] = {-1, -1, -1};
        float ox = (pval(s, P_PATH_X) - .5f) * 2.f * e->fext, oy = (pval(s, P_PATH_Y) - .5f) * 2.f * e->fext;
        for (int k = 0; k < W; k++) {
            int bi = -1; float bw = -1.f;
            for (int i = 0; i < e->n_nodes; i++) {
                int dup = 0; for (int q = 0; q < nw; q++) if (e->node[used[q]].pc == e->node[i].pc) dup = 1;
                if (dup) continue;
                float w = e->node[i].wt - .01f * (float)e->node[i].oct; if (w > bw) { bw = w; bi = i; }
            }
            if (bi < 0) break;
            used[nw] = bi;
            float dX = e->node[bi].x - e->fcx - ox, dY = e->node[bi].y - e->fcy - oy;
            if (e->surface != SURF_PLANE) dX -= floorf(dX + .5f);
            if (e->surface == SURF_TORUS) dY -= floorf(dY + .5f);
            path_unview(s, dX, dY, &wx[nw], &wy[nw]);
            wx[nw] = fclamp(wx[nw], -.4f, .4f); wy[nw] = fclamp(wy[nw], -.4f, .4f); nw++;
        }
        if (nw < 1) { wx[0] = wy[0] = 0.f; nw = 1; }
        float GM = .25f / (float)nw, eps2 = .0064f, Et = -GM * (float)nw * (.8f + 3.f * (1.f - B));
        float X = g->sx[j], Y = g->sy[j], VX = g->sz[j], VY = g->sw[j], dtau = dtc * 6.f, phi = 0.f;
        int sub = (int)(dtau / .01f) + 1; float hh = dtau / (float)sub;
        for (int it = 0; it < sub; it++) {
            float ax = 0.f, ay = 0.f; phi = 0.f;
            for (int k = 0; k < nw; k++) {
                float dx = X - wx[k], dy = Y - wy[k], r2 = dx * dx + dy * dy + eps2, ir = frsqrt(r2), ir3 = ir * ir * ir;
                ax -= GM * dx * ir3; ay -= GM * dy * ir3; phi -= GM * ir;
            }
            VX += hh * ax; VY += hh * ay; X += hh * VX; Y += hh * VY;
            if (X > .48f) { X = .96f - X; VX = -VX; } else if (X < -.48f) { X = -.96f - X; VX = -VX; }
            if (Y > .48f) { Y = .96f - Y; VY = -VY; } else if (Y < -.48f) { Y = -.96f - Y; VY = -VY; }
            /* Energy: the speed is eased toward what the orbit's energy asks for here (so it neither falls in nor flies off) */
            float v2 = VX * VX + VY * VY, want = 2.f * (Et - phi);
            if (want < .02f) want = .02f;
            float k = 1.f + .5f * hh * (want / (v2 + 1e-6f) - 1.f); k = fclamp(k, .9f, 1.1f);
            VX *= k; VY *= k;
        }
        if (!(X == X) || !(VX == VX)) { X = .2f; Y = 0.f; VX = 0.f; VY = .8f; }
        g->sx[j] = X; g->sy[j] = Y; g->sz[j] = VX; g->sw[j] = VY;
        *x = X; *y = Y; *h = smooth01(sqrtf(VX * VX + VY * VY) * .45f);
    } break;
    case GEN_STRIDE: {                                        /* it runs while the stream peaks, and turns while it rests */
        float R = s->meter.Rn, v = R * sqrtf(R) * (2.f + 8.f * B), th = g->sz[j], X = g->sx[j], Y = g->sy[j];
        th += (A - .5f) * 3.f * (1.f - R) * dtc;
        float step = v * dtc; int sub = (int)(step / .02f) + 1; float hh = step / (float)sub;
        for (int it = 0; it < sub; it++) { X += hh * cs(th); Y += hh * sn(th); walls(&X, &Y, &th); }
        g->sx[j] = X; g->sy[j] = Y; g->sz[j] = th - floorf(th);
        *x = X; *y = Y; *h = fclamp(R * 1.6f, 0.f, 1.f);
    } break;
    case GEN_LOOPS: {                                         /* tape loops of unequal length, a note each: they never line up twice */
        /* point j rests on its own note of the key (the strongest notes first, then outward) and swells once in its
           own loop; the loops are 5, 7, 8, 11, 13, 17, 19 and 23 quarter-cycles long, so at 1 cycle per 8 bars the
           whole constellation returns after hours. A: how many of the key's notes they share; B: how long each
           swell lasts in its loop */
        static const float LEN[MAX_PTS] = {5, 7, 8, 11, 13, 17, 19, 23};
        int l = g == &s->layer[1].g; prep(s, l, j);
        int N = g_nord[l]; if (N < 1) { *x = *y = 0.f; *h = 0.f; break; }
        const int *kn = g_kn[l]; int nk = 2 + (int)(A * 4.f + .5f); if (nk > g_nk[l]) nk = g_nk[l]; if (nk < 1) { *x = *y = 0.f; *h = 0.f; break; }
        int pc = s->eng.node[kn[j % nk]].pc, oct = 1 + (j * 3) % 4, hit = kn[j % nk], seen = 0;
        for (int i = 0; i < N; i++) if (s->eng.node[g_ord[l][i]].pc == pc) { hit = g_ord[l][i]; if (seen++ >= oct) break; }
        ncache(l, hit, x, y);
        float ph = t * 4.f / LEN[j] + .37f * (float)j; ph -= floorf(ph);
        float wd = .12f + .5f * B, u = ph / wd;                 /* the swell: a raised cosine wd of the loop long */
        g->mgn[j] = u < 1.f ? .5f - .5f * cs(u) : 0.f;
        *h = g->mgn[j];
    } break;
    case GEN_ERODE: {                                         /* one loop, played over and over, wearing away, then renewed */
        /* a figure of six notes through the scale, the same every pass; each pass wears it: notes thin and drop out,
           the weakest first, until B of it is gone; then it comes back whole over one pass. A: how many passes that
           takes (4 to 64) */
        int l = g == &s->layer[1].g; prep(s, l, j);
        int N = g_nord[l]; if (N < 2) { *x = *y = 0.f; *h = 0.f; break; }
        static const int8_t FIG[6] = {0, 2, 4, 3, 5, 1};      /* the figure, in steps of the scale from its low note */
        static const float WEAR[6] = {.95f, .35f, .6f, .15f, .8f, .5f};   /* when each note goes (0: first) */
        float life = 4.f * fexp2(4.f * A), pass = t / life, age = pass - floorf(pass);   /* 0..1 over the wearing */
        float worn = age < .9f ? age / .9f * (.25f + .75f * B) : (1.f - (age - .9f) / .1f) * (.25f + .75f * B);
        float ph = t - floorf(t), u = ph * 6.f; int i0 = (int)u; float f = u - (float)i0; if (i0 > 5) i0 = 5;
        int lo = N / 4 + 2 * j, a0 = lo + FIG[i0], a1 = lo + FIG[(i0 + 1) % 6];
        if (a0 >= N) a0 = N - 1; if (a1 >= N) a1 = N - 1;
        float gl = smooth01((f - .7f) / .3f);
        float x0, y0, x1, y1; ncache(l, g_ord[l][a0], &x0, &y0); ncache(l, g_ord[l][a1], &x1, &y1);
        *x = x0 + (x1 - x0) * gl; *y = y0 + (y1 - y0) * gl;
        float g0 = 1.f - smooth01((worn - WEAR[i0]) / .15f + .5f), g1 = 1.f - smooth01((worn - WEAR[(i0 + 1) % 6]) / .15f + .5f);
        g->mgn[j] = (g0 + (g1 - g0) * gl) * (1.f - .5f * worn);
        *h = 1.f - worn;
    } break;
    case GEN_CHORDS: {                                        /* the points are a chord; the chord changes: harmonic rhythm */
        /* point j holds the chord's j-th note (the scale's every other note from the chord's root, spread over the
           octaves); one cycle is four chords. A: the progression; B: how open the voicing is */
        static const int8_t PROG[6][4] = {{0, 3, 4, 0}, {0, 5, 3, 4}, {0, 2, 3, 1}, {0, 4, 5, 3}, {0, 0, 3, 0}, {0, 6, 5, 4}};
        int l = g == &s->layer[1].g; prep(s, l, j);
        int N = g_nord[l]; if (N < 2) { *x = *y = 0.f; *h = 0.f; break; }
        int deg = s->scale.series ? 4 : (s->scale.n > 0 ? s->scale.n : 5);
        const int8_t *pr = PROG[shape_idx(A, 6)];
        float ph = t - floorf(t), q4 = ph * 4.f; int seg = (int)q4; float fs = q4 - (float)seg; if (seg > 3) seg = 3;
        int open = 2 + (int)(B * 2.f + .5f);                  /* 2: thirds; 3: fourths; 4: fifths */
        int base = deg + deg / 2;                             /* an octave and a half up from the ding */
        int n0 = base + pr[seg] % deg + j * open, n1 = base + pr[(seg + 1) & 3] % deg + j * open;
        while (n0 >= N) n0 -= deg; while (n1 >= N) n1 -= deg; if (n0 < 0) n0 = 0; if (n1 < 0) n1 = 0;
        float gl = smooth01((fs - .8f) / .2f);
        float x0, y0, x1, y1; ncache(l, g_ord[l][n0], &x0, &y0); ncache(l, g_ord[l][n1], &x1, &y1);
        *x = x0 + (x1 - x0) * gl; *y = y0 + (y1 - y0) * gl;
        *h = (float)seg / 3.f;
    } break;
    default: *x = *y = 0.f; *h = 0.f; break;
    }
}

/* ---------------- one tick ---------------- */
DSP_CTL void gen_tick(Synth *s, int l, float dt) {
    Layer *L = &s->layer[l]; GenSt *g = &L->g; const Rhythm *m = &s->meter;
    int N = s->eng.n_nodes;
    int func = pint(s, LP(l, P_G1_FUNC));
    if (func != g->func || N != g->n) DSP_FAR(setup)(g, func, N);
    float evo = pval(s, P_EVOLVE);                           /* Evolve: a slow, bounded random walk on A and B */
    g->evo_va += (frand(&g->rng) - .5f) * dt * .4f - g->evo_va * dt * .5f;
    g->evo_vb += (frand(&g->rng) - .5f) * dt * .4f - g->evo_vb * dt * .5f;
    g->evo_a = fclamp(g->evo_a + g->evo_va * dt * evo, -.3f * evo, .3f * evo);
    g->evo_b = fclamp(g->evo_b + g->evo_vb * dt * evo, -.3f * evo, .3f * evo);
    float A = fclamp(pnorm(s, LP(l, P_G1_A)) + g->evo_a, 0.f, 1.f), B = fclamp(pnorm(s, LP(l, P_G1_B)) + g->evo_b, 0.f, 1.f);
    float rate = GRATE_V[grate_of(pval(s, LP(l, P_G1_RATE)))];   /* cycles a bar: the nearest whole-number ratio */
    int npts = pint(s, LP(l, P_G1_POINTS)) + 1;
    float spread = pval(s, LP(l, P_G1_SPREAD));
    int env = pint(s, LP(l, P_G1_ENV));
    float erate = ERATE_V[pint(s, LP(l, P_G1_ERATE))] * m->beats;   /* envelope cycles a bar */
    float C = pval(s, LP(l, P_G1_CONTOUR)), reach = pval(s, LP(l, P_G1_REACH)), energy = pval(s, LP(l, P_G1_ENERGY));
    float bar = m->bar_sec > .05f ? m->bar_sec : 2.f;
    float run = (s->transport && !s->mute_seq) ? 1.f : 0.f;
    float lvl = .35f + .65f * energy;
    g->npts = npts;
    /* the route: this layer modulates (it is silent; its Energy is the depth), or it is modulated by the other */
    int mi = gen_modulator_r(s), is_mod = mi == l, is_car = mi >= 0 && mi != l;
    const GenSt *mg = is_car ? &s->layer[mi].g : 0;
    int mroute = mi >= 0 ? pint(s, LP(mi, P_G1_ROUTE)) : ROUTE_PLAY;
    float depth = mi >= 0 ? pval(s, LP(mi, P_G1_ENERGY)) : 0.f;

    for (int i = 0; i < N; i++) { g->upv[i] = g->u[i]; g->u[i] = 0.f; }
    DSP_FAR(balance_check)(s, l, func, N);   /* Balance's gains (from the flash: a few compares a tick, the work rare) */

    /* the keys: a push is a kernel in time, a held key a level: one continuous input signal per node. Both take
       the patch's own envelope: a sharp Contour is a struck note (a 2 ms rise, a short fall, little held, a quick
       release); a soft one a bowed note (a rise of up to a second, a long fall, nearly all of it held, a release of
       seconds). While the transport runs, a held key also breathes with the layer's own envelope (half of it). */
    {
        float sf = 1.f - C, atk = .002f + 1.2f * sf * sf * sf, dec = .12f + 3.f * sf * sf, sus = .25f + .65f * sf, rel = .03f + 2.5f * sf * sf;
        float breathe = env == ENV_HOLD || run < .5f ? 1.f : .5f + .5f * fclamp(g->em[0], 0.f, 1.f);
        float ka = 1.f - fexp2(-dt / (atk * .4f) * 1.442695f), kr = 1.f - fexp2(-dt / rel * 1.442695f), life = atk + 5.f * dec;
        for (int i = 0; i < N; i++) {
            float x = 0.f;
            for (int q = 0; q < 4; q++) if (g->kvel[i][q] > 0.f) {
                g->kage[i][q] += dt; float ag = g->kage[i][q];
                float k = ag < atk ? smooth01(ag / atk) : fexp2(-(ag - atk) / dec * 1.4427f);
                x += g->kvel[i][q] * k * (1.f - sus * .6f); if (ag > life) g->kvel[i][q] = 0.f;
            }
            float ht = g->held[i] ? g->held_v[i] * sus * breathe : 0.f;
            g->hlev[i] += (ht - g->hlev[i]) * (ht > g->hlev[i] ? ka : kr);
            g->kin[i] = x + g->hlev[i];
        }
    }

    /* the motion's clock: continuous, Rate cycles a bar */
    float dtc = rate * dt / bar * run;
    g->t += dtc; if (g->t > 4096.f) g->t -= 4096.f;          /* (every motion's period divides 4096 cycles closely enough) */

    /* the stream, continuous: the accent every envelope takes now (Grip: how much) */
    float grip = .7f * pval(s, P_RH_GRIP);
    float accent = (1.f - grip) + grip * fclamp(.25f + 1.5f * m->Rn, 0.f, 1.f);

    /* the envelope's shape, from Contour: Pulse e = sin(pi psi')^p with psi' = psi / (psi + k (1 - psi)): high Contour
       a sharp front (but a finite slope: a rise of milliseconds, never a step), a narrow peak (percussion as a property of the function); low Contour a broad, even swell */
    float pexp = 1.f + 7.f * C * C, qk = 1.f - .88f * C;
    /* proximity is a smooth bump of radius Rc, w = (1 - (d/Rc)^2)^2 (continuous, with a continuous slope, and exactly
       0 beyond Rc), measured in the layout's own spacing between pitch classes: Reach 0, a point midway between two
       notes sounds each at a tenth and reaches no further; Reach 0.2 at about 0.4; Reach 1 lights the neighbours
       at about half */
    float Rc = s->eng.spacing * (.6f + 1.2f * reach), iR = 1.f / (Rc * Rc), iS = 1.f / (s->eng.spacing * s->eng.spacing);
    g->avg_age += dt;                                        /* the 2 s running averages (exact means until 2 s) */
    /* until the averages know the motion (about a second), Speed and Height read halfway, then open smoothly */
    float conf = smooth01(g->avg_age - .25f);
    float k2 = 1.f - fexp2(-dt / 2.f * 1.4427f); if (dt / g->avg_age > k2) k2 = dt / g->avg_age;

    /* the formula's key: the motion, the point count, and A and B on a grid of 24 (every discrete choice's boundary
       lies on it) */
    int key = ((func * 9 + npts) * 25 + shape_idx(A, 24)) * 25 + shape_idx(B, 24);
    float jd = fexp2(-dt / .1f * 1.4427f);
    if (g->key < 0) { g->key = key; for (int j = 0; j < MAX_PTS; j++) g->jx[j] = g->jy[j] = g->jh[j] = 0.f; }
    for (int j = 0; j < npts; j++) {
        float off = spread * (float)j / (float)npts;          /* the points spread along the same motion */
        float x, y, h;
        g->mgn[j] = 1.f;
        motion(s, g, func, j, g->t, off, dtc, A, B, &x, &y, &h);
        /* a motion's discrete choices (a ratio, a petal count, a number of turns), the motion itself and the number
           of points change the formula, not the time: when one changes, the point keeps its place and glides from it
           onto the new path (the offset decays over about 0.3 s), so the path stays continuous */
        if (key != g->key) { g->jx[j] += g->rx[j] - x; g->jy[j] += g->ry[j] - y; g->jh[j] += g->rh[j] - h; }
        g->rx[j] = x; g->ry[j] = y; g->rh[j] = h;
        g->jx[j] *= jd; g->jy[j] *= jd; g->jh[j] *= jd;
        float mx = x + g->ox[j], my = y + g->oy[j];          /* the motion alone (for its speed: the glide is not the motion) */
        x += g->jx[j]; y += g->jy[j]; h = fclamp(h + g->jh[j], 0.f, 1.f);
        /* Moves: the modulator's path is added to this one's (point j takes point j mod n of the other) */
        if (is_car && mroute == ROUTE_MOVE) { int jj = j % (mg->npts > 0 ? mg->npts : 1); x += depth * mg->x[jj]; y += depth * mg->y[jj]; }
        /* the keys pull the point toward their notes (continuously): the pull is the way to each note on the surface,
           from where the point is drawn, taken back into the motion's own coordinates */
        float fx = 0.f, fy = 0.f, fw = 0.f;
        float q0x, q0y; int anyk = 0;
        for (int i = 0; i < N; i++) if (g->kin[i] > 1e-3f) anyk = 1;
        if (anyk && pint(s, P_KEY_PULL)) {                   /* (Keys pull off: a key plays its note; the points carry on) */
            path_view(s, x, y, &q0x, &q0y);
            for (int i = 0; i < N; i++) if (g->kin[i] > 1e-3f) {
                float dX = s->eng.node[i].x - q0x, dY = s->eng.node[i].y - q0y;
                if (s->eng.surface != SURF_PLANE) dX -= floorf(dX + .5f);
                if (s->eng.surface == SURF_TORUS) dY -= floorf(dY + .5f);
                float mx2, my2; path_unview(s, dX, dY, &mx2, &my2);
                fx += g->kin[i] * mx2; fy += g->kin[i] * my2; fw += g->kin[i];
            }
        }
        float kp = 1.f - fexp2(-dt / .15f * 1.4427f);
        float gx = fx / (1.f + fw), gy = fy / (1.f + fw);       /* one key at full: half way; more: nearer */
        g->ox[j] += (gx - g->ox[j]) * kp; g->oy[j] += (gy - g->oy[j]) * kp;
        g->x[j] = x + g->ox[j]; g->y[j] = y + g->oy[j]; g->ht[j] = h;
        float qx, qy; path_view(s, g->x[j], g->y[j], &qx, &qy);
        /* speed: the distance the motion moved, per bar, smoothed. A formula change (or a wrap round the surface)
           is not motion: that tick keeps the speed it had */
        float mqx, mqy; path_view(s, mx, my, &mqx, &mqy);
        float dx = mqx - g->lx[j], dy = mqy - g->ly[j];
        float sp = sqrtf(dx * dx + dy * dy) / (dt / bar + 1e-9f);
        if (key != g->key || dx * dx + dy * dy > .01f) sp = g->spd[j];
        g->spd[j] += (sp - g->spd[j]) * (1.f - fexp2(-dt / .05f * 1.4427f));
        g->lx[j] = mqx; g->ly[j] = mqy; g->qx[j] = qx; g->qy[j] = qy;
        g->sm[j] += (g->spd[j] - g->sm[j]) * k2;
        g->hm[j] += (h - g->hm[j]) * k2; g->hv[j] += ((h - g->hm[j]) * (h - g->hm[j]) - g->hv[j]) * k2;
        /* the envelope, a function of the point's own continuous state */
        float e, em;
        switch (env) {
        case ENV_PULSE: case ENV_SWELL: {
            /* its phase runs at Env rate (an exact ratio to the bar) and is held to the bar's own clock: after a
               change of rate or tempo it eases back into step (about half a second), so it stays with the stream */
            float nu = erate;
            {   float z = nu * ((float)(m->bar % 3u) + m->tau), er = z - g->eph[j]; er -= floorf(er + .5f);
                g->eph[j] += (nu * dt / bar + er * dt * 2.f) * run; }
            g->eph[j] -= floorf(g->eph[j]);
            float psi = g->eph[j] + off; psi -= floorf(psi);
            if (env == ENV_PULSE) { float pq = psi / (psi + qk * (1.f - psi) + 1e-9f); e = powf_(sn(.5f * pq), pexp); }
            else e = powf_(.5f - .5f * cs(psi), 1.f + 2.f * C);
        } break;
        /* Speed and Height are read against their own running mean and spread (both smooth): loud above the mean,
           quiet below it; Contour sets how steep that is */
        case ENV_SPEED: e = .5f + conf * (smooth01((g->spd[j] / (g->sm[j] + 1e-4f) - 1.f) * (.8f + 2.5f * C) + .5f) - .5f); break;
        case ENV_PASS: {                                      /* nearness to a note: it sounds as it passes through */
            SurfPt qp; surf_pt(s->eng.surface, qx, qy, &qp);
            float bd = 1e30f, bv = 1e30f;
            for (int i = 0; i < N; i++) {
                float d2 = surf_d2(s->eng.surface, &qp, &s->eng.node[i]);
                if (d2 < bd) bd = d2;
                if (!L->vocab_on || (L->vocab_mask & (1u << i))) if (d2 < bv) bv = d2;
            }
            float best_all = N > 0 ? fexp2(-bd * iS * 4.f * 1.4427f) : 0.f;
            float best_allowed = N > 0 ? fexp2(-bv * iS * 4.f * 1.4427f) : 0.f;
            em = powf_(best_all, .6f + 1.6f * C);
            e = powf_(best_allowed, .6f + 1.6f * C);
        } break;
        case ENV_HEIGHT: { float sd = sqrtf(g->hv[j]) + .02f; e = .5f + conf * (smooth01((h - g->hm[j]) / sd * (.35f + .6f * C) + .5f) - .5f); } break;
        case ENV_STREAM: {                                    /* the rhythm's stream itself; Spread delays it point by point */
            float r = rhythm_R(s, m->tau - off / erate), lo = .04f + .5f * C;
            e = smooth01((r - lo) / .16f);
        } break;
        case ENV_DRUMS: {                                     /* a drum's envelope (Envelope rate picks it; 5..9: all) */
            int di = pint(s, LP(l, P_G1_ERATE));
            float de = 0.f;
            if (di < NUM_DRUMS) de = s->dr.v[di].env * s->dr.v[di].g;
            else for (int d = 0; d < NUM_DRUMS; d++) { float x = s->dr.v[d].env * s->dr.v[d].g; if (x > de) de = x; }
            e = powf_(de, .4f + 2.f * C);
        } break;
        default: e = 1.f; break;
        }
        if (env != ENV_PASS) em = e;
        /* Adds and Multiplies: the modulator's envelope (point j mod n) on this one's */
        if (is_car && (mroute == ROUTE_ADD || mroute == ROUTE_MUL)) {
            float me = mg->em[j % (mg->npts > 0 ? mg->npts : 1)];
            if (mroute == ROUTE_ADD) { e += depth * me; em += depth * me; }
            else { e *= 1.f - depth + depth * me; em *= 1.f - depth + depth * me; }
            if (e > 1.2f) e = 1.2f;
            if (em > 1.2f) em = 1.2f;
        }
        g->em[j] = em * run * g->mgn[j];
        g->e[j] = e * run * g->mgn[j];
        /* the drive: the envelope where the point is */
        float amp = is_mod ? 0.f : lvl * g->e[j] * accent;      /* a modulator does not sound */
        if (amp <= 1e-5f) continue;
        SurfPt qp; surf_pt(s->eng.surface, qx, qy, &qp);
        for (int i = 0; i < N; i++) {
            if (L->vocab_on && !(L->vocab_mask & (1u << i))) continue;
            float w = surf_d2(s->eng.surface, &qp, &s->eng.node[i]) * iR; if (w >= 1.f) continue;
            w = 1.f - w; g->u[i] += amp * w * w * s->eng.node[i].wt * g->gw[i];
        }
    }
    if (func == GEN_SWARM) for (int j = 0; j < npts; j++) { g->sx[j] = g->sw[j] - floorf(g->sw[j]); }
    g->key = key; g->la = A;
    for (int j = npts; j < MAX_PTS; j++) g->e[j] = g->em[j] = 0.f;

    for (int i = 0; i < N; i++) {
        g->u[i] += g->kin[i];                                 /* the keys */
        if (g->u[i] > 1.2f) g->u[i] = 1.2f;
        if (g->u[i] >= .1f && g->upv[i] < .1f) { g->rises++; s->n_rises++; }   /* a measurement, nothing acts on it */
    }
    for (int i = N; i < MAX_NODES; i++) g->u[i] = 0.f;
    g->px = g->x[0]; g->py = g->y[0];
    float kp = 1.f - fexp2(-dt / .03f * 1.442695f);
    g->pxs += (g->px - g->pxs) * kp; g->pys += (g->py - g->pys) * kp;
}

/* the notes' loudness: the drive, through a 3 ms symmetric smoothing (no zipper) */
DSP_CTL void gen_harmony(Synth *s, float dt) {
    int N = s->eng.n_nodes;
    float ks = 1.f - fexp2(-dt / .003f * 1.442695f);
    for (int l = 0; l < NUM_LAYERS; l++) {
        GenSt *g = &s->layer[l].g; float rise = 0.f;
        for (int i = 0; i < N; i++) {
            float a0 = g->a[i];
            g->a[i] = a0 + (g->u[i] - a0) * ks;
            if (g->a[i] < 1e-6f) g->a[i] = 0.f;
            if (g->a[i] > a0) rise += g->a[i] - a0;
        }
        for (int i = N; i < MAX_NODES; i++) g->a[i] = 0.f;
        g->onset = rise;
    }
}

/* a key: a push into the system (a kernel in time of its velocity) and, while held, a steady input */
void gen_key(Synth *s, int l, int node, float vel, int on) {
    GenSt *g = &s->layer[l].g;
    if (node < 0 || node >= MAX_NODES) return;
    if (!on) { g->held[node] = 0; return; }
    g->held[node] = 1; g->held_v[node] = vel;
    int q = 0; float oldest = -1.f;
    for (int k = 0; k < 4; k++) { if (g->kvel[node][k] <= 0.f) { q = k; break; } if (g->kage[node][k] > oldest) { oldest = g->kage[node][k]; q = k; } }
    g->kvel[node][q] = vel; g->kage[node][q] = 0.f;
}

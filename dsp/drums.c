/*
 * The drums: four (kick, snare, hat, tom). They make no sound of their own and pick no notes: a hit puts a drum's
 * envelope and character on a note the synth's own motion is already playing, on the drum layer (Drum layer: one,
 * or Split, kick and tom on the layer sounding lower, snare and hat on the higher). Build F (v8): this replaced the
 * per-sample drum synthesis (partial banks, noise combs, a resonator, shapers), for the device's budget.
 *
 * Triggers. The rhythm (rhythm.c) finds the events (peaks of its stream over the threshold: when, how strongly) and
 * decides which drum each one plays (Select, Count), or runs its four coupled oscillators (Lock); it calls
 * drums_trigger. A drum set to Follow "Layer 1" or "Layer 2" is outside that: it reads one point's envelope of that
 * layer (Place picks the point) and fires when it rises through 1 - Density, re-arming 0.12 below. Every hit goes
 * through a small queue so it lands on its sample.
 *
 * The voice. Voice picks which of the drum layer's sounding notes the hit rides: 0 the lowest, 1 the highest. A silent
 * layer: the note under its motion's loudest point. The voice is shared: a hit on a held note swells that note.
 *
 * The envelope, per drum: Attack (1 to 20 ms; a retrigger rises from where it is), Hold, Decay (to -35 dB; Shape by
 * lengthens or shortens it), Curve (exponential .. straight). Worked out once a control tick from the parameters as
 * they are then, so modulation reshapes a hit that is still ringing; the voice's level ramps between ticks, from the
 * sample the hit lands on.
 *
 * The character, on the voice, at full strength while the hit is within 18 dB of its peak, then fading with it (not
 * by the hit's share of the voice's level: a loud note no longer waters it down): Bright tilts the partials up at
 * the start (up to +3 dB an octave, or Dark: down toward its undertones; the note's energy kept, the fundamental at most
 * 6 dB down), Settle is how fast they come back (300 ms down to 9 ms) and how far below they
 * settle; Snap pushes the voice's Wave (its 2nd and 3rd harmonic, past Wave's own top), falling over a length Settle
 * sets (71 ms .. 9 ms). The kick lifts the voice's
 * 90 Hz fade, so its fundamental stays. A hit never touches the voice's pitch: the voice is the synth's note, in tune.
 *
 * Mix. Each drum has its level; Shape by multiplies every drum, continuously, by 1 - Depth + Depth x a layer's
 * envelope (its loudest point's), and lengthens or shortens each hit's decay with it. Duck: the kick's sidechain on
 * the other voices, 1 - 0.75 Duck x the kick's envelope squared, released over 150 ms.
 */
#define MANIFOLD_DSP_UNIT
#include "synth.h"
#include "mstring.h"

#ifndef DR_DCOMP
#define DR_DCOMP 1.f              /* (1.0.6) how much of a Dry hit's energy change (Shape's lengths) its level gives back */
#endif
#ifndef DR_ECAP
#define DR_ECAP .4f               /* (s) past this a hit's length is mostly cut by the next one: it adds no more */
#endif
#define DR_HG 1.f               /* (a hit's level, 0..1; voice.c DR_OUT sets its loudness) */

static const char *const DRUM_N[NUM_DRUMS] = {"Kick", "Snare", "Hat", "Tom"};
static const char *const DSRC_N[NUM_DSRC] = {"The stream", "Layer 1", "Layer 2"};
static const char *const DLAYER_N[NUM_DLAYERS] = {"Layer 1", "Layer 2", "Split"};
const char *drum_name(int d) { return d >= 0 && d < NUM_DRUMS ? DRUM_N[d] : "?"; }
const char *drum_src_name(int v) { return v >= 0 && v < NUM_DSRC ? DSRC_N[v] : "?"; }
const char *drum_layer_name(int v) { return v >= 0 && v < NUM_DLAYERS ? DLAYER_N[v] : "?"; }

/* the hit's Odd/even push at full, as its layer's bank coloured: worked out once a hit (in the flash: rare), the
   render blends each note toward it by its share. Toward Fixed (x > 0): that colour, then x of the way to its drum's
   profile (Body, Crack, Sizzle), the push at full */
static void hit_spec(Synth *s, int d, int l, int node, float x) {
    float of = (pval(s, P_D1_OE + d) - .5f) * 2.f;
    s->dr.pf[d] = (uint8_t)(x > .01f);
    if (x > .01f) {
        float pr[MAX_HARM];
        s->dr.of[d] = 1.f;
        hit_colour(s, l, fclamp(pval(s, P_V1_ODDEVEN + l) + .5f * of, 0.f, 1.f), s->dr.oa[d]);
        hit_profile(s, d, l, node, fclamp(.5f + .5f * of, 0.f, 1.f), pr);
        for (int j = 0; j < MAX_HARM; j++) s->dr.oa[d][j] += x * (pr[j] - s->dr.oa[d][j]);
    } else {
        s->dr.of[d] = of;
        if (of > .02f || of < -.02f)
            hit_colour(s, l, fclamp(pval(s, P_V1_ODDEVEN + l) + .5f * of, 0.f, 1.f), s->dr.oa[d]);
    }
}
/* (1.0.6) The drums' notes, Follow .. Fixed (x). Each drum has its pitch and its band, after the 808's: the kick
   45..76 Hz (its 56), the tom 90..185 (its three toms), the snare 200..320 (its 238), the hat the top octave of the
   layout up to 4 kHz (its sizzle, over 5 kHz, needs a high note); Voice sets the pitch within the band. Fixed (x = 1):
   a note the motion is sounding in the band, the nearest to the pitch, or else the scale's nearest note to it,
   sounding or not (always in tune). Toward Follow the band widens, by (1 - x) / x octaves each way: at 1/2 an octave,
   near 0 the whole layout; at 0 the drum follows as it always did (pick_node). The drum layer's notes (Split: both
   layers'). -1: no note. In the flash: a hit's, rare */
int drum_fixed(const Synth *s, int d, float x, int *layer) {
    int N = s->eng.n_nodes, dl = pint(s, P_DR_LAYER), best = -1, sb = -1, sl = 0;
    float v = pval(s, DP(d, P_D1_VOICE)), bd = 1e9f, sd = 1e9f, tgt, lo, hi, fmax = 0.f;
    for (int l = 0; l < NUM_LAYERS; l++) {
        if (dl != DLAYER_SPLIT && l != (dl == DLAYER_L2)) continue;
        float oct = fexp2((float)(pint(s, LP(l, P_V1_REG)) - 1));
        for (int i = 0; i < N; i++) { float f = s->eng.node[i].freq * oct; if (f < 4000.f && f > fmax) fmax = f; }
    }
    if (d == 0) { lo = 45.f; hi = 76.f; } else if (d == 3) { lo = 90.f; hi = 185.f; }
    else if (d == 1) { lo = 200.f; hi = 320.f; } else { lo = fmax * .5f; hi = fmax; }
    tgt = d == 2 ? fmax * fexp2(-v) : lo * fexp2(v * flog2(hi / lo));
    {   float w = x >= 1.f ? 0.f : x <= .02f ? 12.f : (1.f - x) / x, wf = fexp2(w);   /* (the band, widened) */
        lo /= wf; hi *= wf; }
    for (int l = 0; l < NUM_LAYERS; l++) {
        if (dl != DLAYER_SPLIT && l != (dl == DLAYER_L2)) continue;
        const GenSt *g = &s->layer[l].g;
        float oct = fexp2((float)(pint(s, LP(l, P_V1_REG)) - 1));
        for (int i = 0; i < N; i++) {
            float f = s->eng.node[i].freq * oct;
            if (f < 20.f || f >= 4000.f) continue;
            float dd = flog2(f / tgt); if (dd < 0.f) dd = -dd;
            if (dd < bd) { bd = dd; best = i; *layer = l; }                   /* (the scale's nearest: its own) */
            if (g->a[i] > .02f && f >= lo * .999f && f <= hi * 1.001f && dd < sd) { sd = dd; sb = i; sl = l; }   /* (sounding, in its band) */
        }
    }
    if (sb >= 0) { *layer = sl; return sb; }
    return best;
}

/* (1.0.6) a hit's energy, in seconds at full level: attack, hold and the decay as its curve shapes it (capped where
   the next hit would cut it) - from the patch's own values (b) or where the knobs have them */
static float dr_len(const Synth *s, int d, int b) {
    const float *x = b ? s->base : s->mod; float r[4]; static const uint8_t I[4] = {P_D1_ATTACK, P_D1_HOLD, P_D1_DECAY, P_D1_CURVE};
    for (int k = 0; k < 4; k++) {
        const ParamMeta *m = &g_param_meta[DP(d, I[k])]; float v = fclamp(x[DP(d, I[k])], 0.f, 1.f);
        r[k] = m->curve == CURVE_LOG ? m->lo * fexp2(v * flog2(m->hi / m->lo)) : m->lo + v * (m->hi - m->lo);
    }
    float e = r[0] * (1.f / 3.f) + r[1] + r[2] * (.125f + r[3] * .208f);
    return e > DR_ECAP ? DR_ECAP : e < 1e-3f ? 1e-3f : e;
}
/* the level a Dry patch's hit gets back for it (flash: once a tick, Dry only) */
void dr_lcomp(Synth *s) {
    if (!g_knob_dry && s->dr.v[0].lc == 1.f) return;
    for (int d = 0; d < NUM_DRUMS; d++)
        s->dr.v[d].lc = g_knob_dry ? fexp2(fclamp(-.5f * DR_DCOMP * flog2(dr_len(s, d, 0) / dr_len(s, d, 1)), -2.f, 2.f)) : 1.f;
}

#include "ctl_on.h"
/* a layer's sounding notes (their generator's level over 0.02): how many, the lowest and highest pitch */
#include "ctl_off.h"   /* (the flash code below: its own math, not the RAM's) */
/* (1.0.7: sounding, pick_node and pick_layer in the flash with hit, their only caller: once a hit) */
static int sounding(const Synth *s, int l, float *lo, float *hi) {
    const GenSt *g = &s->layer[l].g; int n = 0; *lo = 1e9f; *hi = 0.f;
    float oct = fexp2((float)(pint(s, LP(l, P_V1_REG)) - 1));
    for (int i = 0; i < s->eng.n_nodes; i++) if (g->a[i] > .02f) {
        float f = s->eng.node[i].freq * oct; n++;
        if (f < *lo) *lo = f; if (f > *hi) *hi = f;
    }
    return n;
}

/* the note a hit rides on layer l: the sounding one Voice points at (0 the lowest .. 1 the highest), or, with nothing
   sounding, the note under the motion's loudest point */
static int pick_node(const Synth *s, int l, float voice) {
    const GenSt *g = &s->layer[l].g; int N = s->eng.n_nodes;
    float lo, hi; int n = sounding(s, l, &lo, &hi);
    if (n > 0) {
        int want = (int)(voice * (float)(n - 1) + .5f);
        for (int i = 0; i < N; i++) if (g->a[i] > .02f) {           /* the one with exactly want sounding notes below it */
            float f = s->eng.node[i].freq; int below = 0;
            for (int j = 0; j < N; j++) if (j != i && g->a[j] > .02f && (s->eng.node[j].freq < f || (s->eng.node[j].freq == f && j < i))) below++;
            if (below == want) return i;
        }
    }
    int jb = 0; float eb = -1.f;
    for (int j = 0; j < g->npts; j++) if (g->em[j] > eb) { eb = g->em[j]; jb = j; }
    int bn = 0; float bd = 1e30f;
    SurfPt sp; DSP_FAR(surf_pt)(s->eng.surface, g->qx[jb], g->qy[jb], &sp);
    for (int i = 0; i < N; i++) { float dd = surf_d2(s->eng.surface, &sp, &s->eng.node[i]); if (dd < bd) { bd = dd; bn = i; } }
    return bn;
}

/* the layer drum d plays on: Drum layer's, or (Split) kick and tom on the layer sounding lower, snare and hat higher */
static int pick_layer(const Synth *s, int d) {
    int dl = pint(s, P_DR_LAYER);
    if (dl != DLAYER_SPLIT) return dl == DLAYER_L2 ? 1 : 0;
    float lo0, hi0, lo1, hi1; int n0 = sounding(s, 0, &lo0, &hi0), n1 = sounding(s, 1, &lo1, &hi1);
    int low = d == 0 || d == 3;
    if (!n0 && !n1) {                                        /* both silent: by the layers' registers */
        int r0 = pint(s, LP(0, P_V1_REG)), r1 = pint(s, LP(1, P_V1_REG));
        return low ? (r1 < r0 ? 1 : 0) : (r1 > r0 ? 1 : 0);
    }
    if (!n0) return 1;
    if (!n1) return 0;
    return low ? (lo1 < lo0 ? 1 : 0) : (hi1 > hi0 ? 1 : 0);
}

/* the hit itself, landing delay samples into the coming block */
/* (1.0.7: in the flash: it runs once a hit, and the effects needed the RAM) */
static void hit(Synth *s, int d, float vel, int delay) {
    DrumV *v = &s->dr.v[d];
    if (s->eng.n_nodes < 1) return;
    float fx = pval(s, P_DR_NOTES); int l = 0, node = -1;
    if (fx > .01f) node = DSP_FAR(drum_fixed)(s, d, fx, &l);   /* (1.0.6) toward Fixed: its band, its pitch */
    if (node < 0) { fx = 0.f; l = DSP_FAR(pick_layer)(s, d); node = DSP_FAR(pick_node)(s, l, pval(s, DP(d, P_D1_VOICE))); }
    if (v->on && (node != v->node || l != v->layer)) { v->old = v->cur; v->ls = 0.f; v->cur.h1 = 0.f; }   /* another voice: the last one fades */
    else v->ls = v->on ? v->env : 0.f;                      /* the same voice: rise from where it is */
    v->node = node; v->layer = l;
    v->g = fclamp(vel, 0.f, 1.f);
    v->delay = delay < 0 ? 0 : delay > SUB - 1 ? SUB - 1 : delay;
    v->pend = 1;
    s->dr.hits++; s->dr.hs[d] = s->nsamp + (uint32_t)v->delay; s->dr.hn[d]++; s->dr.flash[d] = 1.f;
    DSP_FAR(hit_spec)(s, d, l, node, fx);
}

void drums_hit(Synth *s, int d, int node, float vel) { (void)node; if (d >= 0 && d < NUM_DRUMS) DSP_FAR(hit)(s, d, vel, 0); }

#include "ctl_on.h"
DSP_CTL static void queue(DrumV *v, int node, float vel, int32_t t) {
    if (v->nq >= DQ) return;
    v->qt[v->nq] = t; v->qv[v->nq] = vel; v->qn[v->nq] = (int16_t)node; v->nq++;
}

/* a trigger from the rhythm: the hit, t samples from now */
DSP_CTL void drums_trigger(Synth *s, int d, int node, float vel, int32_t t) {
    if (d >= 0 && d < NUM_DRUMS) queue(&s->dr.v[d], node, vel, t < 0 ? 0 : t);
}

/* the envelope at t samples: attack from ls, hold, decay (exponential .. straight by Curve) */
DSP_CTL static float env_at(float t, float ls, float A, float H, float D, float cv) {
    if (t < A) return ls + (1.f - ls) * t / A;
    float td = t - A - H; if (td <= 0.f) return 1.f;
    float ex = fexp2(-5.77f * td / D), ln = 1.f - td / D; if (ln < 0.f) ln = 0.f;
    return ex + cv * (ln - ex);
}

DSP_CTL void drums_tick(Synth *s, float dt) {
    Drums *D = &s->dr;
    DSP_FAR(dr_lcomp)(s);
    for (int d = 0; d < NUM_DRUMS; d++) D->flash[d] *= .96f;
    {   /* Shape by: the layer's loudest point's envelope (continuous), the gain's target for this block */
        int sh = pint(s, P_DR_SHAPE); float g = 1.f, e = .5f;
        if (sh != DSHAPE_NONE) {
            const GenSt *lg = &s->layer[sh - DSHAPE_L1].g; e = 0.f;
            for (int j = 0; j < lg->npts; j++) if (lg->em[j] > e) e = lg->em[j];
            e = fclamp(e, 0.f, 1.f);
            float dp = pval(s, P_DR_DEPTH); g = 1.f - dp + dp * e;
        }
        D->shape_tgt = g; D->shape_e = e;
        D->shape += (D->shape_tgt - D->shape) * .3f;
    }
    int on = pint(s, P_DR_ON) && s->transport;
    if (on) {
        float acc = pval(s, P_DR_ACCENT);
        for (int d = 0; d < NUM_DRUMS; d++) {                 /* Follow a layer: one point's envelope rising */
            float dens = pval(s, DP(d, P_D1_DENS)), place = pval(s, DP(d, P_D1_PLACE));
            int src = pint(s, DP(d, P_D1_SRC));
            DrumV *v = &D->v[d];
            if (dens < .01f || src < DSRC_L1 || pint(s, P_D1_PAT + d) > 0) continue;   /* (a pattern: rhythm.c's) */
            const GenSt *g = &s->layer[src - DSRC_L1].g;
            int np = g->npts > 0 ? g->npts : 1, j = (int)(place * (float)(np - 1) + .5f);
            float e = g->em[j], th = 1.f - .95f * dens;
            if (v->armed[0] && e > th) { queue(v, -1, (1.f - acc) + acc * fclamp(e, 0.f, 1.f), 0); v->armed[0] = 0; }
            else if (e < th - .12f) v->armed[0] = 1;
        }
    }
    for (int d = 0; d < NUM_DRUMS; d++) {                     /* the queue: what lands in this block (the earliest), the rest later */
        DrumV *v = &D->v[d]; int best = -1;
        if (!on) { v->nq = 0; continue; }
        for (int q = 0; q < v->nq; q++) if (v->qt[q] < SUB && (best < 0 || v->qt[q] < v->qt[best])) best = q;
        if (best >= 0) {
            DSP_FAR(hit)(s, d, v->qv[best], v->qt[best]);
            v->qt[best] = v->qt[--v->nq]; v->qv[best] = v->qv[v->nq]; v->qn[best] = v->qn[v->nq];
        }
        for (int q = 0; q < v->nq; q++) { v->qt[q] -= SUB; if (v->qt[q] < 0) v->qt[q] = 0; }
    }
    /* the envelopes and what they put on their voices this tick */
    float lev = pval(s, P_DR_LEVEL) * D->shape * DR_HG, kick = 0.f;
    float dshape = pint(s, P_DR_SHAPE) != DSHAPE_NONE ? 1.f + pval(s, P_DR_DEPTH) * (D->shape_e - .5f) * 1.2f : 1.f;
    for (int d = 0; d < NUM_DRUMS; d++) {
        DrumV *v = &D->v[d]; DrumHit *c = &v->cur, *o = &v->old;
        if (o->h1 > 0.f || o->h0 > 0.f) { o->h0 = o->h1; o->h1 = o->h1 > 1e-3f ? o->h1 * .35f : 0.f; o->off = 0; }   /* (the replaced hit: out in ~3 ms) */
        if (!v->on && !v->pend) { c->h0 = c->h1 = 0.f; continue; }
        float A = pval(s, DP(d, P_D1_ATTACK)) * SR, H = pval(s, DP(d, P_D1_HOLD)) * SR;
        float Dc = pval(s, DP(d, P_D1_DECAY)) * dshape * SR, cv = pval(s, DP(d, P_D1_CURVE));
        if (A < .001f * SR) A = .001f * SR;
        int off = 0;
        if (v->pend) {                                        /* the hit lands this tick */
            off = v->delay; v->pend = 0; v->t = 0.f; v->on = 1;
            c->h0 = c->h1;                                    /* (where the voice is: 0 on a new one, hit() saw to it) */
            c->layer = (int8_t)v->layer; c->node = (int8_t)v->node; c->d = (uint8_t)d;
        } else c->h0 = c->h1;
        if (!v->on) continue;
        float t1 = v->t + (float)(SUB - off);
        float e = env_at(t1, v->ls, A, H, Dc, cv);
        v->env = e; v->t = t1;
        c->off = (uint8_t)off;
        c->h1 = e * v->g * pval(s, DP(d, P_D1_LEVEL)) * lev * v->lc;   /* (lc, Dry: Shape moves the lengths, not much the loudness) */
        /* the character, at the tick's middle */
        float tm = (t1 - .5f * (float)SUB) * (1.f / SR); if (tm < 0.f) tm = 0.f;
        float st = pval(s, DP(d, P_D1_SETTLE)), ts = .3f * fexp2(-5.f * st), se = fexp2(-tm / ts * 1.4427f);
        {   /* Bright: -1 (dark: toward the undertones) .. 1 (toward the top), +-3 dB an octave at the ends, settling; after
               it, a bright hit leaves the note a little under where it was (as far as Settle is quick) */
            float B = pval(s, DP(d, P_D1_BRIGHT));
            c->tilt = .5f * B * se - (B > 0.f ? .25f * B * st * (1.f - se) : 0.f);
        }
        {   /* Snap: up to twice the push at the top (as it was low down), over a length Settle sets: 71 ms .. 25 ms (its
               middle) .. 9 ms */
            float sn = pval(s, DP(d, P_D1_SNAP)), tsn = .025f * fexp2(-3.f * (st - .5f));
            c->snap = sn * (1.f + sn) * fexp2(-tm / tsn * 1.4427f);
        }
        c->w = e * 8.f > 1.f ? 1.f : e * 8.f;   /* (the character at full until the hit falls under -18 dB, then with it) */
        c->oe = (s->dr.pf[d] ? 1.f : (pval(s, P_D1_OE + d) - .5f) * 2.f) * se;   /* its Odd/even push (Fixed: its
                                                    profile), settling as its Bright does */
        c->pl = pval(s, P_D1_HPLUCK + d);                  /* its Pluck on the note (voice.c holds it) */
        if (t1 > A + H && e < 1e-4f) { v->on = 0; v->env = 0.f; c->h1 = 0.f; }
        if (d == 0) kick = v->env * v->g;
    }
    {   /* the duck: the kick's envelope squared, released over 150 ms; its gain on the other voices */
        float dk = on ? .75f * pval(s, P_DR_DUCK) : 0.f, k2 = kick * kick;
        D->duck = k2 > D->duck ? k2 : D->duck * fexp2(-(float)SUB / (.15f * SR) * 1.4427f);
        D->dk0 = D->dk1; D->dk1 = 1.f - dk * D->duck;
    }
    (void)dt;
}
#include "ctl_off.h"

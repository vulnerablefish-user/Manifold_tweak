/*
 * The rhythm. There is no meter, no grid and no stored pattern: only functions of time.
 *
 * The stream.   R(x) = sum_k w_k b(k x - p_k),  x in bars,  b(z) = exp(5 (cos 2 pi z - 1))  (period 1, peak 1 at 0)
 *   k:  1 2 4          always (the bar, its half, its beat)
 *       8 16 32        the twos    \  Threes crossfades the two families (both at the middle: 3 against 4);
 *       3 6 12 24 18   the threes  /  Fine opens the faster ones in turn
 *       4, phase p     the off-beat: Swing turns its phase from 1/2 (straight) through 2/3 (triplet swing) to 3/4
 *                      (hard), and as it comes in the straight 8 16 32 give way to 12 and 24 underneath
 *   Turn shifts the threes (and 18) in time against the twos, by up to a third of a bar: any rotation is a rhythm.
 *
 * An event is a peak of R above the threshold: its time (found between control ticks by a parabola through three
 * values, so it lands on the sample) and its strength (how far over, 0..1). Density is the threshold. Motion lets a
 * generator's point move it. The events carry no instrument.
 *
 * Form (long patterns without bars or fills): slower functions, cos 2 pi (x / q - p), move the threshold (q = 4, 16,
 * 64, 3), the sixteenths' weight (8), the three-feel (5), the selector (2, 16) and the tom (7). With all of them the
 * whole only repeats after 6,720 bars, yet neighbouring bars stay alike because the stream under them never changes.
 *
 * Which drum (the second layer):
 *   Select   a selector function, cos 2 pi (r x - p) at r = 1/2 .. 3 a bar, is read at each event. Each drum scores
 *            1 - 1.6 |strength - its own| (Place: the kick wants the strongest events, the hat the weakest), plus the
 *            selector for the kick, minus it for the snare (so they answer each other), plus its Density, minus
 *            Feedback x how lately it played. The best score plays; the hat also plays under the others. The very
 *            strongest events (where every function peaks together) are always the kick's: the anchor.
 *   Count    the hat on every event, the kick every n-th, the snare every 2n-th (between two kicks), the tom on the
 *            strong ones left; the count starts again at each event of full strength, so the kick keeps its anchor.
 * A drum set to follow a layer is outside this: it fires from that layer's point (drums.c).
 *
 * Patterns (1.0.5): a drum may play a line from the trained library (patterns.c) instead of taking part in the
 * choice above. Its steps are the metre's 16ths, bent by Swing (at the 8ths or the 16ths: within each pair the second
 * moves from 1/2 of the pair to 2/3 .. 3/4), so they lie where the stream's own pulses are and move with them. A step
 * sounds if the stream without swing is strong enough there: R0 >= Th / 2 (a ghost step: R0 >= Th), so Density thins
 * a pattern from its weakest steps (16ths first, the downbeat last). Rotate delays the line by steps, Length loops it
 * over its first n steps (against the bar: polymeter), Fills put a busier neighbour (the library is ordered sparse to
 * busy) on the second half of a phrase's last bar, Ghosts let the patterned drums also take the linear layer's events,
 * quietly. Every hit of a tick, the patterns' and the linear layer's, is collected first: a drum that plays mutes its
 * target's hits on the same tick (kick, snare, tom, hat in that order of precedence), or the linear layer's.
 *
 * Lock (the other engine): four phase oscillators, one a drum, each a little off a whole-number rate (kick 2, snare 2
 * half a cycle round, hat 8, tom 3 a bar). Pull couples each to its exact rate, d(theta) = r (1 + detune) + Pull r
 * sin 2 pi (r x - theta): above its detune it locks, below it slips and drifts and catches again; every hit also
 * nudges the others toward their own next hit. Hits are loudest in step.
 */
#define MANIFOLD_DSP_UNIT
#include "synth.h"
#include "mstring.h"

static const uint8_t RH_K[RH_NH] = {1, 2, 4, 8, 16, 32, 3, 6, 12, 24, 18, 4};
static const float SELRATE_V[NUM_SELRATES] = {.5f, 1.f, 1.5f, 2.f, 3.f};
static const char *const SELRATE_N[NUM_SELRATES] = {"1 per 2 bars", "1 a bar", "3 per 2 bars", "2 a bar", "3 a bar"};
const char *selrate_name(int r) { return r >= 0 && r < NUM_SELRATES ? SELRATE_N[r] : "?"; }
const char *rh_engine_name(int e) { return e == RH_LOCK ? "Lock" : "Threshold"; }
const char *rh_assign_name(int a) { return a == RH_COUNT ? "Count" : "Select"; }

void drums_trigger(Synth *s, int d, int node, float vel, int32_t t);
static const char *const PMUTE_N[NUM_PMUTE] = {"Nothing", "The kick", "The snare", "The hat", "The tom", "The linear drums"};
const char *pmute_name(int m) { return m >= 0 && m < NUM_PMUTE ? PMUTE_N[m] : "?"; }
const PatLine *pat_line(const Synth *s, int d) {
    int i = (int)(s->mod[P_D1_PAT + d] * (float)(PAT_CNT[d] + 1)); if (i > PAT_CNT[d]) i = PAT_CNT[d];
    if (i <= 0 && s->base[P_D1_PAT + d] * (float)(PAT_CNT[d] + 1) >= 1.f) i = 1;   /* (as pat_idx) */
    return i > 0 ? &PAT_LIB[PAT_OFF[d] + i - 1] : 0;
}

#include "ctl_on.h"
/* this tick's hits, the patterns' and the linear layer's, before the mutes */
typedef struct { int8_t d, lin; float v; int32_t t; } Cand;
#define NCAND 16
static Cand cand[NCAND];
static int ncand;
DSP_INL void emit(int d, float v, int32_t t, int lin) {
    if (ncand < NCAND) { cand[ncand].d = (int8_t)d; cand[ncand].lin = (int8_t)lin; cand[ncand].v = v; cand[ncand].t = t; ncand++; }
}
/* a drum's line now: the knob or the matrix may move a set line among its relatives, never to Linear (a line set
   stays a line); a drum set to Linear stays linear */
DSP_INL int pat_idx(const Synth *s, int d) {
    int i = pint(s, P_D1_PAT + d);
    if (i <= 0 && s->base[P_D1_PAT + d] * (float)(PAT_CNT[d] + 1) >= 1.f) i = 1;
    return i > 0 && i <= PAT_CNT[d] ? i : 0;
}
DSP_INL int bits(uint32_t x) { int n = 0; while (x) { x &= x - 1u; n++; } return n; }   /* (no library call in RAM) */
DSP_INL float sm01(float x) { x = fclamp(x, 0.f, 1.f); return x * x * (3.f - 2.f * x); }
DSP_INL float bump(float z) { return fexp2(7.2135f * (fsin(z + .25f) - 1.f)); }   /* exp(5 (cos 2 pi z - 1)) */

/* cos 2 pi (x / q - ph), x the bars since the start: exact however long it has run */
DSP_CTL float rhythm_slow(const Synth *s, int q, float ph) {
    const Rhythm *m = &s->meter;
    float z = ((float)(m->bar % (uint32_t)q) + m->tau) / (float)q - ph;
    return fsin(z + .25f);
}

DSP_CTL float rhythm_R(const Synth *s, float tau) {
    const Rhythm *m = &s->meter; float r = 0.f;
    for (int i = 0; i < RH_NH; i++) if (m->w[i] > 1e-4f) r += m->w[i] * bump((float)RH_K[i] * tau - m->ph[i]);
    return m->top > 1e-6f ? r / m->top : 0.f;
}

DSP_CTL static void weights(Synth *s) {
    Rhythm *m = &s->meter;
    float T = pval(s, P_RH_THREES), F = pval(s, P_RH_FINE), Fm = pval(s, P_RH_FORM), sw = sm01(pval(s, P_RH_SWING) * 3.f);
    float g1 = sm01(F / .3f), g2 = sm01((F - .25f) / .4f), g3 = sm01((F - .6f) / .4f);
    float a = sm01((1.f - T) * 1.6f), b = sm01(T * 1.6f), turn = pval(s, P_RH_TURN);
    float *w = m->w;
    w[0] = .5f; w[1] = .45f; w[2] = .4f;
    w[3] = .45f * a * g1 * (1.f - sw);
    w[4] = .24f * a * g2 * (1.f - sw) * (1.f + Fm * .6f * rhythm_slow(s, 8, .9f));
    w[5] = .12f * a * g3 * (1.f - sw);
    w[6] = .32f * b * ((1.f - Fm) + Fm * (.5f + .5f * rhythm_slow(s, 5, 0.f)));
    w[7] = .40f * b * g1;
    w[8] = .28f * b * g2 + .20f * a * sw * g2;
    w[9] = .13f * b * g3 + .12f * a * sw * g3;
    w[10] = .18f * b * g3 * (.5f + .5f * rhythm_slow(s, 4, .8f));
    w[11] = .34f * a * g1 * sw;
    for (int i = 0; i < RH_NH; i++) m->ph[i] = 0.f;
    for (int i = 6; i <= 10; i++) m->ph[i] = (float)RH_K[i] * turn * (1.f / 3.f);
    m->ph[11] = .5f + .25f * pval(s, P_RH_SWING);
    float top = 0.f; for (int i = 0; i < RH_NH; i++) top += w[i];
    m->top = top;
    {   /* the same without swing: a pattern step's place in the metre (its gate and its strength) */
        float *v = m->w0, t0 = 0.f;
        for (int i = 0; i < RH_NH; i++) v[i] = w[i];
        v[3] = .45f * a * g1; v[4] = .24f * a * g2 * (1.f + Fm * .6f * rhythm_slow(s, 8, .9f)); v[5] = .12f * a * g3;
        v[8] = .28f * b * g2; v[9] = .13f * b * g3; v[11] = 0.f;
        for (int i = 0; i < RH_NH; i++) t0 += v[i];
        m->top0 = t0;
    }
    /* the threshold: Density, the long form, a generator's motion */
    float d = 1.f - pval(s, P_RH_DENS), th = .03f + .6f * d * d;
    th += Fm * (-.043f * rhythm_slow(s, 4, .9f) - .026f * rhythm_slow(s, 16, .95f) + .03f * rhythm_slow(s, 64, 0.f) + .017f * rhythm_slow(s, 3, 0.f));
    th -= pval(s, P_RH_MOTION) * .07f * fclamp(2.2f * s->layer[0].g.pxs, -1.f, 1.f);
    m->th = fclamp(th, .01f, .95f) * top;
    /* the selector, and the tom's lean */
    float r = SELRATE_V[pint(s, P_RH_SELRATE)];
    float z = ((float)(m->bar % 2u) + m->tau) * r - Fm * .08f * rhythm_slow(s, 16, .25f);
    m->sel = fsin(z + .25f) + Fm * .35f * rhythm_slow(s, 2, 0.f);
    m->tomb = .25f * fsin(m->tau) + Fm * .15f * rhythm_slow(s, 7, 0.f);
}

/* Select: the drum this event plays */
DSP_CTL static void assign_select(Synth *s, float st, int delay) {
    Rhythm *m = &s->meter; float acc = pval(s, P_DR_ACCENT), fb = pval(s, P_RH_FEED), gh = pval(s, P_RH_GHOST);
    static const float POL[NUM_DRUMS] = {.5f, -.6f, 0.f, 0.f};
    float best = -1e9f; int bd = -1, hat_in = 0;
    for (int d = 0; d < NUM_DRUMS; d++) {
        float dens = pval(s, DP(d, P_D1_DENS));
        if (pint(s, DP(d, P_D1_SRC)) != DSRC_STREAM || dens < .01f) continue;
        if (pat_idx(s, d) && gh < .01f) continue;          /* (a patterned drum: only as a ghost) */
        if (d == 2) hat_in = dens >= .35f;
        float c = 1.f - pval(s, DP(d, P_D1_PLACE)), df = st - c; if (df < 0.f) df = -df;
        float sc = 1.f - 1.6f * df + POL[d] * m->sel + (d == 3 ? m->tomb : 0.f) + (dens - .5f) * .8f - fb * m->tired[d];
        if (d == 0) sc += 1.2f * sm01((st - .8f) / .15f);        /* the strongest events are the kick's: its anchor */
        if (sc > best) { best = sc; bd = d; }
    }
    if (bd < 0) return;
    float gb = pat_idx(s, bd) ? gh * .6f : 1.f, gt = pat_idx(s, 2) ? gh * .6f : 1.f;   /* (patterned: quiet) */
    emit(bd, ((1.f - acc) + acc * st) * gb, delay, 1);
    if (bd != 2 && hat_in && st > .25f) emit(2, ((1.f - acc) + acc * st) * .6f * gt, delay, 1);
    for (int d = 0; d < NUM_DRUMS; d++) m->tired[d] *= .55f;
    if (bd != 2) m->tired[bd] += 1.f;
}

/* Count: by the events' order */
DSP_CTL static void assign_count(Synth *s, float st, int delay) {
    Rhythm *m = &s->meter; float acc = pval(s, P_DR_ACCENT), vel = (1.f - acc) + acc * st;
    int n = 2 + pint(s, P_RH_COUNT);
    if (st > .9f) m->count = 0;                              /* the strongest event: the count starts again */
    uint32_t c = m->count++;
    float gh = pval(s, P_RH_GHOST);
    #define IN(d) (pint(s, DP(d, P_D1_SRC)) == DSRC_STREAM && pval(s, DP(d, P_D1_DENS)) >= .01f && (!pat_idx(s, d) || gh >= .01f))
    #define GV(d) (pat_idx(s, d) ? gh * .6f : 1.f)
    if (c % (uint32_t)(2 * n) == (uint32_t)n) { if (IN(1)) emit(1, vel * GV(1), delay, 1); }
    else if (c % (uint32_t)n == 0) { if (IN(0)) emit(0, vel * GV(0), delay, 1); }
    else if (st > .3f && c % (uint32_t)n == (uint32_t)(n - 1)) { if (IN(3)) emit(3, vel * GV(3), delay, 1); }
    int hn = n / 4 < 1 ? 1 : n / 4;
    if (IN(2) && c % (uint32_t)hn == 0) emit(2, (.4f + .6f * vel) * GV(2), delay, 1);
    #undef GV
    #undef IN
}

/* Lock: four coupled oscillators, one a drum */
DSP_CTL static void lock_tick(Synth *s, float dx, int run) {
    Rhythm *m = &s->meter;
    static const float DET[NUM_DRUMS] = {0.f, .035f, -.02f, .05f}, PH0[NUM_DRUMS] = {0.f, .5f, 0.f, .1f};
    float dens = pval(s, P_RH_DENS), fine = pval(s, P_RH_FINE), three = pval(s, P_RH_THREES), Fm = pval(s, P_RH_FORM);
    float hat = (three > .5f ? 6.f : 8.f) * (fine > .66f ? 2.f : fine < .33f ? .5f : 1.f) * (dens < .25f ? .5f : 1.f);
    float rate[NUM_DRUMS] = {2.f, 2.f, hat, dens < .33f ? 1.5f : 3.f};
    float K = pval(s, P_RH_PULL) * .4f * (1.f + Fm * .5f * rhythm_slow(s, 8, 0.f));
    K += pval(s, P_RH_MOTION) * .1f * fclamp(2.2f * s->layer[0].g.pxs, -1.f, 1.f);
    if (K < 0.f) K = 0.f;
    float acc = pval(s, P_DR_ACCENT), loose = .5f + pval(s, P_EVOLVE);
    for (int d = 0; d < NUM_DRUMS; d++) {
        float r = rate[d], z = r * (((float)(m->bar % 2u)) + m->tau) - PH0[d];
        float err = z - m->lk[d]; err -= floorf(err + .5f);
        float q = fsin(err + .25f);
        m->lkq[d] += .05f * (q - m->lkq[d]);
        float old = m->lk[d], nw = old + (r * (1.f + DET[d] * loose) + K * r * fsin(err)) * dx;
        if (nw < old) nw = old;
        if (nw >= 1.f) {
            nw -= 1.f;
            if (run && pint(s, DP(d, P_D1_SRC)) == DSRC_STREAM && pval(s, DP(d, P_D1_DENS)) >= .01f && !pat_idx(s, d)) {
                float fr = (1.f - old) / (nw + 1.f - old + 1e-9f);
                float st = fclamp(.55f + .4f * q, .25f, 1.f);
                emit(d, (1.f - acc) + acc * st, (int32_t)(fr * (float)SUB), 1);
                m->nev++; m->ev_s = st; m->ev_flash = 1.f;
                for (int j = 0; j < NUM_DRUMS; j++) if (j != d) m->lk[j] -= .06f * K * fsin(m->lk[j]) * (1.f / PI_F);
            }
        }
        m->lk[d] = nw;
    }
    for (int d = 0; d < NUM_DRUMS; d++) { if (m->lk[d] < 0.f) m->lk[d] += 1.f; if (m->lk[d] >= 1.f) m->lk[d] -= 1.f; }
}

/* a step's place in the bar (0..1), Swing bending it: within each pair (two 16ths, or two 8ths) the second moves
   from half way to f = 1/2 + Swing / 4 of the pair (the stream's own off-beat moves the same way) */
DSP_INL float pat_pos(int n, float f, int lev16) {
    if (lev16) return (float)(n >> 1) * .125f + ((n & 1) ? f * .125f : 0.f);
    int q = n & 3; float u = q == 0 ? 0.f : q == 1 ? f * .5f : q == 2 ? f : f + (1.f - f) * .5f;
    return (float)(n >> 2) * .25f + u * .25f;
}
/* the patterns: the steps that fall in this tick's window (1.5 ticks back, as the stream's events land) */
DSP_CTL static void pat_tick(Synth *s, float dx) {
    Rhythm *m = &s->meter;
    float lag = 1.5f * dx, t1 = m->tau - lag, t0 = t1 - dx;
    float f = .5f + .25f * pval(s, P_RH_SWING), thn = m->th / (m->top > 1e-6f ? m->top : 1.f), acc = pval(s, P_DR_ACCENT), fill = pval(s, P_RH_FILL);
    int lev16 = pint(s, P_RH_SWLEV);
    int a0 = (int)floorf(t0 * 16.f) - 2, a1 = (int)floorf(t1 * 16.f) + 1;   /* (a step moves less than a 16th) */
    for (int d = 0; d < NUM_DRUMS; d++) {
        int pi = pat_idx(s, d);
        if (!pi || pval(s, DP(d, P_D1_DENS)) < .01f) continue;
        for (int a = a0; a <= a1; a++) {
            int n = a & 15, bo = (a - n) / 16;                 /* the step in its bar, the bar relative to this one */
            float pos = (float)bo + pat_pos(n, f, lev16);
            if (!(pos > t0 && pos <= t1)) continue;
            int32_t bar = (int32_t)m->bar + bo; if (bar < 0) continue;
            int li = pi;
            if (n >= 8 && fill > .01f && (bar & 3) == 3) {      /* a phrase's last half-bar: sometimes a busier relative */
                uint32_t h = ((uint32_t)(bar >> 2) * 2654435761u) ^ ((uint32_t)d * 40503u) ^ ((uint32_t)pi * 97u);
                h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
                float u = (float)(h & 0xFFFF) * (1.f / 65536.f);
                if (u < fill * (d == 0 ? .4f : 1.f)) {       /* the first busier relative within five steps' change */
                    const PatLine *P = &PAT_LIB[PAT_OFF[d] + pi - 1];
                    for (int k = pi + 1 + (int)(h >> 16 & 1u); k <= pi + 4 && k <= PAT_CNT[d]; k++) {
                        const PatLine *Q = &PAT_LIB[PAT_OFF[d] + k - 1];
                        if (bits((P->hit ^ Q->hit) & 0xFFFFu) <= 5 && bits(Q->hit & 0xFFFFu) >= bits(P->hit & 0xFFFFu)) { li = k; break; }
                    }
                }
            }
            const PatLine *L = &PAT_LIB[PAT_OFF[d] + li - 1];
            int len = pint(s, P_D1_LEN + d); len = len > 0 ? len + 1 : L->len;
            int rot = pint(s, P_D1_ROT + d);
            int g = (int)((uint32_t)bar * 16u + (uint32_t)n) - rot; g %= len; if (g < 0) g += len;
            int bit = g % L->len;
            if (!(L->hit >> bit & 1u)) continue;
            int gh = (int)(L->ghost >> bit & 1u);
            /* the gate: the stream without swing at the step's straight place */
            float u = (float)n * (1.f / 16.f), r0 = 0.f;
            for (int i = 0; i < RH_NH; i++) if (m->w0[i] > 1e-4f) r0 += m->w0[i] * bump((float)RH_K[i] * u - m->ph[i]);
            r0 = m->top0 > 1e-6f ? r0 / m->top0 : 0.f;
            float gate = gh ? thn : .5f * thn;
            if (r0 < gate) continue;
            float st = fclamp((r0 - gate) / (1.f - gate + 1e-6f), 0.f, 1.f);
            float v = (gh ? .42f : 1.f) * ((1.f - .55f * acc) + .55f * acc * st);
            int32_t t = (int32_t)((pos - t0) / dx * (float)SUB);
            emit(d, v, t < 0 ? 0 : t, 0);
            m->nev++; m->ev_s = st; m->ev_flash = 1.f;
        }
    }
}
/* the tick's hits: the mutes (a drum that plays mutes its target this tick; kick, snare, tom, hat in that order),
   then the drums */
DSP_CTL static void cand_flush(Synth *s) {
    static const int8_t ORD[NUM_DRUMS] = {0, 1, 3, 2};
    uint8_t gone[NCAND] = {0};

    for (int k = 0; k < NUM_DRUMS; k++) {
        int d = ORD[k], tgt = pint(s, P_D1_MUTE + d), plays = 0;
        if (tgt == PMUTE_NONE) continue;        for (int c = 0; c < ncand; c++)
            if (!gone[c] && cand[c].d == d) plays = 1;
        if (!plays) continue;        for (int c = 0; c < ncand; c++) {
            if (gone[c] || cand[c].d == d) continue;
            if (tgt == PMUTE_LINEAR ? cand[c].lin : cand[c].d == tgt - PMUTE_KICK)
                gone[c] = 1;
        }
    }

    for (int c = 0; c < ncand; c++) {
        if (gone[c]) continue;
        int d = cand[c].d;
        if (d >= 0 && d < 4 && s->meter.seq_hits[d] < 255)
            s->meter.seq_hits[d]++;
        drums_trigger(s, d, -1, cand[c].v, cand[c].t);
    }

    ncand = 0;
}

DSP_CTL void rhythm_tick(Synth *s, float dt) {
    Rhythm *m = &s->meter;
    for (int d = 0; d < 4; d++) m->seq_hits[d] = 0;
    m->beats = 4.f;
    m->bar_sec = m->beats * 60.f / pval(s, P_TEMPO);
    float dx = 0.f;
    if (s->transport) {
        dx = dt / m->bar_sec;
        m->tau += dx;
        if (m->tau >= 1.f) { m->tau -= 1.f; m->bar++; s->rand_a = s->rand_b; s->rand_b = frand(&s->rng) * 2.f - 1.f; }
    }
    {   /* SRC_RAND: a new random value each bar, reached smoothly across the bar (no steps) */
        float f = m->tau * m->tau * (3.f - 2.f * m->tau);
        s->rand_hold = s->rand_a + (s->rand_b - s->rand_a) * f;
    }
    weights(s);
    float th1 = m->th;
    m->R2 = m->R1; m->R1 = m->R;
    m->R = rhythm_R(s, m->tau) * m->top;
    m->Rn = m->top > 1e-6f ? m->R / m->top : 0.f;
    m->ev_flash *= .95f;
    int on = pint(s, P_DR_ON) && s->transport;
    ncand = 0;
    if (on && dx > 0.f) pat_tick(s, dx);
    if (pint(s, P_RH_ENGINE) == RH_LOCK) { lock_tick(s, dx, on); cand_flush(s); return; }
    if (dx <= 0.f) { cand_flush(s); return; }
    /* an event: R peaked at the tick before (R1 above both neighbours) and over the threshold. A parabola through
       the three values puts the peak within the tick; the hit lands a fixed 1.5 ticks (96 samples) after it */
    if (m->R1 > m->R && m->R1 >= m->R2 && m->R1 > th1) {
        float den = m->R2 - 2.f * m->R1 + m->R, off = den < -1e-9f ? .5f * (m->R2 - m->R) / den : 0.f;
        float pk = m->R1 - .25f * (m->R2 - m->R) * off;
        float st = fclamp((pk - th1) / (m->top - th1 + 1e-6f), 0.f, 1.f);
        int delay = (int)((float)(SUB / 2) + (float)SUB * fclamp(off, -.5f, .5f));
        m->nev++; m->ev_s = st; m->ev_flash = 1.f;
        if (on) { if (pint(s, P_RH_ASSIGN) == RH_COUNT) assign_count(s, st, delay); else assign_select(s, st, delay); }
    }
    cand_flush(s);
}

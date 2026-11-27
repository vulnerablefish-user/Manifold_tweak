/*
 * Parameter table, value mapping, fast math, randomizer, mutate, macros.
 */
#define MANIFOLD_DSP_UNIT
#include "synth.h"
#include "mstring.h"

/* ---------------- fast math table ---------------- */
float g_sin_tab[1025];

/* ---------------- parameter table ---------------- */
#define M(id,nm,sec,cv,md,st,lo,hi,df,rl,rh,tr) [id] = {nm,sec,cv,md,st,lo,hi,df,rl,rh,tr}
ParamMeta g_param_meta[NUM_PARAMS] = {
 M(P_SCALE,"scale",SEC_SCALE,CURVE_DISC,0,1, 0,1, 0,0,1,0),
 M(P_ROOT,"root",SEC_SCALE,CURVE_LIN,0,1, 21,57, .4f,.2f,.6f,1),
 M(P_OCTAVES,"octaves",SEC_SCALE,CURVE_DISC,0,1, 0,6, .6f,.5f,1,0),
 M(P_SURFACE,"surface",SEC_SCALE,CURVE_DISC,0,1, 0,NUM_SURFACES, 0,0,1,0),
 M(P_ROTATE,"rotation",SEC_SCALE,CURVE_DISC,0,1, 0,12, 0,0,0,0),
 M(P_TEMPER,"temper",SEC_SCALE,CURVE_LIN,0,0, 0,1, 0,0,0,0),

#define LAYER_P(V,G,SV,SG,lvl,fn,ct,rt,ev,er) \
 M(V##_CENTER,"center",SV,CURVE_LIN,1,0, 0,1, .12f,0,.5f,0), \
 M(V##_BAND,"band",SV,CURVE_LIN,1,0, 0,1, .35f,.12f,.75f,1), \
 M(V##_TILT,"tilt",SV,CURVE_LIN,1,0, 0,1, .35f,.2f,.55f,1), \
 M(V##_STRIDE,"stride",SV,CURVE_LIN,1,0, 0,1, 0,0,.5f,0), \
 M(V##_WAVE,"wave",SV,CURVE_LIN,1,0, 0,1, .2f,0,.8f,0), \
 M(V##_PHASE,"phase",SV,CURVE_LIN,1,0, 0,1, .3f,0,.8f,0), \
 M(V##_SPIN,"detune",SV,CURVE_LIN,1,0, 0,1, .35f,.1f,.7f,0), \
 M(V##_TONE,"tone",SV,CURVE_LIN,1,0, 0,1, .6f,.35f,.8f,0), \
 M(V##_BLOOM,"bloom",SV,CURVE_LIN,1,0, 0,1, .6f,.3f,.9f,0), \
 M(V##_WIDTH,"width",SV,CURVE_LIN,1,0, 0,1, .5f,.25f,.9f,1), \
 M(V##_LEVEL,"level",SV,CURVE_LIN,1,0, 0,1, lvl,lvl - .1f,lvl + .1f,0), \
 M(V##_REG,"register",SV,CURVE_DISC,0,0, 0,3, .5f,.34f,.67f,0), \
 M(G##_FUNC,"function",SG,CURVE_DISC,0,0, 0,NUM_GENS, fn,0,1,0), \
 M(G##_A,"a",SG,CURVE_LIN,1,0, 0,1, .5f,0,1,0), \
 M(G##_B,"b",SG,CURVE_LIN,1,0, 0,1, .4f,0,1,1), \
 M(G##_RATE,"rate",SG,CURVE_LOG,1,0, .015f,4, rt,.12f,.8f,1), \
 M(G##_POINTS,"points",SG,CURVE_DISC,0,0, 0,MAX_PTS, .3f,0,.75f,0), \
 M(G##_SPREAD,"spread",SG,CURVE_LIN,1,0, 0,1, .5f,.1f,1,0), \
 M(G##_ENV,"envelope",SG,CURVE_DISC,0,0, 0,NUM_ENVS, ev,0,1,0), \
 M(G##_ERATE,"envelope rate",SG,CURVE_DISC,0,0, 0,NUM_ERATES, er,0,.9f,0), \
 M(G##_CONTOUR,"contour",SG,CURVE_LIN,1,0, 0,1, ct,0,1,1), \
 M(G##_REACH,"reach",SG,CURVE_LIN,1,0, 0,1, .2f,0,.5f,0), \
 M(G##_ENERGY,"energy",SG,CURVE_LIN,1,0, 0,1, .6f,.35f,.85f,1), \
 M(G##_ROUTE,"route",SG,CURVE_DISC,0,0, 0,NUM_ROUTES, 0,0,0,0)
 LAYER_P(P_V1,P_G1,SEC_VOICE1,SEC_GEN1,.7f,.05f,.75f,.75f,.08f,.72f),
 LAYER_P(P_V2,P_G2,SEC_VOICE2,SEC_GEN2,.5f,.6f,.2f,.5f,.25f,.28f),

 M(P_PATH_SCALE,"path scale",SEC_GEN1,CURVE_LIN,1,0, .15f,1, .8f,.5f,1,1),
 M(P_PATH_ROT,"path rotation",SEC_GEN1,CURVE_LIN,1,0, 0,1, 0,0,1,0),
 M(P_PATH_X,"path x",SEC_GEN1,CURVE_LIN,1,0, 0,1, .5f,.35f,.65f,1),
 M(P_PATH_Y,"path y",SEC_GEN1,CURVE_LIN,1,0, 0,1, .5f,.35f,.65f,1),

 M(P_TEMPO,"tempo",SEC_METER,CURVE_LIN,0,0, 10,180, .5294118f,.2058824f,.9117647f,1),   /* 10..180 BPM (default 100; Wild rolls 45..165) */
 M(P_RH_ENGINE,"rhythm engine",SEC_METER,CURVE_DISC,0,0, 0,NUM_RH_ENGINES, 0,0,1,0),
 M(P_RH_DENS,"density",SEC_METER,CURVE_LIN,1,0, 0,1, .5f,.12f,.7f,1),
 M(P_RH_THREES,"threes",SEC_METER,CURVE_LIN,1,0, 0,1, 0,0,1,0),
 M(P_RH_FINE,"fine",SEC_METER,CURVE_LIN,1,0, 0,1, .55f,.3f,.85f,0),
 M(P_RH_SWING,"swing",SEC_METER,CURVE_LIN,1,0, 0,1, 0,0,1,0),
 M(P_RH_TURN,"turn",SEC_METER,CURVE_LIN,1,0, 0,1, 0,0,1,0),
 M(P_RH_FORM,"form",SEC_METER,CURVE_LIN,1,0, 0,1, .6f,.3f,1,0),
 M(P_RH_MOTION,"motion",SEC_METER,CURVE_LIN,1,0, 0,1, .2f,0,.6f,0),
 M(P_RH_ASSIGN,"assign",SEC_METER,CURVE_DISC,0,0, 0,NUM_RH_ASSIGN, 0,0,1,0),
 M(P_RH_SELRATE,"selector rate",SEC_METER,CURVE_DISC,0,0, 0,NUM_SELRATES, .3f,0,1,0),
 M(P_RH_FEED,"feedback",SEC_METER,CURVE_LIN,1,0, 0,1, .4f,.2f,.7f,0),
 M(P_RH_COUNT,"count",SEC_METER,CURVE_DISC,0,0, 0,7, .36f,.15f,.7f,0),
 M(P_RH_PULL,"pull",SEC_METER,CURVE_LIN,1,0, 0,1, .6f,.2f,1,0),
 M(P_RH_GRIP,"grip",SEC_METER,CURVE_LIN,1,0, 0,1, .5f,.2f,.9f,0),
 M(P_EVOLVE,"evolve",SEC_METER,CURVE_LIN,1,0, 0,1, .15f,0,.7f,0),
 M(P_DR_ON,"drums",SEC_DRUMS,CURVE_DISC,0,0, 0,2, .25f,0,1,0),
 M(P_DR_LAYER,"drum layer",SEC_DRUMS,CURVE_DISC,1,0, 0,NUM_DLAYERS, .84f,0,1,0),
 M(P_DR_LEVEL,"drum level",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .7f,.55f,.85f,0),
 M(P_DR_ACCENT,"accent",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .6f,.3f,.9f,0),
 M(P_DR_SHAPE,"shape by",SEC_DRUMS,CURVE_DISC,0,0, 0,NUM_DSHAPES, .17f,0,1,0),
 M(P_DR_DEPTH,"depth",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .6f,.3f,1,0),
 M(P_DR_DUCK,"duck",SEC_DRUMS,CURVE_LIN,1,0, 0,1, 0.f,0,.9f,0),
/* a drum's block: its triggers (density, place, follow), the note it rides on the drum layer (voice: 0 the lowest
   sounding .. 1 the highest), its envelope (attack, hold, decay, curve), its level, its character on the voice
   (bright, settle, snap). Every one moddable and rolled */
#define DRUM_P(D,dn,pl,vo,at,ho,dc,cv,lv,br,se,sn) \
 M(D##_DENS,"density",SEC_DRUMS,CURVE_LIN,1,0, 0,1, dn,.2f,.8f,0), \
 M(D##_PLACE,"place",SEC_DRUMS,CURVE_LIN,1,0, 0,1, pl,pl - .08f < 0 ? 0 : pl - .08f,pl + .08f,0), \
 M(D##_SRC,"follow",SEC_DRUMS,CURVE_DISC,0,0, 0,NUM_DSRC, .1f,0,1,0), \
 M(D##_VOICE,"voice",SEC_DRUMS,CURVE_LIN,1,0, 0,1, vo,vo - .15f < 0 ? 0 : vo - .15f,vo + .15f > 1 ? 1 : vo + .15f,0), \
 M(D##_ATTACK,"attack",SEC_DRUMS,CURVE_LOG,1,0, .001f,.02f, at,0,.5f,0), \
 M(D##_HOLD,"hold",SEC_DRUMS,CURVE_LIN,1,0, 0,.05f, ho,0,.3f,0), \
 M(D##_DECAY,"decay",SEC_DRUMS,CURVE_LOG,1,0, .02f,3, dc,dc - .12f,dc + .1f,1), \
 M(D##_CURVE,"curve",SEC_DRUMS,CURVE_LIN,1,0, 0,1, cv,0,.6f,0), \
 M(D##_LEVEL,"level",SEC_DRUMS,CURVE_LIN,1,0, 0,1, lv,lv,lv,0), \
 M(D##_BRIGHT,"bright",SEC_DRUMS,CURVE_LIN,1,0, -1,1, .5f + .3f * br,.15f,.85f,0), \
 M(D##_SETTLE,"settle",SEC_DRUMS,CURVE_LIN,1,0, 0,1, se,.3f,.9f,0), \
 M(D##_SNAP,"snap",SEC_DRUMS,CURVE_LIN,1,0, 0,1, sn,0,.8f,0)
 DRUM_P(P_D1,0.55f,0,0,0.135f,0.1f,0.621f,0.2f,1,0.3f,0.5f,0.4f),
 DRUM_P(P_D2,0.5f,0.42f,0.6f,0,0.04f,0.46f,0.1f,0.4f,0.6f,0.7f,0.7f),
 DRUM_P(P_D3,0.6f,0.8f,1,0,0,0.25f,0.3f,0.22f,0.7f,0.85f,0.3f),
 DRUM_P(P_D4,0.45f,0.6f,0.3f,0.135f,0.08f,0.571f,0.2f,0.5f,0.4f,0.5f,0.4f),

#define LFO_P(n,R,S,Y) \
 M(R,"lfo" #n " rate",SEC_LFO,CURVE_LOG,1,0, .01f,12, .4f,.1f,.75f,1), \
 M(S,"lfo" #n " shape",SEC_LFO,CURVE_DISC,0,0, 0,9, 0,0,1,0), \
 M(Y,"lfo" #n " sync",SEC_LFO,CURVE_DISC,0,0, 0,2, 0,0,1,0)
 LFO_P(1,P_LFO1_RATE,P_LFO1_SHAPE,P_LFO1_SYNC),
 LFO_P(2,P_LFO2_RATE,P_LFO2_SHAPE,P_LFO2_SYNC),
 LFO_P(3,P_LFO3_RATE,P_LFO3_SHAPE,P_LFO3_SYNC),
 LFO_P(4,P_LFO4_RATE,P_LFO4_SHAPE,P_LFO4_SYNC),
 LFO_P(5,P_LFO5_RATE,P_LFO5_SHAPE,P_LFO5_SYNC),
 LFO_P(6,P_LFO6_RATE,P_LFO6_SHAPE,P_LFO6_SYNC),
 M(P_LFO_COUPLE,"lfo couple",SEC_LFO,CURVE_LIN,1,0, 0,2, 0,0,.6f,0),

 M(P_FZ_SUSTAIN,"fuzz sustain",SEC_FX,CURVE_LIN,1,0, 0,1, .3f,0,.7f,0),
 M(P_FZ_TONE,"fuzz tone",SEC_FX,CURVE_LIN,1,0, 0,1, .45f,.25f,.7f,0),
 M(P_FZ_MIX,"fuzz mix",SEC_FX,CURVE_LIN,1,0, 0,1, 0,0,.7f,0),
 M(P_FLT_CUT,"cutoff",SEC_FX,CURVE_LOG,1,0, 60,18000, .9f,.55f,1,1),
 M(P_FLT_RES,"resonance",SEC_FX,CURVE_LIN,1,0, 0,.95f, .1f,0,.55f,0),
 M(P_FLT_MORPH,"morph",SEC_FX,CURVE_LIN,1,0, 0,1, 0,0,.4f,0),
 M(P_DL_SEND,"delay send",SEC_FX,CURVE_LIN,1,0, 0,1, .25f,0,.55f,0),
 M(P_DL_TIME,"delay time",SEC_FX,CURVE_LIN,1,0, 0,1, .5f,0,1,0),
 M(P_DL_FB,"delay fb",SEC_FX,CURVE_LIN,1,0, 0,1.1f, .4f,.15f,.7f,1),
 M(P_DL_TONE,"delay tone",SEC_FX,CURVE_LIN,1,0, 0,1, .5f,.2f,.8f,1),
 M(P_DL_TAP,"delay tap",SEC_FX,CURVE_DISC,0,0, 0,5, 0,0,1,0),
 M(P_RV_SEND,"reverb mix",SEC_FX,CURVE_LIN,1,0, 0,1, .35f,.15f,.8f,1),
 M(P_RV_SIZE,"reverb size",SEC_FX,CURVE_LIN,1,0, .3f,1, .7f,.3f,1,1),
 M(P_RV_DECAY,"reverb decay",SEC_FX,CURVE_LOG,1,0, .3f,60, .5f,.25f,.85f,1),
 M(P_RV_DAMP,"reverb damp",SEC_FX,CURVE_LIN,1,0, 0,1, .55f,.35f,.8f,1),
 M(P_RV_PRE,"predelay",SEC_FX,CURVE_LIN,1,0, 0,.1f, .2f,0,.6f,0),
 M(P_RV_MOD,"reverb mod",SEC_FX,CURVE_LIN,1,0, 0,1, .4f,.1f,.8f,1),
 M(P_RV_TUNED,"reverb tuned",SEC_FX,CURVE_DISC,0,0, 0,2, 0,0,.6f,0),
 M(P_RV_FREEZE,"freeze",SEC_FX,CURVE_DISC,0,0, 0,2, 0,0,0,0),
 M(P_DL_TO_RV,"delay->reverb",SEC_FX,CURVE_LIN,1,0, 0,1, .2f,0,.6f,0),
 M(P_CMP_THRESH,"comp thresh",SEC_FX,CURVE_LIN,1,0, -36,0, .6f,.4f,.8f,1),
 M(P_CMP_RATIO,"comp ratio",SEC_FX,CURVE_LOG,1,0, 1,20, .35f,.15f,.6f,1),
 M(P_CMP_ATT,"comp attack",SEC_FX,CURVE_LOG,1,0, .5f,80, .4f,.2f,.7f,1),
 M(P_CMP_REL,"comp release",SEC_FX,CURVE_LOG,1,0, 20,800, .5f,.3f,.7f,1),
 M(P_CMP_MIX,"comp mix",SEC_FX,CURVE_LIN,1,0, 0,1, 1,.75f,1,0),
 M(P_CMP_PUMP,"pump",SEC_FX,CURVE_LIN,1,0, 0,1, 0,0,.45f,0),
 M(P_MASTER,"master",SEC_FX,CURVE_LIN,1,0, 0,1.5f, .66f,.66f,.66f,0),
/* the patterns: off (linear) unless a style rolls them; a line's count is the library's (params_init_meta) */
#define PAT_P(n) M(P_D##n##_PAT,"pattern",SEC_DRUMS,CURVE_DISC,1,0, 0,2, 0,0,0,0)
 PAT_P(1), PAT_P(2), PAT_P(3), PAT_P(4),
#define ROT_P(n) M(P_D##n##_ROT,"rotate",SEC_DRUMS,CURVE_DISC,1,0, 0,16, 0,0,0,0)
 ROT_P(1), ROT_P(2), ROT_P(3), ROT_P(4),
#define LEN_P(n) M(P_D##n##_LEN,"length",SEC_DRUMS,CURVE_DISC,1,0, 0,32, 0,0,0,0)
 LEN_P(1), LEN_P(2), LEN_P(3), LEN_P(4),
#define MUTE_P(n) M(P_D##n##_MUTE,"mutes",SEC_DRUMS,CURVE_DISC,0,0, 0,NUM_PMUTE, 0,0,0,0)
 MUTE_P(1), MUTE_P(2), MUTE_P(3), MUTE_P(4),
 M(P_RH_GHOST,"ghosts",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .3f,.1f,.6f,0),
 M(P_RH_FILL,"fills",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .3f,.1f,.5f,0),
 M(P_RH_SWLEV,"swing on",SEC_METER,CURVE_DISC,0,0, 0,2, .75f,0,1,0),
 M(P_KEY_PULL,"keys pull",SEC_GEN1,CURVE_DISC,0,0, 0,2, .75f,0,1,0),
 /* the voices' colour: at their defaults (the sound before them) and not rolled, until the rolls take them */
 M(P_V1_ODDEVEN,"odd/even",SEC_VOICE1,CURVE_LIN,1,0, 0,1, .5f,0,1,0), M(P_V2_ODDEVEN,"odd/even",SEC_VOICE2,CURVE_LIN,1,0, 0,1, .5f,0,1,0),
 M(P_V1_PLUCK,"pluck",SEC_VOICE1,CURVE_LIN,1,0, 0,1, 0,0,.8f,0), M(P_V2_PLUCK,"pluck",SEC_VOICE2,CURVE_LIN,1,0, 0,1, 0,0,.8f,0),
 M(P_D1_OE,"odd/even",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .5f,.2f,.8f,0), M(P_D2_OE,"odd/even",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .5f,.2f,.8f,0),
 M(P_D3_OE,"odd/even",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .5f,.2f,.8f,0), M(P_D4_OE,"odd/even",SEC_DRUMS,CURVE_LIN,1,0, 0,1, .5f,.2f,.8f,0),
 M(P_D1_HPLUCK,"pluck",SEC_DRUMS,CURVE_LIN,1,0, 0,1, 0,0,.7f,0), M(P_D2_HPLUCK,"pluck",SEC_DRUMS,CURVE_LIN,1,0, 0,1, 0,0,.7f,0),
 M(P_D3_HPLUCK,"pluck",SEC_DRUMS,CURVE_LIN,1,0, 0,1, 0,0,.7f,0), M(P_D4_HPLUCK,"pluck",SEC_DRUMS,CURVE_LIN,1,0, 0,1, 0,0,.7f,0),
 M(P_V1_DEPTH,"depth",SEC_VOICE1,CURVE_LIN,1,0, 0,1, .3f,0,1,0), M(P_V2_DEPTH,"depth",SEC_VOICE2,CURVE_LIN,1,0, 0,1, .3f,0,1,0),
 /* (Clarity is structural: it takes effect at a rebuild, under its duck, never glided: a glide would switch partials
    tick by tick, and work the mask out each time) */
 M(P_V1_CLARITY,"clarity",SEC_VOICE1,CURVE_LIN,0,1, 0,1, .5f,0,1,0), M(P_V2_CLARITY,"clarity",SEC_VOICE2,CURVE_LIN,0,1, 0,1, .5f,0,1,0),
 M(P_G1_BAL,"balance",SEC_GEN1,CURVE_LIN,1,0, -1,1, .5f,0,1,0), M(P_G2_BAL,"balance",SEC_GEN2,CURVE_LIN,1,0, -1,1, .5f,0,1,0),
 M(P_DRY,"dry",SEC_FX,CURVE_DISC,0,0, 0,2, 0,0,0,0),
 M(P_DR_NOTES,"drum notes",SEC_DRUMS,CURVE_LIN,1,0, 0,1, 0,0,0,0),
#undef PAT_P
#undef ROT_P
#undef LEN_P
#undef MUTE_P
};

void params_init_meta(void) {
    for (int i = 0; i <= 1024; i++) g_sin_tab[i] = sinf(TWO_PI * (float)i / 1024.f);
    g_param_meta[P_SCALE].hi = (float)scale_count();
    for (int d = 0; d < NUM_DRUMS; d++) g_param_meta[P_D1_PAT + d].hi = (float)PAT_CNT[d] + 1.f;   /* linear, then the lines */
}

/* ---------------- value mapping (cached: pval / pint read val / ival) ---------------- */
DSP_CTL void param_cache(Synth *s, int id) {
    const ParamMeta *m = &g_param_meta[id];
    float v = s->mod[id]; if (v < 0) v = 0; if (v > 1) v = 1;
    s->last_mod[id] = s->mod[id];
    if (m->curve == CURVE_DISC) {
        int n = (int)m->hi, i = (int)(v * (float)n); if (i >= n) i = n - 1; if (i < 0) i = 0;
        s->ival[id] = i; s->val[id] = (float)i;
    } else {
        s->val[id] = m->curve == CURVE_LOG ? m->lo * fexp2(v * flog2(m->hi / m->lo)) : m->lo + v * (m->hi - m->lo);
        s->ival[id] = (int32_t)s->val[id];
    }
}
void params_cache_all(Synth *s) { for (int i = 0; i < NUM_PARAMS; i++) DSP_FAR(param_cache)(s, i); }

/* ---------------- randomizer ---------------- */
static float roll_one(const ParamMeta *m, uint32_t *r) {
    float u = m->tri ? 0.5f * (frand(r) + frand(r)) : frand(r);
    if (m->curve == CURVE_DISC) {
        int n = (int)m->hi; int lo = (int)(m->rlo * n), hi = (int)(m->rhi * n - 1e-4f);
        if (hi < lo) hi = lo;
        int i = lo + (int)(frand(r) * (float)(hi - lo + 1)); if (i > hi) i = hi;
        return ((float)i + 0.5f) / (float)n;
    }
    return m->rlo + u * (m->rhi - m->rlo);
}
static float disc(int id, int i) { return ((float)i + 0.5f) / g_param_meta[id].hi; }

/* a route that would feed a layer back into itself: its own motion's envelope (or layer 1's rises, or the path) moving
   its own motion or path. It oscillates at the control rate (hundreds of notes a second): never rolled */
static int mod_loop(int src, int dst) {
    int g1 = (dst >= P_G1_FUNC && dst <= P_G1_ROUTE) || (dst >= P_PATH_SCALE && dst <= P_PATH_Y) || dst == P_KEY_PULL || dst == P_G1_BAL;
    int g2 = (dst >= P_G2_FUNC && dst <= P_G2_ROUTE) || dst == P_G2_BAL;
    return ((src == SRC_MOT1 || src == SRC_TRIG || src == SRC_PATHX || src == SRC_PATHY) && g1) || (src == SRC_MOT2 && g2);
}
static uint8_t g_roll_dry;                    /* the roll makes a Dry patch (1.0.6): no route to the effects */
static uint8_t g_dry_set;                     /* (1.0.6) this roll decided Dry (and so sets the effects to nothing) */
static uint8_t g_fx_wake;                     /* (1.0.6) a sound roll from a Dry patch to a wet one: its effects rolled too */
static uint8_t g_lock_fx;       /* the player's own locks on the effects and the knobs (not the roll's
                                                 mask: a sound roll leaves the effects alone, yet decides Dry) */
static int fx_off(int id) {                   /* an effect a Dry patch has not got (the compressor it has) */
    return g_param_meta[id].section == SEC_FX && !(id >= P_CMP_THRESH && id <= P_CMP_PUMP);
}
static int rand_moddable(uint32_t *r) {
    for (;;) {
        int id = (int)(frand(r) * NUM_PARAMS) % NUM_PARAMS;
        if (g_roll_dry && fx_off(id)) continue;
        if (g_param_meta[id].moddable && id != P_MASTER && id != P_V1_LEVEL && id != P_V2_LEVEL
            && !(id >= P_D1_PAT && id <= P_D4_LEN)) return id;   /* (a pattern moves only where a style puts it) */
    }
}

void params_roll_mod(Synth *s) {
    uint32_t *r = &s->rng;
    if (!s->locked[SEC_LFO]) {
        memset(s->slot, 0, sizeof s->slot);
        int n = 6 + (int)(frand(r) * 9.f);
        for (int i = 0; i < n && i < MOD_SLOTS; i++) {
            float u = frand(r); int src;
            if (u < .70f) src = SRC_LFO1 + (int)(frand(r) * 6) % 6;
            else if (u < .78f) src = SRC_RAND;
            else if (u < .86f) src = SRC_TRIG;
            else if (u < .92f) src = SRC_ENV;
            else if (u < .98f) src = frand(r) < .5f ? SRC_PATHX : SRC_PATHY;
            else src = SRC_VEL;
            s->slot[i].src = (uint8_t)src;
            for (int t = 0; t < 8; t++) { s->slot[i].dst = (uint8_t)rand_moddable(r); if (!mod_loop(src, s->slot[i].dst)) break; }
            if (mod_loop(src, s->slot[i].dst)) { s->slot[i].src = 0; continue; }
            float d = 0.08f + 0.55f * 0.5f * (frand(r) + frand(r));   /* (1.0.5: deeper) */
            s->slot[i].depth = frand(r) < .5f ? -d : d;
        }
        /* the generators on the drums: with the drums on, often one or two routes from a layer's motion to a drum's
           envelope or character (Motion 2 -> the hats' decay: they open as layer 2 swells) */
        if (s->target[P_DR_ON] > .5f && frand(r) < .6f) {
            static const uint8_t DD[] = {P_D1_DECAY, P_D1_BRIGHT, P_D1_SNAP, P_D1_CURVE, P_D1_ATTACK, P_D1_HOLD, P_D1_LEVEL, P_D1_SETTLE};
            int k = n < MOD_SLOTS ? n : MOD_SLOTS - 2, m = 1 + (frand(r) < .4f);
            for (int j = 0; j < m && k + j < MOD_SLOTS; j++) {
                ModSlot *ms = &s->slot[k + j];
                ms->src = (uint8_t)(frand(r) < .5f ? SRC_MOT1 : SRC_MOT2);
                ms->dst = (uint8_t)DP((int)(frand(r) * 4.f) & 3, DD[(int)(frand(r) * 8.f) & 7]);
                float d = .1f + .25f * frand(r); ms->depth = frand(r) < .7f ? d : -d;
            }
        }
    }
}

/* the knobs' paths: most parameters of each knob's part ride it (four in five), each with its own way to go: how far
   at the ends (either direction, in proportion to the parameter's own musical range) and somewhere else on the way
   (the half-way point may overshoot the end or lean the other way), so the turn is not one straight morph */
void params_roll_knobs(Synth *s) { params_roll_knobs_part(s, 0, NUM_PARAMS); }
void params_roll_knobs_part(Synth *s, int i0, int i1) {    /* (the knobs' paths of parameters i0..i1-1; the macros at the end) */
    uint32_t *r = &s->rng;
    for (int i = i0; i < i1; i++) {
        int8_t *w = s->kpath[i];
        w[0] = w[1] = w[2] = w[3] = 0;
        /* the ones a player reaches for are always on their knob: the filter, the reverb and delay, the bank's centre,
           band and tilt, the voices' Depth and the notes' Balance (the low end: Timbre), the rhythm's density and swing, the motions' rates */
        int must = i == P_FLT_CUT || i == P_FLT_RES || i == P_RV_SEND || i == P_RV_DECAY || i == P_RV_SIZE || i == P_RV_DAMP || i == P_DL_SEND || i == P_DL_FB
                || i == P_V1_TONE || i == P_V2_TONE || i == P_V1_CENTER || i == P_V1_BAND || i == P_V1_TILT || i == P_V2_CENTER || i == P_RH_DENS || i == P_RH_SWING || i == P_G1_RATE || i == P_G1_REACH
                || i == P_V1_DEPTH || i == P_V2_DEPTH || i == P_G1_BAL || i == P_G2_BAL
                || i == P_G1_CONTOUR || i == P_G2_CONTOUR || i == P_V1_WIDTH || i == P_V2_WIDTH || i == P_CMP_PUMP
                || i == P_DR_NOTES;   /* (1.0.6: Shape's and Pump, on a Dry patch) */
        if (knob_of_dry(i, s->target[P_DRY] > .5f) < 0 || (frand(r) > .8f && !must) || (i >= P_D1_PAT && i <= P_D4_LEN)) continue;   /* (patterns: roll_patterns') */
        const ParamMeta *m = &g_param_meta[i];
        float span = m->rhi - m->rlo + .15f; if (span < .45f) span = .45f; if (span > 1.f) span = 1.f;
        float dir = frand(r) < .5f ? -1.f : 1.f;              /* the two ends go opposite ways, mostly: a turn is a journey */
        for (int side = 0; side < 2; side++) {
            float far = (.45f + .55f * frand(r)) * span * (frand(r) < .8f ? (side ? dir : -dir) : (side ? -dir : dir));
            if (i == P_FLT_CUT) far = side ? -(.3f + .3f * frand(r)) : .1f;   /* (the filter closes one way only, never fully) */
            float near = far * (-.3f + 1.5f * frand(r));
            if (i == P_DL_FB) { if (far > .15f) far = .15f; if (near > .15f) near = .15f; }   /* (never into runaway) */
            w[side ? 3 : 0] = (int8_t)(fclamp(far, -1.f, 1.f) * 127.f);
            w[side ? 2 : 1] = (int8_t)(fclamp(near, -1.f, 1.f) * 127.f);
        }
    }
    if (i1 >= NUM_PARAMS) for (int m = 0; m < NUM_MACRO; m++) s->macro[m] = 0.f;
}

/* ---------------- curated styles ---------------- */
static float to_norm(int id, float real) {
    const ParamMeta *m = &g_param_meta[id];
    if (m->curve == CURVE_LOG) return flog2(real / m->lo) / flog2(m->hi / m->lo);
    return (real - m->lo) / (m->hi - m->lo);
}
static void TR(Synth *s, int id, float lo, float hi) {        /* roll a real-unit range */
    float u = frand(&s->rng), v;
    if (g_param_meta[id].curve == CURVE_LOG) v = lo * fexp2(u * flog2(hi / lo)); else v = lo + u * (hi - lo);
    float nv = to_norm(id, v); s->target[id] = nv < 0 ? 0 : nv > 1 ? 1 : nv;
}
static void TD(Synth *s, int id, int idx) { s->target[id] = ((float)idx + .5f) / g_param_meta[id].hi; }
static int pickw(uint32_t *r, const float *w, int n) {        /* weighted choice */
    float t = 0; for (int i = 0; i < n; i++) t += w[i];
    float u = frand(r) * t; for (int i = 0; i < n; i++) { u -= w[i]; if (u <= 0) return w[i] > 0 ? i : 0; }
    return n - 1;
}
static void TP(Synth *s, int id, const float *w) { TD(s, id, pickw(&s->rng, w, (int)g_param_meta[id].hi)); }


/* ---------------- the patterns' roll: a decision tree trained on the corpus (patterns.c) ----------------
   The tree: an entry of the genre by its weight (the root); then each role in turn, kick, snare, hat, the fourth part,
   from an entry drawn by its weight times a kernel exp(-D / 4) of how far its earlier roles are from the ones already
   chosen (D: the Hamming distance over two bars, a ghost half a hit): the conditional distribution the corpus implies,
   P(snare | kick), P(hat | kick, snare) and so on, smoothed so that combinations the corpus never has, but that keep
   its rules, come up. Then now and then a step to a library neighbour (a close variation). Tempo comes from the root,
   swing from the hats' entry (swing belongs to the hats). */
static int pat_d(int d, int a, int b) {                   /* two lines' distance, x2 (0: linear, far from any line) */
    if (!a || !b) return a == b ? 0 : 16;
    const PatLine *A = &PAT_LIB[PAT_OFF[d] + a - 1], *B = &PAT_LIB[PAT_OFF[d] + b - 1];
    uint32_t ha = A->hit, hb = B->hit, ga = A->ghost, gb = B->ghost;
    if (A->len == 16) { ha |= ha << 16; ga |= ga << 16; }
    if (B->len == 16) { hb |= hb << 16; gb |= gb << 16; }
    return 2 * __builtin_popcount(ha ^ hb) + __builtin_popcount(ga ^ gb);
}
/* (1.0.8) a patch saved by 1.0.5..1.0.7: its pattern (a place in the old library, normalized as that library's count
   + 1 values) to the same line's place in this one; its knob paths' and routes' steps, in lines, kept as near as
   their 1/127ths allow */
float pat_v107(int d, float v) {
    int n0 = PAT_V107_CNT[d], i = (int)(v * (float)(n0 + 1));
    if (i < 1) return .5f / (float)(PAT_CNT[d] + 1);          /* (linear) */
    if (i > n0) i = n0;
    return ((float)PAT_V107[PAT_V107_OFF[d] + i - 1] + .5f) / (float)(PAT_CNT[d] + 1);
}
float pat_v107_scale(int d) { return (float)(PAT_V107_CNT[d] + 1) / (float)(PAT_CNT[d] + 1); }

/* a genre's entries: [lo, hi) (breakbeat's tree holds its drum'n'bass entries too: the two are next to each other) */
static void pat_range(int genre, int *lo, int *hi) {
    *lo = PAT_GOFF[genre]; *hi = PAT_GOFF[genre == PG_BREAKS ? PG_DNB + 1 : genre + 1];
}
/* (1.0.8) fam: the root's family: an entry of the same era or sub-style 25 times likelier, so the parts that follow mostly
   keep to it (a son montuno's clave over a son montuno's bass, not a bossa nova's), with the rest of the genre still in reach */
static float pat_w(int e, int genre, int dnb, int upto, const int *chosen, int fam) {
    float x = (float)PAT_ENT[e].w * (genre == PG_BREAKS ? (PAT_ENT[e].genre == PG_DNB ? (dnb ? 1.f : .02f) : (dnb ? .02f : 1.f)) : 1.f);
    if (fam && PAT_ENT[e].fam == fam) x *= 25.f;
    int D = 0;
    for (int r = 0; r < upto; r++) D += pat_d(r, PAT_ENT[e].line[r], chosen[r]);
    return D ? x * fexp2(-(float)D * .5f / 4.f * 1.4427f) : x;
}
/* (1.0.8: the genre's entries only, a hundred or so of the corpus's 678, and no table of weights: the sum, then the
   draw walks them again. The roll runs in the audio's interrupt, a part a block) */
static int pat_pick(Synth *s, int genre, int dnb, int upto, const int *chosen, int fam) {
    int lo, hi; float tot = 0.f;
    pat_range(genre, &lo, &hi);
    for (int e = lo; e < hi; e++) tot += pat_w(e, genre, dnb, upto, chosen, fam);
    float u = frand(&s->rng) * tot;
    for (int e = lo; e < hi; e++) { float w = pat_w(e, genre, dnb, upto, chosen, fam); u -= w; if (u <= 0.f && w > 0.f) return e; }
    return hi > lo ? hi - 1 : 0;
}
/* a close variation: another line of the genre's own corpus within three hits of this one (nearer: likelier) */
static int pat_near(Synth *s, int d, int li, int genre) {
    float tot = 0.f; int pass, gs[2] = {genre, genre == PG_BREAKS ? PG_DNB : -1};
    float u = 0.f;
    for (pass = 0; pass < 2; pass++) {
        if (pass) { if (tot <= 0.f) return li; u = frand(&s->rng) * tot; }
        for (int q = 0; q < 2; q++) {
            if (gs[q] < 0) continue;
            int a = PAT_UOFF[gs[q] * NUM_DRUMS + d], b = PAT_UOFF[gs[q] * NUM_DRUMS + d + 1];
            for (int j = a; j < b; j++) {
                int k = PAT_USE[j], D = k == li ? 99 : pat_d(d, li, k);
                if (D > 6) continue;
                float w = fexp2(-(float)D * .25f * 1.4427f);
                if (!pass) tot += w; else { u -= w; if (u <= 0.f) return k; }
            }
        }
    }
    return li;
}
static void roll_patterns(Synth *s, int genre, int meter, int drums) {
    uint32_t *r = &s->rng; float *t = s->target;
    int dnb = genre == PG_BREAKS && frand(r) < .3f;       /* breakbeat: a third of the rolls at drum'n'bass tempo */
    int ch[NUM_DRUMS] = {0, 0, 0, 0}, root = pat_pick(s, genre, dnb, 0, ch, 0), hat_e = root, fam = PAT_ENT[root].fam;
    for (int d = 0; d < NUM_DRUMS; d++) {
        int e = d == 0 && frand(r) < .5f ? root : pat_pick(s, genre, dnb, d, ch, fam);
        int li = PAT_ENT[e].line[d];
        if (li && frand(r) < (d == 0 ? .15f : .2f)) li = pat_near(s, d, li, genre);   /* a close variation */
        if (d == 3 && li && frand(r) < .2f) li = 0;        /* (the fourth part sometimes left to the stream) */
        ch[d] = li;
        if (d == 2) hat_e = e;
    }
    const PatEntry *E0 = &PAT_ENT[root], *EH = &PAT_ENT[hat_e];
    if (meter) {
        if (genre == PG_BREAKS && E0->genre != PG_DNB) TR(s, P_TEMPO, 124.f, 140.f);   /* (the breaks at breakbeat tempo) */
        else TR(s, P_TEMPO, (float)E0->bpm_lo - 2.f, (float)E0->bpm_hi + 2.f);
        float sw = ((float)EH->sw_lo + frand(r) * (float)(EH->sw_hi - EH->sw_lo)) * .01f;
        t[P_RH_SWING] = sw;
        TD(s, P_RH_SWLEV, EH->lev == 8 ? 0 : 1);
    }
    if (drums) {
        for (int d = 0; d < NUM_DRUMS; d++) {
            TD(s, P_D1_PAT + d, ch[d]);
            TD(s, P_D1_ROT + d, 0); TD(s, P_D1_LEN + d, 0); TD(s, P_D1_MUTE + d, PMUTE_NONE);
            if (ch[d] && t[DP(d, P_D1_DENS)] < .3f) TR(s, DP(d, P_D1_DENS), .35f, .7f);
        }
        if (!ch[1] && genre == PG_HOUSE) TR(s, P_D2_DENS, .02f, .15f);   /* (techno without a snare: the stream's only rarely) */
        /* the fourth part's twists: rotated, or looped against the bar (polymeter), now and then */
        if (ch[3]) {
            float u = frand(r);
            if (u < .18f) TD(s, P_D4_ROT, 2 * (1 + (int)(frand(r) * 6.f)));
            else if (u < .3f) { static const int L[4] = {12, 10, 6, 14}; TD(s, P_D4_LEN, L[(int)(frand(r) * 4.f) & 3] - 1); }
        }
        if (genre == PG_HOUSE && ch[2] && frand(r) < .06f) TD(s, P_D3_LEN, 11);   /* (a 12-step hat: rarely) */
        /* the mutes: house, the fourth part (an open hat) chokes the hat; the breaks, the kick clears the hat; the snare
           sometimes clears the linear layer under it */
        if (genre == PG_HOUSE) { if (ch[3] && frand(r) < .4f) TD(s, P_D4_MUTE, PMUTE_HAT); if (frand(r) < .12f) TD(s, P_D1_MUTE, PMUTE_HAT); }
        else { if (frand(r) < .25f) TD(s, P_D1_MUTE, PMUTE_HAT); if (frand(r) < .2f) TD(s, P_D2_MUTE, PMUTE_LINEAR); }
        /* ghosts and fills */
        if (genre == PG_HOUSE) { if (frand(r) < .3f) t[P_RH_GHOST] = 0.f; else TR(s, P_RH_GHOST, .05f, .35f); }
        else if (genre == PG_HIPHOP) TR(s, P_RH_GHOST, .25f, .65f);
        else if (genre == PG_FUNK) TR(s, P_RH_GHOST, .45f, .85f);    /* (1.0.8: funk lives in its ghosts) */
        else if (genre == PG_LATIN) TR(s, P_RH_GHOST, .2f, .5f);
        else if (genre == PG_INDIE) TR(s, P_RH_GHOST, .05f, .3f);
        else TR(s, P_RH_GHOST, .35f, .8f);
        TR(s, P_RH_FILL, .1f, .6f);
        /* the knobs: Groove moves each line to its busier or sparser relatives (a few lines along the library), Motion
           turns the hat's and the fourth part's rotation (by 8ths) */
        if (!s->locked[SEC_KNOBS]) for (int d = 0; d < NUM_DRUMS; d++) {
            int8_t *w = s->kpath[P_D1_PAT + d], *q = s->kpath[P_D1_ROT + d];
            w[0] = w[1] = w[2] = w[3] = q[0] = q[1] = q[2] = q[3] = 0;
            if (!ch[d]) continue;
            float span = 127.f / (float)(PAT_CNT[d] + 1), far = (2.f + 3.f * frand(r)) * span;
            if (far > 127.f) far = 127.f;
            w[3] = (int8_t)far; w[2] = (int8_t)(far * .45f); w[0] = (int8_t)-far; w[1] = (int8_t)(-far * .45f);
            if (d >= 2 && frand(r) < .5f) {
                float st = 2.f * 127.f / 16.f * (1.f + (float)(int)(frand(r) * 2.f));
                q[3] = (int8_t)st; q[2] = (int8_t)(st * .5f); q[0] = (int8_t)-st; q[1] = (int8_t)(-st * .5f);
            }
        }
    }
}

/* what every roll does, whatever the style: choices that only make sense together are made together */
static void roll_together(Synth *s) {
    uint32_t *r = &s->rng; float *t = s->target;
    for (int l = 0; l < NUM_LAYERS; l++) if (!s->locked[l ? SEC_VOICE2 : SEC_VOICE1]) {
        /* Stride and Cross are strong colours: off in half the rolls. A high, narrow band has no body: widen it */
        if (frand(r) < .55f) t[LP(l, P_V1_STRIDE)] = 0.f;
        else if (frand(r) < .5f) t[LP(l, P_V1_STRIDE)] = .333f;         /* the odd partials */
        float c = t[LP(l, P_V1_CENTER)], need = .25f + .8f * c;
        if (t[LP(l, P_V1_BAND)] < need) t[LP(l, P_V1_BAND)] = need > 1.f ? 1.f : need;
        if (c > .3f && t[LP(l, P_V1_REG)] > .67f) t[LP(l, P_V1_REG)] = .5f;   /* (bright and an octave up: thin) */
    }
    if (!s->locked[SEC_METER]) {
        float u = frand(r);
        t[P_RH_THREES] = u < .55f ? 0.f : u < .7f ? 1.f : .35f + .3f * frand(r);
        t[P_RH_SWING] = (t[P_RH_THREES] < .2f && frand(r) < .4f) ? .4f + .6f * frand(r) : 0.f;
        t[P_RH_ENGINE] = frand(r) < .18f ? .75f : .25f;
        t[P_RH_ASSIGN] = frand(r) < .15f ? .75f : .25f;
        { static const float w[NUM_SELRATES] = {15, 60, 12, 10, 3};
          float tot = 0, x; for (int i = 0; i < NUM_SELRATES; i++) tot += w[i];
          x = frand(r) * tot; int k = 0; while (k < NUM_SELRATES - 1 && (x -= w[k]) > 0) k++;
          t[P_RH_SELRATE] = ((float)k + .5f) / NUM_SELRATES; }
    }
    if (!s->locked[SEC_DRUMS]) for (int d = 0; d < NUM_DRUMS; d++) {
        if (frand(r) < (d >= 2 ? .12f : .04f)) t[DP(d, P_D1_SRC)] = frand(r) < .6f ? .83f : .5f; else t[DP(d, P_D1_SRC)] = .1f;
    }
    if (!s->locked[SEC_FX]) {
        if (frand(r) < .8f) t[P_FZ_MIX] = 0.f; else t[P_FZ_MIX] = .1f + .4f * frand(r);
    }
}

/* ---------------- (1.0.8) the genres' own sounds ----------------
   House, Hip-hop, Breakbeat, Machine, Funk, Latin and Indie start from the rhythmic base (Groove's roll), then each its
   own character: the voices (where the spectrum sits, how bright, how it is struck), the scales a player of it reaches
   for, the motions that make its kind of line, and its room and drive. Ranges in each parameter's own units, lo..hi. */
typedef struct {
    float cen[2], band[2], tilt[2], wave[2], phase[2], spin[2], tone[2], bloom[2];   /* the first voice */
    float ppk, pk[2], pstr;                     /* its Pluck: how often, how fast; Stride (the odd partials) how often */
    float v2lv[2], v2bass, v2wave[2], v2tone[2], v2ppk;   /* the second: level, how often an octave down, Wave, Tone, Pluck */
    const char *sc[8]; float scw[8];            /* the scales and how often */
    float gf[NUM_GENS], grate[2];               /* the rhythm layer's motions (orbit ladder swarm lorenz motif drift billiard
                                                   gravity stride loops erode chords) and their speed */
    float pfz, fz[2], cut[2], res[2], rv[2], rvsz[2], rvdc[2], dl[2], dlfb[2], cmp[2], att[2], pump[2];   /* the effects */
} GenreSound;
static const GenreSound GS[7] = {
    {   /* house: warm, round chords and stabs over a deep bass; minor and Dorian; a clean room, the pump */
        {.05f, .4f}, {.35f, .75f}, {.3f, .52f}, {.1f, .5f}, {.2f, .6f}, {.15f, .45f}, {.55f, .76f}, {.5f, .85f},
        .35f, {.25f, .55f}, .2f,  {.45f, .62f}, .5f, {.1f, .4f}, {.45f, .62f}, .25f,
        {"Minor", "Dorian", "Major", "Minor pentatonic", "Mixolydian", "Harmonics 6-12", "Suspended", 0}, {3, 3, 2, 2, 1, 1, 1, 0},
        {3, 2, 0, 0, 3, 0, 2, 1, 3, 2, 0, 5}, {.3f, 1.f},
        .08f, {.1f, .25f}, {9000.f, 18000.f}, {0.f, .15f}, {.18f, .38f}, {.7f, .9f}, {2.f, 5.f}, {.1f, .28f}, {.25f, .45f},
        {2.f, 3.5f}, {6.f, 14.f}, {.2f, .5f} },
    {   /* hip-hop: dusty and mellow, keys through a filtered sampler; a heavy low bass; pentatonic, Dorian, blues */
        {0.f, .3f}, {.25f, .6f}, {.18f, .4f}, {0.f, .3f}, {.3f, .65f}, {.2f, .5f}, {.42f, .62f}, {.6f, .95f},
        .5f, {.2f, .55f}, .15f,  {.5f, .7f}, .7f, {0.f, .35f}, {.4f, .55f}, .2f,
        {"Minor pentatonic", "Dorian", "Minor", "Blues", "Harmonic minor", "Kumoi", "Major", 0}, {3, 3, 3, 2, 1, 1, 1, 0},
        {2, 1, 0, 0, 4, 0, 1, 0, 2, 4, 0, 4}, {.2f, .8f},
        .35f, {.08f, .25f}, {5000.f, 11000.f}, {0.f, .1f}, {.1f, .25f}, {.5f, .8f}, {1.f, 2.5f}, {.05f, .2f}, {.2f, .4f},
        {2.5f, 4.f}, {8.f, 20.f}, {0.f, .15f} },
    {   /* breakbeat: bright and edgy, rave stabs and a growling bass; minor, Phrygian, Hijaz */
        {.2f, .7f}, {.3f, .8f}, {.45f, .7f}, {.4f, 1.f}, {0.f, .3f}, {.1f, .4f}, {.65f, .88f}, {.6f, 1.f},
        .5f, {.3f, .7f}, .2f,  {.45f, .6f}, .6f, {.4f, .85f}, {.5f, .75f}, .3f,
        {"Minor", "Phrygian", "Minor pentatonic", "Harmonic minor", "Dorian", "Hijaz", "Kurd", 0}, {3, 2, 2, 2, 2, 1, 1, 0},
        {2, 3, 2, 1, 3, 0, 3, 0, 3, 2, 0, 2}, {.4f, 1.5f},
        .45f, {.15f, .5f}, {10000.f, 18000.f}, {0.f, .25f}, {.12f, .3f}, {.6f, .85f}, {1.5f, 4.f}, {.12f, .3f}, {.3f, .5f},
        {2.5f, 4.f}, {5.f, 12.f}, {.1f, .35f} },
    {   /* machine (IDM): glassy, metallic, hollow combs and twangs; the series, whole tone, the hexany */
        {.2f, .85f}, {.1f, .5f}, {.4f, .7f}, {0.f, .7f}, {.45f, 1.f}, {.3f, .8f}, {.6f, .9f}, {.3f, .8f},
        .6f, {.35f, .85f}, .65f,  {.4f, .6f}, .4f, {.2f, .7f}, {.5f, .8f}, .45f,
        {"Harmonic series", "Odd harmonics", "Whole tone", "Diminished", "Hexany 1-3-5-9", "Undertones 8-16", "Lydian", "Harmonics 8-16"},
        {2, 2, 2, 2, 2, 2, 1, 1},
        {1, 2, 3, 3, 1, 0, 3, 2, 2, 1, 2, 0}, {.3f, 1.5f},
        .4f, {.1f, .45f}, {9000.f, 18000.f}, {.05f, .3f}, {.12f, .3f}, {.5f, .8f}, {1.f, 3.f}, {.2f, .4f}, {.3f, .55f},
        {2.f, 3.5f}, {6.f, 15.f}, {0.f, .2f} },
    {   /* funk: clavinet and slap bass, bright, buzzy and percussive, loud notes snap open; Dorian, Mixolydian, blues */
        {.1f, .5f}, {.4f, .8f}, {.45f, .65f}, {.45f, .95f}, {0.f, .25f}, {.1f, .35f}, {.65f, .86f}, {.75f, 1.f},
        .8f, {.45f, .8f}, .15f,  {.5f, .68f}, .75f, {.3f, .7f}, {.5f, .7f}, .6f,
        {"Minor pentatonic", "Dorian", "Mixolydian", "Blues", "Minor", "Major pentatonic", 0, 0}, {3, 3, 3, 2, 1, 1, 0, 0},
        {2, 2, 0, 0, 4, 0, 2, 0, 4, 4, 0, 2}, {.5f, 1.5f},
        .25f, {.1f, .3f}, {6000.f, 14000.f}, {.1f, .35f}, {.06f, .18f}, {.4f, .7f}, {.8f, 2.f}, {.05f, .18f}, {.2f, .4f},
        {2.5f, 4.5f}, {4.f, 10.f}, {.05f, .25f} },
    {   /* latin: struck wood and tuned bars (marimba, vibes), a piano's montuno, a round tumbao bass; major, minor,
           harmonic minor, Phrygian dominant; a warm room */
        {0.f, .35f}, {.15f, .5f}, {.25f, .5f}, {0.f, .25f}, {.3f, .6f}, {.15f, .45f}, {.55f, .8f}, {.6f, .9f},
        .85f, {.4f, .75f}, .3f,  {.45f, .6f}, .6f, {0.f, .3f}, {.45f, .65f}, .5f,
        {"Major", "Harmonic minor", "Minor", "Phrygian dominant", "Dorian", "Mixolydian", "Melodic minor", "Major pentatonic"},
        {3, 3, 2, 2, 2, 2, 1, 1},
        {4, 2, 0, 0, 3, 0, 1, 0, 2, 4, 0, 3}, {.4f, 1.2f},
        .05f, {.08f, .2f}, {10000.f, 18000.f}, {0.f, .1f}, {.22f, .42f}, {.75f, .95f}, {2.f, 4.5f}, {.05f, .18f}, {.2f, .4f},
        {1.6f, 2.8f}, {8.f, 20.f}, {0.f, 0.f} },
    {   /* indie: guitars, jangly to fuzzed, strummed and ringing; major, Mixolydian, Aeolian; a big room, often driven */
        {.15f, .55f}, {.4f, .85f}, {.4f, .65f}, {.5f, 1.f}, {.5f, .9f}, {.2f, .55f}, {.6f, .85f}, {.5f, .85f},
        .6f, {.15f, .45f}, .15f,  {.45f, .65f}, .5f, {.3f, .7f}, {.5f, .75f}, .3f,
        {"Major", "Mixolydian", "Minor", "Dorian", "Major pentatonic", "Minor pentatonic", "Suspended", "Lydian"},
        {3, 3, 3, 2, 2, 1, 1, 1},
        {2, 3, 0, 0, 2, 1, 0, 1, 3, 2, 0, 4}, {.3f, 1.2f},
        .6f, {.2f, .6f}, {10000.f, 18000.f}, {0.f, .1f}, {.25f, .5f}, {.8f, 1.f}, {2.5f, 7.f}, {.15f, .35f}, {.3f, .5f},
        {2.f, 3.5f}, {8.f, 20.f}, {0.f, .1f} },
};
static void genre_sound(Synth *s, int gi) {
    const GenreSound *g = &GS[gi]; uint32_t *r = &s->rng;
    if (!s->locked[SEC_SCALE]) {
        float w[8]; int ix[8], n = 0;
        for (int i = 0; i < 8; i++) if (g->sc[i]) { int k = scale_find(g->sc[i]); if (k >= 0) { ix[n] = k; w[n] = g->scw[i]; n++; } }
        if (n) s->target[P_SCALE] = ((float)ix[pickw(r, w, n)] + .5f) / g_param_meta[P_SCALE].hi;
    }
    if (!s->locked[SEC_VOICE1]) {
        TR(s, P_V1_CENTER, g->cen[0], g->cen[1]); TR(s, P_V1_BAND, g->band[0], g->band[1]); TR(s, P_V1_TILT, g->tilt[0], g->tilt[1]);
        TR(s, P_V1_WAVE, g->wave[0], g->wave[1]); TR(s, P_V1_PHASE, g->phase[0], g->phase[1]); TR(s, P_V1_SPIN, g->spin[0], g->spin[1]);
        TR(s, P_V1_TONE, g->tone[0], g->tone[1]); TR(s, P_V1_BLOOM, g->bloom[0], g->bloom[1]);
        if (frand(r) < g->ppk) TR(s, P_V1_PLUCK, g->pk[0], g->pk[1]); else TR(s, P_V1_PLUCK, 0.f, 0.f);
        if (frand(r) < g->pstr) s->target[P_V1_STRIDE] = .333f; else s->target[P_V1_STRIDE] = 0.f;
    }
    if (!s->locked[SEC_VOICE2]) {
        TR(s, P_V2_LEVEL, g->v2lv[0], g->v2lv[1]); TD(s, P_V2_REG, frand(r) < g->v2bass ? 0 : 1);
        TR(s, P_V2_WAVE, g->v2wave[0], g->v2wave[1]); TR(s, P_V2_TONE, g->v2tone[0], g->v2tone[1]);
        if (frand(r) < g->v2ppk) TR(s, P_V2_PLUCK, .25f, .6f); else TR(s, P_V2_PLUCK, 0.f, 0.f);
    }
    if (!s->locked[SEC_GEN1]) {
        if ((int)(s->target[P_G2_ROUTE] * NUM_ROUTES) == ROUTE_PLAY) {   /* (a motion layer 2 is routed into stays) */
            static const float we[NUM_ENVS] = {6, 0, 1, 2, 1, 0, 6, 0};    /* (Groove's envelopes, its points) */
            int kick = (int)(s->target[P_G1_ENV] * NUM_ENVS) == ENV_DRUMS;   /* (Machine's swell with the kick: kept) */
            TP(s, P_G1_FUNC, g->gf); TD(s, P_G1_POINTS, 2 + (int)(frand(r) * 3.f));
            if (!kick) { TP(s, P_G1_ENV, we);
                         if ((int)(s->target[P_G1_ENV] * NUM_ENVS) == ENV_STREAM) TR(s, P_G1_CONTOUR, .1f, .6f); else TR(s, P_G1_CONTOUR, .8f, 1.f); }
            int fn = (int)(s->target[P_G1_FUNC] * NUM_GENS);
            if (fn == GEN_LOOPS || fn == GEN_ERODE) { if (!kick) TD(s, P_G1_ENV, ENV_HOLD); TD(s, P_G1_POINTS, 3 + (int)(frand(r) * 4.f)); }
            if (fn == GEN_CHORDS) { if (!kick) TD(s, P_G1_ENV, frand(r) < .5f ? ENV_HOLD : ENV_PULSE); TD(s, P_G1_POINTS, 2 + (int)(frand(r) * 2.f)); }
            if (fn == GEN_LOOPS) TR(s, P_G1_RATE, 0.f, .3f);
            else if (fn == GEN_CHORDS) TR(s, P_G1_RATE, .1f, .4f);
            else TR(s, P_G1_RATE, g->grate[0], g->grate[1]);
        }
    }
    if (!s->locked[SEC_FX] || g_fx_wake) {
        if (frand(r) < g->pfz) { TR(s, P_FZ_MIX, g->fz[0], g->fz[1]); TR(s, P_FZ_SUSTAIN, .1f, .7f); TR(s, P_FZ_TONE, .25f, .7f); }
        else TR(s, P_FZ_MIX, 0.f, 0.f);
        TR(s, P_FLT_CUT, g->cut[0], g->cut[1]); TR(s, P_FLT_RES, g->res[0], g->res[1]);
        TR(s, P_RV_SEND, g->rv[0], g->rv[1]); TR(s, P_RV_SIZE, g->rvsz[0], g->rvsz[1]); TR(s, P_RV_DECAY, g->rvdc[0], g->rvdc[1]);
        TR(s, P_DL_SEND, g->dl[0], g->dl[1]); TR(s, P_DL_FB, g->dlfb[0], g->dlfb[1]);
        TR(s, P_CMP_RATIO, g->cmp[0], g->cmp[1]); TR(s, P_CMP_ATT, g->att[0], g->att[1]); TR(s, P_CMP_PUMP, g->pump[0], g->pump[1]);
    }
}

static void apply_style(Synth *s) {
    uint32_t *r = &s->rng; int st0 = s->style;
    if (st0 == STYLE_WILD || st0 >= NUM_STYLES) return;
    /* Machine and the genres (House, Hip-hop, Breakbeat, Funk, Latin, Indie) start from Groove's roll, then each takes
       its own sound (genre_sound, 1.0.8); the genres take their drums from the trained patterns (roll_patterns) */
    int HO = st0 == STYLE_HOUSE, HH = st0 == STYLE_HIPHOP, MA = st0 == STYLE_MACHINE, BR = st0 == STYLE_BREAKS;
    int FU = st0 == STYLE_FUNK, LA = st0 == STYLE_LATIN, IN = st0 == STYLE_INDIE;
    int GEN = HO || HH || BR || FU || LA || IN, CI = 0;
    int st = (MA || GEN) ? STYLE_GROOVE : st0;
    int T = 0, G = st == STYLE_GROOVE, A = st == STYLE_AMBIENT, C = st == STYLE_COSMIC;   /* (T: Tongue drum's sound, 1.0..1.0.4) */
    if (!s->locked[SEC_SCALE]) {
        /* the groups: harmonic (0..), pentatonic, modes, other. Tongue: the series and the pentatonics; groove: the
           pentatonics and modes; ambient: the series, pentatonics, the gentler modes; cosmic: anything */
        const float wg[4][5] = {{3, 6, 3, 1, 0}, {2, 3, 3, 4, 1.5f}, {5, 4, 3, 2, .3f}, {4, 2, 2, 3, 2}};
        int g = pickw(r, wg[st - 1], 5), lo = scale_group_start(g), hi = scale_group_start(g + 1);
        int pick = lo + (int)(frand(r) * (float)(hi - lo)); if (pick >= hi) pick = hi - 1;
        if (g == 0 && frand(r) < .45f) pick = 0;                 /* the harmonic series itself, often */
        if (g == 4 && pick == hi - 1 && frand(r) < .8f) pick = lo;   /* (chromatic: rarely) */
        s->target[P_SCALE] = ((float)pick + .5f) / g_param_meta[P_SCALE].hi;
        TD(s, P_ROTATE, 0);
        /* four to six octaves (1.0.5: six often), or as many as 32 notes allow (a scale of 7 notes: four) */
        { const float w[6] = {0, 0, 0, 3, 3, 4}; TP(s, P_OCTAVES, w); }
        { const float w[3] = {5, 2, 3}; TP(s, P_SURFACE, w); }
        TR(s, P_ROOT, 28, T ? 38 : 36);                       /* E1 to D2: the ding is a bass note */
    }
    if (!s->locked[SEC_VOICE1]) {
        /* tongue: a low, clear band (tines, bells: often the odd partials, a little Cross); groove: wider, more Wave
           and Cross, loud notes open up; ambient: narrow, soft, turning slowly; cosmic: anywhere, strange combs */
        if (T) { TR(s, P_V1_CENTER, 0.f, .45f); TR(s, P_V1_BAND, .15f, .5f); TR(s, P_V1_TILT, .3f, .58f); TR(s, P_V1_WAVE, 0.f, .45f);
                 TR(s, P_V1_PHASE, .2f, .7f); TR(s, P_V1_SPIN, .1f, .4f); TR(s, P_V1_TONE, .5f, .75f); TR(s, P_V1_BLOOM, .5f, .9f); }
        if (G) { TR(s, P_V1_CENTER, .05f, .65f); TR(s, P_V1_BAND, .3f, .85f); TR(s, P_V1_TILT, .35f, .68f); TR(s, P_V1_WAVE, .2f, .95f);
                 TR(s, P_V1_PHASE, 0.f, .5f); TR(s, P_V1_SPIN, .15f, .5f); TR(s, P_V1_TONE, .55f, .82f); TR(s, P_V1_BLOOM, .6f, 1.f); }
        if (A) { TR(s, P_V1_CENTER, 0.f, .4f); TR(s, P_V1_BAND, .2f, .7f); TR(s, P_V1_TILT, .15f, .42f); TR(s, P_V1_WAVE, 0.f, .2f);   /* pads: soft, wide, low */
                 TR(s, P_V1_PHASE, .3f, .8f); TR(s, P_V1_SPIN, .35f, .75f); TR(s, P_V1_TONE, .42f, .66f); TR(s, P_V1_BLOOM, .2f, .5f); }
        if (C) { TR(s, P_V1_CENTER, .1f, .85f); TR(s, P_V1_BAND, .1f, .65f); TR(s, P_V1_TILT, .25f, .68f); TR(s, P_V1_WAVE, 0.f, .8f);
                 TR(s, P_V1_PHASE, .3f, 1.f); TR(s, P_V1_SPIN, .2f, .7f); TR(s, P_V1_TONE, .42f, .82f); TR(s, P_V1_BLOOM, .4f, .9f); }
        TR(s, P_V1_STRIDE, 0.f, C ? .7f : .4f);
        TR(s, P_V1_WIDTH, .35f, .85f);
        TD(s, P_V1_REG, 1);
    }
    if (!s->locked[SEC_VOICE2]) {
        /* the second voice: lower in the series and narrower than the first (a bed under it), wide in the stereo */
        TR(s, P_V2_CENTER, 0.f, C ? .55f : G ? .4f : .3f); TR(s, P_V2_BAND, .15f, G ? .65f : .5f); TR(s, P_V2_TILT, .2f, .55f);
        TR(s, P_V2_STRIDE, 0.f, .4f); TR(s, P_V2_WAVE, 0.f, G ? .6f : .4f); TR(s, P_V2_PHASE, .3f, .8f);
        TR(s, P_V2_SPIN, .25f, .7f); TR(s, P_V2_TONE, .42f, G ? .7f : .62f); TR(s, P_V2_BLOOM, .3f, .7f);
        TR(s, P_V2_WIDTH, .6f, 1.f);
        TR(s, P_V2_LEVEL, T ? .3f : G ? .4f : .5f, T ? .45f : G ? .6f : .7f);
        TD(s, P_V2_REG, G && frand(r) < .5f ? 0 : 1);
    }
    /* the motions:            orbit ladder swarm lorenz motif drift billiard gravity stride loops erode chords */
    if (!s->locked[SEC_GEN1]) {
        /* the rhythm layer: loops and runs with a pulse (tongue, groove), the stream itself, or passing through the
           notes (cosmic), or swelling as it drifts (ambient) */
        const float wf[4][NUM_GENS] = {{4, 4, 1, 1, 3, 0, 4, 3, 3, 2, 1, 2}, {3, 4, 1, 1, 4, 0, 4, 2, 5, 0, 1, 3},
                                       {1, 1, 3, 1, 1, 4, 0, 4, 0, 6, 4, 5}, {2, 2, 2, 3, 2, 1, 3, 3, 2, 3, 3, 3}};
        TP(s, P_G1_FUNC, wf[st - 1]);
        const float we[4][NUM_ENVS] = {{6, 0, 1, 3, 0, 0, 3, 0}, {6, 0, 1, 2, 1, 0, 6, 0}, {1, 4, 3, 2, 2, 1, 0, 0}, {3, 1, 2, 4, 3, 0, 3, 0}};
        TP(s, P_G1_ENV, we[st - 1]);
        TR(s, P_G1_A, 0.f, 1.f); TR(s, P_G1_B, 0.f, 1.f);
        TD(s, P_G1_POINTS, T ? 1 + (int)(frand(r) * 3.f) : G ? 2 + (int)(frand(r) * 3.f) : A ? (int)(frand(r) * 5.f) : (int)(frand(r) * 6.f));   /* (ambient, cosmic: from one point) */
        TR(s, P_G1_SPREAD, .2f, 1.f);
        if (T) { TR(s, P_G1_RATE, .25f, 1.f); TD(s, P_G1_ERATE, frand(r) < .5f ? 10 : 8); TR(s, P_G1_CONTOUR, .7f, .92f); TR(s, P_G1_ENERGY, .5f, .75f); }
        if (G) { TR(s, P_G1_RATE, .33f, 1.5f); TD(s, P_G1_ERATE, frand(r) < .5f ? 10 : frand(r) < .5f ? 12 : 11); TR(s, P_G1_CONTOUR, .8f, 1.f); TR(s, P_G1_ENERGY, .6f, .85f); }
        /* ambient and cosmic: down to one slow point and an envelope over 16 bars (sparse, slow, evolving) */
        if (A) { TR(s, P_G1_RATE, .015f, .3f); TD(s, P_G1_ERATE, (int)(frand(r) * 6.f)); TR(s, P_G1_CONTOUR, 0.f, .3f); TR(s, P_G1_ENERGY, .4f, .7f); }
        if (C) { TR(s, P_G1_RATE, .04f, 1.f); TD(s, P_G1_ERATE, (int)(frand(r) * 11.f)); TR(s, P_G1_CONTOUR, .2f, .9f); TR(s, P_G1_ENERGY, .5f, .8f); }
        if (pint(s, P_G1_ENV) == ENV_STREAM || (int)(s->target[P_G1_ENV] * NUM_ENVS) == ENV_STREAM) TR(s, P_G1_CONTOUR, .1f, .6f);
        {   int fn = (int)(s->target[P_G1_FUNC] * NUM_GENS);  /* Loops and Erode shape their own notes; Chords are held or swell */
            int sp = A || C;                                  /* (ambient, cosmic: fewer, slower: down to one note at a time) */
            if (fn == GEN_LOOPS || fn == GEN_ERODE) { TD(s, P_G1_ENV, ENV_HOLD); if (fn == GEN_LOOPS) TR(s, P_G1_RATE, 0.f, .3f); TD(s, P_G1_POINTS, sp ? 1 + (int)(frand(r) * 3.f) : 3 + (int)(frand(r) * 4.f)); }
            if (fn == GEN_CHORDS) { TD(s, P_G1_ENV, frand(r) < .5f ? ENV_HOLD : G || T ? ENV_PULSE : ENV_SWELL); TR(s, P_G1_RATE, sp ? .03f : .1f, sp ? .2f : .4f); TD(s, P_G1_POINTS, (sp ? 1 : 2) + (int)(frand(r) * 2.f)); } }
        TR(s, P_G1_REACH, .05f, A ? .45f : .25f);
        TR(s, P_PATH_SCALE, .7f, 1.f); TR(s, P_PATH_ROT, 0.f, 1.f);
        /* the keys' pull: off in a share of the rolls (a key plays just its note), most in the beat styles */
        TD(s, P_KEY_PULL, frand(r) < (GEN || MA ? .5f : A ? .3f : .4f) ? 0 : 1);
    }
    if (!s->locked[SEC_GEN2]) {
        /* the second layer: slow points that swell (a bed), or for groove a low counter-motion */
        const float wf[4][NUM_GENS] = {{2, 1, 3, 1, 1, 4, 1, 4, 0, 3, 1, 3}, {2, 2, 1, 1, 2, 1, 3, 3, 3, 0, 1, 4},
                                       {1, 1, 4, 2, 1, 4, 0, 4, 0, 6, 3, 6}, {2, 1, 3, 3, 1, 2, 1, 3, 1, 3, 2, 3}};
        TP(s, P_G2_FUNC, wf[st - 1]);
        const float we[4][NUM_ENVS] = {{0, 5, 2, 1, 1, 2, 1, 0}, {4, 1, 1, 2, 1, 0, 3, 0}, {0, 5, 2, 1, 2, 2, 0, 0}, {1, 3, 2, 2, 3, 1, 2, 0}};
        TP(s, P_G2_ENV, we[st - 1]);
        TR(s, P_G2_A, 0.f, 1.f); TR(s, P_G2_B, 0.f, 1.f);
        TD(s, P_G2_POINTS, A ? (int)(frand(r) * 3.f) : 1 + (int)(frand(r) * 3.f));   /* (ambient: from one point) */
        TR(s, P_G2_SPREAD, .3f, 1.f);
        TR(s, P_G2_RATE, G ? .16f : .015f, G ? .67f : .25f);
        TD(s, P_G2_ERATE, G ? 8 : A ? (int)(frand(r) * 4.f) : (int)(frand(r) * 7.f));
        TR(s, P_G2_CONTOUR, G ? .6f : 0.f, G ? .9f : .3f);
        TR(s, P_G2_REACH, .1f, .45f); TR(s, P_G2_ENERGY, .4f, .7f);
        {   int fn = (int)(s->target[P_G2_FUNC] * NUM_GENS);
            int sp = A || C;
            if (fn == GEN_LOOPS || fn == GEN_ERODE) { TD(s, P_G2_ENV, ENV_HOLD); if (fn == GEN_LOOPS) TR(s, P_G2_RATE, 0.f, .3f); TD(s, P_G2_POINTS, sp ? 1 + (int)(frand(r) * 3.f) : 3 + (int)(frand(r) * 3.f)); }
            if (fn == GEN_CHORDS) { TD(s, P_G2_ENV, frand(r) < .6f ? ENV_HOLD : ENV_SWELL); TR(s, P_G2_RATE, sp ? .03f : .08f, sp ? .15f : .35f); TD(s, P_G2_POINTS, (sp ? 1 : 2) + (int)(frand(r) * 2.f)); } }
        /* sometimes layer 2 modulates layer 1 instead of playing: the stream's envelope gating or accenting a
           chaotic one, or one path carried on another */
        TD(s, P_G1_ROUTE, ROUTE_PLAY);
        const float pm[4] = {.12f, .35f, .1f, .3f};
        if (frand(r) < (MA ? .7f : GEN ? .2f : pm[st - 1])) {
            const float wr[NUM_ROUTES] = {0, 1, 1.5f, 2.5f};
            int ro = pickw(&s->rng, wr, NUM_ROUTES); TD(s, P_G2_ROUTE, ro);
            if (ro != ROUTE_MOVE && frand(r) < .65f) {
                TD(s, P_G2_ENV, ENV_STREAM); TR(s, P_G2_CONTOUR, .2f, .6f);
                if (!s->locked[SEC_GEN1]) { const float wc[NUM_GENS] = {0, 0, 1, 4, 0, 2, 2, 2, 0, 0, 0, 0}; TP(s, P_G1_FUNC, wc); TD(s, P_G1_ENV, ENV_HOLD); }
            }
            if (ro == ROUTE_MOVE) TR(s, P_G2_ENERGY, .2f, .6f); else TR(s, P_G2_ENERGY, .6f, 1.f);
        } else TD(s, P_G2_ROUTE, ROUTE_PLAY);
    }
    /* Balance (1.0.5), each layer its own: a bass layer (register 0) leans dark; the rest from a little dark to fairly
       bright (Ambient near the middle, Cosmic anywhere, Machine either way) */
    for (int l = 0; l < NUM_LAYERS; l++) if (!s->locked[l ? SEC_GEN2 : SEC_GEN1]) {
        int id = P_G1_BAL + l, rg = l ? P_V2_REG : P_V1_REG;
        if (s->target[rg] * g_param_meta[rg].hi < 1.f) TR(s, id, -.8f, -.3f);
        else if (A) TR(s, id, -.3f, .2f);
        else if (C) TR(s, id, -.8f, .8f);
        else if (MA) TR(s, id, -.5f, .5f);
        else TR(s, id, -.3f, .5f);
    }
    /* the voices' colour (1.0.5), each layer its own: Odd/even often left as it is, else toward hollow or even (Ambient
       gently); Pluck in about half the rhythmic rolls (Hip-hop's slower), Machine's fast, a slow one sometimes in Ambient */
    for (int l = 0; l < NUM_LAYERS; l++) if (!s->locked[l ? SEC_VOICE2 : SEC_VOICE1]) {
        int oe = P_V1_ODDEVEN + l, pk = P_V1_PLUCK + l;
        float poe = A ? .35f : (C || MA) ? .6f : .45f, ow = A ? .25f : .45f;
        if (frand(r) < poe) { if (frand(r) < .5f) TR(s, oe, .5f - ow, .45f); else TR(s, oe, .55f, .5f + ow); }
        else TR(s, oe, .5f, .5f);
        /* Depth: most rolls reach below (Ambient and the genres' deep, Machine's less); Clarity: open to strict, a
           little stricter where the notes are many (Ambient, Cosmic) */
        if (A) TR(s, P_V1_DEPTH + l, .35f, .85f);
        else if (frand(r) < (MA ? .55f : .8f)) TR(s, P_V1_DEPTH + l, MA ? .15f : .25f, MA ? .55f : .75f);
        else TR(s, P_V1_DEPTH + l, 0.f, 0.f);
        TR(s, P_V1_CLARITY + l, (A || C) ? .45f : .25f, (A || C) ? .9f : .8f);
        float ppk = A ? .2f : C ? .4f : MA ? .55f : .45f;
        if (frand(r) < ppk) {
            if (A) TR(s, pk, .05f, .3f);
            else if (HH) TR(s, pk, .2f, .55f);
            else if (MA) TR(s, pk, .35f, .85f);
            else TR(s, pk, .25f, .7f);
        } else TR(s, pk, 0.f, 0.f);
    }
    if (!s->locked[SEC_METER]) {
        /* the rhythm: tempo, how many of the stream's peaks pass, the twos or the threes, how fine, the swing, how
           much the long form moves it, and what picks the drum */
        float p3 = T ? .3f : G ? .2f : A ? .3f : C ? .55f : 0.f, psw = G ? .4f : T ? .2f : C ? .25f : .1f, plock = .08f;
        /* (1.0.5: every tempo range wider; ambient and cosmic down to a sparse, slow stream) */
        if (G) { TR(s, P_TEMPO, 80, 132); TR(s, P_RH_DENS, .5f, .7f); TR(s, P_RH_FINE, .5f, .85f); TR(s, P_RH_FORM, .5f, 1.f); }
        if (A) { TR(s, P_TEMPO, 40, 90); TR(s, P_RH_DENS, .08f, .5f); TR(s, P_RH_FINE, .1f, .5f); TR(s, P_RH_FORM, .4f, 1.f); }
        if (C) { TR(s, P_TEMPO, 60, 135); TR(s, P_RH_DENS, .15f, .65f); TR(s, P_RH_FINE, .3f, .8f); TR(s, P_RH_FORM, .6f, 1.f); }
        if (MA) { p3 = .5f; psw = .2f; plock = .45f; TR(s, P_TEMPO, 80, 170); TR(s, P_RH_FINE, .6f, 1.f); TR(s, P_RH_FORM, .7f, 1.f); }   /* IDM */
        if (GEN) { p3 = 0.f; psw = 0.f; plock = 0.f; TR(s, P_RH_DENS, HH ? .55f : .6f, .85f); TR(s, P_RH_FINE, .7f, .95f); TR(s, P_RH_FORM, .3f, .8f); }
        float u = frand(r);
        if (u < p3) { if (frand(r) < (CI ? .7f : .35f)) TR(s, P_RH_THREES, 1.f, 1.f); else TR(s, P_RH_THREES, .38f, .62f); }
        else TR(s, P_RH_THREES, 0.f, 0.f);
        if (u >= p3 && frand(r) < psw) TR(s, P_RH_SWING, .45f, .9f); else TR(s, P_RH_SWING, 0.f, 0.f);
        TR(s, P_RH_TURN, 0.f, (C || MA) ? 1.f : GEN ? 0.f : .15f);
        TR(s, P_RH_MOTION, MA ? .3f : 0.f, MA ? .8f : C ? .5f : .3f);
        TD(s, P_RH_ENGINE, frand(r) < plock ? RH_LOCK : RH_THRESHOLD);
        TR(s, P_RH_PULL, MA ? .15f : .35f, MA ? .7f : 1.f);
        TD(s, P_RH_ASSIGN, frand(r) < (MA ? .25f : .1f) ? RH_COUNT : RH_SELECT);
        { const float w[4][NUM_SELRATES] = {{2, 8, 1, 1, 0}, {1, 8, 1, 2, 0}, {4, 6, 0, 0, 0}, {2, 5, 3, 2, 1}}, wm[NUM_SELRATES] = {1, 3, 3, 2, 2}, wb[NUM_SELRATES] = {1, 8, 0, 2, 0};
          TP(s, P_RH_SELRATE, MA ? wm : BR ? wb : w[st - 1]); }
        TR(s, P_RH_FEED, (MA || BR) ? .5f : .25f, (MA || BR) ? .9f : .6f);
        TD(s, P_RH_COUNT, 1 + (int)(frand(r) * 4.f));
        TR(s, P_RH_GRIP, A ? 0.f : C ? .15f : .35f, A ? .15f : C ? .7f : .9f);   /* (ambient: the notes do not pulse with the stream) */
        TR(s, P_EVOLVE, (A || C) ? .1f : .05f, (A || C) ? .6f : G ? .2f : .35f);   /* (ambient, cosmic: evolving) */
        if (MA) TR(s, P_EVOLVE, .2f, .45f);
        if (GEN) { TD(s, P_RH_ASSIGN, RH_SELECT); TR(s, P_RH_MOTION, 0.f, .12f); }
    }
    if (!s->locked[SEC_DRUMS]) {
        /* the drums: how likely they play, the layer they ride, a kit (a character for each drum's envelope and
           sound), how readily each is chosen, the strength of event it wants, whether it follows a layer */
        float pon = (GEN || MA) ? 1.f : G ? .85f : C ? .4f : .12f;
        int on = frand(r) < pon;
        TD(s, P_DR_ON, on);
        {   /* the drum layer: Split mostly (the kick on the bass notes of any roll); Ambient and Cosmic sometimes one */
            const float wl[NUM_DLAYERS] = {(A || C) ? 1.5f : .6f, (A || C) ? 1.5f : .6f, 4.f};
            TP(s, P_DR_LAYER, wl);
            if (GEN) TD(s, P_DR_LAYER, NUM_DLAYERS - 1); }      /* (the genres: Split, the kick on the bass notes) */
        TR(s, P_DR_LEVEL, .6f, .8f); TR(s, P_DR_ACCENT, .4f, .9f);
        /* Shape by a layer's envelope (the synth's generator as the drums' volume): often, most in Machine and Cosmic */
        float psh = MA ? .6f : C ? .5f : A ? .5f : .3f;
        if (frand(r) < psh) { TD(s, P_DR_SHAPE, frand(r) < .5f ? DSHAPE_L1 : DSHAPE_L2); TR(s, P_DR_DEPTH, .4f, .9f); }
        else TD(s, P_DR_SHAPE, DSHAPE_NONE);
        /* the kick's duck: Machine, Breaks and Groove, sometimes Drum circle and Cosmic; never Ambient */
        float pdk = (MA || G || GEN) ? .7f : C ? .3f : 0.f;
        if (frand(r) < pdk) TR(s, P_DR_DUCK, .4f, 1.f); else TR(s, P_DR_DUCK, 0.f, 0.f);   /* (1.0.5: deeper) */
        /* the kits, as characters: decay's scale, curve, bright, settle, snap (each a lo..hi; the last three unused) */
        enum { K_SKIN, K_BOOM, K_808, K_WOOD, K_METAL, K_GLITCH, NUM_K };
        static const float KT[NUM_K][12] = {
            /* dec  curve      bright     settle     snap       (unused) */
            {1.f,  0, .3f,   .3f, .6f,  .4f, .7f,  .3f, .6f,  .2f, .5f, 1.f},     /* skin: tabla, frame drum, taiko */
            {1.6f, 0, .2f,   .1f, .35f, .2f, .5f,  .2f, .5f,  .4f, .8f, 1.4f},    /* boom: big, round, a long drop */
            {2.f,  0, .15f,  0, .2f,    .3f, .6f,  0, .2f,    .5f, .9f, 1.8f},    /* 808: almost the note alone, the long sweep */
            {.45f, .3f, .7f, .5f, .8f,  .7f, .95f, .4f, .8f,  0, .2f, .5f},       /* wood: short and dry, clicky */
            {1.5f, 0, .2f,   .5f, .9f,  0, .25f,   .1f, .4f,  0, .1f, 1.f},       /* metal: bright and ringing */
            {.35f, .5f, 1.f, .6f, 1.f,  .8f, 1.f,  .6f, 1.f,  .2f, .7f, .4f},     /* glitch: short tuned clicks and zaps */
        };
        static const float WK[10][NUM_K] = {{1, 4, 5, 1, 0, 0},   /* hip-hop: boom and 808 */
                                          {1, 3, 5, 1, 0, 0},     /* groove */
                                          {2, 1, 1, 1, 3, 0},     /* ambient */
                                          {2, 1, 2, 1, 2, 1},     /* cosmic */
                                          {0, 3, 6, 1, 1, 0},     /* house: 808, boom */
                                          {0, 1, 3, 1, 1, 4},     /* machine (IDM) */
                                          {3, 2, 2, 2, 0, 1},     /* breakbeat: skin, wood, boom, 808 */
                                          {3, 2, 1, 3, 0, 0},     /* funk: skin and wood, tight */
                                          {5, 1, 0, 4, 2, 0},     /* latin: skin, wood, a bell's metal */
                                          {4, 4, 0, 2, 1, 0}};    /* indie: a kit's skins, boomy */
        int row = HH ? 0 : HO ? 4 : MA ? 5 : BR ? 6 : FU ? 7 : LA ? 8 : IN ? 9 : st - 1;
        const float *K = KT[pickw(r, WK[row], NUM_K)];
        /*                         kick        snare       hat         tom    (density lo, hi) */
        static const float DN[10][8] = {{.45f, .6f, .45f, .6f, .4f, .6f, .3f, .5f},    /* hip-hop */
                                       {.5f, .65f, .45f, .6f, .45f, .7f, .3f, .5f},    /* groove */
                                       {.4f, .55f, .2f, .4f, .2f, .45f, .35f, .55f},   /* ambient */
                                       {.4f, .6f, .35f, .55f, .35f, .65f, .35f, .65f}, /* cosmic */
                                       {.5f, .65f, .45f, .6f, .45f, .7f, .35f, .6f},   /* house */
                                       {.45f, .65f, .45f, .65f, .45f, .75f, .35f, .65f},/* machine */
                                       {.5f, .62f, .5f, .62f, .5f, .75f, .3f, .45f},   /* breaks */
                                       {.5f, .65f, .5f, .65f, .5f, .75f, .3f, .5f},    /* funk */
                                       {.45f, .6f, .45f, .6f, .5f, .7f, .45f, .65f},   /* latin */
                                       {.5f, .62f, .5f, .62f, .5f, .7f, .3f, .5f}};    /* indie */
        static const float PL[4][2] = {{0.f, .1f}, {.36f, .48f}, {.74f, .88f}, {.5f, .68f}};
        static const float DC[4][2] = {{.28f, .6f}, {.08f, .18f}, {.02f, .05f}, {.22f, .5f}};   /* seconds to -35 dB, before the kit's own scale (1.0.6: the snare
                                                                                                 shorter, the hat a closed hat's) */
        static const float VO[4][2] = {{0.f, .1f}, {.45f, .75f}, {.85f, 1.f}, {.15f, .45f}};  /* the note each rides: kick lowest, hat highest */
        static const float LV[4] = {1.f, .4f, .22f, .5f};      /* (1.0.6: kick 0, snare -8, hat -13, tom -6 dB) */
        float pfol = MA ? .35f : C ? .25f : A ? .25f : .08f;
        for (int d = 0; d < NUM_DRUMS; d++) {
            TR(s, DP(d, P_D1_DENS), DN[row][2 * d], DN[row][2 * d + 1]);
            TR(s, DP(d, P_D1_PLACE), PL[d][0], PL[d][1]);
            int src = DSRC_STREAM;
            if (d >= 2 && frand(r) < pfol) src = frand(r) < .7f ? DSRC_L2 : DSRC_L1;
            TD(s, DP(d, P_D1_SRC), src);
            TR(s, DP(d, P_D1_VOICE), VO[d][0], VO[d][1]);
            TR(s, DP(d, P_D1_ATTACK), .001f, d == 2 ? .002f : K[3] > .5f ? .003f : .006f);   /* (1 ms up; softer for the darker kits) */
            TR(s, DP(d, P_D1_HOLD), 0.f, d == 2 ? .002f : .012f);
            float dl = DC[d][0] * K[0] * (A ? 1.6f : 1.f), dh = DC[d][1] * K[0] * (A ? 1.25f : 1.f);
            if (d == 2) { if (dh > .06f) dh = .06f; if (dl > dh) dl = dh; }   /* (the hat closed: at most 60 ms) */
            TR(s, DP(d, P_D1_DECAY), dl < .02f ? .02f : dl, dh > 3.f ? 3.f : dh);
            TR(s, DP(d, P_D1_CURVE), K[1], K[2]);
            float lv = LV[d] * fexp2((frand(r) - .5f) * 1.f);   /* within +-3 dB of the mix */
            TR(s, DP(d, P_D1_LEVEL), lv > 1.f ? 1.f : lv, lv > 1.f ? 1.f : lv);
            float hb = d == 2 ? .2f : d == 1 ? .1f : 0.f;            /* (the hat and snare a little brighter) */
            /* Bright (-1 dark .. 1): the kit's range at about two thirds of what it was (the hits ran hot); the kick, and
               the tom half the time, dark instead: a fall from the undertones, the drop a pitch sweep would give */
            if (d == 0 ? frand(r) < .7f : d == 3 ? frand(r) < .5f : 0) TR(s, DP(d, P_D1_BRIGHT), d == 0 ? -.7f : -.6f, -.15f);
            else TR(s, DP(d, P_D1_BRIGHT), fclamp(.65f * K[3] + hb, 0.f, 1.f), fclamp(.65f * K[4] + hb, 0.f, 1.f));
            {   float sb = d == 2 ? .35f : hb;              /* (the hat's colour settles quickly: its sizzle goes with it) */
                TR(s, DP(d, P_D1_SETTLE), fclamp(K[5] + sb, 0.f, 1.f), fclamp(K[6] + sb, 0.f, 1.f)); }
            TR(s, DP(d, P_D1_SNAP), K[7], K[8]);
        }
        /* the genres' drum sounds: hip-hop a long, deep kick and short hats; house a punchy kick, a clap, a fourth part
           high and open (an open hat, a shaker); breakbeat a tight kick, a bright snare */
        if (HH) { TR(s, P_D1_DECAY, .6f, 1.4f); TR(s, P_D1_CURVE, 0.f, .15f); TR(s, P_D1_VOICE, 0.f, .05f);
                  TR(s, P_D3_DECAY, .02f, .045f); TR(s, P_D3_HOLD, 0.f, 0.f); TR(s, P_D2_DECAY, .1f, .2f); TR(s, P_D2_SNAP, .5f, .8f); }
        if (HO) { TR(s, P_D1_DECAY, .25f, .5f); TR(s, P_D2_DECAY, .1f, .2f); TR(s, P_D2_SNAP, .5f, .8f); TR(s, P_D2_BRIGHT, .25f, .55f);
                  TR(s, P_D3_DECAY, .025f, .07f); TR(s, P_D4_VOICE, .8f, 1.f); TR(s, P_D4_DECAY, .12f, .35f); if (frand(r) < .5f) TR(s, P_D4_BRIGHT, .3f, .6f); else TR(s, P_D4_BRIGHT, -.6f, -.2f); TR(s, P_D4_SNAP, .2f, .6f); }
        if (BR) { TR(s, P_D1_DECAY, .25f, .6f); TR(s, P_D2_DECAY, .1f, .2f); TR(s, P_D2_BRIGHT, .25f, .55f); TR(s, P_D3_DECAY, .02f, .05f); }
        /* (1.0.8) funk a tight kick, a cracking snare, crisp hats; latin a round bombo or surdo, a woody clave on the
           snare's part, a shaker's hat, congas on the fourth; indie a kit's: a full kick, a snare with body, a tom */
        if (FU) { TR(s, P_D1_DECAY, .2f, .45f); TR(s, P_D1_CURVE, .1f, .3f); TR(s, P_D2_DECAY, .1f, .18f); TR(s, P_D2_SNAP, .5f, .85f);
                  TR(s, P_D2_BRIGHT, .2f, .5f); TR(s, P_D3_DECAY, .02f, .045f); }
        if (LA) { TR(s, P_D1_DECAY, .25f, .6f); TR(s, P_D2_DECAY, .04f, .1f); TR(s, P_D2_BRIGHT, .4f, .8f); TR(s, P_D2_SNAP, .6f, .9f);
                  TR(s, P_D2_VOICE, .6f, .85f); TR(s, P_D3_DECAY, .03f, .06f); TR(s, P_D4_DECAY, .12f, .3f); TR(s, P_D4_VOICE, .45f, .75f);
                  TR(s, P_D4_HPLUCK, .3f, .6f); }
        if (IN) { TR(s, P_D1_DECAY, .25f, .5f); TR(s, P_D2_DECAY, .15f, .3f); TR(s, P_D2_BRIGHT, .25f, .55f); TR(s, P_D3_DECAY, .03f, .06f);
                  TR(s, P_D4_DECAY, .25f, .5f); }
        /* each hit's colour on its note (1.0.5): Pluck, so the note it rides sounds struck (most on the snare and tom);
           Odd/even, a push toward hollow or even that settles with the hit (most on the hat and snare). Ambient: none */
        {
            static const float PPL[4] = {.3f, .45f, .25f, .5f}, POE[4] = {.15f, .35f, .45f, .25f};
            for (int d = 0; d < NUM_DRUMS; d++) {
                float sc = A ? 0.f : C ? .5f : 1.f;
                if (frand(r) < PPL[d] * sc) TR(s, P_D1_HPLUCK + d, .25f, .7f); else TR(s, P_D1_HPLUCK + d, 0.f, 0.f);
                if (frand(r) < POE[d] * sc) { if (frand(r) < .5f) TR(s, P_D1_OE + d, .1f, .35f); else TR(s, P_D1_OE + d, .65f, .9f); }
                else TR(s, P_D1_OE + d, .5f, .5f);
            }
        }
        /* and the other way: in Machine, sometimes layer 1 swells with the kick (its Drums envelope) */
        if (on && MA && !s->locked[SEC_GEN1] && frand(r) < .3f) { TD(s, P_G1_ENV, ENV_DRUMS); TD(s, P_G1_ERATE, 0); TR(s, P_G1_CONTOUR, .1f, .5f); }
    }
    if (GEN && (!s->locked[SEC_METER] || !s->locked[SEC_DRUMS]))   /* the genres' patterns, tempo and swing: the tree */
        roll_patterns(s, HO ? PG_HOUSE : HH ? PG_HIPHOP : BR ? PG_BREAKS : FU ? PG_FUNK : LA ? PG_LATIN : PG_INDIE,
                      !s->locked[SEC_METER], !s->locked[SEC_DRUMS]);
    if (!s->locked[SEC_LFO]) {
        for (int k = 0; k < 6; k++) {
            TR(s, P_LFO1_RATE + 3 * k, A ? .005f : C ? .01f : .02f, A ? .12f : C ? 1.2f : MA ? 3.f : .6f);   /* (1.0.5: wider) */
            const float w[9] = {4, 2, 0, 0, 0, 3, 0, 0, 0}; TP(s, P_LFO1_SHAPE + 3 * k, w);
            TD(s, P_LFO1_SYNC + 3 * k, 0);
        }
        TR(s, P_LFO_COUPLE, 0.f, .3f);
    }
    if (!s->locked[SEC_FX] || g_fx_wake) {
        /* fuzz: Groove, Machine and Breaks often, Cosmic sometimes, a touch in Tongue, rarely Ambient */
        float pfz = (MA || BR) ? .4f : G ? .3f : C ? .2f : T ? .08f : .04f;
        if (frand(r) < pfz) { TR(s, P_FZ_MIX, .1f, (G || MA || BR) ? .5f : .25f); TR(s, P_FZ_SUSTAIN, .1f, .7f); TR(s, P_FZ_TONE, .25f, .7f); }
        else TR(s, P_FZ_MIX, 0.f, 0.f);
        TR(s, P_FLT_CUT, A ? 8000.f : 10000.f, 18000.f); TR(s, P_FLT_RES, 0.f, .15f); TR(s, P_FLT_MORPH, 0.f, .05f);
        TR(s, P_DL_SEND, C ? .2f : .1f, C ? .4f : .28f); TR(s, P_DL_FB, .25f, (A || C) ? .55f : .42f);
        { const float w[10] = {0, 0, 4, 2, 0, 3, 1, 0, 0, 0}, wa[10] = {0, 0, 0, 0, 0, 3, 3, 2, 0, 0}, wc[10] = {0, 0, 6, 0, 2, 2, 0, 0, 0, 0};
          int i = pickw(r, A ? wa : C ? wc : w, 10); s->target[P_DL_TIME] = ((float)i + .5f) / 10.f; }
        TD(s, P_DL_TAP, frand(r) < .5f ? 1 : 0);
        if (T) { TR(s, P_RV_SEND, .3f, .5f); TR(s, P_RV_SIZE, .75f, 1.f); TR(s, P_RV_DECAY, 3.f, 9.f); }
        if (G) { TR(s, P_RV_SEND, .15f, .35f); TR(s, P_RV_SIZE, .6f, .9f); TR(s, P_RV_DECAY, 1.5f, 4.5f); }
        if (A) { TR(s, P_RV_SEND, .5f, .8f); TR(s, P_RV_SIZE, .9f, 1.f); TR(s, P_RV_DECAY, 12.f, 45.f); }
        if (C) { TR(s, P_RV_SEND, .4f, .7f); TR(s, P_RV_SIZE, .85f, 1.f); TR(s, P_RV_DECAY, 6.f, 25.f); }
        TR(s, P_RV_DAMP, A ? .45f : .4f, .75f);   /* (the top rings 44-70 % as long as the rest: a room's, not a plate's) */
        TR(s, P_RV_MOD, .45f, .8f); TD(s, P_RV_TUNED, 0); TR(s, P_DL_TO_RV, .1f, .4f);
        TR(s, P_CMP_THRESH, -18.f, -8.f);
        TR(s, P_CMP_RATIO, G ? 2.f : 1.3f, G ? 3.5f : 2.f);
        TR(s, P_CMP_PUMP, HO ? .15f : 0.f, HO ? .45f : G ? .3f : 0.f);
        TR(s, P_CMP_ATT, G ? 6.f : 12.f, G ? 15.f : 40.f);   /* (slow enough for a 40 Hz note) */
        TR(s, P_CMP_MIX, .75f, 1.f);
    }
    if (MA || GEN) genre_sound(s, HO ? 0 : HH ? 1 : BR ? 2 : MA ? 3 : FU ? 4 : LA ? 5 : 6);   /* (1.0.8) its own sound, last */
}

/* the curated styles' matrix (1.0.5: more routes, more places, deeper): each route a source (the LFOs mostly, the
   bar's random, a motion's envelope, the trigger, the follower, the path) to a place from a list of the musical ones,
   each with the most it may move (a filter never shuts, nothing runs away); how many and how deep by the style.
   The genres sometimes move a pattern too (a slow walk among its relatives, a rotation); Machine moves its own
   rhythm: density, threes, turn, swing, the selector, pull, feedback, fine, form, count */
static const uint8_t MOD_DST[][2] = {     /* the place, its most (hundredths) */
    {P_V1_TILT, 60}, {P_V1_CENTER, 50}, {P_V1_BAND, 50}, {P_V1_WAVE, 70}, {P_V1_PHASE, 80}, {P_V1_SPIN, 70}, {P_V1_TONE, 40},
    {P_V1_BLOOM, 60}, {P_V1_STRIDE, 40}, {P_V1_WIDTH, 70}, {P_V2_TILT, 60}, {P_V2_CENTER, 50}, {P_V2_BAND, 50}, {P_V2_WAVE, 70},
    {P_V2_PHASE, 80}, {P_V2_SPIN, 70}, {P_V2_TONE, 40}, {P_V2_WIDTH, 70}, {P_G1_A, 70}, {P_G1_B, 70}, {P_G1_REACH, 50},
    {P_G1_ENERGY, 40}, {P_G1_SPREAD, 60}, {P_G1_CONTOUR, 50}, {P_G2_A, 70}, {P_G2_B, 70}, {P_G2_REACH, 50}, {P_G2_SPREAD, 60},
    {P_PATH_SCALE, 40}, {P_PATH_ROT, 80}, {P_PATH_X, 50}, {P_PATH_Y, 50}, {P_RH_DENS, 35}, {P_RH_SWING, 40}, {P_RH_FINE, 40},
    {P_RH_THREES, 40}, {P_RH_TURN, 50}, {P_RH_GRIP, 60}, {P_D1_DECAY, 40}, {P_D3_DECAY, 50}, {P_D2_SNAP, 50}, {P_D3_BRIGHT, 30},
    {P_D2_BRIGHT, 30}, {P_D4_VOICE, 50}, {P_D3_VOICE, 40}, {P_FLT_CUT, 30}, {P_FLT_RES, 40}, {P_FZ_MIX, 40}, {P_DL_SEND, 50},
    {P_DL_TIME, 30}, {P_DL_TONE, 50}, {P_RV_SEND, 50}, {P_RV_SIZE, 40}, {P_RV_MOD, 60}, {P_RV_DAMP, 50}, {P_CMP_PUMP, 40},
    {P_V1_ODDEVEN, 50}, {P_V2_ODDEVEN, 50}, {P_V1_PLUCK, 40}, {P_V2_PLUCK, 40}, {P_D2_OE, 40}, {P_D3_OE, 40},   /* (1.0.5) */
    {P_V1_DEPTH, 40}, {P_V2_DEPTH, 40},
};
static const uint8_t MA_DST[] = {P_RH_DENS, P_RH_THREES, P_RH_TURN, P_RH_SWING, P_RH_SELRATE, P_RH_PULL, P_RH_FEED, P_RH_FINE,
                                 P_RH_FORM, P_RH_MOTION, P_RH_COUNT, P_RH_GRIP};
static int mod_src(uint32_t *r) {
    float u = frand(r);
    return u < .55f ? SRC_LFO1 + (int)(frand(r) * 6) % 6 : u < .65f ? SRC_RAND : u < .8f ? (frand(r) < .5f ? SRC_MOT1 : SRC_MOT2)
         : u < .87f ? SRC_TRIG : u < .94f ? SRC_ENV : (frand(r) < .5f ? SRC_PATHX : SRC_PATHY);
}
static void style_mod(Synth *s) {
    if (s->style == STYLE_WILD || s->locked[SEC_LFO]) return;
    uint32_t *r = &s->rng; int st = s->style;
    int A = st == STYLE_AMBIENT, C = st == STYLE_COSMIC, MA = st == STYLE_MACHINE;
    int GEN = st == STYLE_HOUSE || st == STYLE_HIPHOP || st == STYLE_BREAKS || st == STYLE_FUNK || st == STYLE_LATIN || st == STYLE_INDIE;
    int lo = A ? 3 : C ? 5 : 4, hi = A ? 6 : C ? 10 : 8;
    float sc = A ? .5f : C ? 1.f : MA ? .85f : .8f;
    int n = lo + (int)(frand(r) * (float)(hi - lo + 1)), k = 0;
    memset(s->slot, 0, sizeof s->slot);
    for (; k < n && k < MOD_SLOTS; k++) {
        ModSlot *m = &s->slot[k];
        int j = 0, nd = (int)(sizeof MOD_DST / sizeof MOD_DST[0]);
        m->src = (uint8_t)mod_src(r);
        for (int t = 0; t < 8; t++) { j = (int)(frand(r) * (float)nd) % nd; if (!mod_loop(m->src, MOD_DST[j][0])) break; }
        if (mod_loop(m->src, MOD_DST[j][0])) m->src = SRC_LFO1 + (int)(frand(r) * 6) % 6;   /* (an LFO there instead) */
        m->dst = MOD_DST[j][0];
        float d = (float)MOD_DST[j][1] * .01f * sc * (.25f + .75f * frand(r));
        m->depth = frand(r) < .5f ? -d : d;
    }
    if (MA) for (int j = 2 + (int)(frand(r) * 3.f); j > 0 && k < MOD_SLOTS; j--, k++) {   /* Machine: its rhythm, firmly
                                                                                         (2..4 routes at 0.15..0.5: it was 3..6 at 0.25..0.8, too much) */
        ModSlot *m = &s->slot[k];
        m->src = (uint8_t)mod_src(r);
        m->dst = MA_DST[(int)(frand(r) * sizeof MA_DST) % sizeof MA_DST];
        float d = .15f + .35f * frand(r);
        m->depth = frand(r) < .5f ? -d : d;
    }
    if (GEN && !s->locked[SEC_DRUMS] && frand(r) < .35f && k < MOD_SLOTS) {   /* a pattern that walks among its relatives */
        int d = 1 + (int)(frand(r) * 3.f) % 3;                          /* (the snare, the hat or the fourth part) */
        if (s->target[P_D1_PAT + d] * (float)(PAT_CNT[d] + 1) >= 1.f) {
            ModSlot *m = &s->slot[k++];
            int rot = d >= 2 && frand(r) < .4f;
            m->src = (uint8_t)(frand(r) < .6f ? SRC_RAND : SRC_LFO1 + (int)(frand(r) * 6) % 6);
            m->dst = (uint8_t)(rot ? P_D1_ROT + d : P_D1_PAT + d);
            float dd = rot ? 2.f / 16.f : (1.5f + 1.5f * frand(r)) / (float)(PAT_CNT[d] + 1);
            m->depth = frand(r) < .5f ? -dd : dd;
        }
    }
}

void params_roll(Synth *s, uint32_t seed) {
    for (int p = 0; p < ROLL_PARTS; p++) params_roll_part(s, p, seed);
}
/* the roll in parts, each a fraction of a block's work, so a roll run from the audio's interrupt (synth_roll_step) is
   spread over a few blocks: 0 the parameters, the effects' pairs, the matrix's random routes; 1, 2 the knobs' paths
   (each half); 3 the style, its matrix, Wild's patterns */
/* (1.0.6) after the style: the levels' paths, and a Dry patch's kit, routes and Shape */
static void level_paths(Synth *s) {          /* the drums' levels on Groove (+-4 dB), the layers' on Motion (+-3 dB, the two
                                                opposite: a turn shifts which layer leads); always on their knobs */
    uint32_t *r = &s->rng;
    for (int d = 0; d < NUM_DRUMS; d++) {
        int id = DP(d, P_D1_LEVEL); int8_t *w = s->kpath[id]; float v = s->target[id];
        float dir = frand(r) < .5f ? -1.f : 1.f;
        for (int side = 0; side < 2; side++) {
            float db = (2.f + 2.f * frand(r)) * (side ? dir : -dir), nd = db * (.3f + .7f * frand(r));
            w[side ? 3 : 0] = (int8_t)(fclamp(v * (fexp2(db * .1661f) - 1.f), -1.f, 1.f) * 127.f);
            w[side ? 2 : 1] = (int8_t)(fclamp(v * (fexp2(nd * .1661f) - 1.f), -1.f, 1.f) * 127.f);
        }
    }
    {   float v1 = s->target[P_V1_LEVEL], v2 = s->target[P_V2_LEVEL], dir = frand(r) < .5f ? -1.f : 1.f;
        for (int side = 0; side < 2; side++) {
            float db = (1.5f + 1.5f * frand(r)) * (side ? dir : -dir), nd = db * (.3f + .7f * frand(r));
            s->kpath[P_V1_LEVEL][side ? 3 : 0] = (int8_t)(fclamp(v1 * (fexp2(db * .1661f) - 1.f), -1.f, 1.f) * 127.f);
            s->kpath[P_V1_LEVEL][side ? 2 : 1] = (int8_t)(fclamp(v1 * (fexp2(nd * .1661f) - 1.f), -1.f, 1.f) * 127.f);
            s->kpath[P_V2_LEVEL][side ? 3 : 0] = (int8_t)(fclamp(v2 * (fexp2(-db * .1661f) - 1.f), -1.f, 1.f) * 127.f);
            s->kpath[P_V2_LEVEL][side ? 2 : 1] = (int8_t)(fclamp(v2 * (fexp2(-nd * .1661f) - 1.f), -1.f, 1.f) * 127.f);
        }
    }
}
/* a Dry patch's drums as a kit: the kick (1) and the tom (4) long, the snare (2) and the hat (3) short and snappy */
static void dry_kit(Synth *s) {
    for (int d = 0; d < NUM_DRUMS; d++) {
        int lg = d == 0 || d == 3;
        if (lg) { TR(s, DP(d, P_D1_DECAY), d ? .3f : .45f, d ? .9f : 1.5f); TR(s, DP(d, P_D1_HOLD), .005f, .02f); }
        else if (d == 1) { TR(s, DP(d, P_D1_DECAY), .07f, .18f); TR(s, DP(d, P_D1_HOLD), 0.f, .004f); TR(s, DP(d, P_D1_SNAP), .5f, .85f); }
        else { TR(s, DP(d, P_D1_DECAY), .02f, .055f); TR(s, DP(d, P_D1_HOLD), 0.f, .002f); TR(s, DP(d, P_D1_SNAP), .4f, .8f); }
    }
}
/* Shape's paths for the drums' envelopes: one way (rolled) the kick and the tom bloom, long and open, while the snare
   and the hat stay tight (tighter, snappier); the other way all of it pulls in close. Within that, rolled */
static void dry_shape_paths(Synth *s) {
    uint32_t *r = &s->rng;
    float bd = frand(r) < .5f ? -1.f : 1.f;    /* the side of the knob that blooms */
    for (int d = 0; d < NUM_DRUMS; d++) {
        int lg = d == 0 || d == 3;
        static const uint8_t P[6] = {P_D1_DECAY, P_D1_HOLD, P_D1_ATTACK, P_D1_SNAP, P_D1_SETTLE, P_D1_CURVE};
        for (int k = 0; k < 6; k++) {
            int id = DP(d, P[k]); int8_t *w = s->kpath[id];
            for (int side = 0; side < 2; side++) {
                int bloom = (side ? 1.f : -1.f) == bd;
                float lo, hi;
                switch (k) {
                /* (1.0.6: far wider than first built, where a turn did little: the kick's decay from a click to
                   seconds; and Shape's offsets are doubled on a Dry patch, mod.c) */
                case 0: if (lg) { lo = bloom ? .35f : -.2f; hi = bloom ? .6f : -.12f; }      /* decay (the pull: a tight thump, never a click) */
                        else    { lo = bloom ? -.06f : -.3f; hi = bloom ? .06f : -.15f; } break;
                case 1: if (lg) { lo = bloom ? .4f : -.3f;  hi = bloom ? .8f : -.1f; }      /* hold */
                        else    { lo = -.1f; hi = 0.f; } break;
                case 2: if (lg) { lo = bloom ? .1f : -.2f; hi = bloom ? .35f : 0.f; }       /* attack */
                        else    { lo = -.15f; hi = 0.f; } break;
                case 3: if (lg) { lo = bloom ? -.4f : 0.f;  hi = bloom ? -.15f : .3f; }     /* snap */
                        else    { lo = bloom ? .2f : 0.f;  hi = bloom ? .5f : .3f; } break;
                default: lo = -.35f; hi = .35f; break;                                       /* settle, curve */
                }
                float far = lo + (hi - lo) * frand(r), near = far * (.4f + .6f * frand(r));
                w[side ? 3 : 0] = (int8_t)(fclamp(far, -1.f, 1.f) * 127.f);
                w[side ? 2 : 1] = (int8_t)(fclamp(near, -1.f, 1.f) * 127.f);
            }
        }
    }
}
static void roll_finish(Synth *s) {
    uint32_t *r = &s->rng;
    int dry = s->target[P_DRY] > .5f;
    if (!s->locked[SEC_DRUMS]) {               /* the drums' notes: Fixed (a kit) most often in the genres, seldom in
                                                  the drifting styles */
        /* Fixed / blended (0.3 .. 0.8) / Follow, by style */
        static const float FB[NUM_STYLES][2] = {      /* (the share Fixed, the share blended) */
            [STYLE_WILD] = {.15f, .35f}, [STYLE_HIPHOP] = {.3f, .45f}, [STYLE_GROOVE] = {.15f, .45f},
            [STYLE_AMBIENT] = {.05f, .25f}, [STYLE_COSMIC] = {.05f, .3f}, [STYLE_HOUSE] = {.3f, .45f},
            [STYLE_MACHINE] = {.3f, .45f}, [STYLE_BREAKS] = {.25f, .45f},
            [STYLE_FUNK] = {.3f, .45f}, [STYLE_LATIN] = {.35f, .45f}, [STYLE_INDIE] = {.3f, .45f}};
        int st = s->style % NUM_STYLES; float u = frand(r);
        s->target[P_DR_NOTES] = u < FB[st][0] ? 1.f : u < FB[st][0] + FB[st][1] ? .3f + .5f * frand(r) : 0.f;
    }
    if (g_dry_set) {                           /* (1.0.6) Dry: the effects set to do nothing, so not run (fx.c); their
                                                  other settings rolled as ever, for when they are turned up */
        float *t = s->target;
        t[P_FZ_MIX] = t[P_DL_SEND] = t[P_RV_SEND] = 0.f; t[P_RV_FREEZE] = disc(P_RV_FREEZE, 0);
        t[P_FLT_CUT] = 1.f; t[P_FLT_RES] = t[P_FLT_MORPH] = 0.f;
    }
    if (dry && !s->locked[SEC_DRUMS]) dry_kit(s);
    if (dry && !s->locked[SEC_LFO])            /* (the style's routes to the effects: elsewhere) */
        for (int k = 0; k < MOD_SLOTS; k++) {
            ModSlot *m = &s->slot[k];
            if (m->src && m->dst < NUM_PARAMS && fx_off(m->dst)) {
                g_roll_dry = 1;
                m->dst = (uint8_t)rand_moddable(r);
                if (mod_loop(m->src, m->dst)) m->src = 0;
            }
        }
    if (!s->locked[SEC_KNOBS]) {               /* (a sound roll keeps the knobs' paths: a Dry patch it makes keeps
                                                  them too, and its Shape rides them; a roll of the knobs gives it the kit's) */
        level_paths(s);
        if (dry) dry_shape_paths(s);
    }
}

void params_roll_part(Synth *s, int part, uint32_t seed) {
    if (part == 1 || part == 2) {
        if (!s->locked[SEC_KNOBS]) params_roll_knobs_part(s, part == 1 ? 0 : NUM_PARAMS / 2, part == 1 ? NUM_PARAMS / 2 : NUM_PARAMS);
        return;
    }
    uint32_t *r = &s->rng;
    if (part == 3) {
        apply_style(s);
        style_mod(s);
        /* Wild: a quarter of the rolls take patterns too, from any genre's tree */
        if (s->style == STYLE_WILD && !s->locked[SEC_DRUMS] && frand(r) < .25f) {
            static const uint8_t GG[6] = {PG_HOUSE, PG_BREAKS, PG_HIPHOP, PG_FUNK, PG_LATIN, PG_INDIE};
            roll_patterns(s, GG[(int)(frand(r) * 6.f) % 6], 0, 1);
        }
        roll_finish(s);
        return;
    }
    s->rng = seed * 2654435761u + 1u;
    int was_dry = s->target[P_DRY] > .5f;
    for (int i = 0; i < NUM_PARAMS; i++) {
        const ParamMeta *m = &g_param_meta[i];
        if (s->locked[m->section] || i == P_TEMPER) continue;   /* (locked: left as it is; Temper is the player's: never rolled) */
        s->target[i] = (m->rlo == m->rhi && m->curve != CURVE_DISC) ? m->rlo : roll_one(m, r);
        if (m->rlo == 0 && m->rhi == 0) s->target[i] = m->curve == CURVE_DISC ? disc(i, 0) : 0;
    }
    float *t = s->target;
    /* (1.0.6) a share of the rolls Dry (1.0.7: two in five), in every style: any roll of the sound (a tap of REC too), unless the effects
       are locked. A Dry roll sets the effects to do nothing (roll_finish: they stay usable, turned up); a tap from a
       Dry patch to a wet one rolls them too, as they were off */
    g_dry_set = 0;
    if (!g_lock_fx && (!s->locked[SEC_FX] || !s->locked[SEC_VOICE1] || !s->locked[SEC_DRUMS])) {
        t[P_DRY] = disc(P_DRY, frand(r) < .4f);
        g_dry_set = (uint8_t)(t[P_DRY] > .5f);
    }
    g_roll_dry = (uint8_t)(t[P_DRY] > .5f);
    g_fx_wake = (uint8_t)(!g_lock_fx && s->locked[SEC_FX] && was_dry && !g_roll_dry);
    if (g_fx_wake) for (int i = P_FZ_SUSTAIN; i <= P_DL_TO_RV; i++) {
        const ParamMeta *m = &g_param_meta[i];
        t[i] = (m->rlo == m->rhi && m->curve != CURVE_DISC) ? m->rlo : roll_one(m, r);
        if (m->rlo == 0 && m->rhi == 0) t[i] = m->curve == CURVE_DISC ? disc(i, 0) : 0;
    }
    if (!s->locked[SEC_FX] || g_fx_wake) {
        /* filter type and cutoff are rolled together so the voice never vanishes */
        float u = frand(r);
        if (u < .75f) { t[P_FLT_MORPH] = .08f * frand(r); }
        else if (u < .92f) { t[P_FLT_MORPH] = .4f + .15f * frand(r); t[P_FLT_CUT] = .45f + .25f * frand(r); t[P_FLT_RES] *= .6f; }
        else { t[P_FLT_MORPH] = .75f + .1f * frand(r); t[P_FLT_CUT] = .2f + .2f * frand(r); }
        if (frand(r) < .85f) t[P_RV_TUNED] = disc(P_RV_TUNED, 0);
        if (t[P_DL_FB] > .65f && t[P_DL_SEND] > .4f) t[P_DL_SEND] = .25f;
    }
    roll_together(s);
    params_roll_mod(s);
}

void synth_set_style(Synth *s, int style) { s->style = style < 0 ? 0 : style % NUM_STYLES; }

/* ---------------- public helpers ---------------- */
/* the randomizer: every section not locked, or only those in mask. The generators and the meter start again only
   when their sections are rolled, so rolling the effects leaves the rhythm alone */
static void roll_commit(Synth *s, uint32_t seed, float morph_seconds);
void synth_roll_sections(Synth *s, uint32_t seed, float morph_seconds, uint32_t mask) {
    uint8_t keep[NUM_SECTIONS];
    memcpy(keep, s->locked, sizeof keep);
    g_lock_fx = s->locked[SEC_FX];
    for (int i = 0; i < NUM_SECTIONS; i++) if (!(mask >> i & 1u)) s->locked[i] = 1;
    s->seed = seed;
    params_roll(s, seed);
    roll_commit(s, seed, morph_seconds);
    memcpy(s->locked, keep, sizeof keep);
}

/* The same roll, a part a block (synth_roll_begin, then synth_roll_step at each block's start): run from the audio's
   interrupt, a whole roll in one block (with the rebuild and the new notes after it) could overrun it. Until the last
   part the matrix and the knobs' paths are rolled into a copy of their own (swapped in for each part), so the sound
   never has half a matrix; the targets move at once (the morph glides toward them anyway) */
static struct {
    int part;                                   /* the next part (0 none running) */
    uint32_t seed; float morph; uint8_t keep[NUM_SECTIONS], mask_locked[NUM_SECTIONS];
} g_roll;
static ModSlot g_rslot[MOD_SLOTS] DSP_POOL;
static int8_t g_rkp[NUM_PARAMS][KNOB_WP] DSP_POOL;
static float g_rmac[NUM_MACRO] DSP_POOL;
static void roll_swap(Synth *s) {               /* (the live matrix, paths and macros <-> the roll's) */
    ModSlot t[MOD_SLOTS]; memcpy(t, s->slot, sizeof t); memcpy(s->slot, g_rslot, sizeof t); memcpy(g_rslot, t, sizeof t);
    for (int i = 0; i < NUM_PARAMS; i++) for (int k = 0; k < KNOB_WP; k++) { int8_t x = s->kpath[i][k]; s->kpath[i][k] = g_rkp[i][k]; g_rkp[i][k] = x; }
    for (int m = 0; m < NUM_MACRO; m++) { float x = s->macro[m]; s->macro[m] = g_rmac[m]; g_rmac[m] = x; }
}
void synth_roll_begin(Synth *s, uint32_t seed, float morph_seconds, uint32_t mask) {
    if (g_roll.part) { memcpy(s->locked, g_roll.keep, sizeof g_roll.keep); }   /* (a roll before the last finished: dropped) */
    memcpy(g_roll.keep, s->locked, sizeof g_roll.keep);
    g_lock_fx = s->locked[SEC_FX];
    for (int i = 0; i < NUM_SECTIONS; i++) g_roll.mask_locked[i] = (mask >> i & 1u) ? s->locked[i] : 1;
    memcpy(g_rslot, s->slot, sizeof g_rslot); memcpy(g_rkp, s->kpath, sizeof g_rkp); memcpy(g_rmac, s->macro, sizeof g_rmac);
    g_roll.seed = seed; g_roll.morph = morph_seconds; g_roll.part = 1;
}
int synth_roll_step(Synth *s) {                 /* one part; 1 while a roll is under way */
    if (!g_roll.part) return 0;
    uint8_t lk[NUM_SECTIONS];
    memcpy(lk, s->locked, sizeof lk); memcpy(s->locked, g_roll.mask_locked, sizeof lk);   /* (the roll's locks, for its part) */
    int p = g_roll.part - 1;
    if (p == 0) s->seed = g_roll.seed;
    roll_swap(s);
    params_roll_part(s, p, g_roll.seed);
    roll_swap(s);
    memcpy(s->locked, lk, sizeof lk);
    if (++g_roll.part > ROLL_PARTS) {           /* the last: the matrix, the paths and the macros in, the rest as a roll does */
        roll_swap(s);
        memcpy(lk, s->locked, sizeof lk); memcpy(s->locked, g_roll.mask_locked, sizeof lk);
        roll_commit(s, g_roll.seed, g_roll.morph);
        memcpy(s->locked, lk, sizeof lk);
        g_roll.part = 0;
    }
    return 1;
}

static void roll_commit(Synth *s, uint32_t seed, float morph_seconds) {
    int structural = 0;
    if (s->base[P_SCALE] != s->target[P_SCALE] || s->base[P_ROTATE] != s->target[P_ROTATE] || s->base[P_OCTAVES] != s->target[P_OCTAVES])
        synth_vocab_clear(s);
    for (int i = 0; i < NUM_PARAMS; i++) {
        const ParamMeta *m = &g_param_meta[i];
        if (m->structural) { if (s->base[i] != s->target[i]) structural = 1; }
        else if (m->curve == CURVE_DISC || morph_seconds <= 0.f) s->base[i] = s->target[i];
    }
    float ticks = morph_seconds * SR / (float)SUB;
    s->morph_rate = ticks > 1.f ? 1.f / ticks : 1.f;
    s->morphing = 1;
    if (structural) { s->rebuild_pending = 1; s->duck_tgt = 0.f; }
    if (!s->locked[SEC_METER]) { s->meter.tau = 0.f; s->meter.bar = 0; }
    for (int l = 0; l < NUM_LAYERS; l++) if (!s->locked[l ? SEC_GEN2 : SEC_GEN1]) {   /* the keys held carry over */
        GenSt *g = &s->layer[l].g; uint8_t h[MAX_NODES]; float hv[MAX_NODES];
        memcpy(h, g->held, sizeof h); memcpy(hv, g->held_v, sizeof hv);
        gen_reset(g, seed + 77u * (uint32_t)l);
        memcpy(g->held, h, sizeof h); memcpy(g->held_v, hv, sizeof hv);
    }
}
void synth_randomize(Synth *s, uint32_t seed, float morph_seconds) {
    synth_roll_sections(s, seed, morph_seconds, SEC_ALL);
}

void synth_mutate(Synth *s, float amount) {
    uint32_t *r = &s->rng;
    for (int i = 0; i < NUM_PARAMS; i++) {
        const ParamMeta *m = &g_param_meta[i];
        if (s->locked[m->section] || m->structural) continue;
        if (m->curve == CURVE_DISC) {
            if (frand(r) < amount * .15f && m->rhi > m->rlo) s->target[i] = roll_one(m, r);
            continue;
        }
        float v = s->base[i] + amount * (frand(r) * 2.f - 1.f) * .25f * (m->rhi - m->rlo + .05f);
        s->target[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    if (!s->locked[SEC_LFO])                     /* (a locked matrix is the player's: the routes keep their depths) */
        for (int k = 0; k < MOD_SLOTS; k++) if (s->slot[k].src) s->slot[k].depth *= 1.f + amount * (frand(r) - .5f);
    s->morph_rate = 1.f / (0.5f * SR / SUB);
    s->morphing = 1;
}

void synth_vocab_clear(Synth *s) {
    for (int l = 0; l < NUM_LAYERS; l++) {
        s->layer[l].vocab_mask = 0;
        s->layer[l].vocab_on = 0;
    }
    s->vocab_generation++;
}

void synth_set_param(Synth *s, int id, float v) {
    if (v < 0) v = 0; if (v > 1) v = 1;
    if ((id == P_SCALE || id == P_ROTATE || id == P_OCTAVES) && s->base[id] != v)
        synth_vocab_clear(s);
    s->base[id] = s->target[id] = v;
    if (g_param_meta[id].structural) { s->rebuild_pending = 1; s->duck_tgt = 0.f; }
}
void synth_set_macro(Synth *s, int m, float v) {
    if (m >= 0 && m < NUM_MACRO) s->macro[m] = v < -1 ? -1 : v > 1 ? 1 : v;
}

static const char *SURF_NAMES[] = {"plane","torus","sphere"};
static const char *STYLE_NAMES[] = {"wild","hip-hop","groove","ambient","cosmic","house","machine","breakbeat","funk","latin","indie"};

static int slen(const char *b) { int n = 0; while (b[n]) n++; return n; }
void synth_describe(Synth *s, char *buf, int len) {
    snprintf(buf, len, "[%s] seed %u | %s on %s | 1: %s x%d %s, bank c%d w%d | 2: %s x%d %s, bank c%d w%d | %s %d bpm, dens %d%% threes %d%% swing %d%%",
        STYLE_NAMES[s->style % NUM_STYLES], s->seed, s->scale.name, SURF_NAMES[pint(s, P_SURFACE)],
        gen_name(pint(s, P_G1_FUNC)), pint(s, P_G1_POINTS) + 1, env_name(pint(s, P_G1_ENV)), (int)(pval(s, P_V1_CENTER) * 100.f), (int)(pval(s, P_V1_BAND) * 100.f),
        gen_name(pint(s, P_G2_FUNC)), pint(s, P_G2_POINTS) + 1, env_name(pint(s, P_G2_ENV)), (int)(pval(s, P_V2_CENTER) * 100.f), (int)(pval(s, P_V2_BAND) * 100.f),
        rh_engine_name(pint(s, P_RH_ENGINE)), (int)(pval(s, P_TEMPO) + .5f), (int)(pval(s, P_RH_DENS) * 100.f + .5f),
        (int)(pval(s, P_RH_THREES) * 100.f + .5f), (int)(pval(s, P_RH_SWING) * 100.f + .5f));
    if (pint(s, P_DR_ON)) {
        int n = slen(buf);
        static const char *const LN[NUM_DLAYERS] = {"on layer 1", "on layer 2", "split"};
        snprintf(buf + n, len - n, " | drums %s, %s", LN[pint(s, P_DR_LAYER) % NUM_DLAYERS], rh_assign_name(pint(s, P_RH_ASSIGN)));
    }
    if (pval(s, P_FZ_MIX) > .01f) { int n = slen(buf); snprintf(buf + n, len - n, " | fuzz %d%%", (int)(pval(s, P_FZ_MIX) * 100.f + .5f)); }
    int mi = gen_modulator(s);
    if (mi >= 0) {
        int n = slen(buf);
        snprintf(buf + n, len - n, " | layer %d %s (depth %d%%)", mi + 1, route_name(pint(s, LP(mi, P_G1_ROUTE))),
                 (int)(pval(s, LP(mi, P_G1_ENERGY)) * 100.f + .5f));
    }
}

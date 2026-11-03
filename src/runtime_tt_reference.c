#include "runtime_tt_reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

struct Sample { float time, p[3]; uint32_t flags; };
struct Track { char name[64]; unsigned count, loop_start; struct Sample *samples; };
static struct Track tracks[160];
static unsigned track_count;
struct Cursor { uintptr_t owner; const struct Track *track; unsigned sample, marker; float fraction; int lap; unsigned recovery_reports; int recovering;  unsigned char used_hops[512]; };
static struct Cursor cursors[8];

/* Flat hops, including edges without a recorded drift hold, must not break
 * the current slide. Preserve actual ramp/shortcut climbs and drops. */
static int flat_slide_hop(const struct Track *track, unsigned at) {
    const struct Sample *p = track->samples;
    if (!(p[at].flags & 2)) return 0;
    for (unsigned i = at + 1; i < track->count && p[i].time <= p[at].time + .5f; ++i)
        if (fabsf(p[i].p[2] - p[at].p[2]) > 100.f) return 0;
    return 1;
}

static void clear_tracks(void) {
    for (unsigned i = 0; i < track_count; ++i) free(tracks[i].samples);
    memset(tracks, 0, sizeof tracks);
    memset(cursors, 0, sizeof cursors);
    track_count = 0;
}

int runtime_tt_reference_load(const char *path) {
    clear_tracks();
    if (!path || !*path) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    char magic[8]; uint32_t count;
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, "BBGHOST1", 8) ||
        fread(&count, 4, 1, file) != 1 || count == 0 || count > 160) goto invalid;
    for (unsigned i = 0; i < count; ++i) {
        struct Track *track = &tracks[i];
        if (fread(track->name, 1, 64, file) != 64 || !memchr(track->name, 0, 64) || !track->name[0] ||
            fread(&track->count, 4, 1, file) != 1 || track->count < 2 || track->count > 4096) goto invalid;
        for (const char *c = track->name; *c; ++c)
            if (!(*c >= 'a' && *c <= 'z') && !(*c >= '0' && *c <= '9') && *c != '_') goto invalid;
        for (unsigned j = 0; j < i; ++j) if (!strcmp(track->name, tracks[j].name)) goto invalid;
        track->samples = malloc(track->count * sizeof *track->samples);
        track_count = i + 1;
        if (!track->samples || fread(track->samples, sizeof *track->samples, track->count, file) != track->count) goto invalid;
        for (unsigned j = 0; j < track->count; ++j) {
            struct Sample *p = &track->samples[j];
            if (!isfinite(p->time) || p->time < 0 || p->time > 655.35f || (p->flags & ~0xff21fbu) ||
                (j && p->time < track->samples[j - 1].time)) goto invalid;
            for (int k = 0; k < 3; ++k) if (!isfinite(p->p[k]) || fabsf(p->p[k]) > 1e7f) goto invalid;
        }
        /* Join the finish crossing to its forward continuation on the first
         * recorded lap, not to the stationary starting grid behind the line. */
        const float *end = track->samples[track->count - 1].p;
        const float *previous = track->samples[track->count - 2].p;
        float closest = 2500.f * 2500.f;
        for (unsigned j = 0; j + 1 < track->count / 2; ++j) {
            const float *a = track->samples[j].p, *b = track->samples[j + 1].p;
            float length = 0, dot = 0, heading = 0, ahead = 0;
            for (int k = 0; k < 3; ++k) {
                float d = b[k] - a[k];
                length += d*d; dot += (end[k]-a[k])*d;
                heading += d*(end[k]-previous[k]); ahead += (b[k]-end[k])*(end[k]-previous[k]);
            }
            if (length < 1 || heading <= 0 || ahead < 0) continue;
            float t = fmaxf(0, fminf(1, dot / length)), distance = 0;
            for (int k = 0; k < 3; ++k) { float d = end[k]-a[k]-t*(b[k]-a[k]); distance += d*d; }
            if (distance < closest) { closest = distance; track->loop_start = j + 1; }
        }
    }
    if (fgetc(file) != EOF) goto invalid;
    fclose(file);
    return (int)track_count;
invalid:
    fclose(file);
    clear_tracks();
    return 0;
}

static const struct Track *find_track(const char *map) {
    if (!map) return NULL;
    const char *base = map;
    for (const char *c = map; *c; ++c) if (*c == '/' || *c == '\\') base = c + 1;
    char name[64]; size_t n = 0;
    while (base[n] && base[n] != '.' && n < sizeof name - 1) {
        name[n] = (char)tolower((unsigned char)base[n]); ++n;
    }
    if (base[n] && base[n] != '.') return NULL;
    name[n] = 0;
    for (unsigned i = 0; i < track_count; ++i) if (!strcmp(name, tracks[i].name)) return &tracks[i];
    return NULL;
}

static float distance2(const float *a, const float *b) {
    float sum = 0;
    for (int k = 0; k < 3; ++k) { float d = a[k] - b[k]; sum += d * d; }
    return sum;
}

/* Local progress search prevents crossing onto a different lap or an overlapping
 * bridge. Recovery re-acquires spatially; there is no wall-clock playback. */
int runtime_tt_reference_guide(const char *map, uintptr_t owner, unsigned id, int lap, int level,
                              const float position[3], const float velocity[3],
                              float targets[2][3], unsigned *intent) {
    *intent = 0;
    static const char *const tiers[] = {"tropy", "tropy", "tropy", "oxide", "velo", "developer"};
    if (level < 0 || level > 5 || !map) return 0;
    char key[256];
    if (snprintf(key, sizeof key, "%s__%s", map, tiers[level]) >= (int)sizeof key) return 0;
    const struct Track *track = find_track(key);
    if (!track || !owner || id >= 8) return 0;
    for (int k = 0; k < 3; ++k)
        if (!isfinite(position[k]) || !isfinite(velocity[k])) return 0;
    struct Cursor *cursor = &cursors[id];
    if (cursor->owner != owner || cursor->track != track || lap != cursor->lap) {
        *cursor = (struct Cursor){.owner = owner, .track = track, .sample = track->count, .marker = track->count, .lap = lap};
        printf("Native AI: reference id=%u lap=%d level=%d route=%s\n", id, lap, level, track->name);
    }
    const struct Sample *points = track->samples;
    unsigned best = track->count;
    float best_t = 0, best_distance = INFINITY;
    for (int pass = 0; pass < 2 && best == track->count; ++pass) {
        const int local = pass == 0 && cursor->sample < track->count - 1;
        const float forward = local ?
            velocity[0]*(points[cursor->sample+1].p[0]-points[cursor->sample].p[0]) +
            velocity[1]*(points[cursor->sample+1].p[1]-points[cursor->sample].p[1]) : 0;
        const int advancing = local && forward > 0;
        unsigned lo = advancing ? cursor->sample : local && cursor->sample > 12 ? cursor->sample - 12 : 0;
        unsigned hi = local && cursor->sample + 24 < track->count - 1 ? cursor->sample + 24 : track->count - 1;
        for (unsigned i = lo; i < hi; ++i) {
            float delta[3], length2 = 0, dot = 0;
            for (int k = 0; k < 3; ++k) {
                delta[k] = points[i + 1].p[k] - points[i].p[k];
                length2 += delta[k] * delta[k]; dot += (position[k] - points[i].p[k]) * delta[k];
            }
            if (length2 < 1 || length2 > 2500.f * 2500.f) continue;
            float t = fmaxf(0, fminf(1, dot / length2)), projected[3];
            if (advancing && i == cursor->sample) t = fmaxf(t,cursor->fraction);
            for (int k = 0; k < 3; ++k) projected[k] = points[i].p[k] + t * delta[k];
            if (local && fabsf(position[2] - projected[2]) > 500) continue;
            float d = distance2(position, projected);
            if (d < best_distance) { best = i; best_t = t; best_distance = d; }
        }
        if (!local) break;
        if (best_distance > 1600.f * 1600.f) {
            best = track->count; best_distance = INFINITY;
        }
    }
    if (best == track->count) { cursor->sample = track->count; return 0; }
    cursor->sample = best; cursor->fraction = best_t; cursor->lap = lap;
    float time = points[best].time + best_t * (points[best + 1].time - points[best].time);
    unsigned flags = 0;
    /* Hop edges belong to crossed positions, never the nearest future key. */
    const unsigned nearest = best_t < 1.f ? best : best + 1;
    flags |= points[nearest].flags & 0xff01f9u; /* Current hold/direction, never future jump. */
    const unsigned previous = cursor->marker;
    unsigned first = previous < nearest && nearest - previous <= 4 ? previous + 1 : nearest;
    cursor->marker = nearest;
    const float speed = hypotf(velocity[0], velocity[1]);
    const float tangent_x = points[best + 1].p[0] - points[best].p[0];
    const float tangent_y = points[best + 1].p[1] - points[best].p[1];
    const float tangent_length = hypotf(tangent_x, tangent_y);
    /* Technique resumes inside a narrower corridor; ghost steering is continuous. */
    const float recovery_limit = cursor->recovering ? 120.f : 200.f;
    /* A real jump need not match the ghost's vertical timing. Keep its lane
     * while airborne instead of handing steering to the road below the jump.
     * Keep local height separation; spatial reacquisition still uses 3D. */
    int terrain = 0, pickup_block = 0;
    unsigned start = best > 30 ? best - 30 : 0;
    for (unsigned i = start; i < track->count && points[i].time <= time + 1.2f; ++i)
        if (points[i].time >= time - 1.0f && (points[i].flags & 2) && !flat_slide_hop(track, i)) {
            terrain = 1;
            if (points[i].time>=time-.2f && points[i].time<=time+.45f) pickup_block=1;
        }
    float corridor_distance = best_distance;
    if (terrain && !cursor->recovering && previous < track->count) {
        float z = position[2] - points[best].p[2] - best_t * (points[best + 1].p[2] - points[best].p[2]);
        corridor_distance = fmaxf(0, best_distance - z * z);
    }
    const int recovering = corridor_distance >= recovery_limit * recovery_limit;
    if (!recovering && speed > 100 && tangent_length > 1 &&
        (velocity[0] * tangent_x + velocity[1] * tangent_y) > .85f * speed * tangent_length)
        flags |= TT_REFERENCE_ALIGNED;
    /* Consume each edge once per owner/track/lap, even after projection jitter.
     * Off-path reacquisition consumes but never replays a missed takeoff. */
    if (!(flags & TT_REFERENCE_ALIGNED)) flags = 0;
    for (unsigned i = first; i <= nearest; ++i) {
        const unsigned char bit = (unsigned char)(1u << (i & 7));
        if (!(points[i].flags & 2) || (cursor->used_hops[i >> 3] & bit)) continue;
        cursor->used_hops[i >> 3] |= bit;
        if (!(flags & TT_REFERENCE_ALIGNED)) continue;
        flags |= 2;
        if (flat_slide_hop(track, i)) flags |= TT_REFERENCE_DRIFT_HOP;
    }

    /* Recovery still follows the recording. Alignment gates technique only;
     * it must never hand the route back to native splines. */
    if (recovering != cursor->recovering && cursor->recovery_reports < 32) {
        ++cursor->recovery_reports;
        printf("Native AI: tt recovery id=%u lap=%d rejoining_ghost=%d path_error=%.1f corridor_error=%.1f terrain=%d speed=%.1f sample=%u\n",
               id, lap, recovering, sqrtf(best_distance), sqrtf(corridor_distance), terrain, speed, best);
    }
    cursor->recovering = recovering;

    if (terrain) flags |= 4; /* Pickup detours must not displace a shortcut approach/landing. */
    /* Horizontal alignment is insufficient after landing below a shortcut.
     * The runtime confirms grounded contact before requesting native recovery. */
    const float route_height=points[best].p[2]+best_t*(points[best+1].p[2]-points[best].p[2]);
    if (terrain && route_height-position[2]>200.f) flags |= TT_REFERENCE_BELOW_ROUTE;

    static unsigned takeoff_reports[8];
    if ((flags & 2) && previous != nearest && takeoff_reports[id]++ < 16) {
        float dt = points[best + 1].time - points[best].time;
        float recorded_speed = dt > 0 ? sqrtf(distance2(points[best].p, points[best + 1].p)) / dt : 0;
        printf("Native AI: tt takeoff id=%u lap=%d speed=%.1f reference_speed=%.1f path_error=%.1f\n",
               id, lap, speed, recorded_speed, sqrtf(best_distance));
    }
    for (unsigned i = nearest + 1; i < track->count && points[i].time <= time + .25f; ++i)
        if ((flags & TT_REFERENCE_ALIGNED) && (points[i].flags & 2) &&
            !(cursor->used_hops[i >> 3] & (1u << (i & 7))) && distance2(position, points[i].p) <
            powf(fmaxf(40, fminf(240, speed * (2.f / 30.f))), 2)) {
            flags |= 512; /* Release before the edge. */
            if (distance2(position,points[i].p) > powf(speed/30.f + 10.f,2))
                flags |= TT_REFERENCE_BANK_BEFORE_HOP; /* Leave a separate release tick. */
            if (flat_slide_hop(track, i)) flags |= TT_REFERENCE_DRIFT_HOP;
            break;
        }
    /* Follow the recording's actual lane; pickups are the only route detour. */
    for (unsigned i = best; i < track->count && points[i].time <= time + .8f; ++i)
        if (points[i].flags & 2) {
            if (!flat_slide_hop(track, i)) flags |= 4;
        }
    float result[2][3];
    for (int target = 0; target < 2; ++target) {
        float left;
        /* Geometry depends on speed, never on a hop/alignment flag. At race
         * speed a 150-unit recovery target was only ~50ms ahead and caused
         * overcorrection; crossing its threshold also moved the target abruptly. */
        left = fmaxf(target ? 400.f : 180.f, fminf(target ? 1800.f : 900.f,
                       speed * (target ? .5f : .25f)));
        unsigned i = best;
        float fraction = best_t;
        unsigned steps = 0;
        while (steps++ < track->count) {
            unsigned next = i + 1 < track->count ? i + 1 : track->loop_start;
            float length = sqrtf(distance2(points[i].p, points[next].p));
            if (length > 2500) { fraction = 0; break; } /* Stop at the last continuous ghost point. */
            if (length > .01f && left <= length * (1 - fraction)) { fraction += left / length; break; }
            left -= length * (1 - fraction); i = next; fraction = 0;
        }
        if (steps > track->count) { i = best; fraction = best_t; }
        unsigned next = i + 1 < track->count ? i + 1 : track->loop_start;
        for (int k = 0; k < 3; ++k) result[target][k] = points[i].p[k] + fraction * (points[next].p[k] - points[i].p[k]);
    }
    if (pickup_block) flags |= TT_REFERENCE_BOX_BLOCK;
    /* Rejoin needs real 3D proximity, not the airborne horizontal corridor.
     * The broad terrain window must not hold native control for a whole lap. */
    if ((flags & TT_REFERENCE_ALIGNED) && best_distance<120.f*120.f &&
        !(flags & (2u|512u|TT_REFERENCE_BELOW_ROUTE))) flags |= TT_REFERENCE_REJOIN_READY;
    memcpy(targets, result, sizeof result);
    *intent = flags;
    return 1;
}

/* Match the actual kart's boost number to the local recorded slide cycle.
 * The source may contain early, late or fewer than three boosts. */
float runtime_tt_reference_boost_interval(unsigned id, unsigned completed) {
    if (id>=8 || completed>=3) return 0;
    const struct Cursor *cursor=cursors+id;
    const struct Track *track=cursor->track;
    if (!track || cursor->sample>=track->count) return 0;
    const struct Sample *p=track->samples;
    unsigned begin=cursor->sample, end=begin;
    if (!(p[begin].flags&1)) return 0;
    while (begin && (p[begin-1].flags&1)) --begin;
    while (end+1<track->count && (p[end+1].flags&1)) ++end;
    float since=p[begin].time;
    for (unsigned i=begin;i<=end;++i) {
        if (p[i].flags&8192) since=p[i].time;
        if (!(p[i].flags&8)) continue;
        const float interval=p[i].time-since;
        if (!completed) return interval>0 && interval<10 ? interval : 0;
        --completed; since=p[i].time;
    }
    return 0;
}

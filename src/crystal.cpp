// =============================================================================
// Crystal — a 3D echo chamber shaped like a real crystal.
//
// The chamber is an actual crystal habit: symmetry-expanded Miller-index face
// normals intersected as half-spaces (the Wulff construction), so "material"
// picks a genuine form — pyrite's cube, garnet's rhombic dodecahedron, quartz's
// prism with alternating rhombohedra, sphalerite's tetrahedron (the one form
// with no parallel faces at all, and audibly the most diffuse).
//
// Sound enters at an emitter on the surface (drag it on the display) and is
// heard at four listeners placed around the interior — the quad outs. A worker
// thread ray-traces the geometry into a tap set; the audio thread only ever
// reads a multitap delay plus a feedback delay network, so the cost per sample
// is small no matter how complex the crystal.
//
// Research harness that validated all of this lives in the session scratchpad:
// materials measure 5x apart in spectral flatness at small sizes.
// =============================================================================

#include "plugin.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const float C_AIR   = 343.f;
static const int   CR_NL   = 4;      // listeners = quad outs
static const int   CR_TAPS = 48;     // early specular taps kept per listener
static const int   CR_FDN  = 8;      // feedback delay network size (the tail)
static const int   CR_NPATH = 4;     // ray paths drawn (one particle each)
static const int   CR_PATHPTS = 28;  // bounce points along one ray
static const int   CR_NPULSE = 12;   // pulses in flight on screen
static const int   CR_NE = 2;        // emitters (stereo in → quad out)
static const int   CR_LOOPS = 6;     // resonant pockets a sound can get trapped in
static const int   CR_LOOPBUF = 64000;

// Repeat pitch. The pocket loops feed themselves through a shifter, so each pass
// climbs (or falls) by this interval — the crystal answering itself a third up.
struct Shim { const char* name; float ratio; };
static const Shim CR_SHIMMER[] = {
	{"Off",                        1.f},
	{"Major third up (5:4 just)",  1.25f},                      // the original sound
	{"Major third up",             1.259921f},                  // 2^(4/12)
	{"Minor third up",             1.189207f},
	{"Fifth up",                   1.498307f},
	{"Octave up",                  2.f},
	{"Major third down",           0.793701f},
	{"Octave down",                0.5f},
	{"Random (one per pocket)",    0.f},                        // 0 = rolled per loop
};
static const int CR_NSHIM = (int)(sizeof(CR_SHIMMER) / sizeof(CR_SHIMMER[0]));

// ── vector math ─────────────────────────────────────────────────────────────
struct V3 {
	float x = 0, y = 0, z = 0;
	V3() {}
	V3(float a, float b, float c) : x(a), y(b), z(c) {}
	V3 operator+(const V3& o) const { return V3(x + o.x, y + o.y, z + o.z); }
	V3 operator-(const V3& o) const { return V3(x - o.x, y - o.y, z - o.z); }
	V3 operator*(float s) const { return V3(x * s, y * s, z * s); }
};
static inline float dot3(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float len3(const V3& a) { return std::sqrt(dot3(a, a)); }
static inline V3 norm3(const V3& a) { float l = len3(a); return l > 1e-9f ? a * (1.f / l) : a; }
static inline V3 cross3(const V3& a, const V3& b) {
	return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static V3 rotYX(const V3& p, float ry, float rx) {
	float cy = std::cos(ry), sy = std::sin(ry), cx = std::cos(rx), sx = std::sin(rx);
	V3 a(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);
	return V3(a.x, a.y * cx - a.z * sx, a.y * sx + a.z * cx);
}
// The crystal's orientation is a matrix, not three angles. Euler angles compose
// in a fixed order and lock up; a matrix lets each control be exactly what it
// says — a rotation about that one axis, whatever the crystal is already doing.
struct M3 {
	float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
static inline V3 mul3(const M3& a, const V3& p) {
	return V3(a.m[0] * p.x + a.m[1] * p.y + a.m[2] * p.z,
	          a.m[3] * p.x + a.m[4] * p.y + a.m[5] * p.z,
	          a.m[6] * p.x + a.m[7] * p.y + a.m[8] * p.z);
}
static inline M3 mulM(const M3& a, const M3& b) {
	M3 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			float v = 0.f;
			for (int k = 0; k < 3; k++) v += a.m[i * 3 + k] * b.m[k * 3 + j];
			r.m[i * 3 + j] = v;
		}
	return r;
}
static inline M3 axisRot(int axis, float t) {
	float c = std::cos(t), s = std::sin(t);
	M3 r;
	if (axis == 0)      { r.m[4] = c; r.m[5] = -s; r.m[7] = s; r.m[8] = c; }
	else if (axis == 1) { r.m[0] = c; r.m[2] =  s; r.m[6] = -s; r.m[8] = c; }
	else                { r.m[0] = c; r.m[1] = -s; r.m[3] = s; r.m[4] = c; }
	return r;
}
// repeated small rotations drift off the orthogonal manifold; pull them back
static inline void orthonormalize(M3& a) {
	V3 x(a.m[0], a.m[3], a.m[6]), y(a.m[1], a.m[4], a.m[7]);
	x = norm3(x);
	y = norm3(y - x * dot3(x, y));
	V3 z = cross3(x, y);
	a.m[0] = x.x; a.m[3] = x.y; a.m[6] = x.z;
	a.m[1] = y.x; a.m[4] = y.y; a.m[7] = y.z;
	a.m[2] = z.x; a.m[5] = z.y; a.m[8] = z.z;
}

// The speakers stand on a floor, arranged around the crystal. Horizontal here
// means constant y, since the projection sends +y up the screen and z into it.
static const float FLOOR_Y  = 0.f;      // the plane cuts the crystal in half
static const float SPK_RING = 1.75f;
static inline V3 speakerPos(int i) {
	float az = (45.f + i * 90.f) * (float)M_PI / 180.f;
	return V3(std::cos(az) * SPK_RING, FLOOR_Y, std::sin(az) * SPK_RING);
}

// ── crystal geometry ────────────────────────────────────────────────────────
// A crystal is a set of half-spaces {x : n·x <= d}. A CLUSTER is several such
// bodies intergrown — a union, not a convex solid. That is only tractable
// because the tracer shoots rays: a ray exits the union at the furthest exit of
// whichever bodies contain it, so crevices between the points are real, and a
// ray can get caught inside one narrow prism or wander between several.
struct Plane { V3 n; float d; };

struct Body {
	std::vector<Plane> faces;
	std::vector<V3> verts;
	std::vector<std::pair<int, int>> edges;
	float volume = 1.f, area = 1.f, inradius = 1.f;

	void add(V3 n, float d) { faces.push_back({norm3(n), d}); }
	void addPair(V3 n, float d1, float d2) { add(n, d1); add(n * -1.f, d2); }
	void addSigns(float a, float b, float c, float d) {
		for (int i = 0; i < 8; i++) {
			V3 n(a * ((i & 1) ? -1 : 1), b * ((i & 2) ? -1 : 1), c * ((i & 4) ? -1 : 1));
			bool dup = false;
			for (auto& f : faces) if (len3(f.n - norm3(n)) < 1e-4f) { dup = true; break; }
			if (!dup) add(n, d);
		}
	}
	void addPerms(float a, float b, float c, float d) {
		addSigns(a, b, c, d); addSigns(b, c, a, d); addSigns(c, a, b, d);
	}
	void addRing(int n, float polarDeg, float startDeg, float d) {
		for (int k = 0; k < n; k++) {
			float az = (startDeg + k * 360.f / n) * (float)M_PI / 180.f;
			float po = polarDeg * (float)M_PI / 180.f;
			add(V3(std::cos(az) * std::sin(po), std::sin(az) * std::sin(po), std::cos(po)), d);
		}
	}
	bool contains(const V3& p, float eps = 1e-4f) const {
		for (auto& f : faces) if (dot3(f.n, p) > f.d + eps) return false;
		return true;
	}
	// move/scale/turn this body: n·x <= d becomes (Rn)·x <= d*s + (Rn)·T
	void place(V3 t, float s, float ry, float rx) {
		for (auto& f : faces) {
			V3 n = rotYX(f.n, ry, rx);
			f.d = f.d * s + dot3(n, t);
			f.n = n;
		}
	}

	// Vertices are plane triples inside every other half-space; edges are plane
	// pairs sharing exactly two of them. O(F^3) on <=20 faces.
	void buildMesh() {
		verts.clear(); edges.clear();
		int F = (int)faces.size();
		std::vector<std::vector<int>> onPlane(F);
		for (int i = 0; i < F; i++)
			for (int j = i + 1; j < F; j++)
				for (int k = j + 1; k < F; k++) {
					const V3 &a = faces[i].n, &b = faces[j].n, &c = faces[k].n;
					V3 bc = cross3(b, c);
					float det = dot3(a, bc);
					if (std::fabs(det) < 1e-6f) continue;
					V3 p = (bc * faces[i].d + cross3(c, a) * faces[j].d + cross3(a, b) * faces[k].d) * (1.f / det);
					if (!contains(p)) continue;
					int vi = -1;
					for (int v = 0; v < (int)verts.size(); v++) if (len3(verts[v] - p) < 1e-4f) { vi = v; break; }
					if (vi < 0) { vi = (int)verts.size(); verts.push_back(p); }
					for (int m : {i, j, k})
						if (std::find(onPlane[m].begin(), onPlane[m].end(), vi) == onPlane[m].end())
							onPlane[m].push_back(vi);
				}
		for (int i = 0; i < F; i++)
			for (int j = i + 1; j < F; j++) {
				std::vector<int> sh;
				for (int v : onPlane[i])
					if (std::find(onPlane[j].begin(), onPlane[j].end(), v) != onPlane[j].end()) sh.push_back(v);
				if (sh.size() == 2) edges.push_back({sh[0], sh[1]});
			}
		volume = 0.f; area = 0.f;
		V3 cen;
		for (auto& v : verts) cen = cen + v;
		if (!verts.empty()) cen = cen * (1.f / verts.size());
		for (int i = 0; i < F; i++) {
			if (onPlane[i].size() < 3) continue;
			V3 c;
			for (int v : onPlane[i]) c = c + verts[v];
			c = c * (1.f / onPlane[i].size());
			V3 ref = norm3(verts[onPlane[i][0]] - c), up = faces[i].n;
			std::vector<std::pair<float, int>> ord;
			for (int v : onPlane[i]) {
				V3 r = verts[v] - c;
				ord.push_back({std::atan2(dot3(cross3(ref, r), up), dot3(ref, r)), v});
			}
			std::sort(ord.begin(), ord.end());
			for (size_t k = 0; k < ord.size(); k++) {
				V3 a = verts[ord[k].second], b = verts[ord[(k + 1) % ord.size()].second];
				area += 0.5f * len3(cross3(a - c, b - c));
				volume += std::fabs(dot3(c - cen, cross3(a - c, b - c))) / 6.f;
			}
		}
		if (volume < 1e-6f) volume = 1e-6f;
		if (area < 1e-6f) area = 1e-6f;
		inradius = 1e9f;
		for (auto& f : faces) inradius = std::min(inradius, std::fabs(f.d - dot3(f.n, cen)));
		if (inradius > 1e8f) inradius = 1.f;
	}
};

struct Geom {
	std::vector<Body> bodies;
	float volume = 1.f, area = 1.f, inradius = 1.f;

	void finish() {
		volume = 0.f; area = 0.f;
		for (auto& b : bodies) { b.buildMesh(); volume += b.volume; area += b.area; }
		float R = 1e-4f;
		for (auto& b : bodies) for (auto& v : b.verts) R = std::max(R, len3(v));
		for (auto& b : bodies) { for (auto& f : b.faces) f.d /= R; b.buildMesh(); }
		volume = 0.f; area = 0.f;
		for (auto& b : bodies) { volume += b.volume; area += b.area; }
		inradius = bodies.empty() ? 1.f : bodies[0].inradius;
	}

	// Nearest wall of the UNION. A ray leaves a body only if the exit point is
	// not inside a neighbour; if it is, it carries on through the join.
	bool hit(const V3& p, const V3& v, float& tOut, V3& nOut) const {
		float t = 0.f; V3 hn(0, 0, 1); bool any = false;
		for (int iter = 0; iter < 8; iter++) {
			V3 q = p + v * (t + 1e-5f);
			float best = -1.f; V3 bn;
			for (auto& b : bodies) {
				if (!b.contains(q)) continue;
				float te = 1e9f; V3 en;
				for (auto& f : b.faces) {
					float dn = dot3(f.n, v);
					if (dn <= 1e-6f) continue;
					float tt = (f.d - dot3(f.n, q)) / dn;
					if (tt > 1e-6f && tt < te) { te = tt; en = f.n; }
				}
				if (te < 1e8f && t + 1e-5f + te > best) { best = t + 1e-5f + te; bn = en; }
			}
			if (best < 0.f) break;
			t = best; hn = bn; any = true;
		}
		if (!any) return false;
		tOut = t; nOut = hn; return true;
	}
	float surfaceDist(const V3& dir) const {          // where the emitter sits
		float t = 1e9f;
		for (auto& f : bodies[0].faces) { float dn = dot3(f.n, dir); if (dn > 1e-6f) t = std::min(t, f.d / dn); }
		return (t > 1e8f) ? 1.f : t;
	}
};

static const int CR_NMAT = 16;
static const char* CR_MATNAME[CR_NMAT] = {
	"Tetrahedron (sphalerite)", "Cube (pyrite)", "Rhombohedron (calcite)",
	"Octahedron (fluorite)", "Hexagonal prism (beryl)", "Tetragonal (zircon)",
	"Dodecahedron (garnet)", "Pyritohedron (pyrite)", "Orthorhombic (topaz)",
	"Monoclinic (gypsum)", "Triclinic (kyanite)", "Quartz (trigonal)",
	"Quartz cluster", "Pyrite cluster", "Calcite cluster", "Druse (many points)"
};

static void baseHabit(Body& g, int kind) {
	switch (kind) {
		case 0: g.add(V3(1, 1, 1), 1.f); g.add(V3(1, -1, -1), 1.f);
		        g.add(V3(-1, 1, -1), 1.f); g.add(V3(-1, -1, 1), 1.f); break;
		case 1: g.addPerms(1, 0, 0, 1.f); break;
		case 2: g.addRing(3, 44.f, 0.f, 1.f); g.addRing(3, 136.f, 60.f, 1.f); break;
		case 3: g.addSigns(1, 1, 1, 1.f); break;
		case 4: g.addRing(6, 90.f, 0.f, 0.85f); g.addPair(V3(0, 0, 1), 1.2f, 1.2f); break;
		case 5: g.addRing(4, 90.f, 0.f, 0.75f);
		        g.addRing(4, 42.f, 45.f, 1.1f); g.addRing(4, 138.f, 45.f, 1.1f); break;
		case 6: g.addPerms(1, 1, 0, 1.f); break;
		case 7: for (int k = 0; k < 4; k++) {
			        float a1 = (k & 1) ? -1.f : 1.f, b1 = (k & 2) ? -2.f : 2.f;
			        g.add(V3(0.f, a1, b1), 1.f); g.add(V3(b1, 0.f, a1), 1.f); g.add(V3(a1, b1, 0.f), 1.f);
		        } break;
		case 8: g.addPair(V3(1, 0, 0), 0.78f, 0.78f); g.addPair(V3(0, 1, 0), 1.00f, 1.00f);
		        g.addPair(V3(0, 0, 1), 1.30f, 1.30f);
		        g.addRing(4, 44.f, 45.f, 1.18f); g.addRing(4, 136.f, 45.f, 1.18f); break;
		case 9: g.addPair(norm3(V3(1.f, 0.f, 0.32f)), 0.88f, 0.88f);
		        g.addPair(V3(0, 1, 0), 1.02f, 1.02f);
		        g.addPair(norm3(V3(-0.30f, 0.f, 1.f)), 1.14f, 1.14f);
		        g.addRing(4, 48.f, 30.f, 1.22f); g.addRing(4, 132.f, 70.f, 1.22f); break;
		case 10: g.addPair(norm3(V3(1.0f, 0.12f, 0.05f)), 1.00f, 0.86f);
		         g.addPair(norm3(V3(0.18f, 1.0f, 0.11f)), 1.31f, 1.09f);
		         g.addPair(norm3(V3(0.09f, 0.27f, 1.0f)), 0.92f, 1.24f); break;
		default: g.addRing(6, 90.f, 0.f, 0.42f);                    // quartz point, elongated
		         g.addPair(V3(0, 0, 1), 1.35f, 1.35f);
		         g.addRing(3, 30.f, 0.f, 1.18f); g.addRing(3, 30.f, 60.f, 1.24f); break;
	}
}

static Geom makeMaterial(int idx) {
	Geom g;
	int m = clamp(idx, 0, CR_NMAT - 1);
	if (m < 12) {
		Body b; baseHabit(b, m); g.bodies.push_back(b);
		g.finish();
		return g;
	}
	// ── clusters: real intergrown bodies, each its own crystal ────────────────
	struct Sp { float x, y, z, s, ry, rx; };
	if (m == 12) {                                   // quartz: a spray of points
		static const Sp sp[5] = {{0,0,0, 1.00f, 0.f, 0.f}, {0.55f,0.15f,-0.35f, 0.72f, 0.9f, 0.55f},
		                         {-0.5f,0.35f,-0.3f, 0.64f, 2.1f, -0.5f}, {0.15f,-0.6f,-0.25f, 0.58f, 3.6f, 0.7f},
		                         {-0.25f,-0.2f,0.5f, 0.5f, 5.0f, -0.85f}};
		for (auto& q : sp) { Body b; baseHabit(b, 11); b.place(V3(q.x, q.y, q.z), q.s, q.ry, q.rx); g.bodies.push_back(b); }
	} else if (m == 13) {                            // pyrite: interpenetrating cubes
		static const Sp sp[4] = {{0,0,0, 1.f, 0.f, 0.f}, {0.62f,0.5f,0.2f, 0.78f, 0.5f, 0.3f},
		                         {-0.55f,0.3f,-0.45f, 0.7f, 1.1f, -0.4f}, {0.1f,-0.6f,0.45f, 0.66f, 2.3f, 0.6f}};
		for (auto& q : sp) { Body b; baseHabit(b, 1); b.place(V3(q.x, q.y, q.z), q.s, q.ry, q.rx); g.bodies.push_back(b); }
	} else if (m == 14) {                            // calcite: stacked rhombohedra
		static const Sp sp[4] = {{0,0,0, 1.f, 0.f, 0.f}, {0.5f,-0.35f,0.3f, 0.8f, 0.7f, 0.25f},
		                         {-0.45f,0.4f,0.25f, 0.75f, 2.6f, -0.3f}, {0.05f,0.5f,-0.5f, 0.62f, 4.2f, 0.5f}};
		for (auto& q : sp) { Body b; baseHabit(b, 2); b.place(V3(q.x, q.y, q.z), q.s, q.ry, q.rx); g.bodies.push_back(b); }
	} else {                                         // druse: a plate crusted with points
		Body base; baseHabit(base, 8); base.place(V3(0.f, 0.f, -0.55f), 0.85f, 0.f, 0.f);
		g.bodies.push_back(base);
		for (int k = 0; k < 5; k++) {
			float a = k * 1.2566f;
			Body b; baseHabit(b, 11);
			b.place(V3(std::cos(a) * 0.5f, std::sin(a) * 0.5f, 0.35f), 0.42f, a, 0.32f);
			g.bodies.push_back(b);
		}
	}
	g.finish();
	return g;
}

// ── the traced response: what the audio thread actually plays ───────────────
// One traced ray path, in unit-crystal coordinates, for the display to animate.
struct PathViz {
	int n = 0, emitter = 0;
	V3 pt[CR_PATHPTS];
	float cum[CR_PATHPTS] = {};      // cumulative metres from the emitter
};

struct TapSet {
	PathViz path[CR_NPATH];
	int   nPaths = 0;
	float pathSecs = 0.f;            // longest path, in real seconds
	int   n[CR_NE][CR_NL] = {};
	int   delay[CR_NE][CR_NL][CR_TAPS] = {};
	float gain[CR_NE][CR_NL][CR_TAPS] = {};
	// Independent circuits: each is a pathway the sound can get caught in, with its
	// own length, its own survival rate and its own corner of the room to emerge
	// from. Short pockets chatter, long ones toll — together they make a repeat
	// train that breathes instead of marching.
	int   loopDelay[CR_NE][CR_LOOPS] = {};
	float loopGain[CR_NE][CR_LOOPS] = {};
	int   loopOut[CR_NE][CR_LOOPS] = {};
	float loopScale[CR_NE][CR_LOOPS] = {};   // shares an output with how many others
	int   fdnDelay[CR_FDN] = {};
	float fdnFb = 0.5f;                 // per-line feedback for the requested T60
	float earlyGain = 1.f;              // RMS tap gain — the loop's realistic open gain
	float timeScale = 1.f;              // DELAY mode stretches every arrival by this
	float preDelay = 0.f;               // seconds until the tail starts
	bool  ready = false;
};

// Trace the chamber by letting rays LOOSE inside it. A ray leaves the emitter,
// finds the nearest wall, and reflects — over and over. Every wall strike sheds
// an echo toward each listener, so a new echo emerges each time the sound
// reaches a side, and the ray's whole meandering route is what the display draws.
//
// (This replaced an image-source solver, which is more exact for the first few
// reflections but by construction only ever produced short emitter→wall→listener
// hops: a handful of corners, no long wandering path, and nothing to get trapped
// in. Deep bouncing is the point here, so rays win.)
static const int CR_RAYS = 7, CR_BOUNCE = 26;

static void traceInto(TapSet& ts, const Geom& g, float sizeM, float absorb,
                      float tail, const M3& head, const V3* emitDir, float sr,
                      int nEchoes, bool delayMode, float baseDelay, float maxDel) {
	// The crystal is an object in a room, and the four listeners are speakers
	// standing OUTSIDE it. Rotating the crystal therefore turns its faces against
	// fixed microphones, which is what makes rotation audible: a wall strike that
	// used to radiate at speaker A now radiates at B, and the trajectory appears
	// to swing round the quad image.
	Geom sc = g;                                 // this crystal at the requested size, in the ROOM
	for (auto& b : sc.bodies)
		for (auto& f : b.faces) { f.n = mul3(head, f.n); f.d *= sizeM; }

	V3 L[CR_NL];
	for (int i = 0; i < CR_NL; i++) L[i] = speakerPos(i) * sizeM;

	for (int em = 0; em < CR_NE; em++) {
		V3 ed = norm3(mul3(head, emitDir[em]));   // rides with the crystal
		V3 emit = ed * (sc.surfaceDist(ed) * 0.97f);

		// One event, heard four times. Collect the STRIKES rather than four separate
		// tap lists: every listener then hears the same reflections, each at its own
		// level and arrival time — which is what an array of mics in a room hears.
		// Letting each listener keep its own strongest arrivals instead produced
		// four uncorrelated reverbs of near-identical level, and no image at all.
		struct Strike { float t0; float amp; V3 p; V3 n; bool wall; };
		std::vector<Strike> strikes;
		strikes.push_back({0.f, 1.f, emit, V3(), false});       // the direct sound

		for (int r = 0; r < CR_RAYS; r++) {
			// deterministic fan (golden angle) — the same crystal always sounds the
			// same, and a retrace never jolts the tail
			float gr = 2.39996323f * (r + em * 0.37f);
			float cz = 1.f - 2.f * (r + 0.5f) / CR_RAYS;
			float sr2 = std::sqrt(std::max(0.f, 1.f - cz * cz));
			V3 v = norm3(V3(std::cos(gr) * sr2, std::sin(gr) * sr2, cz));
			if (dot3(v, ed) > 0.f) v = v * -1.f;                // fire into the crystal
			V3 p = emit;
			float pathLen = 0.f, amp = 1.f;

			PathViz* pv = nullptr;
			if (ts.nPaths < CR_NPATH && (r % 2) == 0) {
				pv = &ts.path[ts.nPaths++];
				pv->emitter = em; pv->n = 0;
				pv->pt[pv->n] = emit; pv->cum[pv->n] = 0.f; pv->n++;
			}

			for (int b = 0; b < CR_BOUNCE; b++) {
				float tMin; V3 hn;
				if (!sc.hit(p, v, tMin, hn)) break;
				p = p + v * tMin;
				pathLen += tMin;
				amp *= (1.f - absorb);
				if (amp < 2e-4f) break;

				if (pv && pv->n < CR_PATHPTS) { pv->pt[pv->n] = p; pv->cum[pv->n] = pathLen; pv->n++; }

				strikes.push_back({pathLen, amp, p, hn, true});
				v = norm3(v - hn * (2.f * dot3(v, hn)));
			}
			if (pv) {
				ts.pathSecs = std::max(ts.pathSecs, pv->cum[pv->n - 1] / C_AIR);
				for (int k = 0; k < pv->n; k++) pv->pt[k] = pv->pt[k] * (1.f / std::max(sizeM, 1e-6f));
			}
		}

		// how strongly a strike radiates toward one listener, and how far it goes
		auto weigh = [&](const Strike& st, int li, float& d) {
			V3 toL = L[li] - st.p;
			d = std::max(len3(toL), 0.05f);
			if (!st.wall) return st.amp / d;                   // the emitter is omni
			float f = dot3(norm3(toL), st.n);
			if (f <= 0.f) return 0.f;                          // behind the face: blocked
			return st.amp * f * f * f / d;                     // a face fires where it points
		};

		std::sort(strikes.begin(), strikes.end(),
		          [](const Strike& a, const Strike& b) { return a.amp > b.amp; });

		float ref = 1e-9f, tmin = 1e9f;
		for (auto& st : strikes)
			for (int li = 0; li < CR_NL; li++) {
				float d, g = weigh(st, li, d);
				if (g <= 0.f) continue;
				ref = std::max(ref, g);
				tmin = std::min(tmin, (st.t0 + d) / C_AIR);
			}
		float tsc = (delayMode && tmin < 1e8f && tmin > 1e-9f) ? baseDelay / tmin : 1.f;
		ts.timeScale = tsc;

		int got[CR_NL] = {}; float sum[CR_NL] = {};
		float acc[CR_TAPS]; int nAcc = 0;                      // events accepted so far
		// A trapped pocket is a resonance of the crystal's own geometry, so its
		// period is the path INSIDE the crystal — not the path to a listener, which
		// changes as the thing rotates.
		struct Ev { float t; float g; int li; };
		std::vector<Ev> ev;
		float minGap = delayMode ? baseDelay * 0.15f : 0.004f;
		int keep = std::min(CR_TAPS, nEchoes);
		for (size_t c = 0; c < strikes.size() && nAcc < keep; c++) {
			const Strike& st = strikes[c];
			float d[CR_NL], g[CR_NL], tMean = 0.f; int nHear = 0;
			for (int li = 0; li < CR_NL; li++) {
				g[li] = weigh(st, li, d[li]);
				if (g[li] > 0.f) { tMean += (st.t0 + d[li]) / C_AIR; nHear++; }
			}
			if (!nHear) continue;
			tMean = tMean / nHear * tsc;
			if (tMean > maxDel) continue;
			bool close = false;                                // one event, not a cluster
			for (int q = 0; q < nAcc; q++)
				if (std::fabs(tMean - acc[q]) < minGap) { close = true; break; }
			if (close) continue;
			acc[nAcc++] = tMean;
			if (st.wall) {
				float best = 0.f; int bl = 0;
				for (int li = 0; li < CR_NL; li++) if (g[li] > best) { best = g[li]; bl = li; }
				ev.push_back({st.t0 / C_AIR * tsc, best / ref, bl});
			}
			float sign = (nAcc & 1) ? 1.f : -1.f;              // per EVENT, so the four stay correlated
			for (int li = 0; li < CR_NL; li++) {
				if (g[li] <= 0.f || got[li] >= CR_TAPS) continue;
				float t = (st.t0 + d[li]) / C_AIR * tsc;
				float gn = g[li] / ref;
				ts.delay[em][li][got[li]] = clamp((int)(t * sr), 1, (int)(maxDel * sr));
				ts.gain[em][li][got[li]] = sign * gn;
				sum[li] += gn * gn;
				got[li]++;
			}
		}
		for (int li = 0; li < CR_NL; li++) {
			ts.n[em][li] = got[li];
			ts.earlyGain = std::max(ts.earlyGain, std::max(std::sqrt(sum[li]), 1.f));
		}

		// trapped pockets: spread across the arrival range so their periods differ
		{
			std::sort(ev.begin(), ev.end(),
			          [](const Ev& a, const Ev& b) { return a.t < b.t; });
			for (int k = 0; k < CR_LOOPS; k++) {
				if (ev.empty()) { ts.loopDelay[em][k] = 1000 + k * 137; ts.loopGain[em][k] = 0.5f;
				                  ts.loopOut[em][k] = k % CR_NL; continue; }
				size_t pick = (size_t)((k + 0.5f) / CR_LOOPS * ev.size());
				pick = std::min(pick, ev.size() - 1);
				ts.loopDelay[em][k] = clamp((int)(ev[pick].t * sr), 32, CR_LOOPBUF - 4);
				ts.loopGain[em][k] = clamp(0.45f + 0.55f * ev[pick].g, 0.2f, 1.f);
				ts.loopOut[em][k] = ev[pick].li;           // where that echo was loudest
			}
			int cnt[CR_NL] = {};
			for (int k = 0; k < CR_LOOPS; k++) cnt[ts.loopOut[em][k]]++;
			for (int k = 0; k < CR_LOOPS; k++)
				ts.loopScale[em][k] = 1.f / std::sqrt((float)std::max(cnt[ts.loopOut[em][k]], 1));
		}
	}

	// Tail from the real geometry: Sabine on this crystal's volume and area.
	float V = g.volume * sizeM * sizeM * sizeM, S = g.area * sizeM * sizeM;
	float t60 = 0.161f * V / std::max(S * std::max(absorb, 0.01f), 1e-6f);
	t60 = clamp(t60 * (0.35f + 2.6f * tail), 0.05f, 12.f);
	float mfp = 4.f * V / S;
	ts.preDelay = clamp(mfp / C_AIR, 0.0005f, 0.25f);
	static const float SPREAD[CR_FDN] = {1.f, 1.17f, 1.32f, 1.51f, 1.73f, 1.94f, 2.21f, 2.47f};
	float base = clamp(mfp / C_AIR, 0.0016f, 0.09f);
	float avg = 0.f;
	for (int i = 0; i < CR_FDN; i++) {
		ts.fdnDelay[i] = std::max(23, (int)(base * SPREAD[i] * sr));
		avg += ts.fdnDelay[i] / (float)CR_FDN;
	}
	ts.fdnFb = clamp(std::pow(10.f, -3.f * (avg / sr) / t60), 0.f, 0.985f);
	ts.ready = true;
}

// ── module ──────────────────────────────────────────────────────────────────
struct Crystal : Module {
	// NOTE: Rack serialises params/ports POSITIONALLY. Only ever APPEND here.
	enum ParamId {
		SIZE_PARAM, DAMP_PARAM, MATERIAL_PARAM, TAIL_PARAM,
		ROTY_PARAM, ROTX_PARAM, MIX_PARAM,
		EMIT_AZ_PARAM, EMIT_EL_PARAM,
		PING_PARAM, PITCH_PARAM, DECAY_PARAM,
		ECHOES_PARAM, FEEDBACK_PARAM,
		EMIT_B_AZ_PARAM, EMIT_B_EL_PARAM, HEAD_A_PARAM, HEAD_B_PARAM, NAVSPEED_PARAM,
		MODE_PARAM, SPIN_Z_PARAM, SPIN_X_PARAM, SPIN_Y_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		AUDIO_INPUT, SIZE_INPUT, DAMP_INPUT, MATERIAL_INPUT, TAIL_INPUT,
		ROT_INPUT, PING_INPUT, VOCT_INPUT, FEEDBACK_INPUT,
		AUDIO_B_INPUT, AX_INPUT, AY_INPUT, BX_INPUT, BY_INPUT, MIX_INPUT,
		INPUTS_LEN
	};
	enum OutputId { ENUMS(QUAD_OUTPUT, CR_NL), OUTPUTS_LEN };
	enum LightId { PING_LIGHT, LIGHTS_LEN };

	// delay line shared by every tap
	std::vector<float> buf[CR_NE];
	int wr = 0, bufLen = 0;

	// tail
	std::vector<float> fdn[CR_FDN];
	int fdnWr[CR_FDN] = {}, fdnLen[CR_FDN] = {};
	float fdnLp[CR_FDN] = {};
	float hfLp[CR_NL] = {};

	// double-buffered tap sets, swapped by the worker with a crossfade
	TapSet sets[2];
	std::atomic<int> active{0};
	std::atomic<bool> busy{false};
	std::atomic<bool> dirty{true};
	float fade = 0.f;                       // 1 → fully on the previous set
	int prevSet = 0, pendingSet = 0;
	std::atomic<bool> swapPending{false};
	float watchLast[11] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f,
	                       1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
	std::thread worker;
	std::mutex geomMutex;
	Geom geom;
	int geomMat = -1;

	// Internal exciter — Chime's struck bar voice. One per emitter, so a ping can
	// alternate sides: a bar still ringing at A is never dragged over to B.
	float exPhase[CR_NE][3] = {}, exEnv[CR_NE][3] = {};
	float exFreq[CR_NE] = {261.63f, 261.63f};   // latched at the ping (sample & hold)
	float exAtk[CR_NE] = {};                    // short attack window, so a retrigger never steps
	int   pingSide = 0;                         // which emitter the next ping strikes
	bool  pingAlternate = true;
	dsp::SchmittTrigger pingTrig, pingBtn;
	float pingFlash = 0.f;
	std::atomic<int> dispPingSide{0};

	// display mirrors
	std::atomic<float> dispRotY{0.4f}, dispRotX{0.3f};
	// the crystal's heading: integrated from the SPIN rates, shared with the display
	M3 headM;
	float headOdo = 0.f;                 // total turning, so the watch list sees motion
	std::atomic<float> dispHead[9];

	// Envelope follower drives the visualisation: each onset launches a pulse
	// that the display walks along the traced ray paths.
	float env = 0.f, envSlow = 0.f, sinceOnset = 1e6f, sinceTrace = 1e6f;
	std::atomic<float> vizTime{0.f};
	std::atomic<float> pulseT[CR_NPULSE];       // spawn time, <0 = free slot
	std::atomic<float> pulseLvl[CR_NPULSE];
	int pulseNext = 0;
	std::vector<float> loopBuf[CR_NE][CR_LOOPS];
	int   loopWr[CR_NE][CR_LOOPS] = {};
	float loopDelSm[CR_NE][CR_LOOPS] = {};      // glided, so a moving emitter never steps
	float panSm[CR_NE][CR_LOOPS][CR_NL] = {};   // ramped pocket panning
	// Repeat pitch: a real shifter in the pocket loop, so every pass round is
	// transposed by the same interval and the repeats climb (or fall) away. This
	// used to happen by accident, when the pocket delays glided toward a moving
	// target; now it is deliberate and in tune.
	int   shimmer = 0;
	bool  panelDraw = false;     // draw straight onto the faceplate, no dark screen
	float psPhase[CR_NE][CR_LOOPS] = {};
	// Random gives each pocket its own interval, held rather than re-rolled, so the
	// repeats climb away as a chord instead of a scramble.
	int   psPick[CR_NE][CR_LOOPS] = {};
	void rollShimmer() {
		for (int e = 0; e < CR_NE; e++)
			for (int k = 0; k < CR_LOOPS; k++)
				psPick[e][k] = 1 + (int)(random::uniform() * (CR_NSHIM - 2));
	}
	float loopLp[CR_NE][CR_LOOPS] = {};
	float fbDcX[CR_NE] = {}, fbDcY[CR_NE] = {};
	// A one-pole lowpass inside a feedback loop has its highest gain at DC, and
	// tanh on an asymmetric signal keeps making more. Block it in the loop, not
	// just on the way in.
	float pkDcX[CR_NE][CR_LOOPS] = {}, pkDcY[CR_NE][CR_LOOPS] = {};
	float fdDcX[CR_FDN] = {}, fdDcY[CR_FDN] = {};
	float outDcX[CR_NL] = {}, outDcY[CR_NL] = {};
	float dryDcX = 0.f, dryDcY = 0.f;

	Crystal() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SIZE_PARAM, std::log2(0.06f), std::log2(24.f), std::log2(1.2f), "Size", " m", 2.f);
		configParam(DAMP_PARAM, 0.01f, 0.6f, 0.12f, "Damping (absorbed per bounce)", "%", 0.f, 100.f);
		{
			std::vector<std::string> mn;
			for (int i = 0; i < CR_NMAT; i++) mn.push_back(CR_MATNAME[i]);
			configSwitch(MATERIAL_PARAM, 0.f, (float)(CR_NMAT - 1), 1.f, "Material", mn);
		}
		getParamQuantity(MATERIAL_PARAM)->snapEnabled = true;
		configParam(TAIL_PARAM, 0.f, 1.f, 0.35f, "Tail", "%", 0.f, 100.f);
		configParam(ROTY_PARAM, -(float)M_PI, (float)M_PI, 0.4f, "View — yaw (drag the display)");
		configParam(ROTX_PARAM, -1.4f, 1.4f, 0.45f, "View — pitch (drag the display)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.6f, "Dry / wet", "%", 0.f, 100.f);
		configParam(EMIT_AZ_PARAM, -(float)M_PI, (float)M_PI, 0.6f, "Emitter azimuth");
		configParam(EMIT_EL_PARAM, -1.5f, 1.5f, 0.3f, "Emitter elevation");
		configButton(PING_PARAM, "Ping the crystal (alternates emitter A / B)");
		configParam(PITCH_PARAM, -3.f, 3.f, 0.f, "Ping pitch", " oct");
		configParam(DECAY_PARAM, 0.05f, 4.f, 0.7f, "Ping decay", " s");
		configParam(ECHOES_PARAM, 2.f, (float)CR_TAPS, 8.f, "Echoes (reflections kept — few = discrete, many = dense)");
		getParamQuantity(ECHOES_PARAM)->snapEnabled = true;
		configParam(FEEDBACK_PARAM, 0.f, 0.92f, 0.35f, "Feedback (sound re-enters the emitter)", "%", 0.f, 100.f);
		configInput(FEEDBACK_INPUT, "Feedback CV (±5V)");
		configParam(EMIT_B_AZ_PARAM, -(float)M_PI, (float)M_PI, -2.2f, "Emitter B azimuth");
		configParam(EMIT_B_EL_PARAM, -1.5f, 1.5f, -0.4f, "Emitter B elevation");
		configParam(HEAD_A_PARAM, -(float)M_PI, (float)M_PI, 0.f, "Emitter A heading");
		configParam(HEAD_B_PARAM, -(float)M_PI, (float)M_PI, 1.57f, "Emitter B heading");
		configParam(NAVSPEED_PARAM, 0.f, 4.f, 1.f, "Navigation speed");
		// heading, not orientation: these are rates, so the crystal keeps turning
		// and the faces it presents to each speaker change as it goes
		configParam(SPIN_X_PARAM, -0.8f, 0.8f, 0.06f, "Turn about the X axis");
		configParam(SPIN_Y_PARAM, -0.8f, 0.8f, 0.15f, "Turn about the Y axis");
		configParam(SPIN_Z_PARAM, -0.8f, 0.8f, 0.f,   "Turn about the Z axis");
		for (int p : {SPIN_X_PARAM, SPIN_Y_PARAM, SPIN_Z_PARAM}) {
			getParamQuantity(p)->displayMultiplier = 180.f / (float)M_PI;
			getParamQuantity(p)->unit = "°/s";
		}
		configSwitch(MODE_PARAM, 0.f, 1.f, 0.f, "Mode",
			{"Chamber (real acoustic scale)", "Delay (geometry sets the rhythm)"});
		configInput(AUDIO_B_INPUT, "Audio B (enters at emitter B — patch stereo here)");
		configInput(AX_INPUT, "Emitter A · X velocity (±5V)");
		configInput(AY_INPUT, "Emitter A · Y velocity (±5V)");
		configInput(BX_INPUT, "Emitter B · X velocity (±5V)");
		configInput(BY_INPUT, "Emitter B · Y velocity (±5V)");
		configInput(MIX_INPUT, "Dry / wet CV (±5V)");
		configInput(AUDIO_INPUT, "Audio / CV (enters at the emitter)");
		configInput(SIZE_INPUT, "Size CV (±5V)");
		configInput(DAMP_INPUT, "Damping CV (±5V)");
		configInput(MATERIAL_INPUT, "Shape CV (1V per material, simple → complex)");
		configInput(TAIL_INPUT, "Tail CV (±5V)");
		configInput(ROT_INPUT, "View CV (±5V, yaw — camera only, does not change the sound)");
		configInput(PING_INPUT, "Ping trigger");
		configInput(VOCT_INPUT, "Ping V/oct");
		for (int i = 0; i < CR_NL; i++)
			configOutput(QUAD_OUTPUT + i, string::f("Quad %c", 'A' + i));

		for (int i = 0; i < CR_NPULSE; i++) { pulseT[i] = -1.f; pulseLvl[i] = 0.f; }
		rollShimmer();
		bufLen = (int)(3.0f * 96000.f);
		for (int e = 0; e < CR_NE; e++) {
			buf[e].assign(bufLen, 0.f);
			for (int k = 0; k < CR_LOOPS; k++) {
				loopBuf[e][k].assign(CR_LOOPBUF, 0.f);
				loopDelSm[e][k] = 1000.f + k * 137.f;
			}
		}
		for (int i = 0; i < CR_FDN; i++) { fdn[i].assign(8192, 0.f); fdnLen[i] = 1024; }
		geom = makeMaterial(0); geomMat = 0;
	}

	~Crystal() { if (worker.joinable()) worker.join(); }

	void onReset() override {
		for (int e = 0; e < CR_NE; e++) {
			std::fill(buf[e].begin(), buf[e].end(), 0.f);
			for (int k = 0; k < CR_LOOPS; k++) {
				std::fill(loopBuf[e][k].begin(), loopBuf[e][k].end(), 0.f);
				loopLp[e][k] = 0.f; pkDcX[e][k] = pkDcY[e][k] = 0.f;
			}
		}
		for (int i = 0; i < CR_FDN; i++) { std::fill(fdn[i].begin(), fdn[i].end(), 0.f); fdnLp[i] = 0.f;
		                                   fdDcX[i] = fdDcY[i] = 0.f; }
		for (int e = 0; e < CR_NE; e++)
			for (int i = 0; i < 3; i++) { exEnv[e][i] = 0.f; exPhase[e][i] = 0.f; }
		pingSide = 0;
		rollShimmer();
		dirty = true;
	}

	float pv(int p, int in, float scale) {
		float v = params[p].getValue();
		if (in >= 0 && inputs[in].isConnected()) v += inputs[in].getVoltage() * scale;
		return v;
	}

	void relaunch(float sr) {
		if (busy.load() || fade > 0.001f || sinceTrace < 0.14f) return;
		sinceTrace = 0.f;
		busy = true;
		if (worker.joinable()) worker.join();
		float sizeM  = std::exp2(clamp(pv(SIZE_PARAM, SIZE_INPUT, 0.4f),
		                               std::log2(0.06f), std::log2(24.f)));
		float absorb = clamp(pv(DAMP_PARAM, DAMP_INPUT, 0.06f), 0.01f, 0.6f);
		float tail   = clamp(pv(TAIL_PARAM, TAIL_INPUT, 0.1f), 0.f, 1.f);
		int   mat    = clamp((int)std::round(pv(MATERIAL_PARAM, MATERIAL_INPUT, 1.f)), 0, CR_NMAT - 1);
		M3 head = headM;
		int   nEch   = clamp((int)std::round(params[ECHOES_PARAM].getValue()), 2, CR_TAPS);
		bool  dly    = params[MODE_PARAM].getValue() > 0.5f;
		float maxDel = (float)bufLen / sr - 0.05f;             // however long the line really is
		float u      = clamp((std::log2(sizeM) - std::log2(0.06f))
		                     / (std::log2(24.f) - std::log2(0.06f)), 0.f, 1.f);
		float baseD  = 0.03f * std::exp2(u * 5.3f);            // SIZE = delay time, 30ms .. 1.2s
		V3 emitDir[CR_NE];
		for (int e = 0; e < CR_NE; e++) {
			float az = params[e ? EMIT_B_AZ_PARAM : EMIT_AZ_PARAM].getValue();
			float el = params[e ? EMIT_B_EL_PARAM : EMIT_EL_PARAM].getValue();
			emitDir[e] = V3(std::cos(az) * std::cos(el), std::sin(az) * std::cos(el), std::sin(el));
		}
		int target = 1 - active.load();

		V3 ed0 = emitDir[0], ed1 = emitDir[1];
		worker = std::thread([this, sizeM, absorb, tail, mat, head, ed0, ed1, target, sr, nEch, dly, baseD, maxDel]() {
			V3 emitDir[CR_NE] = {ed0, ed1};
			{
				std::lock_guard<std::mutex> lk(geomMutex);
				if (mat != geomMat) { geom = makeMaterial(mat); geomMat = mat; }
			}
			TapSet ts;
			{
				std::lock_guard<std::mutex> lk(geomMutex);
				traceInto(ts, geom, sizeM, absorb, tail, head, emitDir, sr, nEch, dly, baseD, maxDel);
			}
			sets[target] = ts;
			pendingSet = target;
			swapPending = true;              // the audio thread performs the swap
			busy = false;
		});
	}

	float readTaps(const TapSet& ts, int li) {
		float s = 0.f;
		for (int em = 0; em < CR_NE; em++)
			for (int k = 0; k < ts.n[em][li]; k++) {
				int idx = wr - ts.delay[em][li][k];
				while (idx < 0) idx += bufLen;
				s += ts.gain[em][li][k] * buf[em][idx];
			}
		return s;
	}

	// heading is state, not a param: a reloaded patch should pick the crystal up
	// where it was rather than snapping it back to square
	json_t* dataToJson() override {
		json_t* r = json_object();
		json_t* h = json_array();
		for (int i = 0; i < 9; i++) json_array_append_new(h, json_real(headM.m[i]));
		json_object_set_new(r, "heading", h);
		json_object_set_new(r, "shimmer", json_integer(shimmer));
		json_object_set_new(r, "panelDraw", json_boolean(panelDraw));
		json_object_set_new(r, "pingAlternate", json_boolean(pingAlternate));
		json_t* pk = json_array();
		for (int e = 0; e < CR_NE; e++)
			for (int k = 0; k < CR_LOOPS; k++) json_array_append_new(pk, json_integer(psPick[e][k]));
		json_object_set_new(r, "shimmerPicks", pk);
		return r;
	}
	void dataFromJson(json_t* r) override {
		if (json_t* h = json_object_get(r, "heading")) {
			for (int i = 0; i < 9 && i < (int)json_array_size(h); i++)
				headM.m[i] = json_number_value(json_array_get(h, i));
			orthonormalize(headM);
		}
		if (json_t* j = json_object_get(r, "shimmer")) shimmer = (int)json_integer_value(j);
		if (json_t* j = json_object_get(r, "panelDraw")) panelDraw = json_boolean_value(j);
		if (json_t* j = json_object_get(r, "pingAlternate")) pingAlternate = json_boolean_value(j);
		if (json_t* pk = json_object_get(r, "shimmerPicks"))
			for (int e = 0, i = 0; e < CR_NE; e++)
				for (int k = 0; k < CR_LOOPS; k++, i++)
					if (i < (int)json_array_size(pk))
						psPick[e][k] = (int)json_integer_value(json_array_get(pk, i));
		dirty = true;
	}

	void process(const ProcessArgs& args) override {
		// the crystal keeps turning; every retrace re-aims its faces at the speakers
		if ((args.frame & 63) == 0) {
			float dt = args.sampleTime * 64.f;
			float w[3] = {params[SPIN_X_PARAM].getValue() * dt,
			              params[SPIN_Y_PARAM].getValue() * dt,
			              params[SPIN_Z_PARAM].getValue() * dt};
			for (int a = 0; a < 3; a++)
				if (w[a] != 0.f) headM = mulM(axisRot(a, w[a]), headM);
			headOdo += std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]);
			orthonormalize(headM);
		}

		if (dirty.exchange(false)) relaunch(args.sampleRate);
		if (swapPending.exchange(false)) {   // adopt the worker's new tap set
			prevSet = active.load();
			active = pendingSet;
			fade = 1.f;                      // ...crossfading out of the old one
		}

		// retrace when a shape-changing control moves (rate-limited by `busy`)
		if ((args.frame & 2047) == 0) {
			float now[11] = {
				headOdo,
				pv(SIZE_PARAM, SIZE_INPUT, 0.4f),
				pv(DAMP_PARAM, DAMP_INPUT, 0.06f),
				std::round(pv(MATERIAL_PARAM, MATERIAL_INPUT, 1.f)),
				pv(TAIL_PARAM, TAIL_INPUT, 0.1f),
				params[EMIT_AZ_PARAM].getValue(), params[EMIT_EL_PARAM].getValue(),
				params[ECHOES_PARAM].getValue(),
				params[EMIT_B_AZ_PARAM].getValue(), params[EMIT_B_EL_PARAM].getValue(),
				params[MODE_PARAM].getValue()};
			bool changed = false;
			for (int i = 0; i < 11; i++)
				if (std::fabs(now[i] - watchLast[i]) > 2e-3f) { watchLast[i] = now[i]; changed = true; }
			if (changed) relaunch(args.sampleRate);
		}

		// internal exciter: a struck bar at the emitter (Chime's voice)
		bool pt = pingTrig.process(inputs[PING_INPUT].getVoltage(), 0.1f, 1.f);
		bool pb = pingBtn.process(params[PING_PARAM].getValue());
		bool ping = pt || pb;
		float dk = params[DECAY_PARAM].getValue();
		if (ping) {
			if (pingAlternate) pingSide ^= 1;                // ping-pong across the crystal
			else pingSide = 0;
			exFreq[pingSide] = clamp(261.63f * std::exp2(params[PITCH_PARAM].getValue()
			                                   + inputs[VOCT_INPUT].getVoltage()), 8.f, 12000.f);
			for (int i = 0; i < 3; i++)
				if (exEnv[pingSide][i] < 1e-4f) exPhase[pingSide][i] = 0.f;   // restart from silence only
			exAtk[pingSide] = 0.002f;                        // 2ms rise — never a step
			pingFlash = 1.f;
			dispPingSide = pingSide;
		}
		pingFlash -= pingFlash * args.sampleTime / 0.1f;
		lights[PING_LIGHT].setBrightness(pingFlash);
		static const float RAT[3] = {1.f, 3.932f, 9.538f}, AMP[3] = {1.f, 0.4f, 0.15f}, DEC[3] = {1.f, 0.45f, 0.22f};
		float ex[CR_NE] = {};
		float nyq = 0.45f * args.sampleRate;
		for (int e = 0; e < CR_NE; e++) {
			bool exAttacking = exAtk[e] > 0.f;
			if (exAttacking) exAtk[e] -= args.sampleTime;
			float f0 = exFreq[e];                            // held until this side is struck again
			for (int i = 0; i < 3; i++) {
				if (exAttacking) exEnv[e][i] += (1.f - exEnv[e][i]) * std::min(1.f, args.sampleTime / 0.0006f);
				else exEnv[e][i] *= std::exp(-args.sampleTime / (dk * DEC[i]));
				if (exEnv[e][i] <= 1e-5f) continue;
				float fp = f0 * RAT[i];
				if (fp >= nyq) continue;                     // would alias — leave it out
				exPhase[e][i] += fp * args.sampleTime;
				if (exPhase[e][i] >= 1.f) exPhase[e][i] -= 1.f;
				ex[e] += AMP[i] * exEnv[e][i] * std::sin(2.f * M_PI * exPhase[e][i]);
			}
		}

		// Emitter navigation. X/Y CV are velocities, not positions: they are rotated
		// by the emitter's HEADING and integrated, so the emitter drives across the
		// crystal's surface treated as a flat map (azimuth wraps, elevation stops at
		// the poles). Only integrates while something is patched.
		{
			float spd = params[NAVSPEED_PARAM].getValue() * args.sampleTime;
			for (int e = 0; e < CR_NE; e++) {
				int xi = e ? BX_INPUT : AX_INPUT, yi = e ? BY_INPUT : AY_INPUT;
				if (!inputs[xi].isConnected() && !inputs[yi].isConnected()) continue;
				float vx = inputs[xi].getVoltage() / 5.f, vy = inputs[yi].getVoltage() / 5.f;
				float hd = params[e ? HEAD_B_PARAM : HEAD_A_PARAM].getValue();
				float ch = std::cos(hd), sh = std::sin(hd);
				float dAz = (ch * vx - sh * vy) * spd, dEl = (sh * vx + ch * vy) * spd;
				int ap = e ? EMIT_B_AZ_PARAM : EMIT_AZ_PARAM, ep = e ? EMIT_B_EL_PARAM : EMIT_EL_PARAM;
				float az = params[ap].getValue() + dAz, el = params[ep].getValue() + dEl;
				while (az > M_PI) az -= 2.f * M_PI;
				while (az < -M_PI) az += 2.f * M_PI;
				params[ap].setValue(az);
				params[ep].setValue(clamp(el, -1.5f, 1.5f));
			}
		}

		float inA = inputs[AUDIO_INPUT].getVoltage() * 0.2f + ex[0] * 0.6f;
		float inB = inputs[AUDIO_B_INPUT].getVoltage() * 0.2f + ex[1] * 0.6f;
		float in = inA + inB;                                // onset detection watches both

		// Onset detection: a transient at the emitter launches a visible pulse and
		// is what makes the trajectory readable on screen.
		bool A0ready = sets[active.load()].ready;
		float rect = std::fabs(in);
		env += (rect - env) * (rect > env ? 0.4f : 0.0006f);
		envSlow += (env - envSlow) * 0.0004f;
		sinceOnset += args.sampleTime;
		sinceTrace += args.sampleTime;
		bool flowing = env > 0.01f && sinceOnset > 0.55f;     // sustained material still shows travel
		if ((env > envSlow * 2.2f + 0.004f && sinceOnset > 0.06f) || flowing) {
			sinceOnset = 0.f;
			pulseT[pulseNext] = vizTime.load();
			pulseLvl[pulseNext] = clamp(env * 3.f, 0.15f, 1.f);
			pulseNext = (pulseNext + 1) % CR_NPULSE;
		}
		vizTime = vizTime.load() + args.sampleTime;

		// Regeneration: the chamber re-excites itself at the emitter, so a single
		// hit keeps circulating instead of decaying after one pass.
		float fbAmt = clamp(params[FEEDBACK_PARAM].getValue()
		                    + inputs[FEEDBACK_INPUT].getVoltage() / 10.f, 0.f, 0.92f);
		// The early taps read a CLEAN line — the first pass through the crystal.
		float srcIn[CR_NE] = {inA, inB};
		for (int e = 0; e < CR_NE; e++) buf[e][wr] = clamp(srcIn[e], -8.f, 8.f);

		// The ongoing repeats come from the trapped pockets, each recirculating at
		// its own period. Their delays GLIDE to new values rather than jumping, so
		// a moving emitter bends the pitch slightly instead of clicking. The rate is
		// the transposition: gliding at 0.25 samples per sample reads the line at
		// 1.25x, which is a major third — keep it to a few cents.
		float loopDamp = clamp(1.f - params[DAMP_PARAM].getValue() * 1.5f, 0.06f, 0.95f);
		int   psSel   = clamp(shimmer, 0, CR_NSHIM - 1);
		float psRatio = CR_SHIMMER[psSel].ratio;
		const TapSet& AF = sets[active.load()];
		float pocket[CR_NL] = {};
		for (int e = 0; e < CR_NE; e++) {
			float din = srcIn[e];
			fbDcY[e] = din - fbDcX[e] + 0.9995f * fbDcY[e]; fbDcX[e] = din;
			for (int k = 0; k < CR_LOOPS; k++) {
				float tgt = A0ready ? (float)AF.loopDelay[e][k] : loopDelSm[e][k];
				loopDelSm[e][k] += clamp(tgt - loopDelSm[e][k], -0.004f, 0.004f);
				auto rdAt = [&](float dd) {
					int di = (int)dd; float fr = dd - di;
					int i0 = loopWr[e][k] - di;  while (i0 < 0) i0 += CR_LOOPBUF;
					int i1 = i0 - 1;             while (i1 < 0) i1 += CR_LOOPBUF;
					return loopBuf[e][k][i0] * (1.f - fr) + loopBuf[e][k][i1] * fr;
				};
				float r;
				if (psSel == CR_NSHIM - 1)                     // Random: this pocket's own
					psRatio = CR_SHIMMER[clamp(psPick[e][k], 1, CR_NSHIM - 2)].ratio;
				if (psRatio == 1.f) {
					r = rdAt(clamp(loopDelSm[e][k], 4.f, (float)CR_LOOPBUF - 4.f));
				} else {
					// One head sweeping the delay, alone for most of the cycle. The
					// sweep RATE is the transposition: read position is t - D(t), so
					// a delay shrinking at (ratio-1) reads the line at `ratio`. At the
					// wrap it crossfades briefly to a head one full window away, which
					// is exactly where it is about to jump to.
					//
					// Two heads running together the whole time (the obvious form)
					// puts them a fixed phase apart, and where that phase is pi they
					// cancel the partial outright — measured 32 cents of centroid
					// error against 4 cents for this.
					const float W = 2400.f, XF = 0.15f;        // 50ms window, 15% overlap
					float d0 = clamp(loopDelSm[e][k], 4.f, (float)CR_LOOPBUF - 2.f * W - 8.f);
					float rr = psRatio - 1.f;
					bool up = rr > 0.f;
					psPhase[e][k] += std::fabs(rr) / W;
					if (psPhase[e][k] >= 1.f) psPhase[e][k] -= 1.f;
					float p = psPhase[e][k];
					float dA = d0 + (up ? (1.f - p) : (1.f + p)) * W;
					float dB = dA + (up ? W : -W);
					float gA = 1.f, gB = 0.f;
					if (p > 1.f - XF) {
						float u = (p - (1.f - XF)) / XF;
						gA = 0.5f * (1.f + std::cos((float)M_PI * u));
						gB = 1.f - gA;
					}
					r = rdAt(dA) * gA + rdAt(dB) * gB;
								}
				loopLp[e][k] += (r - loopLp[e][k]) * loopDamp;
				float lp = loopLp[e][k];
				pkDcY[e][k] = lp - pkDcX[e][k] + 0.999f * pkDcY[e][k];
				pkDcX[e][k] = lp;
				lp = pkDcY[e][k];
				float g = A0ready ? AF.loopGain[e][k] : 0.5f;
				loopBuf[e][k][loopWr[e][k]] =
					clamp(fbDcY[e] * 0.7f + std::tanh(lp * fbAmt * g * 1.15f) * 0.87f, -8.f, 8.f);
				loopWr[e][k] = (loopWr[e][k] + 1) % CR_LOOPBUF;
				int li = A0ready ? AF.loopOut[e][k] : (k % CR_NL);
				float sc = A0ready ? AF.loopScale[e][k] : 0.5f;
				for (int q = 0; q < CR_NL; q++) {          // pan, ramped — a rerouted
					float tgtq = (q == li) ? sc : 0.f;     // pocket must not step
					panSm[e][k][q] += (tgtq - panSm[e][k][q]) * 0.0006f;
					pocket[q] += lp * 0.5f * panSm[e][k][q];
				}
			}
		}

		const TapSet& A = sets[active.load()];
		const TapSet& B = sets[prevSet];
		if (fade > 0.f) fade = std::max(0.f, fade - args.sampleTime / 0.09f);

		// tail: FDN fed by the early sum, Hadamard-mixed, damped in the loop
		float fdnIn = 0.f;
		float early[CR_NL];
		for (int li = 0; li < CR_NL; li++) {
			float a = A.ready ? readTaps(A, li) : 0.f;
			float b = (fade > 0.f && B.ready) ? readTaps(B, li) : 0.f;
			early[li] = a * (1.f - fade) + b * fade;
			fdnIn += early[li] * 0.25f / std::max(A.earlyGain, 1.f);
		}

		float tailLvl = clamp(params[TAIL_PARAM].getValue(), 0.f, 1.f);
		float fb = A.ready ? A.fdnFb : 0.f;
		float damp = clamp(params[DAMP_PARAM].getValue() * 1.6f, 0.02f, 0.95f);
		float rd[CR_FDN];
		for (int i = 0; i < CR_FDN; i++) {
			int len = clamp(A.ready ? A.fdnDelay[i] : 1024, 23, (int)fdn[i].size() - 1);
			fdnLen[i] = len;
			int idx = fdnWr[i] - len;
			while (idx < 0) idx += (int)fdn[i].size();
			rd[i] = fdn[i][idx];
			fdnLp[i] += (rd[i] - fdnLp[i]) * (1.f - damp);      // HF loss per pass
			fdDcY[i] = fdnLp[i] - fdDcX[i] + 0.999f * fdDcY[i];
			fdDcX[i] = fdnLp[i];
			rd[i] = fdDcY[i];
		}
		float tailOut[CR_NL] = {};
		for (int i = 0; i < CR_FDN; i++) {
			float mix = 0.f;                                    // cheap Hadamard-ish mixing
			for (int j = 0; j < CR_FDN; j++) mix += ((__builtin_popcount(i & j) & 1) ? -rd[j] : rd[j]);
			mix *= 0.3536f;                                     // 1/sqrt(8)
			fdn[i][fdnWr[i]] = fdnIn + mix * fb;
			fdnWr[i] = (fdnWr[i] + 1) % (int)fdn[i].size();
			tailOut[i % CR_NL] += rd[i] * 0.5f * tailLvl;
		}

		float mix = clamp(params[MIX_PARAM].getValue() + inputs[MIX_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float dry = (inA + inB) * 5.f;       // the source itself: audio in + the struck bar
		// the dry path reproduces its source faithfully, offset included — and the
		// jack takes CV as readily as audio, so block it here too (~2Hz, well under
		// anything musical) rather than put a source's offset on an audio output
		dryDcY = dry - dryDcX + 0.99975f * dryDcY;
		dryDcX = dry;
		dry = dryDcY;
		for (int li = 0; li < CR_NL; li++) {
			float wet = early[li] + tailOut[li] + pocket[li] * fbAmt;
			hfLp[li] += (wet - hfLp[li]) * clamp(1.f - damp * 0.7f, 0.05f, 1.f);
			wet = hfLp[li] / std::max(A.earlyGain, 1.f);
			outDcY[li] = wet - outDcX[li] + 0.999f * outDcY[li];   // backstop
			outDcX[li] = wet;
			wet = outDcY[li];
			outputs[QUAD_OUTPUT + li].setVoltage(clamp(dry * (1.f - mix) + wet * mix * 5.f, -10.f, 10.f));
		}

		wr = (wr + 1) % bufLen;
		dispRotY = params[ROTY_PARAM].getValue() + inputs[ROT_INPUT].getVoltage() * 0.31f;
		dispRotX = params[ROTX_PARAM].getValue();
		for (int i = 0; i < 9; i++) dispHead[i] = headM.m[i];
	}
};

// ── display: wireframe crystal, drag to rotate, drag the emitter ────────────
static const NVGcolor XBG     = nvgRGB(0x1A, 0x1A, 0x32);
static const NVGcolor XBLUE   = nvgRGB(0x00, 0x97, 0xDE);
static const NVGcolor XPURPLE = nvgRGB(0x35, 0x35, 0x4D);
static const NVGcolor XORANGE = nvgRGB(0xEC, 0x65, 0x2E);
static const NVGcolor XTEXT   = nvgRGB(0xE8, 0xE8, 0xF0);
static const NVGcolor XDIM    = nvgRGB(0x8A, 0x8A, 0xA5);

struct CrystalDisplay : Widget {
	Crystal* module = nullptr;
	std::shared_ptr<Font> font;
	// Two palettes. On the faceplate everything has to be dark ink on light, and
	// the faint alphas that read on a dark screen vanish, so they are lifted too.
	bool lite = true;
	NVGcolor cText() const { return lite ? nvgRGB(0x2E, 0x2C, 0x2C) : XTEXT; }
	NVGcolor cDim()  const { return lite ? nvgRGB(0x62, 0x62, 0x70) : XDIM; }
	NVGcolor cWire(float a) const {
		return lite ? nvgRGBAf(0.04f, 0.32f, 0.52f, std::min(1.f, a * 1.15f))
		            : nvgRGBAf(0.f, 0.59f, 0.87f, a);
	}
	NVGcolor cGrid(float a) const {
		return lite ? nvgRGBAf(0.30f, 0.31f, 0.36f, a * 1.5f)
		            : nvgRGBAf(0.72f, 0.74f, 0.80f, a);
	}
	// the purples and the gizmo axes are pitched for a dark screen; darken them
	// rather than let them wash out on the faceplate
	NVGcolor cAccent() const { return lite ? nvgRGB(0x5C, 0x33, 0xA8) : nvgRGB(0x9B, 0x6B, 0xE8); }
	NVGcolor cAxis(NVGcolor c) const {
		return lite ? nvgRGBf(c.r * 0.62f, c.g * 0.62f, c.b * 0.62f) : c;
	}
	NVGcolor cPath(bool b, float a) const {
		float m = lite ? 2.4f : 1.f;
		return b ? nvgRGBAf(0.42f, 0.22f, 0.70f, a * m) : nvgRGBAf(0.f, 0.42f, 0.68f, a * m);
	}
	Geom preview;
	int previewMat = -1;
	bool dragEmitter = false, dragB = false;
	float spinY = 0.f, spinX = 0.f;      // rotation momentum, kept after mouse-up
	bool dragging = false;
	float idleY = 0.f, idleX = 0.f, idleT = 0.f;   // slow wander while at rest

	// x/y/z orientation gizmo, drawn into the display's bottom-left corner
	void drawGizmo(NVGcontext* vg, float cy, float cx, const M3& head) {
		float gx = 26.f, gy = box.size.y - 26.f, s = 15.f;
		auto ori = [&](const V3& p) { return rotYX(mul3(head, p), cy, cx); };
		auto pr = [&](const V3& p) {
			V3 r = ori(p);
			float persp = 2.6f / (2.6f + r.z * 0.55f);
			return Vec(gx + r.x * s * persp, gy - r.y * s * persp);
		};
		nvgBeginPath(vg); nvgEllipse(vg, gx, gy, s * 1.05f, s * 0.34f);
		nvgStrokeColor(vg, cGrid(0.45f));
		nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		struct AX { V3 d; NVGcolor c; const char* nm; };
		AX ax[3] = {{V3(1, 0, 0), nvgRGB(0xE8, 0x62, 0x62), "X"},
		            {V3(0, 1, 0), nvgRGB(0x62, 0xD0, 0x8A), "Y"},
		            {V3(0, 0, 1), nvgRGB(0x62, 0x9B, 0xE8), "Z"}};
		for (auto& a0 : ax) {
			AX a = {a0.d, cAxis(a0.c), a0.nm};
			V3 r = ori(a.d);
			Vec o(gx, gy), q = pr(a.d);
			float al = r.z > 0 ? 0.95f : 0.4f;
			nvgBeginPath(vg); nvgMoveTo(vg, o.x, o.y); nvgLineTo(vg, q.x, q.y);
			nvgStrokeColor(vg, nvgRGBAf(a.c.r, a.c.g, a.c.b, al));
			nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 7.f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, nvgRGBAf(a.c.r, a.c.g, a.c.b, al));
				nvgText(vg, q.x, q.y, a.nm, NULL);
			}
		}
	}

	// spin on after the mouse is released, decaying like a flicked object
	void step() override {
		// A gentle, never-repeating drift when at rest — two slow incommensurate
		// oscillators, so it wanders rather than cycles. Kept local to the display
		// so it never touches (or dirties) the saved rotation params.
		if (!dragging && std::fabs(spinY) < 1e-4f && std::fabs(spinX) < 1e-4f) {
			idleT += 1.f / 60.f;
			idleY += 0.00042f * (std::sin(idleT * 0.19f) + 0.7f * std::sin(idleT * 0.073f));
			idleX += 0.00022f * (std::sin(idleT * 0.11f + 1.3f) + 0.6f * std::sin(idleT * 0.041f));
			idleY = clamp(idleY, -0.30f, 0.30f);   // both bounded: an unbounded
			idleX = clamp(idleX, -0.30f, 0.30f);   // integral drifts away over minutes
		}
		if (module && !dragging && (std::fabs(spinY) > 1e-5f || std::fabs(spinX) > 1e-5f)) {
			float ry = module->params[Crystal::ROTY_PARAM].getValue() + spinY;
			float rx = module->params[Crystal::ROTX_PARAM].getValue() + spinX;
			while (ry > M_PI) ry -= 2.f * M_PI;
			while (ry < -M_PI) ry += 2.f * M_PI;
			module->params[Crystal::ROTY_PARAM].setValue(ry);
			module->params[Crystal::ROTX_PARAM].setValue(clamp(rx, -1.4f, 1.4f));
			if (rx >= 1.4f || rx <= -1.4f) spinX = 0.f;
			spinY *= 0.97f; spinX *= 0.97f;
		}
		Widget::step();
	}

	Vec project(const V3& p, float scale) {
		float persp = 2.6f / (2.6f + p.z * 0.55f);
		return Vec(box.size.x / 2 + p.x * scale * persp, box.size.y / 2 - p.y * scale * persp);
	}

	// cam() puts a room point on screen; obj() is for points fixed to the crystal,
	// which turn by its heading first and then ride the camera like everything else
	void drawCrystal(NVGcontext* vg, const Geom& g, float cy, float cx, const M3& head,
	                 const float* az, const float* el, bool live, float flash, int flashSide) {
		auto cam = [&](const V3& p) { return rotYX(p, cy, cx); };
		auto obj = [&](const V3& p) { return rotYX(mul3(head, p), cy, cx); };
		float R = 1e-4f;
		for (auto& bd : g.bodies) for (auto& v : bd.verts) R = std::max(R, len3(v));
		// the view has to hold the room, not just the crystal: the speakers stand
		// on the plane around it and must stay on screen — the plane flattens under
		// the camera's pitch, so it can be drawn larger than its own radius suggests
		float scale = std::min(box.size.x, box.size.y) * 0.44f / (R * 2.2f);

		// The floor the speakers stand on. Without it the four cones float in the
		// void and there is no way to read which way the crystal has been turned.
		{
			// Polar, not square: a square grid shimmers as the camera yaws (thin
			// lines sweeping past each other), while rings and spokes look the same
			// at every yaw — and they say what the plane is, since the speakers sit
			// on one of the rings.
			const float RC = 2.1f;
			float fy = FLOOR_Y * R;
			auto ring = [&](float rad, float alpha, float wdt) {
				nvgBeginPath(vg);
				for (int k = 0; k <= 64; k++) {
					float a = k * 2.f * (float)M_PI / 64.f;
					Vec p = project(cam(V3(std::cos(a) * rad * R, fy, std::sin(a) * rad * R)), scale);
					if (k == 0) nvgMoveTo(vg, p.x, p.y); else nvgLineTo(vg, p.x, p.y);
				}
				nvgStrokeColor(vg, cGrid(alpha));
				nvgStrokeWidth(vg, wdt); nvgStroke(vg);
			};
			for (int i = 1; i <= 5; i++) ring(i * RC / 5.f, i == 5 ? 0.22f : 0.09f, i == 5 ? 0.7f : 0.5f);
			for (int k = 0; k < 12; k++) {                     // spokes, brighter on the speakers
				float a = k * 30.f * (float)M_PI / 180.f;
				bool onSpeaker = (k % 3) == 1;                 // 30/120/210/300 vs the 45 deg ring
				Vec p0 = project(cam(V3()), scale);
				Vec p1 = project(cam(V3(std::cos(a) * RC * R, fy, std::sin(a) * RC * R)), scale);
				nvgBeginPath(vg); nvgMoveTo(vg, p0.x, p0.y); nvgLineTo(vg, p1.x, p1.y);
				nvgStrokeColor(vg, cGrid(onSpeaker ? 0.10f : 0.07f));
				nvgStrokeWidth(vg, 0.5f); nvgStroke(vg);
			}
			for (int i = 0; i < CR_NL; i++) {                  // the ray out to each speaker
				V3 sp = speakerPos(i) * R;
				Vec p0 = project(cam(V3(0.f, fy, 0.f)), scale), p1 = project(cam(sp), scale);
				nvgBeginPath(vg); nvgMoveTo(vg, p0.x, p0.y); nvgLineTo(vg, p1.x, p1.y);
				nvgStrokeColor(vg, cGrid(0.16f));
				nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
			}
		}

		for (auto& bd : g.bodies)
		for (auto& e : bd.edges) {
			V3 a = obj(bd.verts[e.first]), b = obj(bd.verts[e.second]);
			Vec pa = project(a, scale), pb = project(b, scale);
			float depth = 0.5f * (a.z + b.z) / R;                  // far edges recede
			float t = clamp(0.5f + 0.5f * depth, 0.f, 1.f);
			nvgBeginPath(vg); nvgMoveTo(vg, pa.x, pa.y); nvgLineTo(vg, pb.x, pb.y);
			nvgStrokeColor(vg, cWire(live ? (0.28f + 0.62f * t) : 0.35f));
			nvgStrokeWidth(vg, 0.7f + 0.8f * t); nvgStroke(vg);
		}

		// Traced ray paths, and the pulses travelling along them. This is the actual
		// geometry the audio is using — each dot is one reflection sequence from the
		// emitter to a listener, moving at the speed of sound through the crystal
		// (stretched to a visible duration when the crystal is small).
		if (live && module) {
			const TapSet& ts = module->sets[module->active.load()];
			if (ts.ready && ts.nPaths > 0) {
				float now = module->vizTime.load();
				// follow the real audio timing (including DELAY mode's stretch), and
				// only slow it further when the crystal is too small to watch
				float realSecs = std::max(ts.pathSecs * ts.timeScale, 1e-5f);
				float stretch = std::max(1.f, 0.9f / realSecs) * ts.timeScale;
				for (int pi = 0; pi < ts.nPaths; pi++) {
					const PathViz& pv = ts.path[pi];
					if (pv.n < 2) continue;
					nvgBeginPath(vg);                                  // the path itself, faint
					for (int k = 0; k < pv.n; k++) {
						Vec q = project(cam(pv.pt[k]), scale);   // room coordinates, camera only
						if (k == 0) nvgMoveTo(vg, q.x, q.y); else nvgLineTo(vg, q.x, q.y);
					}
					bool eB = pv.emitter != 0;
					nvgStrokeColor(vg, cPath(eB, 0.12f));
					nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);

					// One dot per pulse, on ONE path — a pulse is a single particle
					// making its way round the crystal, not a copy on every route.
					float total = pv.cum[pv.n - 1] / C_AIR * stretch;
					for (int u = pi; u < CR_NPULSE; u += ts.nPaths) {
						float t0 = module->pulseT[u].load();
						if (t0 < 0.f) continue;
						float age = now - t0;
						if (age < 0.f || age > total) continue;
						float travelled = age / total * pv.cum[pv.n - 1];
						int seg = 0;
						while (seg < pv.n - 2 && pv.cum[seg + 1] < travelled) seg++;
						float span = std::max(pv.cum[seg + 1] - pv.cum[seg], 1e-6f);
						float f = clamp((travelled - pv.cum[seg]) / span, 0.f, 1.f);
						V3 p3 = pv.pt[seg] + (pv.pt[seg + 1] - pv.pt[seg]) * f;
						Vec q = project(cam(p3), scale);
						float lvl = module->pulseLvl[u].load() * (1.f - age / total);
						float rr = 1.6f + 2.6f * lvl;
						nvgBeginPath(vg); nvgCircle(vg, q.x, q.y, rr);
						nvgFillColor(vg, nvgRGBAf(0.92f, 0.62f, 0.25f, clamp(lvl, 0.f, 1.f)));
						nvgFill(vg);
						nvgBeginPath(vg); nvgCircle(vg, q.x, q.y, rr * 2.4f);
						nvgStrokeColor(vg, nvgRGBAf(0.92f, 0.4f, 0.18f, 0.25f * lvl));
						nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
					}
				}
			}
		}

		// the two emitters, each a point on the surface in its (az, el) direction
		for (int e = 0; e < CR_NE; e++) {
			V3 dir = norm3(V3(std::cos(az[e]) * std::cos(el[e]),
			                  std::sin(az[e]) * std::cos(el[e]), std::sin(el[e])));
			V3 world = dir * g.surfaceDist(dir);
			Vec pe = project(obj(world), scale);
			NVGcolor col = e ? cAccent() : XORANGE;
			if (e == flashSide && flash > 0.01f) {
				nvgBeginPath(vg); nvgCircle(vg, pe.x, pe.y, 4.f + 16.f * flash);
				nvgStrokeColor(vg, nvgRGBAf(0.92f, 0.4f, 0.18f, 0.7f * flash));
				nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
			}
			// heading arrow: which way this emitter travels under X/Y CV
			if (module) {
				float hd = module->params[e ? Crystal::HEAD_B_PARAM : Crystal::HEAD_A_PARAM].getValue();
				float aa = az[e] + std::cos(hd) * 0.16f, ee = clamp(el[e] + std::sin(hd) * 0.16f, -1.5f, 1.5f);
				V3 d2 = norm3(V3(std::cos(aa) * std::cos(ee), std::sin(aa) * std::cos(ee), std::sin(ee)));
				Vec q = project(obj(d2 * g.surfaceDist(d2)), scale);
				nvgBeginPath(vg); nvgMoveTo(vg, pe.x, pe.y); nvgLineTo(vg, q.x, q.y);
				nvgStrokeColor(vg, nvgRGBAf(col.r, col.g, col.b, 0.8f)); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
				nvgBeginPath(vg); nvgCircle(vg, q.x, q.y, 1.3f); nvgFillColor(vg, col); nvgFill(vg);
			}
			nvgBeginPath(vg); nvgCircle(vg, pe.x, pe.y, 3.f);
			nvgFillColor(vg, col); nvgFill(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 8.f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, col);
				nvgText(vg, pe.x, pe.y - 8.f, e ? "B" : "A", NULL);
			}
		}

		// the four speakers, standing in the room — they do not turn with the crystal
		for (int i = 0; i < CR_NL; i++) {
			Vec q = project(cam(speakerPos(i) * R), scale);
			Vec c = project(cam(V3(0.f, FLOOR_Y * R, 0.f)), scale);
			float dx = c.x - q.x, dy = c.y - q.y, dl = std::sqrt(dx * dx + dy * dy) + 1e-6f;
			dx /= dl; dy /= dl;                                // unit vector toward the crystal
			float px = -dy, py = dx;                           // across the speaker's face
			// a horn: narrow at the back, flaring open toward the crystal, so which
			// way it fires is readable at a glance
			float bx = q.x - dx * 2.5f, by = q.y - dy * 2.5f;  // throat, outermost
			float mx = q.x + dx * 3.5f, my = q.y + dy * 3.5f;  // mouth, facing in
			nvgBeginPath(vg);
			nvgMoveTo(vg, bx + px * 1.8f, by + py * 1.8f);
			nvgLineTo(vg, mx + px * 5.f,  my + py * 5.f);
			nvgLineTo(vg, mx - px * 5.f,  my - py * 5.f);
			nvgLineTo(vg, bx - px * 1.8f, by - py * 1.8f);
			nvgClosePath(vg);
			nvgStrokeColor(vg, cDim()); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			nvgBeginPath(vg); nvgCircle(vg, bx, by, 1.4f);     // driver at the throat
			nvgFillColor(vg, cDim()); nvgFill(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 8.f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, cDim());
				char c[2] = {(char)('A' + i), 0};
				nvgText(vg, q.x - dx * 8.f, q.y - dy * 8.f, c, NULL);
			}
		}
	}

	void draw(const DrawArgs& args) override {
		if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		NVGcontext* vg = args.vg;
		lite = module && module->panelDraw;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, lite ? 0.f : 3.f);
		// the faceplate colour, so the generated screen rect in the SVG is covered
		// and the drawing reads as sitting directly on the panel
		nvgFillColor(vg, lite ? nvgRGB(0xF0, 0xF0, 0xF0) : XBG); nvgFill(vg);

		// Everything below is bound to the screen. The room is wider than the
		// crystal, so the plane's rim and the speaker labels reach past the edge.
		nvgSave(vg);
		nvgScissor(vg, 0, 0, box.size.x, box.size.y);

		int mat = module ? clamp((int)std::round(module->params[Crystal::MATERIAL_PARAM].getValue()), 0, CR_NMAT - 1) : 4;
		if (mat != previewMat) { preview = makeMaterial(mat); previewMat = mat; }

		float ry = (module ? module->dispRotY.load() : 0.5f) + idleY;
		float rx = clamp((module ? module->dispRotX.load() : 0.3f) + idleX, -1.5f, 1.5f);
		M3 head;
		if (module) for (int i = 0; i < 9; i++) head.m[i] = module->dispHead[i].load();
		else head = mulM(axisRot(1, 0.6f), axisRot(0, 0.2f));   // browser preview
		float az[CR_NE], el[CR_NE];
		az[0] = module ? module->params[Crystal::EMIT_AZ_PARAM].getValue() : 0.6f;
		el[0] = module ? module->params[Crystal::EMIT_EL_PARAM].getValue() : 0.3f;
		az[1] = module ? module->params[Crystal::EMIT_B_AZ_PARAM].getValue() : -2.2f;
		el[1] = module ? module->params[Crystal::EMIT_B_EL_PARAM].getValue() : -0.4f;
		drawCrystal(vg, preview, ry, rx, head, az, el, module != nullptr,
		            module ? module->pingFlash : 0.f,
		            module ? module->dispPingSide.load() : 0);

		drawGizmo(vg, ry, rx, head);

		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, cText());
			nvgText(vg, 7.f, 6.f, CR_MATNAME[mat], NULL);
			if (module && module->params[Crystal::MODE_PARAM].getValue() > 0.5f) {
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
				nvgFillColor(vg, cAccent());
				nvgText(vg, box.size.x / 2, 6.f, "DELAY", NULL);
			}
			if (module) {
				float sz = std::exp2(module->params[Crystal::SIZE_PARAM].getValue());
				std::string rd;
				if (module->params[Crystal::MODE_PARAM].getValue() > 0.5f) {
					float u = clamp((std::log2(sz) - std::log2(0.06f))
					                / (std::log2(24.f) - std::log2(0.06f)), 0.f, 1.f);
					float ms = 0.03f * std::exp2(u * 5.3f) * 1000.f;
					rd = (ms < 1000.f) ? string::f("%.0f ms", ms) : string::f("%.2f s", ms / 1000.f);
				} else {
					rd = (sz < 1.f) ? string::f("%.0f cm", sz * 100.f) : string::f("%.1f m", sz);
				}
				nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
				nvgFillColor(vg, cDim());
				nvgText(vg, box.size.x - 7.f, 6.f, rd.c_str(), NULL);
			}
		}
		nvgRestore(vg);
	}

	void onButton(const event::Button& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			dragEmitter = (e.mods & GLFW_MOD_SHIFT) != 0;      // shift-drag moves an emitter
			dragB = (e.mods & GLFW_MOD_ALT) != 0;             // ...alt picks emitter B
			dragging = true; spinY = spinX = 0.f;
			e.consume(this);
			return;
		}
		Widget::onButton(e);
	}
	void onDragMove(const event::DragMove& e) override {
		if (!module) return;
		float z = getAbsoluteZoom();
		if (dragEmitter) {
			int ap = dragB ? Crystal::EMIT_B_AZ_PARAM : Crystal::EMIT_AZ_PARAM;
			int ep = dragB ? Crystal::EMIT_B_EL_PARAM : Crystal::EMIT_EL_PARAM;
			float az = module->params[ap].getValue() + e.mouseDelta.x / z * 0.02f;
			float el = module->params[ep].getValue() - e.mouseDelta.y / z * 0.02f;
			while (az > M_PI) az -= 2.f * M_PI;
			while (az < -M_PI) az += 2.f * M_PI;
			module->params[ap].setValue(az);
			module->params[ep].setValue(clamp(el, -1.5f, 1.5f));
		} else {
			float dy = e.mouseDelta.x / z * 0.02f, dx = e.mouseDelta.y / z * 0.02f;
			float ry = module->params[Crystal::ROTY_PARAM].getValue() + dy;
			float rx = module->params[Crystal::ROTX_PARAM].getValue() + dx;
			while (ry > M_PI) ry -= 2.f * M_PI;
			while (ry < -M_PI) ry += 2.f * M_PI;
			module->params[Crystal::ROTY_PARAM].setValue(ry);
			module->params[Crystal::ROTX_PARAM].setValue(clamp(rx, -1.4f, 1.4f));
			spinY = spinY * 0.7f + dy * 0.3f;                 // remember the throw
			spinX = spinX * 0.7f + dx * 0.3f;
		}
	}
	void onDragEnd(const event::DragEnd& e) override {
		dragEmitter = false; dragging = false;
		if (std::fabs(spinY) < 0.0015f && std::fabs(spinX) < 0.0015f) spinY = spinX = 0.f;
		Widget::onDragEnd(e);
	}
};

struct CrystalLabels : Widget {
	std::shared_ptr<Font> font;
	struct L { Vec p; std::string t; float sz; };
	std::vector<L> labels;
	void add(float x, float y, const std::string& t, float sz = 6.f) { labels.push_back({Vec(x, y), t, sz}); }
	void draw(const DrawArgs& args) override {
		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font || font->handle < 0) return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFillColor(args.vg, nvgRGB(0x40, 0x40, 0x40));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		for (auto& l : labels) { nvgFontSize(args.vg, l.sz); nvgText(args.vg, mm2px(l.p.x), mm2px(l.p.y), l.t.c_str(), NULL); }
		Widget::draw(args);
	}
};

struct CrystalWidget : ModuleWidget {
	CrystalWidget(Crystal* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/crystal.svg")));

		CrystalDisplay* disp = new CrystalDisplay();
		disp->module = module;
		disp->box.pos = mm2px(Vec(4.f, 10.f));
		disp->box.size = mm2px(Vec(134.f, 56.f));
		addChild(disp);

		// main controls: knob row + CV row
		const float yK = 76.f, yC = 89.f;
		const float kx[7] = {13.f, 31.f, 49.f, 67.f, 85.f, 103.f, 121.f};
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[0], yK)), module, Crystal::SIZE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[1], yK)), module, Crystal::DAMP_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[2], yK)), module, Crystal::MATERIAL_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[3], yK)), module, Crystal::TAIL_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[4], yK)), module, Crystal::ECHOES_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[5], yK)), module, Crystal::FEEDBACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(kx[6], yK)), module, Crystal::MIX_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kx[0], yC)), module, Crystal::SIZE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kx[1], yC)), module, Crystal::DAMP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kx[2], yC)), module, Crystal::MATERIAL_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kx[3], yC)), module, Crystal::TAIL_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kx[4], yC)), module, Crystal::ROT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kx[5], yC)), module, Crystal::FEEDBACK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(kx[6], yC)), module, Crystal::MIX_INPUT));

		// emitter navigation: per-emitter heading + X/Y velocity, shared speed
		const float yN = 104.f, yB = 118.f;
		addParam(createParamCentered<Trimpot>(mm2px(Vec(42.f, yN)), module, Crystal::HEAD_A_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.f, yN)), module, Crystal::AX_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(63.f, yN)), module, Crystal::AY_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(76.f, yN)), module, Crystal::HEAD_B_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(87.f, yN)), module, Crystal::BX_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(97.f, yN)), module, Crystal::BY_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(109.f, yN)), module, Crystal::NAVSPEED_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(14.f, 104.f)), module, Crystal::MODE_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(mm2px(Vec(122.f, yN)), module,
			Crystal::PING_PARAM, Crystal::PING_LIGHT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(133.f, yN)), module, Crystal::PING_INPUT));

		// heading: how fast the crystal turns on each axis, centre = held still
		addParam(createParamCentered<Trimpot>(mm2px(Vec(8.f, yB)), module, Crystal::SPIN_X_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(18.f, yB)), module, Crystal::SPIN_Y_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(28.f, yB)), module, Crystal::SPIN_Z_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(42.f, yB)), module, Crystal::AUDIO_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(53.f, yB)), module, Crystal::AUDIO_B_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(66.f, yB)), module, Crystal::VOCT_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(77.f, yB)), module, Crystal::PITCH_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(87.f, yB)), module, Crystal::DECAY_PARAM));
		for (int i = 0; i < CR_NL; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(101.f + i * 11.f, yB)), module, Crystal::QUAD_OUTPUT + i));

		CrystalLabels* lbl = new CrystalLabels();
		lbl->box.size = box.size;
		lbl->add(kx[0], 69.f, "SIZE"); lbl->add(kx[1], 69.f, "DAMP"); lbl->add(kx[2], 69.f, "MATERIAL");
		lbl->add(kx[3], 69.f, "TAIL"); lbl->add(kx[4], 69.f, "ECHOES"); lbl->add(kx[5], 69.f, "FDBK");
		lbl->add(kx[6], 69.f, "MIX");
		lbl->add(kx[4], 95.f, "VIEW CV", 5.f);
		lbl->add(14.f, 97.f, "DELAY", 5.f); lbl->add(14.f, 111.5f, "CHAMBER", 5.f);
		lbl->add(47.f, 97.5f, "EMITTER A", 5.f); lbl->add(81.f, 97.5f, "EMITTER B", 5.f);
		lbl->add(42.f, 110.f, "HDG", 5.f); lbl->add(53.f, 110.f, "X", 5.f); lbl->add(63.f, 110.f, "Y", 5.f);
		lbl->add(76.f, 110.f, "HDG", 5.f); lbl->add(87.f, 110.f, "X", 5.f); lbl->add(97.f, 110.f, "Y", 5.f);
		lbl->add(109.f, 97.5f, "SPEED", 5.f); lbl->add(122.f, 97.5f, "PING", 5.f);
		lbl->add(133.f, 97.5f, "TRIG", 5.f);
		lbl->add(8.f, 124.f, "SPIN X", 5.f); lbl->add(18.f, 124.f, "SPIN Y", 5.f);
		lbl->add(28.f, 124.f, "SPIN Z", 5.f);
		lbl->add(18.f, 111.5f, "TURN", 5.f);
		lbl->add(42.f, 124.f, "IN A", 5.f); lbl->add(53.f, 124.f, "IN B", 5.f);
		lbl->add(66.f, 124.f, "V/OCT", 5.f); lbl->add(77.f, 124.f, "PIT", 5.f); lbl->add(87.f, 124.f, "DEC", 5.f);
		lbl->add(117.f, 124.f, "QUAD A-D", 5.f);
		lbl->add(71.f, 7.f, "drag to rotate · shift-drag emitter A · shift+alt for B", 5.f);
		addChild(lbl);
	}

	void appendContextMenu(Menu* menu) override {
		Crystal* m = dynamic_cast<Crystal*>(module);
		if (!m) return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Repeat pitch",
			[]() {
				std::vector<std::string> n;
				for (int i = 0; i < CR_NSHIM; i++) n.push_back(CR_SHIMMER[i].name);
				return n;
			}(),
			[=]() { return clamp(m->shimmer, 0, CR_NSHIM - 1); },
			[=](int i) { m->shimmer = clamp(i, 0, CR_NSHIM - 1);
			             if (m->shimmer == CR_NSHIM - 1) m->rollShimmer(); }));
		if (m->shimmer == CR_NSHIM - 1)
			menu->addChild(createMenuItem("Re-roll repeat pitches", "", [=]() { m->rollShimmer(); }));
		menu->addChild(createBoolPtrMenuItem("Ping alternates A / B", "", &m->pingAlternate));
		menu->addChild(createBoolPtrMenuItem("Draw on the panel (no screen)", "", &m->panelDraw));
	}
};

Model* modelCrystal = createModel<Crystal, CrystalWidget>("Crystal");

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
                      float tail, float rotY, float rotX, const V3* emitDir, float sr,
                      int nEchoes, bool delayMode, float baseDelay, float maxDel) {
	// The crystal is an object in a room, and the four listeners are speakers
	// standing OUTSIDE it. Rotating the crystal therefore turns its faces against
	// fixed microphones, which is what makes rotation audible: a wall strike that
	// used to radiate at speaker A now radiates at B, and the trajectory appears
	// to swing round the quad image.
	Geom sc = g;                                 // this crystal at the requested size, in the ROOM
	for (auto& b : sc.bodies)
		for (auto& f : b.faces) { f.n = rotYX(f.n, rotY, rotX); f.d *= sizeM; }

	V3 L[CR_NL];
	float lr = sizeM * 1.9f;                     // clear of the hull, whatever the habit
	for (int i = 0; i < CR_NL; i++) {
		float az = (45.f + i * 90.f) * (float)M_PI / 180.f;
		L[i] = V3(std::cos(az), std::sin(az), (i % 2) ? 0.45f : -0.45f) * lr;
	}

	for (int em = 0; em < CR_NE; em++) {
		V3 ed = norm3(rotYX(emitDir[em], rotY, rotX));   // rides with the crystal
		V3 emit = ed * (sc.surfaceDist(ed) * 0.97f);

		std::vector<std::pair<float, float>> taps[CR_NL];       // (seconds, gain)
		for (int li = 0; li < CR_NL; li++)
			taps[li].push_back({std::max(len3(L[li] - emit), 0.05f) / C_AIR,
			                    1.f / std::max(len3(L[li] - emit), 0.05f)});

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

				// The strike radiates outward through the face it hit, so a speaker
				// only hears it if it stands on the outside of that face. That
				// directivity is what turns rotation into movement rather than a
				// level change.
				for (int li = 0; li < CR_NL; li++) {
					V3 toL = L[li] - p;
					float d = std::max(len3(toL), 0.05f);
					float facing = dot3(norm3(toL), hn);
					if (facing <= 0.f) continue;               // behind the face: blocked
					taps[li].push_back({(pathLen + d) / C_AIR, amp * facing / d});
				}
				v = norm3(v - hn * (2.f * dot3(v, hn)));
			}
			if (pv) {
				ts.pathSecs = std::max(ts.pathSecs, pv->cum[pv->n - 1] / C_AIR);
				for (int k = 0; k < pv->n; k++) pv->pt[k] = pv->pt[k] * (1.f / std::max(sizeM, 1e-6f));
			}
		}

		for (int li = 0; li < CR_NL; li++) {
			std::sort(taps[li].begin(), taps[li].end(),
			          [](const std::pair<float, float>& a, const std::pair<float, float>& b) {
				          return std::fabs(a.second) > std::fabs(b.second);
			          });
			float tsc = 1.f;
			if (delayMode) {
				float tmin = 1e9f;
				for (auto& t : taps[li]) tmin = std::min(tmin, t.first);
				tsc = (tmin > 1e-9f) ? baseDelay / tmin : 1.f;
			}
			ts.timeScale = tsc;
			int n = std::min({(int)taps[li].size(), CR_TAPS, nEchoes});
			float ref = std::max(std::fabs(taps[li][0].second), 1e-9f);
			float minGap = delayMode ? baseDelay * 0.15f : 0.004f;
			int got = 0; float sum = 0.f;
			for (size_t c = 0; c < taps[li].size() && got < n; c++) {
				float t = taps[li][c].first * tsc;
				if (t > maxDel) continue;
				bool close = false;
				for (int q = 0; q < got; q++)
					if (std::fabs(t - ts.delay[em][li][q] / sr) < minGap) { close = true; break; }
				if (close) continue;
				ts.delay[em][li][got] = clamp((int)(t * sr), 1, (int)(maxDel * sr));
				ts.gain[em][li][got] = taps[li][c].second / ref;
				if (got & 1) ts.gain[em][li][got] = -ts.gain[em][li][got];
				sum += ts.gain[em][li][got] * ts.gain[em][li][got];
				got++;
			}
			ts.n[em][li] = got;
			ts.earlyGain = std::max(ts.earlyGain, std::max(std::sqrt(sum), 1.f));
		}

		// trapped pockets: spread across the arrival range so their periods differ
		{
			std::vector<std::pair<int, float>> cand;
			for (int li = 0; li < CR_NL; li++)
				for (int k = 0; k < ts.n[em][li]; k++)
					cand.push_back({ts.delay[em][li][k], std::fabs(ts.gain[em][li][k])});
			std::sort(cand.begin(), cand.end());
			for (int k = 0; k < CR_LOOPS; k++) {
				if (cand.empty()) { ts.loopDelay[em][k] = 1000 + k * 137; ts.loopGain[em][k] = 0.5f;
				                    ts.loopOut[em][k] = k % CR_NL; continue; }
				size_t pick = (size_t)((k + 0.5f) / CR_LOOPS * cand.size());
				pick = std::min(pick, cand.size() - 1);
				ts.loopDelay[em][k] = clamp(cand[pick].first, 32, CR_LOOPBUF - 4);
				ts.loopGain[em][k] = clamp(0.45f + 0.55f * cand[pick].second, 0.2f, 1.f);
				ts.loopOut[em][k] = (k * 3 + em) % CR_NL;
			}
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
		MODE_PARAM,
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
	float watchLast[12] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f,
	                       1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
	std::thread worker;
	std::mutex geomMutex;
	Geom geom;
	int geomMat = -1;

	// internal exciter — Chime's struck bar voice, at the emitter
	float exPhase[3] = {}, exEnv[3] = {};
	float exFreq = 261.63f;                     // latched at the ping (sample & hold)
	float exAtk = 0.f;                          // short attack window, so a retrigger never steps
	dsp::SchmittTrigger pingTrig, pingBtn;
	float pingFlash = 0.f;

	// display mirrors
	std::atomic<float> dispRotY{0.4f}, dispRotX{0.3f};

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
	float loopLp[CR_NE][CR_LOOPS] = {};
	float fbDcX[CR_NE] = {}, fbDcY[CR_NE] = {};

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
		configParam(ROTY_PARAM, -(float)M_PI, (float)M_PI, 0.4f, "Rotate the crystal (yaw)");
		configParam(ROTX_PARAM, -1.4f, 1.4f, 0.3f, "Rotate the crystal (pitch)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.6f, "Dry / wet", "%", 0.f, 100.f);
		configParam(EMIT_AZ_PARAM, -(float)M_PI, (float)M_PI, 0.6f, "Emitter azimuth");
		configParam(EMIT_EL_PARAM, -1.5f, 1.5f, 0.3f, "Emitter elevation");
		configButton(PING_PARAM, "Ping the crystal");
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
		configInput(ROT_INPUT, "Rotation CV (±5V, yaw — turns the crystal against the speakers)");
		configInput(PING_INPUT, "Ping trigger");
		configInput(VOCT_INPUT, "Ping V/oct");
		for (int i = 0; i < CR_NL; i++)
			configOutput(QUAD_OUTPUT + i, string::f("Quad %c", 'A' + i));

		for (int i = 0; i < CR_NPULSE; i++) { pulseT[i] = -1.f; pulseLvl[i] = 0.f; }
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
				loopLp[e][k] = 0.f;
			}
		}
		for (int i = 0; i < CR_FDN; i++) { std::fill(fdn[i].begin(), fdn[i].end(), 0.f); fdnLp[i] = 0.f; }
		for (int i = 0; i < 3; i++) { exEnv[i] = 0.f; exPhase[i] = 0.f; }
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
		float ry = params[ROTY_PARAM].getValue() + inputs[ROT_INPUT].getVoltage() * 0.31f;
		float rx = params[ROTX_PARAM].getValue();
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
		worker = std::thread([this, sizeM, absorb, tail, mat, ry, rx, ed0, ed1, target, sr, nEch, dly, baseD, maxDel]() {
			V3 emitDir[CR_NE] = {ed0, ed1};
			{
				std::lock_guard<std::mutex> lk(geomMutex);
				if (mat != geomMat) { geom = makeMaterial(mat); geomMat = mat; }
			}
			TapSet ts;
			{
				std::lock_guard<std::mutex> lk(geomMutex);
				traceInto(ts, geom, sizeM, absorb, tail, ry, rx, emitDir, sr, nEch, dly, baseD, maxDel);
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

	void process(const ProcessArgs& args) override {
		if (dirty.exchange(false)) relaunch(args.sampleRate);
		if (swapPending.exchange(false)) {   // adopt the worker's new tap set
			prevSet = active.load();
			active = pendingSet;
			fade = 1.f;                      // ...crossfading out of the old one
		}

		// retrace when a shape-changing control moves (rate-limited by `busy`)
		if ((args.frame & 2047) == 0) {
			float now[12] = {
				params[ROTY_PARAM].getValue() + inputs[ROT_INPUT].getVoltage() * 0.31f,
				params[ROTX_PARAM].getValue(),
				pv(SIZE_PARAM, SIZE_INPUT, 0.4f),
				pv(DAMP_PARAM, DAMP_INPUT, 0.06f),
				std::round(pv(MATERIAL_PARAM, MATERIAL_INPUT, 1.f)),
				pv(TAIL_PARAM, TAIL_INPUT, 0.1f),
				params[EMIT_AZ_PARAM].getValue(), params[EMIT_EL_PARAM].getValue(),
				params[ECHOES_PARAM].getValue(),
				params[EMIT_B_AZ_PARAM].getValue(), params[EMIT_B_EL_PARAM].getValue(),
				params[MODE_PARAM].getValue()};
			bool changed = false;
			for (int i = 0; i < 12; i++)
				if (std::fabs(now[i] - watchLast[i]) > 2e-3f) { watchLast[i] = now[i]; changed = true; }
			if (changed) relaunch(args.sampleRate);
		}

		// internal exciter: a struck bar at the emitter (Chime's voice)
		bool pt = pingTrig.process(inputs[PING_INPUT].getVoltage(), 0.1f, 1.f);
		bool pb = pingBtn.process(params[PING_PARAM].getValue());
		bool ping = pt || pb;
		float dk = params[DECAY_PARAM].getValue();
		if (ping) {
			exFreq = clamp(261.63f * std::exp2(params[PITCH_PARAM].getValue()
			                                   + inputs[VOCT_INPUT].getVoltage()), 8.f, 12000.f);
			for (int i = 0; i < 3; i++)
				if (exEnv[i] < 1e-4f) exPhase[i] = 0.f;      // only restart phase from silence
			exAtk = 0.002f;                                  // 2ms rise — never a step
			pingFlash = 1.f;
		}
		float f0 = exFreq;                                   // held until the next ping
		bool exAttacking = exAtk > 0.f;
		if (exAttacking) exAtk -= args.sampleTime;
		pingFlash -= pingFlash * args.sampleTime / 0.1f;
		lights[PING_LIGHT].setBrightness(pingFlash);
		static const float RAT[3] = {1.f, 3.932f, 9.538f}, AMP[3] = {1.f, 0.4f, 0.15f}, DEC[3] = {1.f, 0.45f, 0.22f};
		float ex = 0.f;
		float nyq = 0.45f * args.sampleRate;
		for (int i = 0; i < 3; i++) {
			if (exAttacking) exEnv[i] += (1.f - exEnv[i]) * std::min(1.f, args.sampleTime / 0.0006f);
			else exEnv[i] *= std::exp(-args.sampleTime / (dk * DEC[i]));
			if (exEnv[i] <= 1e-5f) continue;
			float fp = f0 * RAT[i];
			if (fp >= nyq) continue;                         // would alias — leave it out
			exPhase[i] += fp * args.sampleTime;
			if (exPhase[i] >= 1.f) exPhase[i] -= 1.f;
			ex += AMP[i] * exEnv[i] * std::sin(2.f * M_PI * exPhase[i]);
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

		float inA = inputs[AUDIO_INPUT].getVoltage() * 0.2f + ex * 0.6f;
		float inB = inputs[AUDIO_B_INPUT].getVoltage() * 0.2f;
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
		// a fast-moving emitter bends the pitch slightly instead of clicking.
		float loopDamp = clamp(1.f - params[DAMP_PARAM].getValue() * 1.5f, 0.06f, 0.95f);
		const TapSet& AF = sets[active.load()];
		float pocket[CR_NL] = {};
		for (int e = 0; e < CR_NE; e++) {
			float din = srcIn[e];
			fbDcY[e] = din - fbDcX[e] + 0.9995f * fbDcY[e]; fbDcX[e] = din;
			for (int k = 0; k < CR_LOOPS; k++) {
				float tgt = A0ready ? (float)AF.loopDelay[e][k] : loopDelSm[e][k];
				loopDelSm[e][k] += clamp(tgt - loopDelSm[e][k], -0.25f, 0.25f);
				float d = clamp(loopDelSm[e][k], 4.f, (float)CR_LOOPBUF - 4.f);
				int di = (int)d; float fr = d - di;
				int i0 = loopWr[e][k] - di;      while (i0 < 0) i0 += CR_LOOPBUF;
				int i1 = i0 - 1;                 while (i1 < 0) i1 += CR_LOOPBUF;
				float r = loopBuf[e][k][i0] * (1.f - fr) + loopBuf[e][k][i1] * fr;
				loopLp[e][k] += (r - loopLp[e][k]) * loopDamp;
				float g = A0ready ? AF.loopGain[e][k] : 0.5f;
				loopBuf[e][k][loopWr[e][k]] =
					clamp(fbDcY[e] * 0.7f + std::tanh(loopLp[e][k] * fbAmt * g * 1.15f) * 0.87f, -8.f, 8.f);
				loopWr[e][k] = (loopWr[e][k] + 1) % CR_LOOPBUF;
				int li = A0ready ? AF.loopOut[e][k] : (k % CR_NL);
				pocket[li] += loopLp[e][k] * 0.5f;
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
			rd[i] = fdnLp[i];
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
		for (int li = 0; li < CR_NL; li++) {
			float wet = early[li] + tailOut[li] + pocket[li] * fbAmt;
			hfLp[li] += (wet - hfLp[li]) * clamp(1.f - damp * 0.7f, 0.05f, 1.f);
			wet = hfLp[li] / std::max(A.earlyGain, 1.f);
			outputs[QUAD_OUTPUT + li].setVoltage(clamp(dry * (1.f - mix) + wet * mix * 5.f, -10.f, 10.f));
		}

		wr = (wr + 1) % bufLen;
		dispRotY = params[ROTY_PARAM].getValue() + inputs[ROT_INPUT].getVoltage() * 0.31f;
		dispRotX = params[ROTX_PARAM].getValue();
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
	Geom preview;
	int previewMat = -1;
	bool dragEmitter = false, dragB = false;
	float spinY = 0.f, spinX = 0.f;      // rotation momentum, kept after mouse-up
	bool dragging = false;
	float idleY = 0.f, idleX = 0.f, idleT = 0.f;   // slow wander while at rest

	// x/y/z orientation gizmo, drawn into the display's bottom-left corner
	void drawGizmo(NVGcontext* vg, float ry, float rx) {
		float gx = 26.f, gy = box.size.y - 26.f, s = 15.f;
		auto pr = [&](const V3& p) {
			V3 r = rotYX(p, ry, rx);
			float persp = 2.6f / (2.6f + r.z * 0.55f);
			return Vec(gx + r.x * s * persp, gy - r.y * s * persp);
		};
		nvgBeginPath(vg); nvgEllipse(vg, gx, gy, s * 1.05f, s * 0.34f);
		nvgStrokeColor(vg, nvgRGBAf(0.35f, 0.35f, 0.45f, 0.45f));
		nvgStrokeWidth(vg, 0.7f); nvgStroke(vg);
		struct AX { V3 d; NVGcolor c; const char* nm; };
		AX ax[3] = {{V3(1, 0, 0), nvgRGB(0xE8, 0x62, 0x62), "X"},
		            {V3(0, 1, 0), nvgRGB(0x62, 0xD0, 0x8A), "Y"},
		            {V3(0, 0, 1), nvgRGB(0x62, 0x9B, 0xE8), "Z"}};
		for (auto& a : ax) {
			V3 r = rotYX(a.d, ry, rx);
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
			idleX = clamp(idleX, -0.30f, 0.30f);
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

	void drawCrystal(NVGcontext* vg, const Geom& g, float ry, float rx,
	                 const float* az, const float* el, bool live, float flash) {
		float R = 1e-4f;
		for (auto& bd : g.bodies) for (auto& v : bd.verts) R = std::max(R, len3(v));
		// the view has to hold the room, not just the crystal: the speakers stand
		// at 1.9 crystal radii and must stay on screen
		float scale = std::min(box.size.x, box.size.y) * 0.40f / (R * 1.9f);

		for (auto& bd : g.bodies)
		for (auto& e : bd.edges) {
			V3 a = rotYX(bd.verts[e.first], ry, rx), b = rotYX(bd.verts[e.second], ry, rx);
			Vec pa = project(a, scale), pb = project(b, scale);
			float depth = 0.5f * (a.z + b.z) / R;                  // far edges recede
			float t = clamp(0.5f + 0.5f * depth, 0.f, 1.f);
			nvgBeginPath(vg); nvgMoveTo(vg, pa.x, pa.y); nvgLineTo(vg, pb.x, pb.y);
			nvgStrokeColor(vg, nvgRGBAf(0.f, 0.59f, 0.87f, live ? (0.28f + 0.62f * t) : 0.35f));
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
						Vec q = project(pv.pt[k], scale);   // already in room coordinates
						if (k == 0) nvgMoveTo(vg, q.x, q.y); else nvgLineTo(vg, q.x, q.y);
					}
					bool eB = pv.emitter != 0;
					nvgStrokeColor(vg, eB ? nvgRGBAf(0.55f, 0.35f, 0.85f, 0.12f)
					                      : nvgRGBAf(0.f, 0.59f, 0.87f, 0.12f));
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
						Vec q = project(p3, scale);
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
			Vec pe = project(rotYX(world, ry, rx), scale);
			NVGcolor col = e ? nvgRGB(0x9B, 0x6B, 0xE8) : XORANGE;
			if (e == 0 && flash > 0.01f) {
				nvgBeginPath(vg); nvgCircle(vg, pe.x, pe.y, 4.f + 16.f * flash);
				nvgStrokeColor(vg, nvgRGBAf(0.92f, 0.4f, 0.18f, 0.7f * flash));
				nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
			}
			// heading arrow: which way this emitter travels under X/Y CV
			if (module) {
				float hd = module->params[e ? Crystal::HEAD_B_PARAM : Crystal::HEAD_A_PARAM].getValue();
				float aa = az[e] + std::cos(hd) * 0.16f, ee = clamp(el[e] + std::sin(hd) * 0.16f, -1.5f, 1.5f);
				V3 d2 = norm3(V3(std::cos(aa) * std::cos(ee), std::sin(aa) * std::cos(ee), std::sin(ee)));
				Vec q = project(rotYX(d2 * g.surfaceDist(d2), ry, rx), scale);
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
			float a = (45.f + i * 90.f) * (float)M_PI / 180.f;
			V3 lp = V3(std::cos(a), std::sin(a), (i % 2) ? 0.45f : -0.45f) * (R * 1.9f);
			Vec q = project(lp, scale);
			Vec c = project(V3(), scale);
			float dx = c.x - q.x, dy = c.y - q.y, dl = std::sqrt(dx * dx + dy * dy) + 1e-6f;
			dx /= dl; dy /= dl;
			nvgBeginPath(vg);                                  // a cone aimed inward
			nvgMoveTo(vg, q.x + dx * 5.f, q.y + dy * 5.f);
			nvgLineTo(vg, q.x - dy * 3.f, q.y + dx * 3.f);
			nvgLineTo(vg, q.x + dy * 3.f, q.y - dx * 3.f);
			nvgClosePath(vg);
			nvgStrokeColor(vg, XDIM); nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 8.f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, XDIM);
				char c[2] = {(char)('A' + i), 0};
				nvgText(vg, q.x, q.y - 7.f, c, NULL);
			}
		}
	}

	void draw(const DrawArgs& args) override {
		if (!font) font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg); nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(vg, XBG); nvgFill(vg);

		int mat = module ? clamp((int)std::round(module->params[Crystal::MATERIAL_PARAM].getValue()), 0, CR_NMAT - 1) : 4;
		if (mat != previewMat) { preview = makeMaterial(mat); previewMat = mat; }

		float ry = (module ? module->dispRotY.load() : 0.5f) + idleY;
		float rx = clamp((module ? module->dispRotX.load() : 0.3f) + idleX, -1.5f, 1.5f);
		float az[CR_NE], el[CR_NE];
		az[0] = module ? module->params[Crystal::EMIT_AZ_PARAM].getValue() : 0.6f;
		el[0] = module ? module->params[Crystal::EMIT_EL_PARAM].getValue() : 0.3f;
		az[1] = module ? module->params[Crystal::EMIT_B_AZ_PARAM].getValue() : -2.2f;
		el[1] = module ? module->params[Crystal::EMIT_B_EL_PARAM].getValue() : -0.4f;
		drawCrystal(vg, preview, ry, rx, az, el, module != nullptr,
		            module ? module->pingFlash : 0.f);

		drawGizmo(vg, ry, rx);

		if (font) {
			nvgFontFaceId(vg, font->handle); nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgFillColor(vg, XTEXT);
			nvgText(vg, 7.f, 6.f, CR_MATNAME[mat], NULL);
			if (module && module->params[Crystal::MODE_PARAM].getValue() > 0.5f) {
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
				nvgFillColor(vg, nvgRGB(0x9B, 0x6B, 0xE8));
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
				nvgFillColor(vg, XDIM);
				nvgText(vg, box.size.x - 7.f, 6.f, rd.c_str(), NULL);
			}
		}
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
		lbl->add(42.f, 124.f, "IN A", 5.f); lbl->add(53.f, 124.f, "IN B", 5.f);
		lbl->add(66.f, 124.f, "V/OCT", 5.f); lbl->add(77.f, 124.f, "PIT", 5.f); lbl->add(87.f, 124.f, "DEC", 5.f);
		lbl->add(117.f, 124.f, "QUAD A-D", 5.f);
		lbl->add(71.f, 7.f, "drag to rotate · shift-drag emitter A · shift+alt for B", 5.f);
		addChild(lbl);
	}
};

Model* modelCrystal = createModel<Crystal, CrystalWidget>("Crystal");

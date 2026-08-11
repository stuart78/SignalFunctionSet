#include "plugin.hpp"
#include <cmath>
#include <vector>
#include <deque>

// GRAVITY — Double-pendulum chaos LFO.
//
// A normalized double pendulum (m1=m2=1, l1=l2=1) is integrated with RK4 at
// audio rate. Its motion is read three ways:
//   * X / Y          — the lower bob (tip) position, bipolar +/-5V
//   * 6 SECTOR CVs    — distance-from-center, morph-crossfaded across the six
//                       60-degree wedges by the tip's angular position
//   * 6 GATE outs     — retriggerable holds fired as the tip sweeps across each
//                       of the six boundary rays (high while loitering, drops
//                       once the tip is clear)
//
// Controls: SPEED (sim time scale), CHAOS (energy target — tame swing -> full
// tumble), plus ragdoll drag of either bob on the display to relaunch motion.

static const int NUM_SECTORS = 6;
static const float DEG = (float) (M_PI / 180.0);

// Pendulum lengths / masses (normalized).
static const float L1 = 1.f;
static const float L2 = 1.f;
static const float M1 = 1.f;
static const float M2 = 1.f;
static const float GRAV = 9.81f;
static const float REACH = L1 + L2;   // max tip radius

// double clamp (rack::clamp only has int/float overloads).
static inline double clampd(double x, double lo, double hi) {
	return x < lo ? lo : (x > hi ? hi : x);
}

// Wrap an angle to (-pi, pi].
static inline float wrapPi(float a) {
	while (a > (float) M_PI) a -= (float) (2.0 * M_PI);
	while (a <= -(float) M_PI) a += (float) (2.0 * M_PI);
	return a;
}


struct Gravity : Module {
	enum ParamId {
		SPEED_PARAM,
		CHAOS_PARAM,
		GRAVITY_PARAM,
		MODE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		SPEED_INPUT,
		CHAOS_INPUT,
		GRAVITY_INPUT,
		MODE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		X_OUTPUT,
		Y_OUTPUT,
		RADIUS_OUTPUT,
		ANGLE_OUTPUT,
		ENUMS(SECTOR_OUTPUT, NUM_SECTORS),
		ENUMS(GATE_OUTPUT, NUM_SECTORS),
		// Derived from the same ray crossings, so they hold for exactly as long
		// as the ray gates do and never fire without one.
		ANY_GATE_OUTPUT, ODD_GATE_OUTPUT, EVEN_GATE_OUTPUT, NEW_GATE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(GATE_LIGHT, NUM_SECTORS),
		ENUMS(SECTOR_LIGHT, NUM_SECTORS),
		LIGHTS_LEN
	};

	// --- Modes ---------------------------------------------------------------
	// Each mode produces a single "tracked point" (drives X/Y/RADIUS/ANGLE/
	// sectors) plus a set of "gate points" whose ray crossings fire the gates.
	enum Mode { MODE_PENDULUM = 0, MODE_GRAVWELL = 1, MODE_BILLIARDS = 2, MODE_HUNGRY = 3, MODE_TURTLE = 4, MODE_PATTERN = 5, NUM_MODES = 6 };
	int mode = MODE_PENDULUM;
	int prevMode = MODE_PENDULUM;

	// --- Gravity Well state --------------------------------------------------
	// N planets orbit the center (inner faster); a rocket is N-body pulled
	// between them. CHAOS sets planet count + launch energy.
	static const int MAX_PLANETS = 5;
	int   gwPlanetCount = 3;
	double gwPlanetAng[MAX_PLANETS]  = {};   // current orbital angle
	double gwPlanetRad[MAX_PLANETS]  = {};   // orbital radius (fixed per planet)
	double gwPlanetRate[MAX_PLANETS] = {};   // angular velocity (inner faster)
	double gwPlanetMass[MAX_PLANETS] = {};
	double gwRx = 0.6, gwRy = 0.0;           // rocket position (pendulum coords)
	double gwVx = 0.0, gwVy = 0.8;           // rocket velocity
	bool   gwInit = false;

	// --- Billiards state -----------------------------------------------------
	// Elastic balls in a circle. Ball 0 = cue (heavier, the tracked point).
	// CHAOS sets the count of non-cue balls. KE is normalized to stay alive.
	static const int MAX_BALLS = 9;          // cue + up to 8
	int    bzCount = 1;                       // active ball count (incl. cue)
	double bzX[MAX_BALLS]  = {};
	double bzY[MAX_BALLS]  = {};
	double bzVx[MAX_BALLS] = {};
	double bzVy[MAX_BALLS] = {};
	double bzMass[MAX_BALLS] = {};
	bool   bzInit = false;
	static constexpr double BALL_R   = 0.10;   // collision radius (pendulum units)
	static constexpr double CUE_MASS = 2.5;
	// Slingshot drag (cue): pull back, release launches opposite the drag.
	bool   bzAiming = false;
	double bzAimX = 0.0, bzAimY = 0.0;         // current drag point (pendulum coords)

	// --- Hungry Man (Pac-Man) state -----------------------------------------
	// A polar-grid maze of CELLS (corridors), with the drawn lines as WALLS.
	// Cells = nodes Hungry Man walks; a wall between two cells is drawn unless a
	// passage was carved (spanning tree). 6 sectors aligns the wedges with the
	// 6 gate rays. Cell (ring r, sector s): r in 0..HM_RINGS-1, s in 0..HM_SPOKES-1.
	//   node index = r * HM_SPOKES + s
	// Cell CENTER sits between walls: radius (r+0.5)/HM_RINGS, angle (s+0.5)*60°.
	static const int HM_SPOKES = 12;   // 12 maze segments (2 per gate sector)
	static const int HM_RINGS  = 4;
	static const int HM_NODES  = HM_SPOKES * HM_RINGS;
	// Passage flags between cells (true = open corridor, false = wall drawn):
	bool   hmPassRing[HM_NODES] = {};           // open between (r,s) and (r+1,s); idx = r*S+s, r<RINGS-1
	bool   hmPassRad[HM_NODES]  = {};           // open between (r,s) and (r,(s+1)%S); idx = r*S+s
	// Adjacency (built from passages) for BFS pathfinding.
	int    hmAdj[HM_NODES][4] = {};
	int    hmAdjN[HM_NODES] = {};
	// Dots live in cell centers: one per cell; big dots replace small on chosen cells.
	bool   hmBigDot[HM_NODES] = {};
	bool   hmSmallDot[HM_NODES] = {};
	int    hmBigCount = 0, hmSmallCount = 0;
	// Traversal: moving from cell hmFrom to cell hmTo, progress 0..1.
	int    hmFrom = 0, hmTo = 0;
	float  hmProg = 0.f;
	int    hmTarget = -1;
	float  hmHeading = 0.f;                      // facing (screen radians, set in display)
	bool   hmInit = false;
	// Score + leveling. Eating a small dot = 1, big dot = 5. Clearing the board
	// advances the level: a fresh maze is drawn and "LEVEL N" flashes briefly.
	int    hmLevel = 1;
	long   hmScore = 0;
	float  hmLevelFlash = 0.f;                   // seconds remaining on the banner

	// --- Turtle (LOGO graphics) state ---------------------------------------
	// The tracked point IS the turtle. It executes random LOGO-style drawing
	// commands. SPEED = pen speed, CHAOS = how often a new command is issued,
	// GRAVITY = bias toward common (FD/turn) vs esoteric (spiral/home/hop) moves.
	enum TurtleCmd {
		TU_STRAIGHT, TU_VEER_L, TU_VEER_R, TU_ARC_L, TU_ARC_R,
		TU_CORNER_L, TU_CORNER_R, TU_SPIRAL, TU_SETHEAD, TU_PENHOP, TU_HOME,
		TU_NUMCMD
	};
	float  tuX = 0.f, tuY = 0.f;       // position (model coords)
	float  tuHeading = (float) M_PI;   // dir vector = (sin h, cos h); PI = up
	bool   tuPenDown = true;
	float  tuCmdTime = 0.f;            // elapsed in current command (s)
	float  tuCmdDur = 0.6f;            // total duration of current command (s)
	float  tuMoveRate = 0.f;          // model units / s (forward)
	float  tuTurnRate = 0.f;          // rad / s
	float  tuTurnAccel = 0.f;         // rad / s^2 (spirals)
	int    tuCmd = TU_STRAIGHT;
	int    tuCmdSeq = 0;             // increments each new command (UI watches this)
	int    tuTeleport = 0;            // increments on jumps (trail break marker)
	bool   tuInit = false;

	// --- Pattern (turtle spirograph / Maurer rose) state --------------------
	// A generative pattern engine that shares the turtle's position/trail/icon.
	// Each figure is a closed rose r = sin(n*theta) walked at integer-degree
	// steps d (a division of 360) so it always closes into a symmetric picture.
	// GRAVITY = symmetry (petal count), CHAOS = intricacy (smooth rose -> dense
	// web -> harmonic/fractal lobes). The turtle traces the precomputed verts.
	static const int PT_MAXV = 1900;
	float  ptVx[PT_MAXV] = {};
	float  ptVy[PT_MAXV] = {};
	int    ptCount = 0;
	int    ptSeg = 0;
	float  ptProg = 0.f;
	float  ptDwell = 0.f;            // pause after completing a figure (s)
	int    ptGen = 0;                // increments per new figure (UI clears trail)
	int    ptN = 6, ptQ = 1, ptD = 24;  // descriptor for the on-screen label (p/q, step)
	bool   ptInit = false;

	// Radial mapping: an open hub of HM_R0 leaves the center clear (Pac-Man
	// style) and spreads the inner-ring cells so they don't pile up at center.
	static constexpr float HM_R0 = 0.30f;       // inner hole as fraction of RMAX
	static float hmRingFrac(float ringPos) {    // ringPos 0..HM_RINGS -> 0..1
		return HM_R0 + (1.f - HM_R0) * ringPos / (float) HM_RINGS;
	}
	static int hmCell(int r, int s) {
		return r * HM_SPOKES + ((s % HM_SPOKES) + HM_SPOKES) % HM_SPOKES;
	}
	// Cell center -> pendulum coords.
	void hmNodePos(int node, float& x, float& y) const {
		int r = node / HM_SPOKES;
		int s = node % HM_SPOKES;
		float rad = hmRingFrac(r + 0.5f) * (REACH * 0.92f);
		float ang = (s + 0.5f) * (2.f * (float) M_PI / HM_SPOKES);
		x = rad * std::sin(ang);
		y = rad * std::cos(ang);
	}

	// Physical length of the from->to segment (model units). Arc moves measure
	// along the ring arc (matching the visual interpolation); radial moves use
	// the chord. Used to keep Hungry Man's LINEAR speed constant so he doesn't
	// sprint along long outer-ring arcs.
	float hmSegLen(int from, int to) const {
		if (from == to) return 1e-6f;
		int ra = from / HM_SPOKES, rb = to / HM_SPOKES;
		if (ra == rb) {                              // arc along the ring
			int s0 = from % HM_SPOKES, s1 = to % HM_SPOKES;
			int ds = s1 - s0;
			if (ds >  HM_SPOKES / 2) ds -= HM_SPOKES;
			if (ds < -HM_SPOKES / 2) ds += HM_SPOKES;
			float rad = hmRingFrac(ra + 0.5f) * (REACH * 0.92f);
			return std::fabs((float) ds) * rad * (2.f * (float) M_PI / HM_SPOKES);
		}
		float xa, ya, xb, yb;                        // radial chord
		hmNodePos(from, xa, ya); hmNodePos(to, xb, yb);
		return std::hypot(xb - xa, yb - ya);
	}

	// Tracked point (filled by the active mode each sample; pendulum coords).
	float trackedX = 0.f, trackedY = 0.f;
	// Gate points for this sample (filled by the active mode).
	int   gatePtCount = 0;
	float gatePtX[MAX_BALLS] = {};
	float gatePtY[MAX_BALLS] = {};

	// --- Pendulum state (analytic double pendulum) ---
	double th1 = 2.6, th2 = 2.7;   // angles from downward vertical
	double w1 = 0.0,  w2 = 0.0;    // angular velocities
	double energyLP = 4.0;         // low-passed kinetic proxy for the servo

	// --- Drag (ragdoll) state, written by the display widget ---
	enum DragJoint { DRAG_NONE = 0, DRAG_ELBOW = 1, DRAG_TIP = 2 };
	int dragJoint = DRAG_NONE;
	float dragTargetX = 0.f, dragTargetY = 0.f;   // pendulum coords (+y down)
	double prevDragTh1 = 0.0, prevDragTh2 = 0.0;
	double w1Smooth = 0.0, w2Smooth = 0.0;

	// --- Centre of gravity: the direction (radians, 0 = straight down) the
	// gravity torque pulls toward. Rotating it physically reorients the pendulum;
	// CV-spinning it drags the whole thing around the circle. ---
	double gravAngle = 0.0;
	float gwSunPull = 0.5f;   // GW: central pull magnitude 0..1 (from GRAV knob)

	// --- Gate / sector engine ---
	// Per gate-point continuous-angle trackers so multiple points (billiards
	// balls) can each fire ray crossings independently.
	double gpUnwrapped[MAX_BALLS] = {};
	float  gpPrevPhi[MAX_BALLS]   = {};
	int    gpLastBucket[MAX_BALLS] = {};
	bool   gpInit[MAX_BALLS] = {};
	float gateTimer[NUM_SECTORS] = {};
	float anyTimer = 0.f, oddTimer = 0.f, evenTimer = 0.f, newTimer = 0.f;
	int   lastRayFired = -1;      // for NEW: the previous crossing, whenever it was
	float sectorOut[NUM_SECTORS] = {};
	// 0 = morph: a sector speaks only while the point is inside its wedge.
	// 1 = bipolar distance: every sector reports where the point sits along ITS
	//     axis, whatever sextant the point is actually in, so all six stay live.
	int   sectorMode = 0;
	bool initialized = false;

	// --- Exposed for the display (UI thread reads these; benign races) ---
	float dispB1x = 0.f, dispB1y = 0.f;   // elbow, pendulum coords
	float dispB2x = 0.f, dispB2y = 0.f;   // tip
	float dispGateFlash[NUM_SECTORS] = {};

	// --- Options (context menu, persisted) ---
	float gateHoldSec = 0.06f;       // retriggerable gate hold time
	int trailLength = 90;            // trail ring-buffer length (frames); 0 = off

	Gravity() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SPEED_PARAM, 0.f, 1.f, 0.4f, "Speed", "");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.6f, "Chaos", "");
		configParam(GRAVITY_PARAM, -1.f, 1.f, 0.f, "Gravity direction", " deg", 0.f, 180.f);
		configSwitch(MODE_PARAM, 0.f, 5.f, 0.f, "Mode",
			{"Pendulum", "Gravity Well", "Billiards", "Hungry Man", "Turtle", "Pattern"});
		paramQuantities[MODE_PARAM]->snapEnabled = true;
		configInput(SPEED_INPUT, "Speed CV");
		configInput(CHAOS_INPUT, "Chaos CV");
		configInput(GRAVITY_INPUT, "Gravity direction CV");
		configInput(MODE_INPUT, "Mode CV (0-10V -> 4 modes)");
		configOutput(X_OUTPUT, "Tip X");
		configOutput(Y_OUTPUT, "Tip Y");
		configOutput(RADIUS_OUTPUT, "Radius (folded -5V .. +5V extended)");
		configOutput(ANY_GATE_OUTPUT, "Any gate — any ray crossed");
		configOutput(ODD_GATE_OUTPUT, "Odd gate — rays 1, 3, 5 (ray 1 is 0 deg)");
		configOutput(EVEN_GATE_OUTPUT, "Even gate — rays 2, 4, 6");
		configOutput(NEW_GATE_OUTPUT, "New gate — a different ray from the one before");
		configOutput(ANGLE_OUTPUT, "Angle from gravity (±180° -> ±5V)");
		for (int i = 0; i < NUM_SECTORS; i++) {
			configOutput(SECTOR_OUTPUT + i, string::f("Sector %d distance (morph 0-10V, or bipolar +-5V)", i + 1));
			configOutput(GATE_OUTPUT + i, string::f("Ray %d gate", i + 1));
		}
	}

	// Forces (angular accelerations) of the analytic double pendulum.
	// Angular accelerations via the mass-matrix form. Gravity pulls toward
	// `gravAngle` (a movable centre of gravity): the gravity torques use
	// (theta - gravAngle), while the inertial/Coriolis terms depend only on the
	// angle difference and are unaffected. gravAngle = 0 -> straight down.
	void accel(double a1, double a2, double v1, double v2, double& o1, double& o2) {
		double d = a1 - a2;
		double cd = std::cos(d), sd = std::sin(d);
		double M11 = (M1 + M2) * L1;
		double M12 = M2 * L2 * cd;
		double M21 = L1 * cd;
		double M22 = L2;
		double R1 = -M2 * L2 * v2 * v2 * sd - (M1 + M2) * GRAV * std::sin(a1 - gravAngle);
		double R2 =  L1 * v1 * v1 * sd       - GRAV * std::sin(a2 - gravAngle);
		double det = M11 * M22 - M12 * M21;
		if (std::abs(det) < 1e-9) det = (det < 0 ? -1e-9 : 1e-9);
		o1 = (R1 * M22 - M12 * R2) / det;
		o2 = (M11 * R2 - M21 * R1) / det;
	}

	void rk4Step(double h) {
		double k1a1, k1a2; accel(th1, th2, w1, w2, k1a1, k1a2);
		double k1t1 = w1, k1t2 = w2;

		double k2a1, k2a2; accel(th1 + 0.5 * h * k1t1, th2 + 0.5 * h * k1t2,
		                         w1 + 0.5 * h * k1a1, w2 + 0.5 * h * k1a2, k2a1, k2a2);
		double k2t1 = w1 + 0.5 * h * k1a1, k2t2 = w2 + 0.5 * h * k1a2;

		double k3a1, k3a2; accel(th1 + 0.5 * h * k2t1, th2 + 0.5 * h * k2t2,
		                         w1 + 0.5 * h * k2a1, w2 + 0.5 * h * k2a2, k3a1, k3a2);
		double k3t1 = w1 + 0.5 * h * k2a1, k3t2 = w2 + 0.5 * h * k2a2;

		double k4a1, k4a2; accel(th1 + h * k3t1, th2 + h * k3t2,
		                         w1 + h * k3a1, w2 + h * k3a2, k4a1, k4a2);
		double k4t1 = w1 + h * k3a1, k4t2 = w2 + h * k3a2;

		th1 += (h / 6.0) * (k1t1 + 2 * k2t1 + 2 * k3t1 + k4t1);
		th2 += (h / 6.0) * (k1t2 + 2 * k2t2 + 2 * k3t2 + k4t2);
		w1  += (h / 6.0) * (k1a1 + 2 * k2a1 + 2 * k3a1 + k4a1);
		w2  += (h / 6.0) * (k1a2 + 2 * k2a2 + 2 * k3a2 + k4a2);
	}

	void relaunch() {
		// Throw it back up into the chaotic regime.
		th1 = 1.5 + 2.0 * (random::uniform() - 0.5) * M_PI;
		th2 = 1.5 + 2.0 * (random::uniform() - 0.5) * M_PI;
		w1 = (random::uniform() - 0.5) * 4.0;
		w2 = (random::uniform() - 0.5) * 4.0;
	}

	// Two-link inverse kinematics: angles that place the tip at (tx,ty).
	// Picks the elbow side nearest the current pose for continuity.
	void ikTip(double tx, double ty, double& outTh1, double& outTh2) {
		double dist = std::sqrt(tx * tx + ty * ty);
		dist = clampd(dist, (double) std::abs(L1 - L2) + 1e-3, (double) REACH - 1e-3);
		// Rescale target onto the reachable circle so trig stays valid.
		double ang = std::atan2(tx, ty);          // direction from down-vertical
		double cosB = (dist * dist + L1 * L1 - L2 * L2) / (2 * L1 * dist);
		cosB = clampd(cosB, -1.0, 1.0);
		double beta = std::acos(cosB);

		double cand1 = ang - beta;
		double cand2 = ang + beta;
		// Choose the candidate whose resulting th1 is closest to current.
		double pick = (std::abs(wrapPi(cand1 - th1)) <= std::abs(wrapPi(cand2 - th1)))
		            ? cand1 : cand2;
		outTh1 = pick;
		double b1x = L1 * std::sin(pick), b1y = L1 * std::cos(pick);
		outTh2 = std::atan2(tx - b1x, ty - b1y);
	}

	void process(const ProcessArgs& args) override {
		// --- Time scale ---
		float speed = params[SPEED_PARAM].getValue();
		if (inputs[SPEED_INPUT].isConnected())
			speed += inputs[SPEED_INPUT].getVoltage() / 10.f;   // ~1 oct / 10V handled below
		float speedOct = clamp(speed, -0.5f, 1.5f) * 9.f - 3.f;
		float speedMult = std::pow(2.f, speedOct);
		double h = args.sampleTime * speedMult;

		float chaos = params[CHAOS_PARAM].getValue();
		if (inputs[CHAOS_INPUT].isConnected())
			chaos += inputs[CHAOS_INPUT].getVoltage() / 10.f;
		chaos = clamp(chaos, 0.f, 1.f);

		// GRAVITY knob is mode-dependent:
		//  - Pendulum / Billiards: DIRECTION. Knob -1..1 -> -180..180 deg; CV
		//    adds 36 deg/V, so a 0-10V phasor spins gravity once around.
		//  - Gravity Well: MAGNITUDE of the central sun's pull (shown as a
		//    center circle sized by the amount).
		float grav = params[GRAVITY_PARAM].getValue();
		gravAngle = (double) grav * M_PI;
		if (inputs[GRAVITY_INPUT].isConnected())
			gravAngle += inputs[GRAVITY_INPUT].getVoltage() * (M_PI / 5.0);
		// GW pull: full knob sweep -1..+1 -> 0..1; CV nudges at 0.1/V.
		gwSunPull = clamp((grav + 1.f) * 0.5f
			+ (inputs[GRAVITY_INPUT].isConnected() ? inputs[GRAVITY_INPUT].getVoltage() * 0.1f : 0.f),
			0.f, 1.f);

		// --- MODE select (knob + CV; 0-10V spans all modes) ---
		int modeRaw = (int) std::round(params[MODE_PARAM].getValue());
		if (inputs[MODE_INPUT].isConnected())
			modeRaw += (int) std::floor(inputs[MODE_INPUT].getVoltage() / (10.f / (float) NUM_MODES) + 0.001f);
		mode = clamp(modeRaw, 0, (int) NUM_MODES - 1);
		if (mode != prevMode) { initMode(mode); prevMode = mode; }

		// Substep so integrators stay stable when SPEED pushes h large.
		int nsub = 1;
		while (h / nsub > 0.004 && nsub < 16) nsub++;
		double hsub = h / nsub;

		gatePtCount = 0;

		// ---- Mode physics: fill trackedX/Y + gate points + display state ----
		if (mode == MODE_PENDULUM) {
			if (dragJoint == DRAG_NONE) {
				for (int s = 0; s < nsub; s++) rk4Step(hsub);
				// Energy servo: hold a kinetic target set by CHAOS.
				double instE = w1 * w1 + w2 * w2;
				energyLP += (instE - energyLP) * 0.0008;
				if (energyLP < 1e-4) energyLP = 1e-4;
				double targetE = 0.3 + 13.7 * chaos;
				double f = std::pow(targetE / energyLP, 0.6 * h);
				f = clampd(f, 0.985, 1.015);
				w1 *= f; w2 *= f;
				w1 = clampd(w1, -60.0, 60.0);
				w2 = clampd(w2, -60.0, 60.0);
				w1Smooth = w1; w2Smooth = w2;
			}
			else {
				double tx = dragTargetX, ty = dragTargetY;
				if (dragJoint == DRAG_TIP) {
					double nt1, nt2;
					ikTip(tx, ty, nt1, nt2);
					double dv1 = wrapPi(nt1 - prevDragTh1) / std::max(h, 1e-6);
					double dv2 = wrapPi(nt2 - prevDragTh2) / std::max(h, 1e-6);
					w1Smooth += (dv1 - w1Smooth) * 0.05;
					w2Smooth += (dv2 - w2Smooth) * 0.05;
					th1 = nt1; th2 = nt2;
					w1 = clampd(w1Smooth, -40.0, 40.0);
					w2 = clampd(w2Smooth, -40.0, 40.0);
					prevDragTh1 = nt1; prevDragTh2 = nt2;
				}
				else {
					double nt1 = std::atan2(tx, ty);
					double dv1 = wrapPi(nt1 - prevDragTh1) / std::max(h, 1e-6);
					w1Smooth += (dv1 - w1Smooth) * 0.05;
					for (int s = 0; s < nsub; s++) {
						rk4Step(hsub);
						th1 = nt1;
						w1 = clampd(w1Smooth, -40.0, 40.0);
					}
					prevDragTh1 = nt1;
				}
			}
			float b1x = L1 * std::sin((float) th1);
			float b1y = L1 * std::cos((float) th1);
			float b2x = b1x + L2 * std::sin((float) th2);
			float b2y = b1y + L2 * std::cos((float) th2);
			dispB1x = b1x; dispB1y = b1y;
			dispB2x = b2x; dispB2y = b2y;
			trackedX = b2x; trackedY = b2y;
			gatePtX[0] = b2x; gatePtY[0] = b2y; gatePtCount = 1;
		}
		else if (mode == MODE_GRAVWELL) {
			stepGravWell(hsub, nsub, chaos);
			trackedX = (float) gwRx; trackedY = (float) gwRy;
			dispB1x = 0.f; dispB1y = 0.f;          // no arm in this mode
			dispB2x = trackedX; dispB2y = trackedY;
			gatePtX[0] = trackedX; gatePtY[0] = trackedY; gatePtCount = 1;
		}
		else if (mode == MODE_BILLIARDS) {
			stepBilliards(hsub, nsub, chaos);
			trackedX = (float) bzX[0]; trackedY = (float) bzY[0];
			dispB1x = 0.f; dispB1y = 0.f;
			dispB2x = trackedX; dispB2y = trackedY;
			for (int i = 0; i < bzCount && i < MAX_BALLS; i++) {
				gatePtX[i] = (float) bzX[i];
				gatePtY[i] = (float) bzY[i];
			}
			gatePtCount = bzCount;
		}
		else if (mode == MODE_HUNGRY) {
			// Use RAW sample time, not the pendulum's exponential speedMult — the
			// engine applies its own linear rate. (Feeding the ~1448x speedMult
			// here made the per-sample while-loop iterate many cells, spiking CPU
			// and making the UI/pots sluggish.)
			stepHungry(args.sampleTime, chaos, grav);
			dispB1x = 0.f; dispB1y = 0.f;
			dispB2x = trackedX; dispB2y = trackedY;
			gatePtX[0] = trackedX; gatePtY[0] = trackedY; gatePtCount = 1;
		}
		else if (mode == MODE_TURTLE) {
			// Own linear rate (like Hungry Man) — raw sample time, not speedMult.
			stepTurtle(args.sampleTime, speed, chaos, grav);
			dispB1x = 0.f; dispB1y = 0.f;
			dispB2x = trackedX; dispB2y = trackedY;
			gatePtX[0] = trackedX; gatePtY[0] = trackedY; gatePtCount = 1;
		}
		else { // MODE_PATTERN
			stepPattern(args.sampleTime, speed, chaos, grav);
			dispB1x = 0.f; dispB1y = 0.f;
			dispB2x = trackedX; dispB2y = trackedY;
			gatePtX[0] = trackedX; gatePtY[0] = trackedY; gatePtCount = 1;
		}

		// ---- Shared output tail (driven by the tracked point) ----
		float tx = trackedX, ty = trackedY;
		outputs[X_OUTPUT].setVoltage(clamp(tx / REACH * 5.f, -5.f, 5.f));
		outputs[Y_OUTPUT].setVoltage(clamp(-ty / REACH * 5.f, -5.f, 5.f));

		float radius = std::sqrt(tx * tx + ty * ty);        // 0..REACH
		float phi = std::atan2(tx, ty);                     // 0 at straight down

		outputs[RADIUS_OUTPUT].setVoltage(clamp(radius / REACH * 10.f - 5.f, -5.f, 5.f));
		float angFromGrav = wrapPi(phi - (float) gravAngle);
		outputs[ANGLE_OUTPUT].setVoltage(clamp(angFromGrav / (float) M_PI * 5.f, -5.f, 5.f));

		// Sector morph from the tracked point.
		float radV = radius / REACH * 10.f;
		for (int i = 0; i < NUM_SECTORS; i++) {
			float center = (i * 60.f + 30.f) * DEG;
			float target;
			if (sectorMode == 0) {
				float dd = std::abs(wrapPi(phi - center)) / (60.f * DEG);
				float wgt = clamp(1.f - dd, 0.f, 1.f);
				target = wgt * radV;
			} else {
				// Project the point onto this sector's own axis. phi is measured
				// from straight down, so the axis is (sin, cos) — the same frame
				// the panel places the jacks in. +5V means the point is ON the rim
				// in this sector's direction, 0V the centre, -5V the far rim: the
				// distance from the edge is (5 - out) in the same units.
				float proj = (tx * std::sin(center) + ty * std::cos(center)) / REACH;
				target = clamp(proj, -1.f, 1.f) * 5.f;
			}
			sectorOut[i] += (target - sectorOut[i]) * 0.02f;
			outputs[SECTOR_OUTPUT + i].setVoltage(sectorOut[i]);
			// Sector LED: brightness tracks signal strength (0..10V -> 0..1).
			lights[SECTOR_LIGHT + i].setBrightness(sectorMode == 0
				? clamp(sectorOut[i] / 10.f, 0.f, 1.f)
				: clamp(std::abs(sectorOut[i]) / 5.f, 0.f, 1.f));
		}

		// Ray-cross gates: every gate point fires the ray it crosses. Each point
		// keeps its own continuous-angle tracker so crossings never alias.
		for (int p = 0; p < gatePtCount && p < MAX_BALLS; p++) {
			float pphi = std::atan2(gatePtX[p], gatePtY[p]);
			if (!gpInit[p]) {
				gpPrevPhi[p] = pphi;
				gpUnwrapped[p] = pphi;
				gpLastBucket[p] = (int) std::floor(gpUnwrapped[p] / (60.0 * DEG));
				gpInit[p] = true;
			}
			gpUnwrapped[p] += wrapPi(pphi - gpPrevPhi[p]);
			gpPrevPhi[p] = pphi;
			int bucket = (int) std::floor(gpUnwrapped[p] / (60.0 * DEG));
			if (bucket != gpLastBucket[p]) {
				int dir = (bucket > gpLastBucket[p]) ? 1 : -1;
				for (int b = gpLastBucket[p]; b != bucket; b += dir) {
					int boundary = (dir > 0) ? (b + 1) : b;
					int ray = ((boundary % NUM_SECTORS) + NUM_SECTORS) % NUM_SECTORS;
					gateTimer[ray] = gateHoldSec;
					dispGateFlash[ray] = 1.f;
					anyTimer = gateHoldSec;
					// ray index 0 is ray "1" at 0 degrees, so index parity is flipped
					((ray % 2 == 0) ? oddTimer : evenTimer) = gateHoldSec;
					if (ray != lastRayFired) newTimer = gateHoldSec;
					lastRayFired = ray;
				}
				gpLastBucket[p] = bucket;
			}
		}

		for (int i = 0; i < NUM_SECTORS; i++) {
			if (gateTimer[i] > 0.f) gateTimer[i] -= args.sampleTime;
			bool gateHigh = gateTimer[i] > 0.f;
			outputs[GATE_OUTPUT + i].setVoltage(gateHigh ? 10.f : 0.f);
			// Gate LED: simply on while the gate is high.
			lights[GATE_LIGHT + i].setBrightness(gateHigh ? 1.f : 0.f);
			if (dispGateFlash[i] > 0.f) dispGateFlash[i] -= args.sampleTime * 6.f;
		}
		{
			float* t[4] = {&anyTimer, &oddTimer, &evenTimer, &newTimer};
			const int o[4] = {ANY_GATE_OUTPUT, ODD_GATE_OUTPUT, EVEN_GATE_OUTPUT, NEW_GATE_OUTPUT};
			for (int i = 0; i < 4; i++) {
				if (*t[i] > 0.f) *t[i] -= args.sampleTime;
				outputs[o[i]].setVoltage(*t[i] > 0.f ? 10.f : 0.f);
			}
		}
	}

	// ---- Mode (re)initialization ----
	void initMode(int m) {
		// Reset all gate-point trackers so crossings re-seed cleanly.
		for (int p = 0; p < MAX_BALLS; p++) gpInit[p] = false;
		if (m == MODE_GRAVWELL) initGravWell();
		else if (m == MODE_BILLIARDS) initBilliards();
		else if (m == MODE_HUNGRY) initHungry();
		else if (m == MODE_TURTLE) initTurtle();
		else if (m == MODE_PATTERN) initPattern();
	}

	// ===================== Turtle (LOGO graphics) =====================
	// Commonality of each command (0..1): higher = more conventional. GRAVITY
	// reshapes the selection weights toward common (high) or esoteric (low).
	static constexpr float tuCommon(int cmd) {
		// FD, veer, arc are everyday strokes; corners common; spiral/sethead/
		// hop/home are the "fancy" moves used sparingly by default.
		return (cmd == TU_STRAIGHT) ? 1.00f
		     : (cmd == TU_VEER_L || cmd == TU_VEER_R) ? 0.85f
		     : (cmd == TU_ARC_L  || cmd == TU_ARC_R)  ? 0.65f
		     : (cmd == TU_CORNER_L || cmd == TU_CORNER_R) ? 0.55f
		     : (cmd == TU_SPIRAL)  ? 0.25f
		     : (cmd == TU_SETHEAD) ? 0.20f
		     : (cmd == TU_PENHOP)  ? 0.15f
		     : 0.10f;  // TU_HOME
	}

	int pickTurtleCmd(float grav) {
		// alpha > 1 favors common commands, < 1 (and negative) favors esoteric.
		float alpha = 1.f + grav * 3.f;          // grav +1 -> 4, -1 -> -2
		float w[TU_NUMCMD]; float sum = 0.f;
		for (int i = 0; i < TU_NUMCMD; i++) {
			float c = clamp(tuCommon(i), 0.05f, 1.f);
			w[i] = std::pow(c, alpha);
			sum += w[i];
		}
		float r = random::uniform() * sum;
		for (int i = 0; i < TU_NUMCMD; i++) { r -= w[i]; if (r <= 0.f) return i; }
		return TU_STRAIGHT;
	}

	void newTurtleCmd(float speed, float chaos, float grav) {
		int cmd = pickTurtleCmd(grav);
		tuCmd = cmd;
		tuCmdTime = 0.f;
		// Duration: high CHAOS -> short commands (frequent changes of direction).
		float u = 1.f - clamp(chaos, 0.f, 1.f);
		tuCmdDur = 0.12f + 1.8f * u * u;          // 0.12 .. 1.92 s
		float sp = clamp(speed, 0.f, 1.5f);
		float V = 0.45f + 4.5f * sp;              // forward units/s
		float W = (55.f + 320.f * sp) * DEG;      // base turn rad/s
		tuTurnAccel = 0.f;
		tuPenDown = true;
		switch (cmd) {
			case TU_STRAIGHT: tuMoveRate = V;        tuTurnRate = 0.f; break;
			case TU_VEER_L:   tuMoveRate = V;        tuTurnRate = +W * 0.35f; break;
			case TU_VEER_R:   tuMoveRate = V;        tuTurnRate = -W * 0.35f; break;
			case TU_ARC_L:    tuMoveRate = V * 0.85f; tuTurnRate = +W; break;
			case TU_ARC_R:    tuMoveRate = V * 0.85f; tuTurnRate = -W; break;
			case TU_CORNER_L: tuMoveRate = V * 0.2f;  tuTurnRate = +W * 2.6f; break;
			case TU_CORNER_R: tuMoveRate = V * 0.2f;  tuTurnRate = -W * 2.6f; break;
			case TU_SPIRAL: {
				float dir = (random::uniform() < 0.5f) ? 1.f : -1.f;
				tuMoveRate = V; tuTurnRate = dir * W * 0.25f; tuTurnAccel = dir * W * 1.6f;
				break;
			}
			case TU_SETHEAD:  tuHeading = random::uniform() * 2.f * (float) M_PI;
			                  tuMoveRate = V; tuTurnRate = 0.f; break;
			case TU_PENHOP:   tuPenDown = false; tuMoveRate = V * 1.6f; tuTurnRate = 0.f; break;
			case TU_HOME:     tuTeleport++; tuX = 0.f; tuY = 0.f;
			                  tuHeading = random::uniform() * 2.f * (float) M_PI;
			                  tuMoveRate = V; tuTurnRate = 0.f; break;
		}
		tuCmdSeq++;
	}

	void initTurtle() {
		tuX = 0.f; tuY = 0.f;
		tuHeading = (float) M_PI;     // facing up
		tuPenDown = true;
		tuTeleport++;                 // break trail continuity from any prior mode
		tuInit = true;
		trackedX = tuX; trackedY = tuY;
		newTurtleCmd(params[SPEED_PARAM].getValue(),
		             params[CHAOS_PARAM].getValue(),
		             params[GRAVITY_PARAM].getValue());
	}

	void stepTurtle(float h, float speed, float chaos, float grav) {
		if (!tuInit) initTurtle();
		tuCmdTime += h;
		if (tuCmdTime >= tuCmdDur) newTurtleCmd(speed, chaos, grav);

		tuTurnRate += tuTurnAccel * h;
		tuHeading  += tuTurnRate * h;
		if (tuHeading > 1e4f || tuHeading < -1e4f) tuHeading = wrapPi(tuHeading);

		float step = tuMoveRate * h;
		float nx = tuX + std::sin(tuHeading) * step;
		float ny = tuY + std::cos(tuHeading) * step;

		// Bounce off the circular boundary so the drawing stays in the field.
		const float lim = REACH * 0.96f;
		float r = std::sqrt(nx * nx + ny * ny);
		if (r > lim && r > 1e-6f) {
			float Nx = nx / r, Ny = ny / r;          // outward normal
			float Dx = std::sin(tuHeading), Dy = std::cos(tuHeading);
			float dot = Dx * Nx + Dy * Ny;
			Dx -= 2.f * dot * Nx; Dy -= 2.f * dot * Ny;  // reflect heading
			tuHeading = std::atan2(Dx, Dy);
			nx = Nx * lim; ny = Ny * lim;            // pin just inside the rim
		}
		tuX = nx; tuY = ny;
		trackedX = tuX; trackedY = tuY;
	}

	// ===================== Pattern (turtle spirograph) =====================
	static int igcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a < 0 ? -a : a; }

	// Build a fresh rose figure into ptVx/ptVy. All angle steps are integer
	// degrees (divisions of 360) so the figure closes cleanly.
	void genPattern(float chaos, float grav) {
		float g = (grav + 1.f) * 0.5f;          // 0..1
		float c = clamp(chaos, 0.f, 1.f);

		// GRAVITY -> the FORM: a rational-frequency rose  r = sin((p/q)*theta).
		// q=1 gives plain few-petal roses; q>1 makes the curve take several turns
		// to close, weaving self-overlapping multi-loop rose-stars. The list runs
		// simple -> intricate so gravity genuinely changes the shape, not just its
		// petal count. (period = q*360, still all integer-degree divisions.)
		struct Form { int p, q; };
		static const Form formSet[10] = {
			{2,1}, {3,1}, {4,1}, {5,1},     // simple roses (low gravity)
			{5,2}, {7,2}, {7,3}, {9,4},     // multi-loop rose-stars (mid)
			{11,4}, {12,5}                   // dense webs (high gravity)
		};
		int fi = clamp((int) std::round(g * 9.f), 0, 9);
		int p = formSet[fi].p, q = formSet[fi].q;

		// CHAOS -> degree step d: small/even = smooth curve; coprime/large = web.
		static const int stepLow[4]  = {2, 3, 4, 6};
		static const int stepMid[4]  = {12, 18, 24, 36};
		static const int stepHigh[6] = {31, 47, 59, 73, 97, 131};
		int d;
		if      (c < 0.34f) d = stepLow [(int) (random::uniform() * 4) & 3];
		else if (c < 0.67f) d = stepMid [(int) (random::uniform() * 4) & 3];
		else                d = stepHigh[(int) (random::uniform() * 6) % 6];

		// High CHAOS adds a harmonic to the radius -> lobed / fractal-ish petals.
		float harmAmp = (c > 0.6f) ? (c - 0.6f) / 0.4f * 0.5f : 0.f;     // 0..0.5
		int   harmM   = p + 1 + (int) (random::uniform() * p);

		// Not every figure fills the field edge-to-edge.
		float fill = 0.55f + 0.42f * random::uniform();                  // 0.55..0.97

		int period = q * 360;                    // degrees until the curve closes
		int V = period / igcd(d, period);        // vertices before closure
		if (V > PT_MAXV - 2) V = PT_MAXV - 2;

		float ratio = (float) p / (float) q;
		float maxr = 1e-6f;
		for (int k = 0; k <= V; k++) {
			float th = (float) (k * d) * DEG;
			float rr = std::sin(ratio * th) * (1.f + harmAmp * std::sin(harmM * th));
			float x = rr * std::cos(th);
			float y = rr * std::sin(th);
			ptVx[k] = x; ptVy[k] = y;
			float m = std::sqrt(x * x + y * y);
			if (m > maxr) maxr = m;
		}
		float scale = fill * REACH / maxr;
		for (int k = 0; k <= V; k++) { ptVx[k] *= scale; ptVy[k] *= scale; }

		ptCount = V + 1;
		ptSeg = 0; ptProg = 0.f; ptDwell = 0.f;
		ptN = p; ptQ = q; ptD = d;
		ptGen++;                                // UI clears the trail on change
		trackedX = ptVx[0]; trackedY = ptVy[0];
		tuHeading = (ptCount > 1) ? std::atan2(ptVx[1] - ptVx[0], ptVy[1] - ptVy[0]) : 0.f;
		tuPenDown = true;
	}

	void initPattern() {
		ptInit = true;
		genPattern(params[CHAOS_PARAM].getValue(), params[GRAVITY_PARAM].getValue());
	}

	void stepPattern(float h, float speed, float chaos, float grav) {
		if (!ptInit) initPattern();

		float V = 0.4f + 5.0f * clamp(speed, 0.f, 1.5f);   // model units/s
		float dist = h * V;
		int guard = 0;
		while (dist > 0.f && guard++ < 64) {
			// Reached the end: never stop — clear and start the next figure now.
			if (ptSeg >= ptCount - 1) { genPattern(chaos, grav); continue; }
			float ax = ptVx[ptSeg],     ay = ptVy[ptSeg];
			float bx = ptVx[ptSeg + 1], by = ptVy[ptSeg + 1];
			float segLen = std::hypot(bx - ax, by - ay);
			if (segLen < 1e-6f) { ptSeg++; ptProg = 0.f; continue; }
			float remain = (1.f - ptProg) * segLen;
			if (dist < remain) { ptProg += dist / segLen; dist = 0.f; }
			else               { dist -= remain; ptSeg++; ptProg = 0.f; }
		}

		if (ptSeg < ptCount - 1) {
			float ax = ptVx[ptSeg],     ay = ptVy[ptSeg];
			float bx = ptVx[ptSeg + 1], by = ptVy[ptSeg + 1];
			tuX = ax + (bx - ax) * ptProg;
			tuY = ay + (by - ay) * ptProg;
			tuHeading = std::atan2(bx - ax, by - ay);
		} else if (ptCount > 0) {
			tuX = ptVx[ptCount - 1]; tuY = ptVy[ptCount - 1];
		}
		tuPenDown = true;
		trackedX = tuX; trackedY = tuY;
	}

	// ===================== Gravity Well =====================
	void initGravWell() {
		gwPlanetCount = 2 + (int) std::round(params[CHAOS_PARAM].getValue() * 3.f);  // 2..5
		gwPlanetCount = clamp(gwPlanetCount, 2, MAX_PLANETS);
		for (int i = 0; i < MAX_PLANETS; i++) {
			float t = (i + 1) / (float)(MAX_PLANETS + 1);
			gwPlanetRad[i]  = 0.55 + 1.55 * t;                 // wider spread; outers sit further out
			gwPlanetRate[i] = 0.9 / (0.4 + gwPlanetRad[i]);    // inner faster
			// Mass grows with orbital radius: inner planets light, outer planets
			// heavy. The big outer worlds yank the orbit hard on each flyby for
			// chaotic, unpredictable arcs (the spring still prevents escape/capture).
			gwPlanetMass[i] = 0.4 + 2.6 * t + 0.4 * random::uniform();
			gwPlanetAng[i]  = random::uniform() * 2.0 * M_PI;
		}
		// Start in a circular spring orbit: for F=-K*r, circular speed v = r*sqrt(K).
		double r0 = 0.85;
		double K0 = 2.2 + 5.0 * (double) gwSunPull;
		gwRx = r0; gwRy = 0.0;
		double vCirc = r0 * std::sqrt(K0);
		gwVx = 0.0; gwVy = vCirc;
		gwInit = true;
	}

	void stepGravWell(double hsub, int nsub, float chaos) {
		if (!gwInit) initGravWell();
		gwPlanetCount = 2 + (int) std::round(chaos * 3.f);
		gwPlanetCount = clamp(gwPlanetCount, 2, MAX_PLANETS);
		const double G = 0.18;
		// Central force is a SPRING (linear, F = -k*r toward center), not 1/r^2.
		// A spring gives stable bounded ellipses at ANY energy: it can't pull the
		// rocket into the center (force -> 0 there) and can't let it escape (force
		// grows with distance). This sidesteps the knife-edge orbital balance of
		// Newtonian gravity. GRAV sets the spring stiffness (orbit size/speed).
		// Spring stiffness from GRAV. Floor raised so even at GRAV=0 the central
		// pull is strong enough to reclaim the rocket from a heavy planet —
		// prevents planet capture at low grav / high chaos.
		const double K = 2.2 + 5.0 * (double) gwSunPull;   // stiffness from GRAV knob
		const double soft2 = 0.18 * 0.18;
		const double PG = G * 2.2;
		// Per-planet acceleration cap: keeps the sharp flyby kick but BOUNDS the
		// well so a planet can never trap the rocket into a tight bound orbit.
		// Set near the spring's max pull so the spring always wins eventually.
		const double PLANET_ACC_CAP = 9.0;
		for (int s = 0; s < nsub; s++) {
			for (int i = 0; i < gwPlanetCount; i++)
				gwPlanetAng[i] += gwPlanetRate[i] * hsub;
			double ax = -K * gwRx;     // central spring
			double ay = -K * gwRy;
			for (int i = 0; i < gwPlanetCount; i++) {
				double px = gwPlanetRad[i] * std::cos(gwPlanetAng[i]);
				double py = gwPlanetRad[i] * std::sin(gwPlanetAng[i]);
				double dx = px - gwRx, dy = py - gwRy;
				double r2 = dx * dx + dy * dy + soft2;
				double inv = 1.0 / (r2 * std::sqrt(r2));
				double pax = PG * gwPlanetMass[i] * dx * inv;
				double pay = PG * gwPlanetMass[i] * dy * inv;
				// Clamp this planet's acceleration magnitude.
				double pa2 = pax * pax + pay * pay;
				if (pa2 > PLANET_ACC_CAP * PLANET_ACC_CAP) {
					double s2 = PLANET_ACC_CAP / std::sqrt(pa2);
					pax *= s2; pay *= s2;
				}
				ax += pax; ay += pay;
			}
			gwVx += ax * hsub; gwVy += ay * hsub;
			gwRx += gwVx * hsub; gwRy += gwVy * hsub;
			// Safety rim: a violent flyby could in principle overshoot; reflect
			// so the rocket always stays in the visible disc.
			double d2 = gwRx * gwRx + gwRy * gwRy;
			double WALL = REACH * 1.02;
			if (d2 > WALL * WALL) {
				double d = std::sqrt(d2);
				double nx = gwRx / d, ny = gwRy / d;
				double vn = gwVx * nx + gwVy * ny;
				if (vn > 0.0) { gwVx -= 2.0 * vn * nx; gwVy -= 2.0 * vn * ny; }
				gwRx = nx * WALL; gwRy = ny * WALL;
			}
		}
		// Very light energy governor: only nudges back when the orbit drifts far
		// outside a generous band, so heavy-planet flybys are free to throw the
		// orbit around. Without this the perturbations would slowly pump energy.
		double e = gwVx * gwVx + gwVy * gwVy + K * (gwRx * gwRx + gwRy * gwRy);
		double targetE = K * 0.85 * 0.85 * (1.0 + 0.8 * chaos);
		double ratio = e / std::max(targetE, 1e-6);
		// Only correct when >2x or <0.5x the target — leave the chaotic middle alone.
		if (ratio > 2.0 || ratio < 0.5) {
			double f = std::sqrt(targetE / e);
			f = clampd(f, 0.995, 1.005);
			gwVx *= f; gwVy *= f;
			gwRx *= f; gwRy *= f;
		}
	}

	// ===================== Billiards =====================
	void initBilliards() {
		bzCount = 1 + (int) std::round(params[CHAOS_PARAM].getValue() * 8.f);  // cue + 0..8
		bzCount = clamp(bzCount, 1, MAX_BALLS);
		for (int i = 0; i < MAX_BALLS; i++) {
			double a = random::uniform() * 2.0 * M_PI;
			double r = 0.2 + 0.7 * random::uniform();
			bzX[i] = r * std::cos(a);
			bzY[i] = r * std::sin(a);
			bzMass[i] = (i == 0) ? CUE_MASS : 1.0;
			// small random initial velocities; cue gets a bit more
			double va = random::uniform() * 2.0 * M_PI;
			double vs = (i == 0) ? 0.9 : 0.4 * random::uniform();
			bzVx[i] = vs * std::cos(va);
			bzVy[i] = vs * std::sin(va);
		}
		bzInit = true;
	}

	double billiardsKE() {
		double e = 0.0;
		for (int i = 0; i < bzCount; i++)
			e += 0.5 * bzMass[i] * (bzVx[i] * bzVx[i] + bzVy[i] * bzVy[i]);
		return e;
	}

	void stepBilliards(double hsub, int nsub, float chaos) {
		if (!bzInit) initBilliards();
		int want = 1 + (int) std::round(chaos * 8.f);
		want = clamp(want, 1, MAX_BALLS);
		if (want != bzCount) {
			// Grow/shrink the active set without disturbing existing balls.
			if (want > bzCount) {
				for (int i = bzCount; i < want; i++) {
					double a = random::uniform() * 2.0 * M_PI;
					double r = 0.2 + 0.7 * random::uniform();
					bzX[i] = r * std::cos(a); bzY[i] = r * std::sin(a);
					bzMass[i] = 1.0;
					double va = random::uniform() * 2.0 * M_PI, vs = 0.4 * random::uniform();
					bzVx[i] = vs * std::cos(va); bzVy[i] = vs * std::sin(va);
				}
			}
			bzCount = want;
			for (int p = 0; p < MAX_BALLS; p++) gpInit[p] = false;
		}

		// Aiming (slingshot): hold the cue still at its spot while the user drags;
		// release applies an impulse opposite to the drag (handled in onDragEnd).
		double keBefore = billiardsKE();
		double wallR = REACH - BALL_R;

		// Table tilt: GRAV direction adds a constant acceleration so balls pool
		// toward one side of the circle (changes which rays get hit most).
		const double TILT = 1.1;
		double gx = std::sin(gravAngle) * TILT;
		double gy = std::cos(gravAngle) * TILT;

		for (int s = 0; s < nsub; s++) {
			for (int i = 0; i < bzCount; i++) {
				if (i == 0 && bzAiming) continue;        // cue frozen while aiming
				bzVx[i] += gx * hsub;                    // table tilt
				bzVy[i] += gy * hsub;
				bzX[i] += bzVx[i] * hsub;
				bzY[i] += bzVy[i] * hsub;
				// Wall: reflect about the inward normal, clamp inside.
				double d2 = bzX[i] * bzX[i] + bzY[i] * bzY[i];
				if (d2 > wallR * wallR) {
					double d = std::sqrt(d2);
					double nx = bzX[i] / d, ny = bzY[i] / d;
					double vn = bzVx[i] * nx + bzVy[i] * ny;
					if (vn > 0.0) { bzVx[i] -= 2.0 * vn * nx; bzVy[i] -= 2.0 * vn * ny; }
					bzX[i] = nx * wallR; bzY[i] = ny * wallR;
				}
			}
			// Ball-ball elastic collisions (equal-radius, mass-weighted).
			for (int i = 0; i < bzCount; i++) {
				for (int j = i + 1; j < bzCount; j++) {
					double dx = bzX[j] - bzX[i], dy = bzY[j] - bzY[i];
					double d2 = dx * dx + dy * dy;
					double minD = 2.0 * BALL_R;
					if (d2 > 1e-12 && d2 < minD * minD) {
						double d = std::sqrt(d2);
						double nx = dx / d, ny = dy / d;
						// separate overlap proportionally to inverse mass
						double overlap = minD - d;
						double im1 = 1.0 / bzMass[i], im2 = 1.0 / bzMass[j];
						double imsum = im1 + im2;
						bzX[i] -= nx * overlap * (im1 / imsum);
						bzY[i] -= ny * overlap * (im1 / imsum);
						bzX[j] += nx * overlap * (im2 / imsum);
						bzY[j] += ny * overlap * (im2 / imsum);
						// relative velocity along normal
						double rvx = bzVx[j] - bzVx[i], rvy = bzVy[j] - bzVy[i];
						double vn = rvx * nx + rvy * ny;
						if (vn < 0.0) {
							double imp = -2.0 * vn / imsum;   // elastic
							bzVx[i] -= imp * im1 * nx; bzVy[i] -= imp * im1 * ny;
							bzVx[j] += imp * im2 * nx; bzVy[j] += imp * im2 * ny;
						}
					}
				}
			}
		}

		// KE normalization: elastic collisions conserve energy; this corrects
		// integrator drift so motion neither dies nor blows up. Skip while aiming.
		if (!bzAiming) {
			double keAfter = billiardsKE();
			if (keAfter > 1e-6 && keBefore > 1e-6) {
				double f = std::sqrt(keBefore / keAfter);
				f = clampd(f, 0.98, 1.02);
				for (int i = 0; i < bzCount; i++) { bzVx[i] *= f; bzVy[i] *= f; }
			}
		}
	}

	// Apply the slingshot launch (called from the display on drag release).
	void billiardsLaunch(double dragX, double dragY) {
		// Launch opposite the drag, power proportional to drag distance.
		double dx = bzX[0] - dragX, dy = bzY[0] - dragY;
		double power = std::sqrt(dx * dx + dy * dy) * 2.5;
		double d = std::sqrt(dx * dx + dy * dy);
		if (d > 1e-6) { bzVx[0] = dx / d * power; bzVy[0] = dy / d * power; }
	}

	// ===================== Hungry Man (Pac-Man) =====================
	// Cell neighbors: a cell (r,s) connects to (r-1,s), (r+1,s), (r,s-1), (r,s+1)
	// IF the wall between them was carved open. Passage flags:
	//   ring open between (r,s)-(r+1,s): hmPassRing[r*S+s], valid r in 0..RINGS-2
	//   rad  open between (r,s)-(r,s+1): hmPassRad[r*S+s]  (wraps around)
	bool hmOpen(int a, int b) const {
		int ra = a / HM_SPOKES, sa = a % HM_SPOKES;
		int rb = b / HM_SPOKES, sb = b % HM_SPOKES;
		if (ra == rb) {                     // same ring -> radial(angular) passage
			int s0 = sa, s1 = sb;
			if ((s0 + 1) % HM_SPOKES == s1) return hmPassRad[ra * HM_SPOKES + s0];
			if ((s1 + 1) % HM_SPOKES == s0) return hmPassRad[ra * HM_SPOKES + s1];
			return false;
		}
		if (sa == sb) {                     // adjacent rings -> ring passage
			int rlo = ra < rb ? ra : rb;
			if (rlo + 1 == (ra > rb ? ra : rb) && rlo < HM_RINGS - 1)
				return hmPassRing[rlo * HM_SPOKES + sa];
		}
		return false;
	}

	void hmRebuildAdj() {
		for (int n = 0; n < HM_NODES; n++) hmAdjN[n] = 0;
		for (int r = 0; r < HM_RINGS; r++) for (int s = 0; s < HM_SPOKES; s++) {
			int n = hmCell(r, s);
			int nbs[4] = { (r > 0) ? hmCell(r - 1, s) : -1,
			               (r < HM_RINGS - 1) ? hmCell(r + 1, s) : -1,
			               hmCell(r, s - 1), hmCell(r, s + 1) };
			for (int k = 0; k < 4; k++) {
				if (nbs[k] < 0 || nbs[k] == n) continue;
				if (hmOpen(n, nbs[k])) hmAdj[n][hmAdjN[n]++] = nbs[k];
			}
		}
	}

	// Carve a perfect maze over the CELL grid (walls between cells). Kruskal
	// over the cell-adjacency graph; an opened adjacency = no wall drawn.
	// Full reset: new game at level 1, score 0.
	void initHungry() {
		hmLevel = 1;
		hmScore = 0;
		hmLevelFlash = 0.f;
		hmInit = true;
		hmNewMaze();
	}

	// Carve a fresh maze + dots and drop Hungry Man at the start. Does NOT touch
	// level/score (used both at game start and on level advance).
	void hmNewMaze() {
		for (int i = 0; i < HM_NODES; i++) { hmPassRing[i] = false; hmPassRad[i] = false; }

		// Candidate passages between adjacent cells.
		struct Cand { int a, b; bool ring; int idx; };
		static Cand cand[HM_NODES * 2];
		int nc = 0;
		for (int r = 0; r < HM_RINGS - 1; r++) for (int s = 0; s < HM_SPOKES; s++)
			cand[nc++] = {hmCell(r, s), hmCell(r + 1, s), true, r * HM_SPOKES + s};
		for (int r = 0; r < HM_RINGS; r++) for (int s = 0; s < HM_SPOKES; s++)
			cand[nc++] = {hmCell(r, s), hmCell(r, s + 1), false, r * HM_SPOKES + s};

		static int parent[HM_NODES];
		for (int n = 0; n < HM_NODES; n++) parent[n] = n;
		for (int i = nc - 1; i > 0; i--) {
			int j = (int) (random::uniform() * (i + 1)); if (j > i) j = i;
			Cand t = cand[i]; cand[i] = cand[j]; cand[j] = t;
		}
		auto openPass = [&](const Cand& c) {
			if (c.ring) hmPassRing[c.idx] = true; else hmPassRad[c.idx] = true;
		};
		for (int i = 0; i < nc; i++) {
			int ra = cand[i].a, rb = cand[i].b;
			while (parent[ra] != ra) { parent[ra] = parent[parent[ra]]; ra = parent[ra]; }
			while (parent[rb] != rb) { parent[rb] = parent[parent[rb]]; rb = parent[rb]; }
			if (ra != rb) { parent[ra] = rb; openPass(cand[i]); }
		}
		// NOTE: pure spanning tree only — no braiding. Adding extra passages
		// punches loops, and parallel loops read as confusing double-width
		// lanes. A perfect maze keeps every corridor exactly one cell wide.

		hmRebuildAdj();
		hmFrom = hmTo = hmCell(0, 0);
		hmProg = 0.f;
		hmTarget = -1;
		hmSpawnDots(params[CHAOS_PARAM].getValue(), params[GRAVITY_PARAM].getValue());
		float x, y; hmNodePos(hmFrom, x, y); trackedX = x; trackedY = y;
	}

	// One dot per cell (small), with CHAOS-count cells upgraded to big dots,
	// biased toward center (grav low) or edge (grav high).
	void hmSpawnDots(float chaos, float grav) {
		hmSmallCount = 0; hmBigCount = 0;
		for (int n = 0; n < HM_NODES; n++) { hmSmallDot[n] = true; hmBigDot[n] = false; hmSmallCount++; }
		int want = 1 + (int) std::round(chaos * 6.f);     // 1..7 big dots
		float bias = (grav + 1.f) * 0.5f;                 // 0 center .. 1 edge
		int tries = 0;
		while (hmBigCount < want && tries < 200) {
			tries++;
			float u = random::uniform();
			float shaped = (bias < 0.5f) ? std::pow(u, 1.f + (0.5f - bias) * 3.f)
			                             : 1.f - std::pow(1.f - u, 1.f + (bias - 0.5f) * 3.f);
			int r = (int) (shaped * HM_RINGS); if (r >= HM_RINGS) r = HM_RINGS - 1;
			int s = (int) (random::uniform() * HM_SPOKES) % HM_SPOKES;
			int node = hmCell(r, s);
			if (!hmBigDot[node]) { hmBigDot[node] = true; hmBigCount++; }
		}
	}

	// BFS from `start` to the nearest cell satisfying `want` (0 = big dot only,
	// 1 = any dot). Returns the first cell to step toward, or -1 if none exist.
	// `avoid` is a cell to deprioritize as the first step (so he doesn't reverse
	// when an equal-distance alternative exists) — pass -1 for none.
	int hmNextStep(int start, int want, int avoid) {
		static int prev[HM_NODES];
		static bool seen[HM_NODES];
		static int queue[HM_NODES];
		for (int n = 0; n < HM_NODES; n++) { seen[n] = false; prev[n] = -1; }
		int qh = 0, qt = 0;
		queue[qt++] = start; seen[start] = true;
		// Visit `avoid` last among start's neighbors by seeding it pre-seen only
		// if there's another option; simplest: just enqueue neighbors with the
		// avoid cell pushed to the back.
		int found = -1;
		while (qh < qt) {
			int cur = queue[qh++];
			bool hit = (want == 0) ? hmBigDot[cur] : (hmBigDot[cur] || hmSmallDot[cur]);
			if (cur != start && hit) { found = cur; break; }
			// Enqueue neighbors; defer `avoid` if it's a direct neighbor of start.
			int deferred = -1;
			for (int k = 0; k < hmAdjN[cur]; k++) {
				int nb = hmAdj[cur][k];
				if (seen[nb]) continue;
				if (cur == start && nb == avoid) { deferred = nb; continue; }
				seen[nb] = true; prev[nb] = cur; queue[qt++] = nb;
			}
			if (deferred >= 0 && !seen[deferred]) { seen[deferred] = true; prev[deferred] = cur; queue[qt++] = deferred; }
		}
		if (found < 0) return -1;
		int node = found;
		while (prev[node] != start && prev[node] != -1) node = prev[node];
		return node;
	}

	void stepHungry(float h, float chaos, float grav) {
		if (!hmInit) initHungry();
		// Constant LINEAR speed: spd is in "reference cells" per second, where a
		// reference cell == one radial step. We convert to model units and then
		// consume that distance across however many segments it spans, so a long
		// outer-ring arc takes proportionally longer to traverse.
		float cellsPerSec = 0.6f + 28.0f * clamp(params[SPEED_PARAM].getValue(), 0.f, 1.f);
		const float refLen = (1.f - HM_R0) / HM_RINGS * (REACH * 0.92f); // radial step len
		float dist = h * cellsPerSec * refLen;   // model units to travel this frame

		// Banner countdown (real time, independent of speed).
		if (hmLevelFlash > 0.f) hmLevelFlash -= h;

		// Safety cap on iterations (prevents any runaway from starving threads).
		int guard = 0;
		while (dist > 0.f && guard++ < 16) {
			float segLen = hmSegLen(hmFrom, hmTo);
			float remain = (1.f - hmProg) * segLen;
			if (hmFrom != hmTo && dist < remain) {
				hmProg += dist / segLen;
				dist = 0.f;
				break;
			}
			// Reached hmTo (or we were stationary): eat its dots, score, advance.
			dist -= remain;
			if (hmSmallDot[hmTo]) { hmSmallDot[hmTo] = false; hmSmallCount--; hmScore += 1; }
			if (hmBigDot[hmTo])   { hmBigDot[hmTo] = false; hmBigCount--; hmTarget = -1; hmScore += 5; }
			int prevCell = hmFrom;
			hmFrom = hmTo;
			// Board cleared: advance a level — flash "LEVEL N", draw a fresh maze.
			if (hmBigCount == 0 && hmSmallCount == 0) {
				hmLevel++;
				hmLevelFlash = 2.0f;
				hmNewMaze();
				dist = 0.f;
				break;
			}
			// Head to nearest big dot; if none, nearest ANY dot (full BFS, so he
			// can never stall while dots remain). Deprioritize reversing.
			int next = hmNextStep(hmFrom, 0, prevCell);
			if (next < 0) next = hmNextStep(hmFrom, 1, prevCell);
			if (next < 0) next = hmFrom;          // ultimate guard
			hmTo = next;
			hmProg = 0.f;
		}

		// Interpolate position between cell centers. Same-ring moves arc along
		// the ring; ring moves are straight radial lerps.
		float ax, ay, bx, by;
		hmNodePos(hmFrom, ax, ay);
		hmNodePos(hmTo, bx, by);
		int ra = hmFrom / HM_SPOKES, rb = hmTo / HM_SPOKES;
		if (ra == rb && hmFrom != hmTo) {
			int s0 = hmFrom % HM_SPOKES, s1 = hmTo % HM_SPOKES;
			int ds = s1 - s0;
			if (ds > HM_SPOKES / 2) ds -= HM_SPOKES;
			if (ds < -HM_SPOKES / 2) ds += HM_SPOKES;
			float rad = hmRingFrac(ra + 0.5f) * (REACH * 0.92f);
			float ang = (s0 + 0.5f + ds * hmProg) * (2.f * (float) M_PI / HM_SPOKES);
			trackedX = rad * std::sin(ang);
			trackedY = rad * std::cos(ang);
		} else {
			trackedX = ax + (bx - ax) * hmProg;
			trackedY = ay + (by - ay) * hmProg;
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "th1", json_real(th1));
		json_object_set_new(root, "th2", json_real(th2));
		json_object_set_new(root, "w1", json_real(w1));
		json_object_set_new(root, "w2", json_real(w2));
		json_object_set_new(root, "gateHoldSec", json_real(gateHoldSec));
		json_object_set_new(root, "sectorMode", json_integer(sectorMode));
		json_object_set_new(root, "trailLength", json_integer(trailLength));
		json_object_set_new(root, "mode", json_integer(mode));
		json_object_set_new(root, "hmLevel", json_integer(hmLevel));
		json_object_set_new(root, "hmScore", json_integer(hmScore));
		return root;
	}

	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "th1")) th1 = json_number_value(j);
		if (json_t* j = json_object_get(root, "th2")) th2 = json_number_value(j);
		if (json_t* j = json_object_get(root, "w1")) w1 = json_number_value(j);
		if (json_t* j = json_object_get(root, "w2")) w2 = json_number_value(j);
		if (json_t* j = json_object_get(root, "gateHoldSec")) gateHoldSec = json_number_value(j);
		if (json_t* j = json_object_get(root, "sectorMode")) sectorMode = clamp((int) json_integer_value(j), 0, 1);
		if (json_t* j = json_object_get(root, "trailLength")) trailLength = (int) json_integer_value(j);
		if (json_t* j = json_object_get(root, "mode")) {
			mode = clamp((int) json_integer_value(j), 0, (int) NUM_MODES - 1);
			prevMode = mode;
			initMode(mode);
		}
		// Restore score/level AFTER initMode() (which resets a fresh Hungry game).
		if (json_t* j = json_object_get(root, "hmLevel")) hmLevel = (int) json_integer_value(j);
		if (json_t* j = json_object_get(root, "hmScore")) hmScore = (long) json_integer_value(j);
	}
};


// ---------------------------------------------------------------------------
// Display: circular field, hex overlay, two tumbling arms, fading tip + elbow
// trails, gate-ray flashes, sector morph readout. Ragdoll drag on either bob.
// ---------------------------------------------------------------------------
struct GravityDisplay : OpaqueWidget {
	Gravity* module = nullptr;
	std::shared_ptr<Font> font;
	// Trails are UI-only: sampled once per frame, never persisted.
	std::vector<Vec> tipTrail, elbowTrail;

	// Turtle drawing trail (UI-only, long persistence via a big ring buffer).
	struct TurtlePt { float x, y; bool pen; bool brk; };
	std::deque<TurtlePt> turtleTrail;
	int tuLastTeleport = -1;
	// Instruction log (UI-only): the last few LOGO commands, newest at the end.
	std::deque<std::string> turtleLog;
	int tuLastSeq = -1;

	// Draw the rocket centered at (cx,cy), nose pointed along headingRad
	// (screen angle, y-down), scaled so its long axis ~= sizePx. The shape is
	// the res/rocket.svg path baked directly into nanovg beziers (viewBox
	// 28.8 x 50.84, art nose pointing toward -y), so there's no file to load.
	void drawRocket(NVGcontext* vg, float cx, float cy, float headingRad, float sizePx) {
		const float W = 28.8f, H = 50.84f;
		float scale = sizePx / H;
		nvgSave(vg);
		nvgTranslate(vg, cx, cy);
		nvgRotate(vg, headingRad + (float) M_PI / 2.f);  // nose (-y) -> heading
		nvgScale(vg, scale, scale);
		nvgTranslate(vg, -W * 0.5f, -H * 0.5f);
		nvgBeginPath(vg);
		nvgMoveTo(vg, 21.26f, 35.34f);
		nvgBezierTo(vg, 21.48f, 33.28f, 21.60f, 31.08f, 21.60f, 28.80f);
		nvgBezierTo(vg, 21.60f, 16.87f, 16.80f,  0.00f, 14.40f,  0.00f);
		nvgBezierTo(vg, 12.00f,  0.00f,  7.20f, 16.87f,  7.20f, 28.80f);
		nvgBezierTo(vg,  7.20f, 31.08f,  7.32f, 33.27f,  7.54f, 35.34f);
		nvgBezierTo(vg,  3.05f, 37.78f,  0.00f, 42.53f,  0.00f, 48.00f);
		nvgBezierTo(vg,  0.00f, 48.69f,  0.05f, 49.37f,  0.14f, 50.03f);
		nvgBezierTo(vg,  0.26f, 50.88f,  1.38f, 51.14f,  1.87f, 50.44f);
		nvgBezierTo(vg,  3.86f, 47.57f,  6.43f, 45.38f,  9.35f, 44.20f);
		nvgBezierTo(vg,  9.60f, 48.00f,  9.60f, 45.60f, 14.40f, 45.60f);
		nvgBezierTo(vg, 19.20f, 45.60f, 19.20f, 48.00f, 19.45f, 44.20f);
		nvgBezierTo(vg, 22.37f, 45.39f, 24.94f, 47.57f, 26.93f, 50.44f);
		nvgBezierTo(vg, 27.42f, 51.14f, 28.54f, 50.88f, 28.66f, 50.03f);
		nvgBezierTo(vg, 28.75f, 49.37f, 28.80f, 48.69f, 28.80f, 48.00f);
		nvgBezierTo(vg, 28.80f, 42.53f, 25.75f, 37.78f, 21.26f, 35.34f);
		nvgClosePath(vg);
		nvgFillColor(vg, nvgRGBA(0xEC, 0x65, 0x2E, 0xFF));
		nvgFill(vg);
		nvgRestore(vg);
	}

	static const int DRAG_GRAV = 99;   // dragging the gravity marker on the rim
	int dragging = Gravity::DRAG_NONE;
	Vec dragPos;   // widget-local cursor, kept current through the drag

	// Set the GRAVITY param so the centre of gravity points at the cursor.
	void setGravFromPos(Vec p) {
		float mx, my; toModel(p, mx, my);
		float ang = std::atan2(mx, my);              // 0 = down, matches field
		if (module) module->params[Gravity::GRAVITY_PARAM].setValue(clamp(ang / (float) M_PI, -1.f, 1.f));
	}

	float radiusPx() { return std::min(box.size.x, box.size.y) * 0.5f - 6.f; }
	Vec centerPx()   { return Vec(box.size.x * 0.5f, box.size.y * 0.5f); }
	float ppu()      { return radiusPx() / REACH; }   // pixels per pendulum unit

	// pendulum coords (+y down) -> widget pixels
	Vec toPx(float x, float y) {
		Vec c = centerPx();
		return Vec(c.x + x * ppu(), c.y + y * ppu());
	}
	// widget pixels -> pendulum coords
	void toModel(Vec p, float& x, float& y) {
		Vec c = centerPx();
		x = (p.x - c.x) / ppu();
		y = (p.y - c.y) / ppu();
	}

	static const int DRAG_CUE = 98;   // billiards slingshot aim

	void onButton(const ButtonEvent& e) override {
		if (module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			dragPos = e.pos;

			// Billiards: click near the cue starts a slingshot aim.
			if (module->mode == Gravity::MODE_BILLIARDS) {
				Vec cue = toPx(module->trackedX, module->trackedY);
				if (e.pos.minus(cue).norm() < 14.f) {
					dragging = DRAG_CUE;
					module->bzAiming = true;
					float mx, my; toModel(e.pos, mx, my);
					module->bzAimX = mx; module->bzAimY = my;
					e.consume(this);
					return;
				}
			}

			// Pendulum: grab a joint (ragdoll).
			if (module->mode == Gravity::MODE_PENDULUM) {
				Vec tip = toPx(module->dispB2x, module->dispB2y);
				Vec elbow = toPx(module->dispB1x, module->dispB1y);
				float dTip = e.pos.minus(tip).norm();
				float dElbow = e.pos.minus(elbow).norm();
				float thresh = 11.f;
				int joint = Gravity::DRAG_NONE;
				if (dTip <= dElbow && dTip < thresh) joint = Gravity::DRAG_TIP;
				else if (dElbow < thresh) joint = Gravity::DRAG_ELBOW;
				if (joint != Gravity::DRAG_NONE) {
					dragging = joint;
					float mx, my; toModel(e.pos, mx, my);
					module->dragTargetX = mx; module->dragTargetY = my;
					module->prevDragTh1 = module->th1;
					module->prevDragTh2 = module->th2;
					module->w1Smooth = 0.0; module->w2Smooth = 0.0;
					module->dragJoint = joint;
					e.consume(this);
					return;
				}
			}

			// Outer rim (all modes): grab the gravity marker and spin gravity.
			if (e.pos.minus(centerPx()).norm() > radiusPx() * 0.7f) {
				dragging = DRAG_GRAV;
				setGravFromPos(e.pos);
				e.consume(this);
				return;
			}
		}
		OpaqueWidget::onButton(e);
	}

	void onDragStart(const DragStartEvent& e) override { OpaqueWidget::onDragStart(e); }

	void onDragMove(const DragMoveEvent& e) override {
		if (module && dragging != Gravity::DRAG_NONE) {
			float zoom = getAbsoluteZoom();
			if (zoom <= 0.f) zoom = 1.f;
			dragPos = dragPos.plus(e.mouseDelta.div(zoom));
			if (dragging == DRAG_GRAV) {
				setGravFromPos(dragPos);
			} else if (dragging == DRAG_CUE) {
				float mx, my; toModel(dragPos, mx, my);
				module->bzAimX = mx; module->bzAimY = my;
			} else {
				float mx, my; toModel(dragPos, mx, my);
				module->dragTargetX = mx; module->dragTargetY = my;
			}
		}
		OpaqueWidget::onDragMove(e);
	}

	void onDragEnd(const DragEndEvent& e) override {
		if (module && dragging != Gravity::DRAG_NONE) {
			if (dragging == DRAG_CUE) {
				module->billiardsLaunch(module->bzAimX, module->bzAimY);
				module->bzAiming = false;
			} else if (dragging != DRAG_GRAV) {
				module->dragJoint = Gravity::DRAG_NONE;   // release -> let it fly
			}
			dragging = Gravity::DRAG_NONE;
		}
		OpaqueWidget::onDragEnd(e);
	}

	void pushTrail() {
		if (!module) return;
		int len = module->trailLength;
		if (len <= 0) { tipTrail.clear(); elbowTrail.clear(); return; }
		tipTrail.push_back(toPx(module->dispB2x, module->dispB2y));
		elbowTrail.push_back(toPx(module->dispB1x, module->dispB1y));
		while ((int) tipTrail.size() > len) tipTrail.erase(tipTrail.begin());
		while ((int) elbowTrail.size() > len) elbowTrail.erase(elbowTrail.begin());
	}

	void drawTrail(NVGcontext* vg, std::vector<Vec>& trail, NVGcolor col) {
		int n = (int) trail.size();
		for (int i = 1; i < n; i++) {
			float a = (float) i / n;          // newer = brighter
			nvgBeginPath(vg);
			nvgMoveTo(vg, trail[i - 1].x, trail[i - 1].y);
			nvgLineTo(vg, trail[i].x, trail[i].y);
			nvgStrokeColor(vg, nvgRGBAf(col.r, col.g, col.b, a * a * 0.7f));
			nvgStrokeWidth(vg, 1.2f);
			nvgStroke(vg);
		}
	}

	// Hungry Man: draw the carved maze passages, dots, and Pac-Man.
	void drawHungry(NVGcontext* vg) {
		if (!module) return;
		const NVGcolor COL_WALL = nvgRGBA(0x3a, 0x4a, 0x9a, 0xFF);   // maze walls (blue)
		const NVGcolor COL_DOT  = nvgRGBA(0xF0, 0xC0, 0x60, 0xF0);   // small dots
		const NVGcolor COL_BIG  = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);   // big dots (orange)
		const NVGcolor COL_PAC  = nvgRGBA(0xF5, 0xD0, 0x30, 0xFF);   // hungry man (yellow)
		const int   S = Gravity::HM_SPOKES;
		const int   RINGS = Gravity::HM_RINGS;
		const float RMAX = REACH * 0.92f;
		const float TAU = 2.f * (float) M_PI;
		Vec c = centerPx();

		auto ringRadiusPx = [&](int boundary) {        // boundary 0..RINGS
			return Gravity::hmRingFrac((float) boundary) * RMAX * ppu();
		};
		auto arcWall = [&](int boundary, int s) {       // arc across sector s at ring boundary
			float rpx = ringRadiusPx(boundary);
			float a0 = s * (TAU / S), a1 = (s + 1) * (TAU / S);
			nvgBeginPath(vg);
			int seg = 10;
			for (int i = 0; i <= seg; i++) {
				float a = a0 + (a1 - a0) * (float) i / seg;
				float px = c.x + std::sin(a) * rpx;
				float py = c.y + std::cos(a) * rpx;
				if (i == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
			}
			nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.6f);
			nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
		};
		auto radWall = [&](int s, int r) {              // radial spoke between sectors s|s+1, ring r
			float a = (s + 1) * (TAU / S);
			float r0 = ringRadiusPx(r), r1 = ringRadiusPx(r + 1);
			nvgBeginPath(vg);
			nvgMoveTo(vg, c.x + std::sin(a) * r0, c.y + std::cos(a) * r0);
			nvgLineTo(vg, c.x + std::sin(a) * r1, c.y + std::cos(a) * r1);
			nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.6f);
			nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
		};

		// Outer boundary ring + inner hub ring (both always closed).
		nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, ringRadiusPx(RINGS));
		nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);
		nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, ringRadiusPx(0));
		nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);

		// Ring walls between (r,s) and (r+1,s): draw unless that passage is open.
		for (int r = 0; r < RINGS - 1; r++)
			for (int s = 0; s < S; s++)
				if (!module->hmPassRing[r * S + s]) arcWall(r + 1, s);
		// Radial walls between (r,s) and (r,s+1): draw unless open. Skip the
		// innermost ring's radial walls so the center stays an open hub.
		for (int r = 0; r < RINGS; r++)
			for (int s = 0; s < S; s++)
				if (!module->hmPassRad[r * S + s]) radWall(s, r);

		// Dots at cell centers.
		float t = (float) system::getTime();
		float pulse = 0.75f + 0.25f * std::sin(t * 6.f);
		for (int n = 0; n < Gravity::HM_NODES; n++) {
			float x, y; module->hmNodePos(n, x, y); Vec p = toPx(x, y);
			if (module->hmBigDot[n]) {
				nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, 3.6f * pulse);
				nvgFillColor(vg, COL_BIG); nvgFill(vg);
			} else if (module->hmSmallDot[n]) {
				nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, 1.4f);
				nvgFillColor(vg, COL_DOT); nvgFill(vg);
			}
		}

		// Hungry Man: chomping wedge facing its travel direction.
		Vec hp = toPx(module->trackedX, module->trackedY);
		float fx, fy, tx, ty;
		module->hmNodePos(module->hmFrom, fx, fy);
		module->hmNodePos(module->hmTo, tx, ty);
		Vec hf = toPx(fx, fy), htn = toPx(tx, ty);
		float scr = (module->hmFrom == module->hmTo) ? 0.f
			: std::atan2(htn.y - hf.y, htn.x - hf.x);
		// Mouth opens/closes 0..~40° each side.
		float mouth = (0.05f + 0.30f * (0.5f + 0.5f * std::sin(t * 14.f))) * (float) M_PI;
		float rad = 6.0f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, hp.x, hp.y);
		nvgArc(vg, hp.x, hp.y, rad, scr + mouth, scr + TAU - mouth, NVG_CW);
		nvgClosePath(vg);
		nvgFillColor(vg, COL_PAC);
		nvgFill(vg);

		// HUD: score + level stacked inside the open center hub. Hidden while the
		// "LEVEL N" banner is flashing (the banner takes the center then).
		if (font && font->handle >= 0 && module->hmLevelFlash <= 0.f) {
			nvgFontFaceId(vg, font->handle);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFontSize(vg, 11.f);
			nvgFillColor(vg, nvgRGBA(0xF0, 0xC0, 0x60, 0xE0));
			nvgText(vg, c.x, c.y - 7.f, string::f("%ld", module->hmScore).c_str(), NULL);
			nvgFontSize(vg, 8.f);
			nvgFillColor(vg, nvgRGBA(0xF0, 0xC0, 0x60, 0x90));
			nvgText(vg, c.x, c.y + 7.f, string::f("LV %d", module->hmLevel).c_str(), NULL);
		}

		// "LEVEL N" banner flashes in the center on a new level.
		if (module->hmLevelFlash > 0.f && font && font->handle >= 0) {
			float a = clamp(module->hmLevelFlash / 2.0f, 0.f, 1.f);
			// gentle blink
			float blink = 0.55f + 0.45f * std::sin(t * 10.f);
			float alpha = a * blink;
			nvgFontFaceId(vg, font->handle);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFontSize(vg, 22.f);
			nvgFillColor(vg, nvgRGBAf(0.96f, 0.82f, 0.19f, alpha));
			nvgText(vg, c.x, c.y, string::f("LEVEL %d", module->hmLevel).c_str(), NULL);
		}
	}

	// Sample the turtle's position into the long-lived trail (once per frame).
	void recordTurtle() {
		if (!module) return;
		bool brk = module->tuTeleport != tuLastTeleport;
		tuLastTeleport = module->tuTeleport;
		float x = module->trackedX, y = module->trackedY;
		if (!turtleTrail.empty() && !brk) {
			const TurtlePt& last = turtleTrail.back();
			float dx = x - last.x, dy = y - last.y;
			const float minD = 0.006f * REACH;
			if (dx * dx + dy * dy < minD * minD) return;   // barely moved; skip
		}
		turtleTrail.push_back({x, y, module->tuPenDown, brk});
		const size_t MAXLEN = 6000;   // long persistence; oldest strokes age out
		while (turtleTrail.size() > MAXLEN) turtleTrail.pop_front();
	}

	// Build a LOGO-style label for the turtle's current command.
	std::string turtleLabel() {
		int cmd = module->tuCmd;
		int dist   = (int) std::round(module->tuMoveRate * module->tuCmdDur * 100.f);
		int turn   = (int) std::round(std::fabs(module->tuTurnRate * module->tuCmdDur) / DEG);
		int head   = ((int) std::round(module->tuHeading / DEG) % 360 + 360) % 360;
		switch (cmd) {
			case Gravity::TU_STRAIGHT: return string::f("FD %d", dist);
			case Gravity::TU_VEER_L:   return string::f("LT %d FD %d", turn, dist);
			case Gravity::TU_VEER_R:   return string::f("RT %d FD %d", turn, dist);
			case Gravity::TU_ARC_L:    return string::f("ARC L %d", turn);
			case Gravity::TU_ARC_R:    return string::f("ARC R %d", turn);
			case Gravity::TU_CORNER_L: return string::f("LT %d", turn);
			case Gravity::TU_CORNER_R: return string::f("RT %d", turn);
			case Gravity::TU_SPIRAL:   return string::f("SPIRAL %d", dist);
			case Gravity::TU_SETHEAD:  return string::f("SETH %d", head);
			case Gravity::TU_PENHOP:   return string::f("PU FD %d PD", dist);
			case Gravity::TU_HOME:     return "HOME";
		}
		return "FD";
	}

	// Draw a small top-down turtle at (px,py) facing heading hd (model radians).
	void drawTurtleIcon(NVGcontext* vg, float px, float py, float hd, bool penDown) {
		NVGcolor shell = penDown ? nvgRGBA(0x3F, 0xA8, 0x6A, 0xFF) : nvgRGBA(0x70, 0x74, 0x80, 0xFF);
		NVGcolor limb  = penDown ? nvgRGBA(0x5A, 0xD0, 0x89, 0xFF) : nvgRGBA(0x8A, 0x8E, 0x9A, 0xFF);
		NVGcolor edge  = nvgRGBA(0x1E, 0x4A, 0x32, penDown ? 0xFF : 0x80);
		nvgSave(vg);
		nvgTranslate(vg, px, py);
		nvgRotate(vg, -hd);          // local +y points along heading
		// legs (4) + tail + head, drawn under the shell
		nvgFillColor(vg, limb);
		const float legX = 3.4f, legY = 3.0f, legR = 1.7f;
		for (int sx = -1; sx <= 1; sx += 2)
			for (int sy = -1; sy <= 1; sy += 2) {
				nvgBeginPath(vg); nvgCircle(vg, sx * legX, sy * legY, legR); nvgFill(vg);
			}
		nvgBeginPath(vg); nvgCircle(vg, 0.f, 6.4f, 2.1f); nvgFill(vg);   // head (front)
		nvgBeginPath(vg);                                                // tail (back)
		nvgMoveTo(vg, 0.f, -7.0f); nvgLineTo(vg, -1.4f, -4.6f); nvgLineTo(vg, 1.4f, -4.6f);
		nvgClosePath(vg); nvgFill(vg);
		// shell (ellipse, longer along travel)
		nvgBeginPath(vg); nvgEllipse(vg, 0.f, 0.f, 4.3f, 5.2f);
		nvgFillColor(vg, shell); nvgFill(vg);
		nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, 0.8f); nvgStroke(vg);
		// shell plates
		nvgBeginPath(vg);
		nvgMoveTo(vg, -3.6f, 0.f); nvgLineTo(vg, 3.6f, 0.f);
		nvgMoveTo(vg, 0.f, -4.6f); nvgLineTo(vg, 0.f, 4.6f);
		nvgStrokeColor(vg, edge); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
		nvgRestore(vg);
	}

	// Stroke the recorded turtle trail (shared by Turtle + Pattern modes).
	void drawTurtlePath(NVGcontext* vg) {
		const NVGcolor PEN = nvgRGBA(0x4C, 0xC8, 0x8A, 0xBF);  // plotter green, ~75% opacity
		nvgStrokeColor(vg, PEN);
		nvgStrokeWidth(vg, 1.4f);
		nvgLineCap(vg, NVG_ROUND);
		nvgLineJoin(vg, NVG_ROUND);
		bool open = false;
		for (size_t i = 1; i < turtleTrail.size(); i++) {
			const TurtlePt& a = turtleTrail[i - 1];
			const TurtlePt& b = turtleTrail[i];
			bool seg = a.pen && b.pen && !b.brk;   // both ends drawing, no jump
			if (seg) {
				Vec pa = toPx(a.x, a.y), pb = toPx(b.x, b.y);
				if (!open) { nvgBeginPath(vg); nvgMoveTo(vg, pa.x, pa.y); open = true; }
				nvgLineTo(vg, pb.x, pb.y);
			} else if (open) {
				nvgStroke(vg); open = false;
			}
		}
		if (open) nvgStroke(vg);
	}

	void drawTurtle(NVGcontext* vg) {
		if (!module) return;
		recordTurtle();

		// New command? append its instruction to the log (UI thread only).
		if (module->tuCmdSeq != tuLastSeq) {
			tuLastSeq = module->tuCmdSeq;
			turtleLog.push_back(turtleLabel());
			while (turtleLog.size() > 7) turtleLog.pop_front();
		}

		drawTurtlePath(vg);

		// Instruction log: small text, top-left, newest brightest at the bottom.
		if (font && font->handle >= 0 && !turtleLog.empty()) {
			Vec c = centerPx();
			float R = radiusPx();
			nvgFontFaceId(vg, font->handle);
			nvgFontSize(vg, 7.5f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			float x = c.x - R * 0.66f;
			float y0 = c.y - R * 0.62f;
			int n = (int) turtleLog.size();
			for (int i = 0; i < n; i++) {
				float frac = (i + 1) / (float) n;             // older = dimmer
				float a = 0.20f + 0.75f * frac;
				bool newest = (i == n - 1);
				NVGcolor col = newest ? nvgRGBAf(0.93f, 0.40f, 0.18f, 1.f)  // current = orange
				                      : nvgRGBAf(0.30f, 0.78f, 0.54f, a);   // history = green
				nvgFillColor(vg, col);
				nvgText(vg, x, y0 + i * 9.f, turtleLog[i].c_str(), NULL);
			}
		}

		// The turtle itself: a top-down turtle facing its heading.
		Vec hp = toPx(module->trackedX, module->trackedY);
		drawTurtleIcon(vg, hp.x, hp.y, module->tuHeading, module->tuPenDown);
	}

	int ptLastGen = -1;
	void drawPattern(NVGcontext* vg) {
		if (!module) return;
		// A new figure was generated -> wipe the canvas for a clean redraw.
		if (module->ptGen != ptLastGen) {
			ptLastGen = module->ptGen;
			turtleTrail.clear();
		}
		recordTurtle();
		drawTurtlePath(vg);

		// Generating-rule label, top-left.
		if (font && font->handle >= 0) {
			Vec c = centerPx();
			float R = radiusPx();
			nvgFontFaceId(vg, font->handle);
			nvgFontSize(vg, 8.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, nvgRGBAf(0.30f, 0.78f, 0.54f, 0.9f));
			std::string form = (module->ptQ > 1)
				? string::f("ROSE  %d/%d", module->ptN, module->ptQ)
				: string::f("ROSE  n=%d", module->ptN);
			nvgText(vg, c.x - R * 0.66f, c.y - R * 0.62f, form.c_str(), NULL);
			nvgText(vg, c.x - R * 0.66f, c.y - R * 0.62f + 10.f,
				string::f("step=%d°", module->ptD).c_str(), NULL);
		}

		// The turtle, tracing the figure.
		Vec hp = toPx(module->trackedX, module->trackedY);
		drawTurtleIcon(vg, hp.x, hp.y, module->tuHeading, module->tuPenDown);
	}

	// Browser thumbnail: a representative Hungry Man maze. No module state
	// exists here, so carve a fixed-seed maze locally and draw it like the
	// live mode (walls, dots, a big dot or two, the chomping wedge, score).
	void drawHungryPreview(NVGcontext* vg) {
		const int S = Gravity::HM_SPOKES, RINGS = Gravity::HM_RINGS, NODES = S * RINGS;
		const float RMAX = REACH * 0.92f, TAU = 2.f * (float) M_PI;
		Vec c = centerPx();
		const NVGcolor COL_WALL = nvgRGBA(0x3a, 0x4a, 0x9a, 0xFF);
		const NVGcolor COL_DOT  = nvgRGBA(0xF0, 0xC0, 0x60, 0xF0);
		const NVGcolor COL_BIG  = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
		const NVGcolor COL_PAC  = nvgRGBA(0xF5, 0xD0, 0x30, 0xFF);

		auto cell = [&](int r, int s) { return r * S + ((s % S) + S) % S; };

		// Fixed-seed spanning-tree maze (same algorithm as initHungry).
		bool passRing[64] = {}, passRad[64] = {};
		struct Cand { int a, b; bool ring; int idx; };
		Cand cand[128]; int nc = 0;
		for (int r = 0; r < RINGS - 1; r++) for (int s = 0; s < S; s++)
			cand[nc++] = {cell(r, s), cell(r + 1, s), true, r * S + s};
		for (int r = 0; r < RINGS; r++) for (int s = 0; s < S; s++)
			cand[nc++] = {cell(r, s), cell(r, s + 1), false, r * S + s};
		unsigned st = 0x9E3779B1u;
		auto rnd = [&]() { st = st * 1664525u + 1013904223u; return st; };
		for (int i = nc - 1; i > 0; i--) { int j = rnd() % (i + 1); Cand t = cand[i]; cand[i] = cand[j]; cand[j] = t; }
		int parent[64]; for (int n = 0; n < NODES; n++) parent[n] = n;
		for (int i = 0; i < nc; i++) {
			int ra = cand[i].a, rb = cand[i].b;
			while (parent[ra] != ra) { parent[ra] = parent[parent[ra]]; ra = parent[ra]; }
			while (parent[rb] != rb) { parent[rb] = parent[parent[rb]]; rb = parent[rb]; }
			if (ra != rb) { parent[ra] = rb; if (cand[i].ring) passRing[cand[i].idx] = true; else passRad[cand[i].idx] = true; }
		}

		auto ringR = [&](int b) { return Gravity::hmRingFrac((float) b) * RMAX * ppu(); };
		auto arcWall = [&](int b, int s) {
			float rpx = ringR(b), a0 = s * (TAU / S), a1 = (s + 1) * (TAU / S);
			nvgBeginPath(vg);
			for (int i = 0; i <= 10; i++) { float a = a0 + (a1 - a0) * i / 10.f; float px = c.x + std::sin(a) * rpx, py = c.y + std::cos(a) * rpx; if (i == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py); }
			nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.6f); nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
		};
		auto radWall = [&](int s, int r) {
			float a = (s + 1) * (TAU / S), r0 = ringR(r), r1 = ringR(r + 1);
			nvgBeginPath(vg); nvgMoveTo(vg, c.x + std::sin(a) * r0, c.y + std::cos(a) * r0); nvgLineTo(vg, c.x + std::sin(a) * r1, c.y + std::cos(a) * r1);
			nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.6f); nvgLineCap(vg, NVG_ROUND); nvgStroke(vg);
		};

		nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, ringR(RINGS)); nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.6f); nvgStroke(vg);
		nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, ringR(0));     nvgStrokeColor(vg, COL_WALL); nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
		for (int r = 0; r < RINGS - 1; r++) for (int s = 0; s < S; s++) if (!passRing[r * S + s]) arcWall(r + 1, s);
		for (int r = 0; r < RINGS; r++)     for (int s = 0; s < S; s++) if (!passRad[r * S + s])  radWall(s, r);

		auto nodePos = [&](int n, float& x, float& y) {
			int r = n / S, s = n % S; float rad = Gravity::hmRingFrac(r + 0.5f) * RMAX; float ang = (s + 0.5f) * (TAU / S);
			x = rad * std::sin(ang); y = rad * std::cos(ang);
		};
		int pac = cell(2, 4), big1 = cell(1, 8), big2 = cell(3, 1);
		for (int n = 0; n < NODES; n++) {
			if (n == pac) continue;
			float x, y; nodePos(n, x, y); Vec p = toPx(x, y);
			if (n == big1 || n == big2) { nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, 3.4f); nvgFillColor(vg, COL_BIG); nvgFill(vg); }
			else                        { nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, 1.4f); nvgFillColor(vg, COL_DOT); nvgFill(vg); }
		}

		float px, py; nodePos(pac, px, py); Vec hp = toPx(px, py);
		float scr = (float) M_PI * 0.5f, mouth = 0.30f * (float) M_PI;   // facing along +ring
		nvgBeginPath(vg); nvgMoveTo(vg, hp.x, hp.y); nvgArc(vg, hp.x, hp.y, 6.f, scr + mouth, scr + TAU - mouth, NVG_CW); nvgClosePath(vg);
		nvgFillColor(vg, COL_PAC); nvgFill(vg);

		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (font && font->handle >= 0) {
			nvgFontFaceId(vg, font->handle); nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFontSize(vg, 11.f); nvgFillColor(vg, nvgRGBA(0xF0, 0xC0, 0x60, 0xE0)); nvgText(vg, c.x, c.y - 7.f, "128", NULL);
			nvgFontSize(vg, 8.f);  nvgFillColor(vg, nvgRGBA(0xF0, 0xC0, 0x60, 0x90)); nvgText(vg, c.x, c.y + 7.f, "LV 2", NULL);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { OpaqueWidget::drawLayer(args, layer); return; }
		if (!module) {
			drawHungryPreview(args.vg);
			return;
		}
		NVGcontext* vg = args.vg;

		const NVGcolor COL_BLUE   = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);
		const NVGcolor COL_ORANGE = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
		const NVGcolor COL_GRID   = nvgRGBA(0x35, 0x35, 0x4D, 0xFF);
		const NVGcolor COL_TIP    = nvgRGBA(0xEC, 0x65, 0x2E, 0xFF);
		const NVGcolor COL_ELBOW  = nvgRGBA(0x00, 0x97, 0xDE, 0xFF);

		if (!font || font->handle < 0)
			font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));

		Vec c = centerPx();
		float R = radiusPx();

		// Outer circle
		nvgBeginPath(vg);
		nvgCircle(vg, c.x, c.y, R);
		nvgStrokeColor(vg, COL_GRID);
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);

		// Six boundary rays + gate flash highlight (fixed to the panel jacks).
		// In Hungry Man the static rays clutter the maze, so draw a ray ONLY
		// while it's flashing (i.e. just crossed) — it pulses, then fades out.
		bool hungry = module->mode == Gravity::MODE_HUNGRY;
		for (int k = 0; k < NUM_SECTORS; k++) {
			float a = k * 60.f * DEG;
			float ex = c.x + std::sin(a) * R;
			float ey = c.y + std::cos(a) * R;
			float flash = clamp(module->dispGateFlash[k], 0.f, 1.f);
			if (hungry && flash < 0.01f) continue;
			nvgBeginPath(vg);
			nvgMoveTo(vg, c.x, c.y);
			nvgLineTo(vg, ex, ey);
			NVGcolor rc = nvgRGBAf(
				COL_GRID.r + (COL_ORANGE.r - COL_GRID.r) * flash,
				COL_GRID.g + (COL_ORANGE.g - COL_GRID.g) * flash,
				COL_GRID.b + (COL_ORANGE.b - COL_GRID.b) * flash,
				1.f);
			nvgStrokeColor(vg, rc);
			nvgStrokeWidth(vg, 1.f + flash * 1.5f);
			nvgStroke(vg);
		}

		// Sector morph arcs near the rim — brightness = that sector's CV level
		for (int i = 0; i < NUM_SECTORS; i++) {
			float lvl = (module->sectorMode == 0)
			          ? clamp(module->sectorOut[i] / 10.f, 0.f, 1.f)
			          : clamp(std::abs(module->sectorOut[i]) / 5.f, 0.f, 1.f);
			if (lvl < 0.01f) continue;
			float a0 = (i * 60.f) * DEG;
			float a1 = ((i + 1) * 60.f) * DEG;
			nvgBeginPath(vg);
			int seg = 10;
			for (int s = 0; s <= seg; s++) {
				float a = a0 + (a1 - a0) * s / seg;
				float rr = R - 3.f;
				float px = c.x + std::sin(a) * rr;
				float py = c.y + std::cos(a) * rr;
				if (s == 0) nvgMoveTo(vg, px, py); else nvgLineTo(vg, px, py);
			}
			nvgStrokeColor(vg, nvgRGBAf(COL_BLUE.r, COL_BLUE.g, COL_BLUE.b, lvl));
			nvgStrokeWidth(vg, 2.5f);
			nvgStroke(vg);
		}

		// (Sector/gate index numbers removed — the LEDs by each jack now convey
		// activity, so on-screen labels were just visual noise.)

		// Gravity marker. In Gravity Well it's the central SUN, a filled circle
		// at the origin sized by the pull amount. In the other modes it's a rim
		// dot showing the direction gravity pulls.
		{
			NVGcolor gold = nvgRGBA(0xD0, 0xA8, 0x50, 0xFF);
			if (module->mode == Gravity::MODE_GRAVWELL) {
				float sunR = 4.f + module->gwSunPull * 12.f;   // 4..16 px by pull
				// soft halo
				nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, sunR + 3.f);
				nvgFillColor(vg, nvgRGBAf(gold.r, gold.g, gold.b, 0.18f)); nvgFill(vg);
				nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, sunR);
				nvgFillColor(vg, gold); nvgFill(vg);
			} else if (module->mode != Gravity::MODE_TURTLE && module->mode != Gravity::MODE_PATTERN) {
				// (Turtle/Pattern use GRAVITY for weighting/symmetry, not a direction.)
				float g = (float) module->gravAngle;
				float mx = c.x + std::sin(g) * R;
				float my = c.y + std::cos(g) * R;
				float ix = c.x + std::sin(g) * (R - 9.f);
				float iy = c.y + std::cos(g) * (R - 9.f);
				nvgBeginPath(vg);
				nvgMoveTo(vg, ix, iy); nvgLineTo(vg, mx, my);
				nvgStrokeColor(vg, nvgRGBAf(gold.r, gold.g, gold.b, 0.5f));
				nvgStrokeWidth(vg, 1.2f);
				nvgStroke(vg);
				nvgBeginPath(vg); nvgCircle(vg, mx, my, 3.2f);
				nvgFillColor(vg, gold); nvgFill(vg);
			}
		}

		// Trails (skip in Hungry Man + Turtle + Pattern — own visuals)
		if (module->mode != Gravity::MODE_HUNGRY && module->mode != Gravity::MODE_TURTLE
		    && module->mode != Gravity::MODE_PATTERN) {
			pushTrail();
			drawTrail(vg, elbowTrail, COL_ELBOW);
			drawTrail(vg, tipTrail, COL_TIP);
		}

		// --- Per-mode body rendering ---
		if (module->mode == Gravity::MODE_PENDULUM) {
			Vec b1 = toPx(module->dispB1x, module->dispB1y);
			Vec b2 = toPx(module->dispB2x, module->dispB2y);
			nvgBeginPath(vg);
			nvgMoveTo(vg, c.x, c.y);
			nvgLineTo(vg, b1.x, b1.y);
			nvgLineTo(vg, b2.x, b2.y);
			nvgStrokeColor(vg, nvgRGBA(0xC0, 0xC8, 0xD0, 0xFF));
			nvgStrokeWidth(vg, 2.f);
			nvgLineCap(vg, NVG_ROUND);
			nvgStroke(vg);
			nvgBeginPath(vg); nvgCircle(vg, c.x, c.y, 2.5f);
			nvgFillColor(vg, COL_GRID); nvgFill(vg);
			nvgBeginPath(vg); nvgCircle(vg, b1.x, b1.y, 4.f);
			nvgFillColor(vg, COL_ELBOW); nvgFill(vg);
			nvgBeginPath(vg); nvgCircle(vg, b2.x, b2.y, 5.f);
			nvgFillColor(vg, COL_TIP); nvgFill(vg);
		}
		else if (module->mode == Gravity::MODE_GRAVWELL) {
			// Planets (gold) on their orbits, rocket = tracked tip (orange).
			NVGcolor gold = nvgRGBA(0xD0, 0xA8, 0x50, 0xFF);
			for (int i = 0; i < module->gwPlanetCount; i++) {
				float px = (float)(module->gwPlanetRad[i] * std::cos(module->gwPlanetAng[i]));
				float py = (float)(module->gwPlanetRad[i] * std::sin(module->gwPlanetAng[i]));
				Vec p = toPx(px, py);
				float rad = 2.5f + 3.5f * (float) module->gwPlanetMass[i];
				// faint orbit ring
				nvgBeginPath(vg);
				nvgCircle(vg, c.x, c.y, (float)(module->gwPlanetRad[i]) * ppu());
				nvgStrokeColor(vg, nvgRGBAf(gold.r, gold.g, gold.b, 0.12f));
				nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
				nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, rad);
				nvgFillColor(vg, gold); nvgFill(vg);
			}
			Vec r = toPx(module->trackedX, module->trackedY);
			// Orange rocket, nose pointed along its velocity heading.
			float hx = (float) module->gwVx, hy = (float) module->gwVy;
			float heading = (std::fabs(hx) + std::fabs(hy) > 1e-6f)
				? std::atan2(hy, hx) : -(float) M_PI / 2.f;
			drawRocket(vg, r.x, r.y, heading, 16.f);
		}
		else if (module->mode == Gravity::MODE_BILLIARDS) {
			for (int i = 0; i < module->bzCount && i < Gravity::MAX_BALLS; i++) {
				Vec p = toPx((float) module->bzX[i], (float) module->bzY[i]);
				bool cue = (i == 0);
				float rad = (float)(Gravity::BALL_R * ppu()) * (cue ? 1.15f : 1.f);
				nvgBeginPath(vg); nvgCircle(vg, p.x, p.y, rad);
				nvgFillColor(vg, cue ? COL_TIP : COL_ELBOW); nvgFill(vg);
			}
			// Slingshot aim line while dragging the cue.
			if (module->bzAiming) {
				Vec cue = toPx(module->trackedX, module->trackedY);
				Vec aim = toPx((float) module->bzAimX, (float) module->bzAimY);
				nvgBeginPath(vg);
				nvgMoveTo(vg, cue.x, cue.y); nvgLineTo(vg, aim.x, aim.y);
				nvgStrokeColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.6f));
				nvgStrokeWidth(vg, 1.2f); nvgStroke(vg);
				// launch direction (opposite the drag), dashed-ish preview
				Vec opp(2.f * cue.x - aim.x, 2.f * cue.y - aim.y);
				nvgBeginPath(vg);
				nvgMoveTo(vg, cue.x, cue.y); nvgLineTo(vg, opp.x, opp.y);
				nvgStrokeColor(vg, nvgRGBAf(COL_TIP.r, COL_TIP.g, COL_TIP.b, 0.5f));
				nvgStrokeWidth(vg, 1.f); nvgStroke(vg);
			}
		}
		else if (module->mode == Gravity::MODE_HUNGRY) {
			drawHungry(vg);
		}
		else if (module->mode == Gravity::MODE_TURTLE) {
			drawTurtle(vg);
		}
		else { // MODE_PATTERN
			drawPattern(vg);
		}

		OpaqueWidget::drawLayer(args, layer);
	}
};


struct GravityWidget : ModuleWidget {
	GravityWidget(Gravity* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/gravity.svg")));

		// Centred circular display (centred at 76,64 on the 30HP panel)
		const float CX = 76.f, CY = 64.f;
		const float Rg = 38.f, Rs = 49.f;   // gate ring, sector ring radii
		GravityDisplay* display = new GravityDisplay();
		display->module = module;
		display->box.pos = mm2px(Vec(CX - 30.f, CY - 30.f));
		display->box.size = mm2px(Vec(60.f, 60.f));
		addChild(display);

		// Radial jack mandala: gates on the inner ring at each ray angle, sector
		// CVs on the outer ring at each wedge centre — aligned with the field.
		// Each jack gets a small red LED offset OUTWARD (away from center) along
		// its own radius — same distance for all, so they form two tidy rings
		// just past the jacks.
		const float ledOffset = 8.0f;   // mm from jack center, away from panel center
		for (int k = 0; k < NUM_SECTORS; k++) {
			float ag = k * 60.f * DEG;
			float as = (k * 60.f + 30.f) * DEG;
			Vec gp(CX + std::sin(ag) * Rg, CY + std::cos(ag) * Rg);
			Vec sp(CX + std::sin(as) * Rs, CY + std::cos(as) * Rs);
			addOutput(createOutputCentered<PJ301MPort>(mm2px(gp), module, Gravity::GATE_OUTPUT + k));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(sp), module, Gravity::SECTOR_OUTPUT + k));
			Vec gl(CX + std::sin(ag) * (Rg + ledOffset), CY + std::cos(ag) * (Rg + ledOffset));
			Vec sl(CX + std::sin(as) * (Rs + ledOffset), CY + std::cos(as) * (Rs + ledOffset));
			addChild(createLightCentered<SmallLight<RedLight>>(
				mm2px(gl), module, Gravity::GATE_LIGHT + k));
			addChild(createLightCentered<SmallLight<RedLight>>(
				mm2px(sl), module, Gravity::SECTOR_LIGHT + k));
		}

		// Left column: 4 pot + CV-jack pairs, top->bottom MODE / GRAVITY /
		// CHAOS / SPEED (positions from the panel reticules at x=10.16mm).
		const float colL = 10.16f;
		struct LCtl { int param; int input; float py; float jy; };
		const LCtl lc[4] = {
			{Gravity::MODE_PARAM,    Gravity::MODE_INPUT,     35.55f,  45.72f},
			{Gravity::GRAVITY_PARAM, Gravity::GRAVITY_INPUT,  60.96f,  71.12f},
			{Gravity::CHAOS_PARAM,   Gravity::CHAOS_INPUT,    86.36f,  96.52f},
			{Gravity::SPEED_PARAM,   Gravity::SPEED_INPUT,   111.76f, 121.93f},
		};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(colL, lc[i].py)), module, lc[i].param));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colL, lc[i].jy)), module, lc[i].input));
		}

		// Derived gates: 2x2 plate, top-right, mirroring the position outputs at
		// the bottom-right. They lived in a loose row along the foot when they
		// were added, because the panel had no home for them; res/gravity.svg now
		// gives them one, and the labels come with it -- so no PanelLabels here.
		// Reading order across the plate is ANY / NEW, ODD / EVEN, which is what
		// the artwork says: the two that fire on every crossing on top, the two
		// that split the ring by parity below.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(127.00f,  22.01f)), module, Gravity::ANY_GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(142.24f,  22.01f)), module, Gravity::NEW_GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(127.00f,  37.25f)), module, Gravity::ODD_GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(142.24f,  37.25f)), module, Gravity::EVEN_GATE_OUTPUT));

		// Right outputs: 2x2 plate, bottom-right. X Y on top, ANGLE RADIUS below.
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(127.00f, 106.67f)), module, Gravity::X_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(142.24f, 106.67f)), module, Gravity::Y_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(127.00f, 121.93f)), module, Gravity::ANGLE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(142.24f, 121.93f)), module, Gravity::RADIUS_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Gravity* module = dynamic_cast<Gravity*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Relaunch (kick)", "", [=]() { module->relaunch(); }));

		if (module->mode == Gravity::MODE_TURTLE || module->mode == Gravity::MODE_PATTERN) {
			GravityDisplay* disp = nullptr;
			for (Widget* w : this->children)
				if ((disp = dynamic_cast<GravityDisplay*>(w))) break;
			bool turtle = (module->mode == Gravity::MODE_TURTLE);
			menu->addChild(createMenuItem("Clear drawing", "", [=]() {
				if (disp) disp->turtleTrail.clear();
				if (turtle) module->initTurtle(); else module->initPattern();
			}));
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Sector CV",
			{"Morph (0-10V, in-sector only)", "Bipolar distance (+-5V, always live)"},
			[=]() { return module->sectorMode; },
			[=](int i) { module->sectorMode = clamp(i, 0, 1); }));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Gate hold"));
		struct HoldOpt { const char* name; float sec; };
		static const HoldOpt holds[3] = {{"Tight (30ms)", 0.03f}, {"Medium (60ms)", 0.06f}, {"Gluey (120ms)", 0.12f}};
		for (int i = 0; i < 3; i++) {
			float sec = holds[i].sec;
			menu->addChild(createCheckMenuItem(holds[i].name, "",
				[=]() { return std::abs(module->gateHoldSec - sec) < 1e-4f; },
				[=]() { module->gateHoldSec = sec; }));
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Trail length"));
		struct TrailOpt { const char* name; int len; };
		static const TrailOpt trails[4] = {{"Off", 0}, {"Short", 40}, {"Medium", 90}, {"Long", 180}};
		for (int i = 0; i < 4; i++) {
			int len = trails[i].len;
			menu->addChild(createCheckMenuItem(trails[i].name, "",
				[=]() { return module->trailLength == len; },
				[=]() { module->trailLength = len; }));
		}
	}
};


Model* modelGravity = createModel<Gravity, GravityWidget>("Gravity");

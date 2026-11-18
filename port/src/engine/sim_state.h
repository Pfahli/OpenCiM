#pragma once
// The simulation's state, shared by engine/sim.cpp (vehicles, lines, traffic) and engine/sim_citizens.cpp (citizens,
// pedestrians, passengers). Not part of the engine's interface: see engine/sim.h.
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/grid.h"
#include "engine/scene3d.h"
#include "engine/sim_graph.h"
#include "engine/skinning.h"
#include "render/math3d.h"
#include "render/renderer3d.h"
#include "script/vm.h"

namespace regs::engine {

class Display;
struct SimState;
struct SimSounds;
struct SimParticles;
struct TrafficMaps;
class TransportObj;

namespace sim {

// a vehicle or character template (gsGridObject) as movement and drawing use it
// the script-field slots the per-frame field syncs write into (setPoseFields, syncVehicleFields): fieldValues
// entries are never erased and unordered_map keeps element addresses, so the pointers stay valid while the object
// lives; serial says which object they belong to (serials are never reused, 0 = none yet)
struct FieldSlots {
  uint64_t serial = 0;
  script::Value *position = nullptr, *globalPos = nullptr, *orientation = nullptr;
  script::Value *model = nullptr, *instance = nullptr;  // <model>, <instance>: looked up on their first write
};

struct UnitModel {
  ObjectObj* object = nullptr;
  float frontOffset = 0, rearOffset = 0, frontAnchor = 0, rearAnchor = 0;
  float acceleration = 1, braking = 1, turning = 1, speed = 1;
  int length = 1, capacity = 0;
  bool named = false;  // has a displayName: hover and selection pick it
  const ColormapObj* colorMap = nullptr;
  render::Vec3 boundsMin, boundsMax;
};

struct Unit {
  int model = -1;
  bool reversed = false;
  uint32_t tint = 0;  // colorize ARGB from the model's colour map (0 = none)
  uint16_t seed = 0;  // the colorSeed (agent +0x80) the tint came from; sim_save.cpp writes it per car
};

// A mover's track: the nodes it has driven and will drive, with their start distances. Distances are of the head's
// rear point (the agent's posOnNode, grid-traffic.md §6.4a).
struct Track {
  std::vector<int> nodes;
  std::vector<float> start;  // cumulative chord length before node i
  float end(const LaneGraph& G) const { return nodes.empty() ? 0 : start.back() + G.nodes[size_t(nodes.back())].length; }
  void push(const LaneGraph& G, int n) {
    start.push_back(end(G));
    nodes.push_back(n);
  }
  // index of the node at distance s (clamped to the track)
  size_t at(float s) const;
};

// An agent with its trailing cars (0x94-byte vehicle agent chain): one consist on its track, holding the lane nodes
// [holdBegin, holdEnd) of the track as their occupant.
struct Mover {
  bool live = false;
  bool transit = false;  // agent flag 0x200: a line vehicle, ignores waiting markers
  bool parked = false;   // a parked car: drawn, never moved
  bool train = false;
  bool extra = false;    // a background trip of the port's traffic density above 1x
  bool boat = false;     // a background boat (vehicles+0x10c consists) on the water lanes
  bool aircraft = false; // a background aircraft (vehicles+0xfc models): owns its whole route
  float animTime = -1;   // aircraft: the skeleton clip time of gear and doors (FUN_101081f0), −1 = rest pose
  std::vector<Unit> units;
  Track track;
  float dist = 0, speed = 0, waitTime = 0;
  int holdBegin = 0, holdEnd = 0;
  int limit = -1;        // last track index the head may use (a line vehicle's leg end); −1: the whole track
  int lengthNodes = 1;   // L
  int reserved = -1;     // the node this mover queues on as its reserver
  int vehicle = -1;      // line vehicles: index into SimState::vehicles
  int citizen = -1;      // a citizen's car (agent +0x30)
  uint16_t colorSeed = 0;
  bool reckless = false; // agent flag 0x800: does not plan to stop for nodes it does not hold (grid-traffic.md §6.6)
  bool crashing = false; // agent flag 0x40000: claims frozen, decelerates 4/s, drifts by crashOffset
  render::Vec3 crashOffset{};
  script::Ref<script::Object> controller;  // traffic: the gsGridVehicleController that picking sees
  FieldSlots controllerSlots;               // its pose fields (syncVehicleFields)
  // animated parts (rotors, propellers, paddle wheels; gsGridVehicle::move L:0x081adca0.c:604-690): the shader
  // parameter custom4 of the model's materials, x = the accumulated angle, y = the spin-up r (0..12; −1 = the
  // template's initial value, read as full speed when moving); stepAnimatedParts
  float rotorAngle = 0, rotorSpin = -1;
  float travelled = 0;   // port: distance the head advanced since the mover was placed (jam relief, sim_jams.cpp)
  bool testPin = false;  // port test switch REGS_TEST_JAM=<type>:hard: a blocker the jam relief must not clear
  bool plow = false;     // port: a snowplow of the mods (engine/snowplows.h): not counted as road traffic
};

// a pending aircraft spawn: entry/exit, the runway nodes 15 and 30 steps on (back), the gate and its taxi path
struct AirPending {
  int entry = -1, a = -1, b = -1, gate = -1;
  std::vector<int> path;
};

struct NodeRt {
  int occupant = -1, reserver = -1;  // node +0x18 / +0x1c
  bool marker = false;               // flag 0x1000000: someone waits to enter
  int keep = -1;                     // port jam relief: kept for this line vehicle's mover (sim_jams.cpp keptFor)
};

// A door item (item flag 0x1000 GRID_ITEM_CROSS_TRACK with a doorOpenLen; vanilla: the level crossing
// track_double_cross_small), gsGridItem::move L:0x08182b10.c:96-167 (docs/simulation/grid-traffic.md §5.1.1)
struct DoorItem {
  uint32_t item = 0;
  std::vector<int> nodes;  // its train-track nodes (node flag 0x1000): any occupant opens the door
  render::Vec3 centre;     // the item centre: the bell's position
  float openStart = 0, openLen = 0, closeStart = 0, closeLen = 0;  // doorOpenStart/Len, doorCloseStart/Len
  float phase = 0;         // item +0x64: 0..1 opening, 0..−1 closing, −1 at rest (the ctor sets 0)
  float time = -1;         // the clip time animate() got last, −1 = no skeleton (bind pose)
  bool opening = false;    // an occupied node in the last update: the bell (moveSound) loops
  bool active = false;     // the item's bucket is active (camera stand-in): updated, its sound may play
  bool nearOnly = false;   // item flag 0x4000000: animated only while eyeDist < 300
};

struct LineStopState {
  int item = -1;                // network item
  script::Ref<script::Object> stop;  // its gsTransportStopController
  int platform = -1;            // lane node of this stop (FUN_10110c10)
  int altPlatform = -1;         // its ALTERNATIVE_STOP node (metro: the other direction at an end station), −1 none
  int legStart = -1, legEnd = -1;  // the segment to the next stop: from this stop's platform to the next one's
  std::vector<int> path;        // segment to the next stop's platform, after this platform (FUN_10114200)
  float cost = 0;               // travel time of the segment
  bool reachable = false;
  uint32_t flags = 0;
  float sinceDeparture = 0;
  int passengersTransported = 0, waitingTime = 0;
  int docked = -1;              // vehicle in the doors-open window
  int waiting = 0, mood[3] = {0, 0, 0};  // citizens waiting here, and those in each mood tier (node +0x58..+0x64)
  std::vector<int> transfers;   // global node ids reachable by a transfer after riding this node (§6.1)
  // the route strip of the segment (gsTransportSegment::updateMesh, engine/sim_paths.cpp), drawn while the line is visible
  std::shared_ptr<render::GpuMesh> pathMesh;
  uint64_t pathKey = 0;
};

struct VehicleState {
  script::Ref<script::Object> object;
  FieldSlots objectSlots;  // its <model>, <instance> and pose fields (syncVehicleFields)
  int line = -1;
  int mover = -1;
  std::vector<Unit> units;
  int stopA = -1, stopB = -1;   // line stop indices: just left, approaching
  int extraNodes = 0;           // nodes of the next leg already on the track (half the vehicle past the platform)
  // port: at a turn on the same track (a metro end station) the nodes straight on past the platform the train drove
  // onto so its middle stands at the stop; the turned leg starts back over their opposites (reverseConsist)
  std::vector<int> turnNodes;
  int turnSkip = 0;  // of them, how many lie before the node whose opposite the turned leg starts on
  float stopTime = 0;
  bool halted = false;          // flag 0x400: docked at stopB
  float doorPos = -1;           // the skeleton clip time of the doors (FUN_101081f0), −1 = closed (rest pose)
  float travelDistance = 0, attractiveness = 1;
  int passengerCount = 0, passengersTransported = 0, capacity = 0;
  int boardingReserved = 0;     // +0x174: citizens walking to the doors
  bool broken = false;
  bool brokenHalted = false;    // agent flag 0x10000: broken down and standing at the next stop
  float breakdownClock = 0;     // +0x18c: one breakdown roll per second
  bool deleted = false;         // destroyed (sold, moved to the depot): a tombstone, so vehicle indices stay valid
  bool spinUp = false;          // port: a helicopter done at its pad spins the rotor up before it takes off
  bool bellRung = false, announced = false;  // +0x184 / +0x185: the vehicle bell (< 8 nodes left) and the stop's
                                             // announcement (< 32) played on this leg
  // port QoL, jam relief of buses and trams (engine/sim_jams.cpp, docs/simulation/transport.md §10); not saved
  float jamClock = 0;           // seconds standing (head < 1 unit on) outside dwell, breakdown and crash
  float jamMark = 0;            // the mover's travelled distance when the head last advanced a unit
  bool relief = false;          // step 2 runs: the cars around turn round
  float reliefStill = 0;        // seconds without a unit of progress since step 2 began
  float reliefNext = 0;         // seconds to the next car turned
  float reliefFrom = 0;         // travelled distance when step 2 began
  int reliefCleared = 0;        // cars turned or sent off in this relief
  bool jammedFired = false;     // onJammed fired since the head last advanced a unit
  std::vector<int> kept;        // nodes kept for it in step 2 (NodeRt::keep): the node it waits for and its crossings
  int respawnAt = -1;           // step 3: the stop to spawn at again (−1 = none)
  float respawnWait = 0;        // seconds waited for that platform
};

struct LineState {
  script::Ref<script::Object> object;
  int type = 0;
  float headway = 0, effAttractiveness = 0, avgWait = 0;
  bool closed = false;
  std::vector<LineStopState> stops;
  std::vector<int> vehicles;
  int firstNode = 0;            // global id of stops[0] (line nodes are counted across all lines in file order)
  bool deleted = false;         // destroyed (deleteLine): a tombstone without stops or vehicles, not saved
  std::vector<render::GpuMaterial> pathMaterials, tempPathMaterials;  // the route strips' materials as last drawn
};

// ---- citizens (docs/simulation/citizens.md) ----

struct CharacterModel {  // a character pool record (addCharacter)
  int model = -1;
  uint32_t eraMask = 0xffffffff;
  int gender = -1;
};
struct CarModel {  // a car pool record (addCar): no parts = no car
  std::vector<int> parts;
  int weight = 0;
  uint32_t eraMask = 0xffffffff;
};

struct Route {  // a planned transit route (0x10 B): boarding node, every intermediate node, by global node id
  float cost = 0;
  std::vector<int> nodes;
  int leg = 0;
};

struct Citizen {
  bool alive = false;
  uint32_t flags = 0, id = 0, tripQueue = 0, usedTransport = 0;
  int cycleTimer = 0, waitTime = 0;
  int character = -1;          // visible pedestrian
  int car = -1;                // mover of the private car
  int lineNode = -1;           // global line node the citizen waits at
  int vehicle = -1;            // line vehicle ridden
  int home = -1, work = -1, start = -1, target = -1;  // items
  int charModel = -1, carModel = -1;                  // pool indices (class pool / generic car pool of the class)
  int driveStart = -1, driveEnd = -1;                 // road nodes of the drive
  Route route;
  bool hasRoute = false;
  bool toCar = false;          // DRIVE phase 0x100 of the original: the pedestrian walks to the parked car
  int replanFrom = -1;         // REPLAN (0x40): the stop item the citizen was evacuated at
  script::Ref<script::Object> controller;
};

// a visible pedestrian walking a q-cell path (gsGridCharacters, docs/simulation/grid-traffic.md §7)
struct Character {
  bool alive = false;
  int model = -1;              // UnitModel index
  int citizen = -1;
  std::vector<uint32_t> path;  // q-cells packed (qz << 16 | qx), the walk to follow
  size_t next = 0;             // next path entry
  render::Vec3 pos;
  float yaw = 0, speed = 1;
  float virtualLeft = -1;      // off-screen walks: seconds left instead of a path (≥ 0 while virtual)
  render::Vec3 goal;           // where a virtual walk ends
  bool done() const { return virtualLeft >= 0 ? virtualLeft <= 0 : next >= path.size(); }
  CharacterAnimation anim;     // walk / idle blend (engine/skinning.h)
  script::Ref<script::Object> controller;  // made on demand (characterController): an event, a pick, a script
  FieldSlots controllerSlots;               // its pose fields (syncCharacterFields)
  uint8_t animal = 0;          // 1 a bird (0x07050300 list), 2 a ground animal (0x07050400); people 0
  uint16_t anchor = 0;         // animals: the home block (x >> 5) | (z >> 5) << 8 in q-cells (+0x80)
  int target[3] = {0, 0, 0};   // birds: the q-cell and altitude (1/256 cell) flown to (+0x20..+0x2c)
};

// ground walk surface: per cell 16 q-cells × 2 bits (0 none, 1 road crossing, 2 pavement, 3 open ground)
// q-cell z values carry the walk layer in bits 13-15 (0 = the ground, k = the cell's k-th deck), so a path step
// (qz << 16 | qx) keeps its layer
constexpr int kQzMask = 0x1fff, kLayerShift = 13;  // maps up to 2048 cells, up to 7 decks a cell

// a walk layer cell off the ground: bridges, tunnels, bases above the terrain (pathfinding.md §1.4)
struct WalkDeck {
  int32_t height = 0;           // 1/256 cell
  uint32_t word = 0xffffffffu;  // 2-bit codes per q-cell; a new layer cell starts open
};

struct WalkDoors {
  std::vector<uint32_t> cells;  // the item's own pavement q-cells, packed like path steps
  bool checked = false;         // only those leading out kept
};

struct WalkGrid {
  int width = 0, length = 0;
  std::vector<uint32_t> words;
  std::vector<int32_t> height;  // per cell, 1/256 cell: the surface of the item walked on, −1 = the terrain
  std::vector<int32_t> deckList;               // per cell: index into decks, −1 = none
  std::vector<std::vector<WalkDeck>> decks;    // a cell's layers in the order the items made them
  int maxDecks = 0;
  std::map<int, WalkDoors> doors;              // per item
  const WalkDeck* deck(int cx, int cz, int level) const {
    if (level <= 0 || cx < 0 || cz < 0 || cx >= width || cz >= length) return nullptr;
    int list = deckList[size_t(cz) * size_t(width) + size_t(cx)];
    if (list < 0 || size_t(level) > decks[size_t(list)].size()) return nullptr;
    return &decks[size_t(list)][size_t(level) - 1];
  }
  int code(int qx, int qz) const {
    int level = qz >> kLayerShift;
    qz &= kQzMask;
    if (qx < 0 || qz < 0 || qx >= width * 4 || qz >= length * 4) return 0;
    uint32_t w = 0;
    if (level == 0) w = words[size_t(qz >> 2) * size_t(width) + size_t(qx >> 2)];
    else if (const WalkDeck* d = deck(qx >> 2, qz >> 2, level)) w = d->word;
    return int((w >> (2 * ((qx & 3) + (qz & 3) * 4))) & 3);
  }
};

// ---- helpers shared by the files (engine/sim.cpp) ----

SimState* simOf(GridObj* g);
TransportObj* transport(script::Object* o);
LineState* lineOf(script::Object* o);
VehicleState* vehicleOf(script::Object* o);
int newMover(SimState& s);
bool lineActive(script::VM& vm, const LineState& line);
bool spawnLineVehicle(SimState& s, VehicleState& v, LineState& line, int at);
// the stop vector entry of a stop item (false for other items or one already registered)
bool registerStop(script::VM& vm, GridObj* grid, SimState& s, size_t i);
// FUN_10110c10: a stop's platform nodes for its line
void stopPlatforms(script::VM& vm, GridObj* grid, SimState& s, const LineState& line, LineStopState& st);
// FUN_10114200: the travel cost a → b (0 = unreachable); with out, the leg is stored there
float segmentBetween(SimState& s, const LineState& line, const LineStopState& a, const LineStopState& b, LineStopState* out);
// the segment of stop k to the next one (cleared at the end of an open line); true when reachable
bool computeSegment(SimState& s, LineState& line, size_t k);
// platforms and every segment of a line; the unreachable count
int relinkLine(script::VM& vm, GridObj* grid, SimState& s, LineState& line);
void syncVehicleFields(SimState& s);

// MSVC rand(): the engine's random stream
int rnd(SimState& s);
float rndFloat(SimState& s, float a, float b);  // FUN_10083250: a + (b − a)·rand()/32767
float fieldFloat(script::VM& vm, const script::Object* o, std::u16string_view name, float def);
int fieldInt(script::VM& vm, const script::Object* o, std::u16string_view name, int def);
int modelFor(script::VM& vm, GridObj* grid, SimState& s, const std::string& name);
float unitLength(const SimState& s, const Unit& u);
// gsColorMap::getRandomColor: the model's paint for a seed (0 = no colour map)
uint32_t paint(const SimState& s, int model, uint32_t seed);
bool canEnter(const SimState& s, int n, int id, bool ignoreMarks);
float freeAhead(const SimState& s, const Mover& m);
void despawn(script::VM& vm, SimState& s, int id);
int spawnTraffic(script::VM& vm, SimState& s, const std::vector<Unit>& units, int from, const std::vector<int>& path, bool train);
// a parked car on a lane node (agent flag 0x2000): it holds the node; -1 when the node is taken
int parkCar(script::VM& vm, SimState& s, const std::vector<Unit>& units, int node);
void claimBody(SimState& s, int id);
void releaseAll(SimState& s, int id);
int lastUsable(const Mover& m);
float trailLength(const SimState& s, const Mover& m);
// FUN_100e2630: the node of the same track the other way round (−1 = none)
int oppositeNode(const LaneGraph& G, int n);
// ---- jam relief of buses and trams (engine/sim_jams.cpp, port QoL; transport.md §10) ----
// one tick of a spawned line vehicle: detection, turning the cars round, the respawn; true when it left the map
bool stepJamRelief(script::VM& vm, SimState& s, LineState& line, int vi, float dt);
// an unspawned vehicle after a relief respawn: the stop it must spawn at (−1 = the usual search from the first stop)
int jamRespawnStop(SimState& s, VehicleState& v, LineState& line, float dt);
// it spawned: the port's onRegsJamMoved ticker event if the relief moved it
void jamRespawned(script::VM& vm, SimState& s, VehicleState& v, int vi);
// the node is kept by a line vehicle in jam relief other than mover `id` (canEnter refuses it then)
bool keptFor(const SimState& s, const NodeRt& r, int id);
// the test switch REGS_TEST_JAM=<line type>[:hard]: a car parked across the way of a bus or tram
void stepJamTest(script::VM& vm, SimState& s, float dt);
// the riders of a vehicle the relief takes off the map get off at `at` and plan again from the nearer of its two
// stops (engine/sim_citizens.cpp); the number of riders
int evacuateRiders(SimState& s, int vi, const render::Vec3& at);
// ---- aircraft and helicopters (engine/sim_air.cpp) ----
// inline flight nodes (0x2000000): appended to the graph's node vector without buckets or links, one owner each,
// recycled once released
int newInlineNode(SimState& s, LaneNode n);
void freeInlineNode(SimState& s, int i);
// one spawn attempt of a background aircraft (landing or departing, pending ones first)
void stepAircraft(script::VM& vm, SimState& s);
// a plane at the end of its route (or stuck) is removed
bool aircraftDone(const SimState& s, const Mover& m);
// a helicopter leg from pad node to pad node: inline Bézier nodes, then the arrival pad
void helicopterLeg(SimState& s, int from, int to, std::vector<int>& out);
bool debugSim();
// ---- door items (engine/sim_doors.cpp) ----
// the door items of the map with their train-track nodes, after the load and after live edits (phases kept by item)
void buildDoors(script::VM& vm, GridObj* grid, SimState& s);
// one item update of the door items (gsGridItem::move), every sub-step and with dt = 0 at game speed 0
void stepDoors(SimState& s, float dt);
// the port's stand-in for gsGridItem::move running in an active bucket: within this horizontal distance of the eye
constexpr float kActiveRadius = 450.0f;
// the port's density knobs for new sandbox games (gsGridCitizens.regsPopulationDensity, gsGridVehicles.regsTrafficDensity;
// 0.5, 1, 2 or 4, default 1)
float populationDensity(SimState& s);
float trafficDensity(SimState& s);
struct PlacedPose {
  render::Vec3 position, forward;
};
PlacedPose unitPose(const SimState& s, const Mover& v, const Unit& u, float r);
render::Mat4 poseMatrix(const PlacedPose& p);
// writes position, globalPos and orientation; with slots, through the cached field slots and only the values that
// differ (the fields end the same either way)
void setPoseFields(script::Object* o, const PlacedPose& p, FieldSlots* slots = nullptr);
// the same for a character at pos facing (sin yaw, 0, cos yaw): the orientation straight from the yaw
void setCharacterPoseFields(script::Object* o, const render::Vec3& pos, float yaw, FieldSlots* slots = nullptr);
// the slots of o, (re)looked up when they belong to another object
FieldSlots& fieldSlots(script::Object* o, FieldSlots& slots);
// writes v into slot unless it already holds exactly that (same type and bits)
void setSlot(script::Value& slot, const script::Value& v);
// the nearest door point of a line vehicle for a pedestrian at from (FUN_10108350); false without a live agent
bool vehicleDoorPoint(const SimState& s, const VehicleState& v, const render::Vec3& from, render::Vec3& out);
// the world centre of an item and its footprint after the orientation swap
render::Vec3 itemCentre(script::VM& vm, GridObj* grid, int item, int* sx = nullptr, int* sz = nullptr);

// ---- the citizens' file (engine/sim_citizens.cpp) ----

void registerCitizenNatives(script::VM& vm);
// gsGrid.saveToFile: the simulation sections (engine/sim_save.cpp)
void registerSimSave();
// after the lines and the traffic: walk surface, place pools, the save's citizens and pedestrians
struct SavedAgentRef {
  int mover = -1;  // the mover the agent became (moving or parked), −1 if not placed
};
void loadCitizens(script::VM& vm, GridObj* grid, SimState& s, const std::vector<uint8_t>& file,
                  const std::vector<SavedAgentRef>& agents);
// one grid sub-step: citizens round robin, pedestrians
void stepCitizens(script::VM& vm, SimState& s, float dt);
// a car of a citizen reached the end of its path or gave up
void carArrived(script::VM& vm, SimState& s, int mover, bool stuck);
// draw list and views of the pedestrians
void collectCharacters(Display& d, script::VM& vm, SimState& s, const render::Vec3& eye, const render::Mat4& viewProj,
                       float farZ, std::vector<render::DrawItem>& items);
void syncCharacterFields(SimState& s);
// live map edits (simItemsChanged): the walk layers of the cells [x0, x1] × [z0, z1] stamped again (doors of the items
// around found again, pedestrians walking through on a new path); the place pools of added and removed items with
// citizens.md §2.3's evictions
void restampWalk(SimState& s, int x0, int z0, int x1, int z1);
void citizensItemsChanged(SimState& s, const std::vector<uint32_t>& added, const std::vector<uint32_t>& removed);
// stops leaving their lines: trips through the removed line nodes or on a changed line's vehicles end, then the
// remaining nodes get their new global ids (and the transfers are linked again)
void citizensLeaveLineNodes(SimState& s, const std::vector<uint8_t>& nodeGone, const std::vector<uint8_t>& lineChanged);
void citizensRemapLineNodes(SimState& s, const std::vector<int>& newNode);
// a line vehicle docked or left: boarding and alighting happen in the citizen tick, it only needs the state

}  // namespace sim

struct SimState {
  LaneGraph graph;
  std::vector<sim::NodeRt> rt;
  std::vector<sim::UnitModel> models;
  std::map<std::string, int> modelIndex;
  std::vector<sim::LineState> lines;
  std::vector<sim::VehicleState> vehicles;
  std::vector<sim::Mover> movers;
  std::vector<int> freeMovers;
  std::vector<script::Ref<script::Object>> stops;  // gsTransportLines stop vector: every stop item's controller
  std::map<int, int> stopOfItem;   // network item → index into stops
  script::Ref<script::Object> linesModule, vehiclesModule, citizensModule, charactersModule;
  GridObj* grid = nullptr;
  script::VM* vm = nullptr;
  uint32_t seed = 1;               // the C rand() stream
  // traffic sources: road trips (stand-in when there are no citizens) and the train spawner (FUN_1010ae20)
  std::vector<int> roadStarts, roadEnds, trainStarts[2], trainEnds[2];  // [0] cargo, [1] passenger
  std::vector<int> waterStarts, waterEnds;  // water-class START / END nodes (graph+0x154 set; START wins)
  int boatsLive = 0;
  // runway-class sets (boats-aircraft.md §5.1): landing entries, landing gates, departing gates, departing exits
  std::vector<int> airEntries, airGatesIn, airGatesOut, airExits;
  int aircraftLive = 0;
  sim::AirPending landing, departing;  // a spawn waiting for its nodes to be free (vehicles+0x168..0x197)
  std::vector<sim::DoorItem> doors;    // engine/sim_doors.cpp
  std::unordered_map<uint32_t, float> doorTimes;  // per door item: DoorItem::time, for the item renderer
  std::vector<int> freeInline;         // inline flight nodes to reuse
  std::vector<uint32_t> spawnPoints;   // GRID_ITEM_SPAWNPOINT items still to turn into animals (characters+0x114)
  float birdAcc = 0, animalAcc = 0;
  int birdCursor = 0, animalCursor = 0;
  int followed = -1, followShots = 0, followFrame = 0;  // REGS_DEBUG_FOLLOW: the vehicle, a screenshot burst
  std::string followShotName = "follow-turn-";             // the burst's file names
  std::vector<std::vector<sim::Unit>> roadConsists;
  int roadTarget = 0, roadLive = 0, trainsLive = 0;
  int extraLive = 0;               // background trips running for the traffic density
  int plowsLive = 0;               // snowplows on the streets (engine/snowplows.h)
  // the mods' road speed (gsGrid.regsRoadSpeed / regsRoadSpeedCleared, engine/snowplows.h): × the target speed of
  // road vehicles, buses and trams on road and street-tram lanes; the second on lanes a snowplow cleared
  float roadSpeed = 1.0f, roadSpeedCleared = 1.0f;
  float citizenCarsAvg = -1;       // running average of the citizens' moving cars (the 1x traffic)
  int spawned = 0, despawned = 0, stuck = 0;
  // line nodes by global id: (line, stop index)
  std::vector<std::pair<int, int>> lineNodes;
  // citizens
  std::vector<sim::Citizen> citizens;
  std::vector<int> freeCitizens;
  std::map<uint32_t, int> citizenById;
  size_t citizenCursor = 0;
  float citizenAcc = 0;
  std::vector<sim::CharacterModel> charPools[7];
  std::vector<sim::CarModel> carPools[7];          // GRID_VISIT_GENERIC per class
  std::map<int, std::vector<sim::CarModel>> fleetPools;  // utility fleets: visitType · 7 + class (addCar with a type)
  int utilityTrips = 0;
  std::vector<int> freeHomes[7], freeWork[7], visitPlaces[13];
  int population[7] = {}, unemployed[7] = {};
  int walkCount[7] = {}, driveCount[7] = {}, travelCount[7] = {}, canTravelCount[7] = {}, priceTooHighCount[7] = {};
  bool citizensLoaded = false;  // citizens are simulated: saved ones, or home slots to fill (else the traffic stand-in)
  // the loaded save's records as the simulation took them over (engine/sim_save.cpp writes the others back as loaded):
  // per 0x07040200 agent the mover of its chain head (−1 = not placed: boats, aircraft; trailers and line-vehicle
  // agents stay −1 here), per 0x07050200 record its character (−1 = not loaded: animals)
  std::vector<int> loadedAgentMover, loadedCharacter;
  std::vector<sim::Character> characters;
  std::vector<int> freeCharacters;
  sim::WalkGrid walk;
  int poolsVersion = -1;          // the citizens module's addCharacter / addCar state last read
  render::Vec3 eye;               // the main camera, last drawn frame: pedestrians near it walk real paths
  bool eyeKnown = false;
  int trips = 0, boarded = 0, alighted = 0, carsSpawned = 0, carsParked = 0, parkedReused = 0;
  int doorWalks = 0;  // boardings that started with a walk to the doors (debug log)
  int walkFailKind[2] = {};  // debug: failed walk searches that hit the expansion cap / found no connection
  int breakdowns = 0, evacuated = 0, replanned = 0, crashes = 0;
  int walkPaths = 0, walkFailed = 0, walkCodes[4] = {};  // debugging: q-cells of the walked paths by walk code
  bool tearingDown = false;  // a new map replaces this state: its script objects go without touching it
  std::shared_ptr<SimSounds> sounds;  // world sound slots (engine/sim_sounds.cpp), made on first use
  std::shared_ptr<SimParticles> particles;  // emitter instances of vehicles and items (engine/particles.cpp)
  std::shared_ptr<TrafficMaps> trafficMaps;  // recent traffic for the mods' shader hooks (engine/traffic_maps.h)
};

// script objects of the transport classes, pointing back into the state
class TransportObj : public script::Object {
 public:
  using Object::Object;
  std::weak_ptr<SimState> sim;
  int index = -1;  // line, vehicle, stop, citizen or character index
  // a line or vehicle leaves the simulation (engine/sim_lines.cpp: deleteLine, selling, the depot)
  void onDestroy(script::VM& vm) override;
};

// gsTransportLines.createLine, gsTransportLine stop and vehicle editing, active, statistics (engine/sim_lines.cpp)
void registerLineNatives(script::VM& vm);
// the route strips of the visible lines (engine/sim_paths.cpp)
void collectLinePaths(Display& d, script::VM& vm, SimState& s, const render::Vec3& eye, float farZ, std::vector<render::DrawItem>& items);
// the boats' wake ribbons (gsBoatContrain, engine/particles.cpp)
void collectBoatWakes(Display& d, script::VM& vm, SimState& s, const render::Vec3& eye, float farZ, std::vector<render::DrawItem>& items);
// gsGaugeCtrl, gsVideo_OGL.renderViewport, gsTextureTarget_OGL.resetTexture (engine/ui_lines.cpp)
void registerLinePanelNatives(script::VM& vm);

}  // namespace regs::engine

// A sampling profiler of the main thread for developer builds (needs the PDB: build with /Zi and link with /DEBUG).
//   REGS_PROFILE=<first frame>:<frames>:<report file>
// From the first frame on a second thread stops the main thread about a thousand times a second, records its call
// stack and lets it run on; after <frames> frames the report is written: the functions by their own time, by the time
// spent in them and below, and the call tree of everything above 1 % of the samples. Time the main thread waits (for
// the GPU in the present call, in a sleep of the frame limit) is sampled like any other, so run with vsync and the
// frame limit off. Costs nothing when the variable is not set.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

namespace {

constexpr int kDepth = 56;            // frames kept per sample (the innermost ones)
constexpr size_t kMaxSamples = 150000;

struct Sample {
  unsigned short depth;
  DWORD64 pc[kDepth];
};

Sample* samples = nullptr;
std::atomic<size_t> sampleCount{0};
std::atomic<bool> sampling{false}, quitThread{false};
HANDLE mainThread = nullptr, samplerThread = nullptr;
long long firstFrame = -1, frameCount = 0;
std::string reportFile;
LARGE_INTEGER startTick{}, endTick{};

DWORD WINAPI samplerMain(void*) {
  timeBeginPeriod(1);
  while (!quitThread.load(std::memory_order_relaxed)) {
    Sleep(1);
    if (!sampling.load(std::memory_order_relaxed)) continue;
    size_t n = sampleCount.load(std::memory_order_relaxed);
    if (n >= kMaxSamples) continue;
    if (SuspendThread(mainThread) == DWORD(-1)) continue;
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    Sample& s = samples[n];
    s.depth = 0;
    if (GetThreadContext(mainThread, &ctx)) {
      // no allocation and no DbgHelp while the main thread stands still (it may hold the heap's or the loader's lock)
      while (s.depth < kDepth && ctx.Rip) {
        s.pc[s.depth++] = ctx.Rip;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn) {  // a leaf function: the return address lies on top of the stack
          if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
          ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
          ctx.Rsp += 8;
          continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
      }
    }
    ResumeThread(mainThread);
    if (s.depth) sampleCount.store(n + 1, std::memory_order_relaxed);
  }
  timeEndPeriod(1);
  return 0;
}

struct Node {
  size_t total = 0, self = 0;
  std::map<std::string, Node> children;
};

void printTree(std::FILE* f, const Node& node, size_t all, int depth) {
  std::vector<std::pair<const std::string*, const Node*>> kids;
  for (const auto& [name, child] : node.children)
    if (child.total * 100 >= all) kids.push_back({&name, &child});
  std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return a.second->total > b.second->total; });
  for (const auto& [name, child] : kids) {
    std::fprintf(f, "%*s%5.1f %%  %s", depth * 2, "", 100.0 * double(child->total) / double(all), name->c_str());
    if (child->self * 200 >= all) std::fprintf(f, "   [self %.1f %%]", 100.0 * double(child->self) / double(all));
    std::fputc('\n', f);
    if (depth < 30) printTree(f, *child, all, depth + 1);
  }
}

void writeReport() {
  size_t n = sampleCount.load();
  std::FILE* f = std::fopen(reportFile.c_str(), "w");
  if (!f) return;
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  // the PDB next to the exe: the exe names only "regs.pdb" (/PDBALTPATH), so the search path must hold its folder
  char exeDir[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
  if (char* slash = std::strrchr(exeDir, '\\')) *slash = 0;
  SymInitialize(proc, exeDir, TRUE);
  std::unordered_map<DWORD64, std::string> names;  // by address
  auto nameOf = [&](DWORD64 pc) -> const std::string& {
    auto it = names.find(pc);
    if (it != names.end()) return it->second;
    char buf[sizeof(SYMBOL_INFO) + 512] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::string name;
    IMAGEHLP_MODULE64 mod{};
    mod.SizeOfStruct = sizeof mod;
    std::string module = SymGetModuleInfo64(proc, pc, &mod) ? mod.ModuleName : "?";
    if (SymFromAddr(proc, pc, &disp, sym)) {
      name = sym->Name;
      if (_stricmp(module.c_str(), "regs") != 0) name = module + "!" + name;
    } else {
      name = module + "!?";
    }
    return names.emplace(pc, std::move(name)).first->second;
  };
  std::unordered_map<std::string, size_t> self, inclusive;
  Node root;
  std::vector<const std::string*> stack;
  for (size_t i = 0; i < n; ++i) {
    const Sample& s = samples[i];
    stack.clear();
    for (int d = 0; d < s.depth; ++d) stack.push_back(&nameOf(d ? s.pc[d] - 1 : s.pc[d]));  // return addresses: the call before
    ++self[*stack[0]];
    std::vector<const std::string*> seen = stack;
    std::sort(seen.begin(), seen.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
    seen.erase(std::unique(seen.begin(), seen.end(), [](const std::string* a, const std::string* b) { return *a == *b; }), seen.end());
    for (const std::string* name : seen) ++inclusive[*name];
    Node* node = &root;
    ++node->total;
    for (size_t d = stack.size(); d-- > 0;) {
      node = &node->children[*stack[d]];
      ++node->total;
    }
    ++node->self;
  }
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  double seconds = double(endTick.QuadPart - startTick.QuadPart) / double(freq.QuadPart);
  std::fprintf(f, "profile of the main thread: %zu samples over %lld frames in %.2f s (%.2f ms a frame, %.1f fps)\n\n", n,
               frameCount, seconds, 1000.0 * seconds / double(std::max<long long>(1, frameCount)),
               double(frameCount) / std::max(seconds, 1e-9));
  auto table = [&](const char* title, const std::unordered_map<std::string, size_t>& m, size_t rows) {
    std::vector<std::pair<std::string, size_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(f, "%s\n", title);
    for (size_t i = 0; i < v.size() && i < rows; ++i)
      std::fprintf(f, "  %5.1f %%  %s\n", 100.0 * double(v[i].second) / double(std::max<size_t>(1, n)), v[i].first.c_str());
    std::fputc('\n', f);
  };
  table("own time (the function itself, without what it calls):", self, 70);
  table("time in the function and below:", inclusive, 110);
  std::fprintf(f, "call tree (branches from 1 %% of the samples):\n");
  printTree(f, root, std::max<size_t>(1, n), 0);
  std::fclose(f);
}

}  // namespace

// Called by the main loop before each frame (frame counts from 0).
void profilerFrame(long long frame) {
  static bool parsed = false;
  if (!parsed) {
    parsed = true;
    const char* spec = std::getenv("REGS_PROFILE");
    char file[1024] = {};
    long long first = 0, count = 0;
    if (!spec || std::sscanf(spec, "%lld:%lld:%1023[^\n]", &first, &count, file) != 3 || count <= 0) return;
    samples = static_cast<Sample*>(VirtualAlloc(nullptr, kMaxSamples * sizeof(Sample), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!samples) return;
    firstFrame = first, frameCount = count, reportFile = file;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    samplerThread = CreateThread(nullptr, 0, samplerMain, nullptr, 0, nullptr);
  }
  if (firstFrame < 0) return;
  if (frame == firstFrame) {
    QueryPerformanceCounter(&startTick);
    sampling = true;
  } else if (frame == firstFrame + frameCount) {
    sampling = false;
    QueryPerformanceCounter(&endTick);
    quitThread = true;
    WaitForSingleObject(samplerThread, 1000);
    writeReport();
    firstFrame = -1;
  }
}

#include "ZhuyinActive.h"

#include "ZhuyinData.h"
#include "ZhuyinIdentity.h"

namespace zhuyin {

namespace {
ActiveEngine g_active;
SwapStats g_stats;
StackGuardFn g_stackGuard = nullptr;
}  // namespace

void setStackGuard(const StackGuardFn fn) { g_stackGuard = fn; }
bool resolverStackOk() {
  if (!g_stackGuard || g_stackGuard()) return true;
  g_stats.stackStops++;
  return false;
}

ActiveEngine activeEngine() { return g_active; }

void setActiveEngine(ZhuyinData* data, const int fontId, ZhuyinEngine* engine) {
  g_active.data = data;
  g_active.engine = data ? engine : nullptr;
  g_active.dataSerial = data ? data->stateSerial() : 0;
  g_active.fontId = data ? fontId : 0;
  g_active.generation++;
}

bool engineUsable(const ActiveEngine& e) {
  return e.data != nullptr && e.data->loaded() && e.data->stateSerial() == e.dataSerial;
}

SwapBinding bindingOf(const ActiveEngine& e, const PagePlace& place) {
  SwapBinding b;
  b.dataset = e.data ? e.data->datasetId() : 0;
  b.semantics = ZHUYIN_SEMANTICS_VERSION;
  b.fontId = e.fontId;
  b.place = place;
  return b;
}

void EngineRegistration::attach(ZhuyinData* data, const int fontId, ZhuyinEngine* engine) {
  release();
  data_ = data;
  setActiveEngine(data, fontId, engine);
  generation_ = g_active.generation;
}

void EngineRegistration::release() {
  if (data_ && g_active.data == data_ && g_active.generation == generation_) setActiveEngine(nullptr);
  data_ = nullptr;
  generation_ = 0;
}

SwapStats& swapStats() { return g_stats; }

}  // namespace zhuyin

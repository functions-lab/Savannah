/**
 * @file dodecode.h
 * @brief Declaration file for the DoDecode class.
 */

#ifndef DODECODE_H_
#define DODECODE_H_

#include <cstdint>
#include <memory>

#include "config.h"
#include "doer.h"
#include "mac_scheduler.h"
#include "memory_manage.h"
#include "message.h"
#include "phy_stats.h"
#include "scrambler.h"
#include "stats.h"

class DoDecode : public Doer {
 public:
  DoDecode(Config* in_config, int in_tid,
           PtrCube<kFrameWnd, kMaxSymbols, kMaxUEs, int8_t>& demod_buffers,
           PtrCube<kFrameWnd, kMaxSymbols, kMaxUEs, int8_t>& decoded_buffers,
           MacScheduler* mac_sched, PhyStats* in_phy_stats,
           Stats* in_stats_manager);
  ~DoDecode() override;

  EventData Launch(size_t tag) override;

 private:
  int16_t* resp_var_nodes_;
  PtrCube<kFrameWnd, kMaxSymbols, kMaxUEs, int8_t>& demod_buffers_;
  PtrCube<kFrameWnd, kMaxSymbols, kMaxUEs, int8_t>& decoded_buffers_;
  MacScheduler* mac_sched_;
  PhyStats* phy_stats_;
  DurationStat* duration_stat_;
  std::unique_ptr<AgoraScrambler::Scrambler> scrambler_;
};

#endif  // DODECODE_H_

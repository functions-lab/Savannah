/**
 * @file agora_worker.cc
 * @brief Implementation file for the main Agora worker class
 */

#include "agora_worker.h"

#include "concurrent_queue_wrapper.h"
#include "csv_logger.h"
#include "dobeamweights.h"
#include "dodecode.h"
#include "dodemul.h"
#include "doencode.h"
#include "dofft.h"
#include "doifft.h"
#include "doprecode.h"
#include "logger.h"

#if defined(USE_ACC100)
#include "dodecode_acc.h"
#endif

AgoraWorker::AgoraWorker(Config* cfg, MacScheduler* mac_sched, Stats* stats,
                         PhyStats* phy_stats, MessageInfo* message,
                         AgoraBuffer* buffer, FrameInfo* frame)
    : base_worker_core_offset_(cfg->CoreOffset() + 1 + cfg->SocketThreadNum()),
      config_(cfg),
      mac_sched_(mac_sched),
      stats_(stats),
      phy_stats_(phy_stats),
      message_(message),
      buffer_(buffer),
      frame_(frame) {
#ifdef SINGLE_THREAD
  tid = 0;  // starts with 0 but has only one thread (master)
  cur_qid = 0;
  empty_queue_itrs = 0;
  empty_queue = true;

  InitializeWorker();
#else
  CreateThreads();
#endif
}

#ifdef SINGLE_THREAD
AgoraWorker::~AgoraWorker() {}
#else
AgoraWorker::~AgoraWorker() { JoinThreads(); }
#endif

#ifdef SINGLE_THREAD
void AgoraWorker::InitializeWorker() {
  /* Limit the number of thread to be 1 */
  if (config_->WorkerThreadNum() != 1) {
    AGORA_LOG_ERROR("Worker: Single-core mode allows only 1 worker thread!\n");
    exit(EXIT_FAILURE);
  }

  AGORA_LOG_INFO("Worker: Initialize worker (function)\n");

  /* Initialize operators */
  // debug: use make_shared intead of make_unique to escape the scope
  auto compute_beam = std::make_shared<DoBeamWeights>(
      config_, tid, buffer_->GetCsi(), buffer_->GetCalibDl(),
      buffer_->GetCalibUl(), buffer_->GetCalibDlMsum(),
      buffer_->GetCalibUlMsum(), buffer_->GetCalib(),
      buffer_->GetUlBeamMatrix(), buffer_->GetDlBeamMatrix(), mac_sched_,
      phy_stats_, stats_);

  auto compute_fft = std::make_shared<DoFFT>(
      config_, tid, buffer_->GetFft(), buffer_->GetCsi(), buffer_->GetCalibDl(),
      buffer_->GetCalibUl(), phy_stats_, stats_);

  // Downlink workers
  auto compute_ifft = std::make_shared<DoIFFT>(config_, tid, buffer_->GetIfft(),
                                               buffer_->GetDlSocket(), stats_);

  auto compute_precode = std::make_shared<DoPrecode>(
      config_, tid, buffer_->GetDlBeamMatrix(), buffer_->GetIfft(),
      buffer_->GetDlModBits(), mac_sched_, stats_);

  auto compute_encoding = std::make_shared<DoEncode>(
      config_, tid, Direction::kDownlink,
      (kEnableMac == true) ? buffer_->GetDlBits() : config_->DlBits(),
      (kEnableMac == true) ? kFrameWnd : 1, buffer_->GetDlModBits(), mac_sched_,
      stats_);

  // Uplink workers
#if defined(USE_ACC100)
  auto compute_decoding =
      std::make_shared<DoDecode_ACC>(config_, tid, buffer_->GetDemod(),
                                     buffer_->GetDecod(), phy_stats_, stats_);
#else
  auto compute_decoding = std::make_shared<DoDecode>(
      config_, tid, buffer_->GetDemod(), buffer_->GetDecod(), mac_sched_,
      phy_stats_, stats_);
#endif

  auto compute_demul = std::make_shared<DoDemul>(
      config_, tid, buffer_->GetFft(), buffer_->GetUlBeamMatrix(),
      buffer_->GetUeSpecPilot(), buffer_->GetEqual(), buffer_->GetDemod(),
      buffer_->GetUlPhaseBase(), buffer_->GetUlPhaseShiftPerSymbol(),
      mac_sched_, phy_stats_, stats_);

  ///*************************
  computers_vec.push_back(std::move(compute_beam));
  computers_vec.push_back(std::move(compute_fft));
  events_vec.push_back(EventType::kBeam);
  events_vec.push_back(EventType::kFFT);

  if (config_->Frame().NumULSyms() > 0) {
    computers_vec.push_back(std::move(compute_decoding));
    computers_vec.push_back(std::move(compute_demul));
    events_vec.push_back(EventType::kDecode);
    events_vec.push_back(EventType::kDemul);
  }

  if (config_->Frame().NumDLSyms() > 0) {
    computers_vec.push_back(std::move(compute_ifft));
    computers_vec.push_back(std::move(compute_precode));
    computers_vec.push_back(std::move(compute_encoding));
    events_vec.push_back(EventType::kIFFT);
    events_vec.push_back(EventType::kPrecode);
    events_vec.push_back(EventType::kEncode);
  }

  AGORA_LOG_INFO("Worker: Initialization finished\n");
}

void AgoraWorker::RunWorker() {
  // if (config_->Running() == true) {
  for (size_t i = 0; i < computers_vec.size(); i++) {
    if (computers_vec.at(i)->TryLaunch(
            *message_->GetTaskQueue(events_vec.at(i), cur_qid),
            message_->GetCompQueue(cur_qid))) {
      empty_queue = false;
      break;
    }
  }
  // If all queues in this set are empty for 5 iterations,
  // check the other set of queues
  if (empty_queue == true) {
    empty_queue_itrs++;
    if (empty_queue_itrs == 5) {
      if (frame_->cur_sche_frame_id_ != frame_->cur_proc_frame_id_) {
        cur_qid ^= 0x1;
      } else {
        cur_qid = (frame_->cur_sche_frame_id_ & 0x1);
      }
      empty_queue_itrs = 0;
    }
  } else {
    empty_queue = true;
  }
  // }
}

#else

void AgoraWorker::CreateThreads() {
  // /* Limit the number of thread to be 1 */
  // if (config_->WorkerThreadNum() != 1) {
  //   AGORA_LOG_ERROR("Worker: Current implementation allows only 1 worker thread!\n");
  //   exit(EXIT_FAILURE);
  // }

  AGORA_LOG_SYMBOL("Worker: creating %zu workers\n",
                   config_->WorkerThreadNum());
  for (size_t i = 0; i < config_->WorkerThreadNum(); i++) {
    workers_.emplace_back(&AgoraWorker::WorkerThread, this, i);
  }
}

void AgoraWorker::JoinThreads() {
  for (auto& worker_thread : workers_) {
    AGORA_LOG_SYMBOL("Agora: Joining worker thread\n");
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
  }
}

void AgoraWorker::WorkerThread(int tid) {
  PinToCoreWithOffset(ThreadType::kWorker, base_worker_core_offset_, tid);

  /* Initialize operators */
  auto compute_beam = std::make_unique<DoBeamWeights>(
      config_, tid, buffer_->GetCsi(), buffer_->GetCalibDl(),
      buffer_->GetCalibUl(), buffer_->GetCalibDlMsum(),
      buffer_->GetCalibUlMsum(), buffer_->GetCalib(),
      buffer_->GetUlBeamMatrix(), buffer_->GetDlBeamMatrix(), mac_sched_,
      phy_stats_, stats_);

  auto compute_fft = std::make_unique<DoFFT>(
      config_, tid, buffer_->GetFft(), buffer_->GetCsi(), buffer_->GetCalibDl(),
      buffer_->GetCalibUl(), phy_stats_, stats_);

  // Downlink workers
  auto compute_ifft = std::make_unique<DoIFFT>(config_, tid, buffer_->GetIfft(),
                                               buffer_->GetDlSocket(), stats_);

  auto compute_precode = std::make_unique<DoPrecode>(
      config_, tid, buffer_->GetDlBeamMatrix(), buffer_->GetIfft(),
      buffer_->GetDlModBits(), mac_sched_, stats_);

  auto compute_encoding = std::make_unique<DoEncode>(
      config_, tid, Direction::kDownlink,
      (kEnableMac == true) ? buffer_->GetDlBits() : config_->DlBits(),
      (kEnableMac == true) ? kFrameWnd : 1, buffer_->GetDlModBits(), mac_sched_,
      stats_);

  // Uplink workers
#if defined(USE_ACC100)
  // RtAssert(config_->WorkerThreadNum() == 1, "ACC100: not compatible with multi thread.");
  auto compute_decoding =
      std::make_unique<DoDecode_ACC>(config_, tid, buffer_->GetDemod(),
                                     buffer_->GetDecod(), phy_stats_, stats_);
#else
  auto compute_decoding = std::make_unique<DoDecode>(
      config_, tid, buffer_->GetDemod(), buffer_->GetDecod(), mac_sched_,
      phy_stats_, stats_);
#endif

  auto compute_demul = std::make_unique<DoDemul>(
      config_, tid, buffer_->GetFft(), buffer_->GetUlBeamMatrix(),
      buffer_->GetUeSpecPilot(), buffer_->GetEqual(), buffer_->GetDemod(),
      buffer_->GetUlPhaseBase(), buffer_->GetUlPhaseShiftPerSymbol(),
      mac_sched_, phy_stats_, stats_);

  std::vector<Doer*> computers_vec;
  std::vector<EventType> events_vec;
  ///*************************
  computers_vec.push_back(compute_beam.get());
  computers_vec.push_back(compute_fft.get());
  events_vec.push_back(EventType::kBeam);
  events_vec.push_back(EventType::kFFT);

  if (config_->Frame().NumULSyms() > 0) {
    computers_vec.push_back(compute_decoding.get());
    computers_vec.push_back(compute_demul.get());
    events_vec.push_back(EventType::kDecode);
    events_vec.push_back(EventType::kDemul);
  }

  if (config_->Frame().NumDLSyms() > 0) {
    computers_vec.push_back(compute_ifft.get());
    computers_vec.push_back(compute_precode.get());
    computers_vec.push_back(compute_encoding.get());
    events_vec.push_back(EventType::kIFFT);
    events_vec.push_back(EventType::kPrecode);
    events_vec.push_back(EventType::kEncode);
  }

  size_t cur_qid = 0;
  size_t empty_queue_itrs = 0;
  bool empty_queue = true;
  while (config_->Running() == true) {
    for (size_t i = 0; i < computers_vec.size(); i++) {
      if (computers_vec.at(i)->TryLaunch(
              *message_->GetTaskQueue(events_vec.at(i), cur_qid),
              message_->GetCompQueue(cur_qid),
              message_->GetWorkerPtok(cur_qid, tid))) {
        empty_queue = false;
        break;
      }
    }
    // If all queues in this set are empty for 5 iterations,
    // check the other set of queues
    if (empty_queue == true) {
      empty_queue_itrs++;
      if (empty_queue_itrs == 5) {
        if (frame_->cur_sche_frame_id_ != frame_->cur_proc_frame_id_) {
          cur_qid ^= 0x1;
        } else {
          cur_qid = (frame_->cur_sche_frame_id_ & 0x1);
        }
        empty_queue_itrs = 0;
      }
    } else {
      empty_queue = true;
    }
  }
  AGORA_LOG_SYMBOL("Agora worker %d exit\n", tid);
}
#endif
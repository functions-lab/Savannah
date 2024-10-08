/**
 * @file txrx_worker_client_uhd.cc
 * @brief Implementation of PacketTxRx datapath functions for communicating
 * with real usrp hardware
 */

#include "txrx_worker_client_uhd.h"

#include <cassert>
#include <complex>

#include "comms-lib.h"
#include "datatype_conversion.h"
#include "gettime.h"
#include "logger.h"
#include "message.h"

static constexpr bool kDebugBeaconChannels = false;
static constexpr size_t kSyncDetectChannel = 0;
static constexpr bool kVerifyFirstSync = true;
static constexpr size_t kReSyncRetryCount = 1000000u;
static constexpr float kBeaconDetectWindow = 2.33f;
static constexpr size_t kBeaconsToStart = 2;
static constexpr bool kPrintClientBeaconSNR = true;
static constexpr ssize_t kMaxBeaconAdjust = 5;
static constexpr bool kThreadedTx = false;

static constexpr bool kDebugRxTimes = false;
static constexpr bool kSyncRadio = false;

TxRxWorkerClientUhd::TxRxWorkerClientUhd(
    size_t core_offset, size_t tid, size_t interface_count,
    size_t interface_offset, Config* const config, size_t* rx_frame_start,
    moodycamel::ConcurrentQueue<EventData>* event_notify_q,
    moodycamel::ConcurrentQueue<EventData>* tx_pending_q,
    moodycamel::ProducerToken& tx_producer,
    moodycamel::ProducerToken& notify_producer,
    std::vector<RxPacket>& rx_memory, std::byte* const tx_memory,
    std::mutex& sync_mutex, std::condition_variable& sync_cond,
    std::atomic<bool>& can_proceed, RadioSet& radio_config)
    : TxRxWorker(core_offset, tid, interface_count, interface_offset,
                 config->NumUeChannels(), config, rx_frame_start,
                 event_notify_q, tx_pending_q, tx_producer, notify_producer,
                 rx_memory, tx_memory, sync_mutex, sync_cond, can_proceed),
      radio_(radio_config),
      program_start_ticks_(0),
      doResync(false),
      num_ue_stream(config->NumUeChannels()),
      adjust_Tx(0),
      frame_zeros_(
          config->NumUeChannels(),
          std::vector<std::complex<int16_t>>(
              (config->SampsPerSymbol() * config->Frame().NumTotalSyms()),
              std::complex<int16_t>(0, 0))),
      //kOffsetOfData to allocate space for the Packet * header.
      frame_storage_(
          config->NumUeChannels(),
          std::vector<std::complex<int16_t>>(
              Packet::kOffsetOfData +
                  (config->SampsPerSymbol() * config->Frame().NumTotalSyms()),
              std::complex<int16_t>(0, 0))),
      rx_frame_pkts_(config->NumUeChannels()),
      rx_pkts_ptrs_(config->NumUeChannels()) {
  for (size_t ch = 0; ch < config->NumUeChannels(); ch++) {
    auto* pkt_memory = reinterpret_cast<Packet*>(frame_storage_.at(ch).data());
    auto& scratch_rx_memory = rx_frame_pkts_.at(ch);
    scratch_rx_memory.Set(pkt_memory);
    rx_pkts_ptrs_.at(ch) = &scratch_rx_memory;

    AGORA_LOG_TRACE(
        "TxRxWorkerClientUhd - rx pkt memory %ld:%ld data location %ld\n",
        reinterpret_cast<intptr_t>(pkt_memory),
        reinterpret_cast<intptr_t>(scratch_rx_memory.RawPacket()),
        reinterpret_cast<intptr_t>(scratch_rx_memory.RawPacket()->data_));
  }
  //throw std::runtime_error("Rx Prt locations");
  RtAssert(interface_count == 1,
           "Interface count must be set to 1 for use with this class");

  RtAssert(config->UeHwFramer() == false, "Must have ue hw framer disabled");
  InitRxStatus();
}

TxRxWorkerClientUhd::~TxRxWorkerClientUhd() = default;

//Main Thread Execution loop
void TxRxWorkerClientUhd::DoTxRx() {
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_offset_, tid_);

  AGORA_LOG_INFO("TxRxWorkerClientUhd[%zu] has %zu:%zu total radios %zu\n",
                 tid_, interface_offset_,
                 (interface_offset_ + num_interfaces_) - 1, num_interfaces_);

  std::vector<void*> rx_locs;
  //Maybe needs to be num channels * interfaces
  std::vector<std::vector<std::complex<short>>> rx_ignore_memory(
      num_interfaces_,
      std::vector<std::complex<short>>(Configuration()->SampsPerSymbol()));
  for (size_t i = 0; i < num_interfaces_; i++) {
    rx_locs.emplace_back(rx_ignore_memory.at(i).data());
  }

  running_ = true;
  std::thread tx_thread;

  const size_t samples_per_symbol = Configuration()->SampsPerSymbol();
  const size_t samples_per_frame =
      samples_per_symbol * Configuration()->Frame().NumTotalSyms();

  //Thread Sync
  WaitSync();
  program_start_ticks_ = GetTime::Rdtsc();

  if (num_interfaces_ == 0) {
    AGORA_LOG_WARN("TxRxWorkerClientUhd[%zu] has no interfaces, exiting\n",
                   tid_);
    running_ = false;
    return;
  } else if (num_interfaces_ > 1) {
    throw std::runtime_error(
        "TxRxWorkerClientUhd does not support multiple interfaces per thread");
  }
  size_t local_interface = 0;
  long long rx_time = 0;

  //Probably most efficient to make this a multiple of 64
  const size_t beacon_detect_window = static_cast<size_t>(
      static_cast<float>(samples_per_symbol) * kBeaconDetectWindow);
  const size_t alignment_samples = samples_per_frame - beacon_detect_window;
  RtAssert(beacon_detect_window < samples_per_frame,
           "Frame must be greater than the beacon detect window");

  //Turns out detecting more than 1 beacon is helpful for the start
  size_t beacons_detected = 0;
  while ((beacons_detected < kBeaconsToStart) &&
         (Configuration()->Running() == true)) {
    const ssize_t sync_index =
        SyncBeacon(local_interface, beacon_detect_window);
    if (sync_index >= 0) {
      auto rx_adjust_samples = sync_index - Configuration()->BeaconLen() -
                               Configuration()->OfdmTxZeroPrefix();
      AGORA_LOG_INFO(
          "TxRxWorkerClientUhd [%zu]: Beacon detected for radio %zu, "
          "sync_index: %ld, rx sample offset: %ld, window %zu, samples in "
          "frame %zu, alignment removal %zu\n",
          tid_, local_interface + interface_offset_, sync_index,
          rx_adjust_samples, beacon_detect_window, samples_per_frame,
          alignment_samples);

      AdjustRx(local_interface, alignment_samples + rx_adjust_samples);
      beacons_detected++;
    } else if (Configuration()->Running()) {
      AGORA_LOG_WARN(
          "TxRxWorkerClientUhd [%zu]: Beacon could not be detected on "
          "interface %zu - sync_index: %ld\n",
          tid_, local_interface, sync_index);
      throw std::runtime_error("rx sample offset is less than 0");
    }
  }
  long long time0 = 0;
  //Set initial frame and symbol to max value so we start at 0
  //size_t rx_frame_id = SIZE_MAX;
  //size_t rx_symbol_id = Configuration()->Frame().NumTotalSyms() - 1;
  size_t rx_frame_id = 0;
  size_t rx_symbol_id = 0;

  bool resync = false;
  size_t resync_retry_cnt = 0;
  size_t resync_success = 0;
  const size_t max_cfo = 200;  // in ppb, For Iris
  // If JSON input if not default (0),
  // Else calculate based of ppb and frame length
  const size_t frame_sync_period =
      static_cast<int>(Configuration()->UeResyncPeriod()) > 0
          ? static_cast<unsigned long>(Configuration()->UeResyncPeriod())
          : static_cast<size_t>(
                std::floor(1e9 / (max_cfo * Configuration()->SampsPerFrame())));

  std::stringstream sout;

  //No Need to preschedule the TX_FRAME_DELTA init in software framer mode
  //Beacon sync detected run main rx routines
  while (Configuration()->Running()) {
    if ((Configuration()->FramesToTest() > 0) &&
        (rx_frame_id > Configuration()->FramesToTest())) {
      Configuration()->Running(false);
      break;
    }

    size_t tx_status = 0;
    if (kThreadedTx == false) {
      if (time0 != 0) {
        // std::cout<<"DoTx called"<<std::endl;
        tx_status = DoTx(time0);
        doResync = false;
      }
    }
    if (tx_status == 0) {
      const auto rx_pkts =
          DoRx(local_interface, rx_frame_id, rx_symbol_id, rx_time);
      if (kDebugPrintInTask) {
        AGORA_LOG_INFO(
            "DoTxRx[%zu]: radio %zu received frame id %zu, symbol id %zu at "
            "time %lld\n",
            tid_, local_interface + interface_offset_, rx_frame_id,
            rx_symbol_id, rx_time);
      }
      //Rx Success
      if (rx_pkts.size() > 0) {
        if ((rx_frame_id == 0) && (rx_frame_id == 0) &&
            (local_interface == 0)) {
          time0 = rx_time;
          //Launch TX attempt
          if (kThreadedTx) {
            std::cout << "DoTxThread called" << std::endl;
            tx_thread =
                std::thread(&TxRxWorkerClientUhd::DoTxThread, this, time0);
          }

          if (kVerifyFirstSync) {
            for (size_t ch = 0; ch < channels_per_interface_; ch++) {
              const ssize_t sync_index = FindSyncBeacon(
                  reinterpret_cast<std::complex<int16_t>*>(
                      rx_pkts.at(ch)->data_),
                  samples_per_symbol, Configuration()->ClCorrScale().at(tid_));
              if (sync_index >= 0) {
                AGORA_LOG_INFO(
                    "TxRxWorkerClientUhd [%zu]: Initial Sync - radio %zu, "
                    "frame "
                    "%zu, symbol %zu sync_index: %ld, rx sample offset: %ld "
                    "time0 %lld\n",
                    tid_, (local_interface + interface_offset_) + ch,
                    rx_frame_id, rx_symbol_id, sync_index,
                    sync_index - Configuration()->BeaconLen() -
                        Configuration()->OfdmTxZeroPrefix(),
                    time0);
              } else {
                throw std::runtime_error(
                    "No Beacon Detected at Frame 0 / Symbol 0");
              }
            }
          }  // end verify first sync
        }

        // resync every frame_sync_period frames:
        // Only sync on beacon symbols
        if ((rx_symbol_id == Configuration()->Frame().GetBeaconSymbolLast()) &&
            ((rx_frame_id / frame_sync_period) > 0) &&
            ((rx_frame_id % frame_sync_period) == 0)) {
          resync = true;
        }

        //If we have a beacon and we would like to resync
        if (resync &&
            (rx_symbol_id == Configuration()->Frame().GetBeaconSymbolLast())) {
          const ssize_t sync_index = FindSyncBeacon(
              reinterpret_cast<std::complex<int16_t>*>(
                  rx_pkts.at(kSyncDetectChannel)->data_),
              samples_per_symbol, Configuration()->ClCorrScale().at(tid_));
          if (sync_index >= 0) {
            const ssize_t adjust = sync_index - Configuration()->BeaconLen() -
                                   Configuration()->OfdmTxZeroPrefix();
            adjust_Tx = adjust;
            doResync = true;
            if (std::abs(adjust) > kMaxBeaconAdjust) {
              AGORA_LOG_TRACE(
                  "TxRxWorkerClientUhd [%zu]: Re-syncing ignored due to "
                  "excess "
                  "offset %ld - channel %zu, sync_index: %ld, tries %zu\n ",
                  tid_, adjust, kSyncDetectChannel, sync_index,
                  resync_retry_cnt);
            } else {
              AGORA_LOG_INFO(
                  "TxRxWorkerClientUhd [%zu]: Re-syncing channel %zu, "
                  "sync_index: %ld, rx sample offset: %ld tries %zu\n ",
                  tid_, kSyncDetectChannel, sync_index, adjust,
                  resync_retry_cnt);
              resync_success++;
              resync = false;
              //Display all the other channels
              if (kDebugBeaconChannels) {
                for (size_t ch = 0; ch < channels_per_interface_; ch++) {
                  if (ch != kSyncDetectChannel) {
                    const ssize_t aux_channel_sync =
                        FindSyncBeacon(reinterpret_cast<std::complex<int16_t>*>(
                                           rx_pkts.at(ch)->data_),
                                       samples_per_symbol,
                                       Configuration()->ClCorrScale().at(tid_));
                    AGORA_LOG_INFO(
                        "TxRxWorkerClientUhd [%zu]: beacon status channel "
                        "%zu, "
                        "sync_index: %ld, rx sample offset: %ld\n",
                        tid_, ch, aux_channel_sync,
                        aux_channel_sync -
                            (Configuration()->BeaconLen() +
                             Configuration()->OfdmTxZeroPrefix()));
                  }
                }
              }
              resync_retry_cnt = 0;
            }
          } else {
            resync_retry_cnt++;
            if (resync_retry_cnt > kReSyncRetryCount) {
              AGORA_LOG_ERROR(
                  "TxRxWorkerClientUhd [%zu]: Exceeded resync retry limit "
                  "(%zu) "
                  "for client %zu reached after %zu resync successes at "
                  "frame: "
                  "%zu.  Stopping!\n",
                  tid_, kReSyncRetryCount, local_interface + interface_offset_,
                  resync_success, rx_frame_id);
              Configuration()->Running(false);
              break;
            }
          }
        }  // end resync

      }  //    if (rx_pkts.size() > 0) {

      rx_time_ue_ = rx_time;
      //Asummes each Rx returns symbols
      local_interface++;
      if (local_interface == num_interfaces_) {
        local_interface = 0;
        // Update global frame_id and symbol_id
        rx_symbol_id++;
        if (rx_symbol_id == Configuration()->Frame().NumTotalSyms()) {
          rx_symbol_id = 0;
          rx_frame_id++;
        }
      }  // interface rollover

      //Necessary?
      //std::this_thread::yield();
    }  // end tx_success
  }    // end main while loop
  running_ = false;
  if (tx_thread.joinable()) {
    tx_thread.join();
  }
}

//RX data, should return channel number of packets || 0
// frame_id  in - frame id of the current rx packet
// symbol_id in - symbol id of the current rx packet
std::vector<Packet*> TxRxWorkerClientUhd::DoRx(size_t interface_id,
                                               size_t frame_id,
                                               size_t symbol_id,
                                               long long& receive_time) {
  const size_t radio_id = interface_id + interface_offset_;
  const size_t first_ant_id = radio_id * channels_per_interface_;

  long long rx_time;
  Radio::RxFlags rx_flags;
  std::vector<Packet*> result_packets;
  auto& rx_info = rx_status_.at(interface_id);
  const size_t num_rx_samps = Configuration()->SampsPerSymbol();

  //Check for completion
  if (rx_info.SamplesAvailable() > 0) {
    AGORA_LOG_WARN("DoRx - Unexpected samples availble %zu exiting...\n",
                   rx_info.SamplesAvailable());
    throw std::runtime_error("Need to implement this!!!");
    ResetRxStatus(interface_id, true);
  }

  AGORA_LOG_TRACE(
      "TxRxWorkerClientUhd[%zu]: DoRx - Calling RadioRx[%zu], available %zu, "
      "offset %ld, requesting samples %zu:%zu\n",
      tid_, radio_id, rx_info.SamplesAvailable(), 0, num_rx_samps,
      Configuration()->SampsPerSymbol());

  auto rx_locations = rx_info.GetRxPtrs();
  const int rx_status =
      radio_.RadioRx(radio_id, rx_locations, num_rx_samps, rx_flags, rx_time);

  if (rx_status == static_cast<int>(Configuration()->SampsPerSymbol())) {
    const size_t new_samples = static_cast<size_t>(rx_status);
    rx_info.Update(new_samples, rx_time);

    if (kDebugRxTimes) {
      if ((rx_time_ue_ + rx_status) != rx_time) {
        AGORA_LOG_WARN(
            "TxRxWorkerClientUhd[%zu]: DoRx Unexpected Rx time "
            "%lld:%lld(%lld)\n",
            tid_, rx_time, static_cast<long long>(rx_time_ue_ + rx_status),
            rx_time_ue_);
      }
    }
    receive_time = rx_info.StartTime();

    if (kDebugPrintInTask) {
      AGORA_LOG_INFO(
          "TxRxWorkerClientUhd[%zu]: DoRx (Frame %zu, Symbol %zu, Radio "
          "%zu) - at time %lld\n",
          tid_, frame_id, symbol_id, radio_id, receive_time);
    }

    const bool publish_symbol = IsRxSymbol(symbol_id);
    if (publish_symbol) {
      auto packets = rx_info.GetRxPackets();
      for (size_t ch = 0; ch < channels_per_interface_; ch++) {
        auto* rx_packet = packets.at(ch);
        auto* raw_pkt = rx_packet->RawPacket();
        new (raw_pkt) Packet(frame_id, symbol_id, 0, first_ant_id + ch);
        result_packets.push_back(raw_pkt);

        AGORA_LOG_FRAME(
            "TxRxWorkerClientUhd[%zu]: DoRx Downlink (Frame %zu, Symbol "
            "%zu, Ant %zu) from Radio %zu at time %lld\n",
            tid_, frame_id, symbol_id, first_ant_id + ch, radio_id,
            receive_time);

        // Push kPacketRX event into the queue.
        EventData rx_message(EventType::kPacketRX, rx_tag_t(*rx_packet).tag_);
        NotifyComplete(rx_message);
      }
    }  // end is RxSymbol
    ResetRxStatus(interface_id, (publish_symbol == false));
  } else {
    // AGORA_LOG_ERROR(
    //     "TxRxWorkerClientUhd[%zu]::DoRx: Unexpected Rx return status %dn\n",
    //     tid_, rx_status);
    // throw std::runtime_error(
    //     "TxRxWorkerClientUhd::DoRx:Unexpected Rx return status");
  }
  return result_packets;
}

size_t TxRxWorkerClientUhd::DoTxThread(long long time0) {
  PinToCoreWithOffset(ThreadType::kWorkerTXRX, core_offset_, tid_ + 6);

  AGORA_LOG_INFO(
      "TxRxWorkerClientUhd[%zu] Tx Thread -- has %zu:%zu total radios %zu\n",
      tid_, interface_offset_, (interface_offset_ + num_interfaces_) - 1,
      num_interfaces_);

  //Making GetPendingTxEvents / DoTx event based / sleep wakeup would be preferrable here
  while (Configuration()->Running()) {
    const auto tx_status = DoTx(time0);
    if (tx_status == 0) {
      //Sleep or yield here.
      //std::this_thread::yield();
    }
  }
  return 0;
}

//Tx data
size_t TxRxWorkerClientUhd::DoTx(long long time0) {
  auto tx_events = GetPendingTxEvents();

  for (const EventData& current_event : tx_events) {
    RtAssert((current_event.event_type_ == EventType::kPacketTX) ||
                 (current_event.event_type_ == EventType::kPacketPilotTX),
             "Wrong Event Type in TX Queue!");

    //Assuming the 1 message per radio per frame
    const size_t frame_id = gen_tag_t(current_event.tags_[0u]).frame_id_;
    const size_t ue_ant = gen_tag_t(current_event.tags_[0u]).ue_id_;
    const size_t interface_id = ue_ant / channels_per_interface_;
    const size_t ant_offset = ue_ant % channels_per_interface_;

    AGORA_LOG_FRAME(
        "TxRxWorkerClientUhd::DoTx[%zu]: Request to Transmit (Frame %zu, "
        "User %zu, Ant %zu) time0 %lld\n",
        tid_, frame_id, interface_id, ue_ant, time0);

    RtAssert((interface_id >= interface_offset_) &&
                 (interface_id <= (num_interfaces_ + interface_offset_)),
             "Invalid Tx interface Id");
    RtAssert(interface_id == tid_,
             "TxRxWorkerClientUhd::DoTx - Ue id was not the expected values");

    //For Tx we need all channels_per_interface_ antennas before we can transmit
    //we will assume that if you get the last antenna, you have already received all
    //other antennas (enforced in the passing utility)

    // if (!kSyncRadio){
    //   int interval = 1000;
    //   int extra_time = 25;
    //   int frame_group = 0;
    //   if (frame_id > 1000){
    //     frame_group = (frame_id - 1000) / interval;
    //   }
    //   if (frame_group > 0) {
    //     time0 = time0 + (frame_group * extra_time);
    //   }
    // }

    if (doResync) {
      time0 = time0 + adjust_Tx / num_ue_stream;
    }

    if ((ant_offset + 1) == channels_per_interface_) {
      // Transmit pilot(s)
      for (size_t ch = 0; ch < channels_per_interface_; ch++) {
        const size_t pilot_ant = (interface_id * channels_per_interface_) + ch;
        //Each pilot will be in a different tx slot (called for each pilot)
        TxPilot(pilot_ant, frame_id, time0);

        //Pilot transmit complete for pilot ue
        if (current_event.event_type_ == EventType::kPacketPilotTX) {
          auto complete_event =
              EventData(EventType::kPacketPilotTX,
                        gen_tag_t::FrmSymUe(frame_id, 0, pilot_ant).tag_);
          NotifyComplete(complete_event);
        }
      }  //For each channel

      if (current_event.event_type_ == EventType::kPacketTX) {
        // Transmit data for all symbols (each cannel transmits for each symbol)
        TxUplinkSymbols(interface_id, frame_id, time0);
        //Notify the tx is complete for all antennas on the interface
        for (size_t ch = 0; ch < channels_per_interface_; ch++) {
          const size_t tx_ant = (interface_id * channels_per_interface_) + ch;
          //Frame transmit complete
          auto complete_event =
              EventData(EventType::kPacketTX,
                        gen_tag_t::FrmSymUe(frame_id, 0, tx_ant).tag_);
          NotifyComplete(complete_event);
        }
        AGORA_LOG_TRACE(
            "TxRxWorkerClientUhd::DoTx[%zu]: Frame %zu Transmit Complete for "
            "Ue %zu\n",
            tid_, frame_id, interface_id);
      }
    }
  }  // End all events
  return tx_events.size();
}

///\todo for the multi radio case should let this return if not enough data is found
/// This function blocks untill all the discard_samples are received for a given local_interface
void TxRxWorkerClientUhd::AdjustRx(size_t local_interface,
                                   size_t discard_samples) {
  const size_t radio_id = local_interface + interface_offset_;
  long long rx_time = 0;

  size_t request_samples = discard_samples;
  TxRxWorkerRx::RxStatusTracker rx_tracker(channels_per_interface_);
  rx_tracker.Reset(rx_pkts_ptrs_);

  while (Configuration()->Running() && (request_samples > 0)) {
    auto rx_locations = rx_tracker.GetRxPtrs();
    Radio::RxFlags out_flags;
    const int rx_status = radio_.RadioRx(radio_id, rx_locations,
                                         request_samples, out_flags, rx_time);

    if (rx_status < 0) {
      AGORA_LOG_ERROR("AdjustRx [%zu]: BAD SYNC Received (%d/%zu) %lld\n", tid_,
                      rx_status, request_samples, rx_time);
    } else {
      size_t new_samples = static_cast<size_t>(rx_status);
      rx_tracker.Update(new_samples, rx_time);
      if (new_samples <= request_samples) {
        request_samples -= new_samples;
      } else {
        AGORA_LOG_ERROR(
            "SycBeacon [%zu]: BAD SYNC Rx more samples then requested "
            "(%zu/%zu) %lld\n",
            tid_, new_samples, request_samples, rx_time);
      }
    }
  }  // request_samples > 0
  rx_time_ue_ = rx_time;
}

///\todo for the multi radio case should let this return if not enough data is found
ssize_t TxRxWorkerClientUhd::SyncBeacon(size_t local_interface,
                                        size_t sample_window) {
  const size_t radio_id = local_interface + interface_offset_;
  ssize_t sync_index = -1;
  long long rx_time = 0;
  assert(sample_window <= (Configuration()->SampsPerSymbol() *
                           Configuration()->Frame().NumTotalSyms()));

  size_t request_samples = sample_window;
  TxRxWorkerRx::RxStatusTracker rx_tracker(channels_per_interface_);
  rx_tracker.Reset(rx_pkts_ptrs_);

  while (Configuration()->Running() && (sync_index < 0)) {
    auto rx_locations = rx_tracker.GetRxPtrs();
    Radio::RxFlags out_flags;
    const int rx_status = radio_.RadioRx(radio_id, rx_locations,
                                         request_samples, out_flags, rx_time);

    if (rx_status < 0) {
      AGORA_LOG_ERROR("SyncBeacon [%zu]: BAD SYNC Received (%d/%zu) %lld\n",
                      tid_, rx_status, sample_window, rx_time);
    } else if (rx_status > 0) {
      const size_t new_samples = static_cast<size_t>(rx_status);
      const bool is_cont = rx_tracker.CheckContinuity(rx_time);
      if (is_cont == false) {
        AGORA_LOG_WARN(
            "SyncBeacon - Received new non-contiguous samples %zu, ignoring "
            "%zu, %zu \n",
            new_samples, rx_tracker.SamplesAvailable(), sample_window);
        //Samples do not align, throw out all old + new samples.
        rx_tracker.DiscardOld(new_samples, rx_time);
      } else {
        rx_tracker.Update(new_samples, rx_time);
        if (new_samples == request_samples) {
          AGORA_LOG_TRACE(
              "SyncBeacon - Samples %zu:%zu, Window %zu - Check Beacon %ld\n",
              new_samples, rx_tracker.SamplesAvailable(), sample_window,
              reinterpret_cast<intptr_t>(
                  rx_pkts_ptrs_.at(kSyncDetectChannel)->RawPacket()->data_));

          sync_index = FindSyncBeacon(
              reinterpret_cast<std::complex<int16_t>*>(
                  rx_pkts_ptrs_.at(kSyncDetectChannel)->RawPacket()->data_),
              sample_window, Configuration()->ClCorrScale().at(tid_));
          //Throw out samples until we detect the beacon
          request_samples = sample_window;
          rx_tracker.Reset(rx_pkts_ptrs_);
        } else if (new_samples < request_samples) {
          AGORA_LOG_TRACE("SyncBeacon - Samples %zu:%zu, Window %zu\n",
                          new_samples, rx_tracker.SamplesAvailable(),
                          sample_window);
          request_samples -= new_samples;
        } else {
          AGORA_LOG_ERROR(
              "SycBeacon [%zu]: BAD SYNC Rx more samples then requested "
              "(%zu/%zu) %lld\n",
              tid_, new_samples, request_samples, rx_time);
        }
      }  // is continuous
    }
  }  // end while sync_index < 0
  return sync_index;
}

ssize_t TxRxWorkerClientUhd::FindSyncBeacon(
    const std::complex<int16_t>* check_data, size_t sample_window,
    float corr_scale) {
  ssize_t sync_index = -1;
  assert(sample_window <= (Configuration()->SampsPerSymbol() *
                           Configuration()->Frame().NumTotalSyms()));

  sync_index = CommsLib::FindBeaconAvx(check_data, Configuration()->GoldCf32(),
                                       sample_window, corr_scale);

  if (kPrintClientBeaconSNR && (sync_index >= 0) &&
      ((sync_index + Configuration()->BeaconLen()) < sample_window)) {
    ///\todo Remove this float conversion to speed up function
    float sig_power = 0;
    float noise_power = 0;
    for (size_t i = 0; i < Configuration()->BeaconLen(); i++) {
      const size_t power_idx = sync_index - i;
      const size_t noise_idx = sync_index + i + 1;
      std::complex<float> power_value;
      std::complex<float> noise_value;
      ConvertShortToFloat(
          reinterpret_cast<const short*>(&check_data[power_idx]),
          reinterpret_cast<float*>(&power_value), 2);

      ConvertShortToFloat(
          reinterpret_cast<const short*>(&check_data[noise_idx]),
          reinterpret_cast<float*>(&noise_value), 2);

      sig_power += std::pow(std::abs(power_value), 2);
      noise_power += std::pow(std::abs(noise_value), 2);
    }
    AGORA_LOG_INFO("TxRxWorkerClientUhd: Sync Beacon - SNR %2.1f dB\n",
                   +10 * std::log10(sig_power / noise_power));
  }
  return sync_index;
}

bool TxRxWorkerClientUhd::IsRxSymbol(size_t symbol_id) {
  auto symbol_type = Configuration()->GetSymbolType(symbol_id);
  bool is_rx;

  if ((symbol_type == SymbolType::kBeacon) ||
      (symbol_type == SymbolType::kDL)) {
    is_rx = true;
  } else {
    is_rx = false;
  }
  return is_rx;
}

// All UL symbols
void TxRxWorkerClientUhd::TxUplinkSymbols(size_t radio_id, size_t frame_id,
                                          long long time0) {
  const size_t tx_frame_id = frame_id + TX_FRAME_DELTA;
  const size_t samples_per_symbol = Configuration()->SampsPerSymbol();
  const size_t samples_per_frame =
      samples_per_symbol * Configuration()->Frame().NumTotalSyms();
  Radio::TxFlags flags_tx = Radio::TxFlags::kTxFlagNone;

  std::vector<void*> tx_data(channels_per_interface_);
  for (size_t ul_symbol_idx = 0;
       ul_symbol_idx < Configuration()->Frame().NumULSyms(); ul_symbol_idx++) {
    const size_t tx_symbol_id =
        Configuration()->Frame().GetULSymbol(ul_symbol_idx);

    bool start_tx = false;
    bool end_tx = false;
    size_t prev_symbol = tx_symbol_id;
    size_t next_symbol = tx_symbol_id;

    if (ul_symbol_idx != 0) {
      prev_symbol = Configuration()->Frame().GetULSymbol(ul_symbol_idx - 1);
    }
    if ((ul_symbol_idx + 1) != Configuration()->Frame().NumULSyms()) {
      next_symbol = Configuration()->Frame().GetULSymbol(ul_symbol_idx + 1);
    }

    //If no tx symbol before, then start
    if ((prev_symbol + 1) != tx_symbol_id) {
      start_tx = true;
    }
    //If no tx symbol after, then end
    if ((tx_symbol_id + 1) != next_symbol) {
      end_tx = true;
    }

    if (start_tx == true && end_tx == true) {
      flags_tx = Radio::TxFlags::kStartEndTransmit;
    } else if (start_tx == true) {
      flags_tx = Radio::TxFlags::kStartTransmit;
    } else if (end_tx == true) {
      flags_tx = Radio::TxFlags::kEndTransmit;
    } else {
      flags_tx = Radio::TxFlags::kTxFlagNone;
    }

    for (size_t ch = 0; ch < channels_per_interface_; ch++) {
      const size_t tx_ant = (radio_id * channels_per_interface_) + ch;
      if (kDebugUplink) {
        tx_data.at(ch) = reinterpret_cast<void*>(
            &Configuration()
                 ->UlIqT()[ul_symbol_idx]
                          [tx_ant * Configuration()->SampsPerSymbol()]);
      } else {
        auto* pkt = GetUlTxPacket(frame_id, tx_symbol_id, tx_ant);
        tx_data.at(ch) = reinterpret_cast<void*>(pkt->data_);
      }
    }

    long long tx_time = time0 + (tx_frame_id * samples_per_frame) +
                        (tx_symbol_id * samples_per_symbol) -
                        Configuration()->ClTxAdvance().at(radio_id);

    if (tx_time < rx_time_ue_) {
      AGORA_LOG_ERROR(
          "Requested tx time %lld is in the past.  Last Rx Time %lld. "
          "Transmission will not be correct - diff %lld\n",
          tx_time, rx_time_ue_, rx_time_ue_ - tx_time);
    }
    const int tx_status = radio_.RadioTx(radio_id, tx_data.data(),
                                         samples_per_symbol, flags_tx, tx_time);
    if (tx_status < static_cast<int>(samples_per_symbol)) {
      std::cout << "BAD Write (UL): For Ue " << radio_id << " " << tx_status
                << "/" << samples_per_symbol << std::endl;
    }
    if (false) {
      AGORA_LOG_INFO(
          "TxRxWorkerClientUhd::DoTx[%zu]: Transmitted Symbol (Frame "
          "%zu:%zu, Symbol %zu, Ue %zu) at time %lld:%lld:%lld flags %d\n",
          tid_, frame_id, tx_frame_id, tx_symbol_id, radio_id, tx_time,
          rx_time_ue_, tx_time - rx_time_ue_, static_cast<int>(flags_tx));
    }
  }
}

void TxRxWorkerClientUhd::TxPilot(size_t pilot_ant, size_t frame_id,
                                  long long time0) {
  const size_t tx_frame_id = frame_id + TX_FRAME_DELTA;
  const size_t pilot_channel = (pilot_ant % channels_per_interface_);
  const size_t radio = pilot_ant / channels_per_interface_;
  const size_t samples_per_symbol = Configuration()->SampsPerSymbol();
  const size_t samples_per_frame =
      samples_per_symbol * Configuration()->Frame().NumTotalSyms();
  long long tx_time;

  std::vector<void*> tx_data(channels_per_interface_);
  for (size_t ch = 0; ch < channels_per_interface_; ch++) {
    if (ch == pilot_channel) {
      tx_data.at(ch) = Configuration()->PilotCi16().data();
    } else {
      tx_data.at(ch) = frame_zeros_.at(ch).data();
    }
  }

  const size_t pilot_symbol_id =
      Configuration()->Frame().GetPilotSymbol(pilot_ant);

  //Assume nothing is before the pilot...
  Radio::TxFlags flags_tx = Radio::TxFlags::kStartEndTransmit;

  //See if we need to set end burst for the last channel
  // (see if the next symbol is an uplink symbol)
  if ((pilot_channel + 1) == channels_per_interface_) {
    if (Configuration()->Frame().NumULSyms() > 0) {
      const size_t first_ul_symbol = Configuration()->Frame().GetULSymbol(0);
      if ((pilot_symbol_id + 1) == (first_ul_symbol)) {
        flags_tx = Radio::TxFlags::kStartTransmit;
      }
    }
  } else {
    flags_tx = Radio::TxFlags::kStartTransmit;
  }

  tx_time = time0 + (tx_frame_id * samples_per_frame) +
            (pilot_symbol_id * samples_per_symbol) -
            Configuration()->ClTxAdvance().at(radio);

  const int tx_status = radio_.RadioTx(radio, tx_data.data(),
                                       samples_per_symbol, flags_tx, tx_time);

  if (tx_status < 0) {
    std::cout << "BAD Radio Tx: (PILOT)" << tx_status << "For Ue Radio "
              << radio << "/" << samples_per_symbol << std::endl;
  } else if (tx_status != static_cast<int>(samples_per_symbol)) {
    std::cout << "BAD Write: (PILOT)" << tx_status << "For Ue Radio " << radio
              << "/" << samples_per_symbol << std::endl;
  }

  if (kDebugPrintInTask) {
    AGORA_LOG_INFO(
        "TxRxWorkerClientUhd::DoTx[%zu]: Transmitted Pilot  (Frame "
        "%zu:%zu, Symbol %zu, Ue %zu, Ant %zu:%zu) at time %lld flags "
        "%d\n",
        tid_, frame_id, tx_frame_id, pilot_symbol_id, radio, pilot_channel,
        pilot_ant, tx_time, static_cast<int>(flags_tx));
  }
}

void TxRxWorkerClientUhd::InitRxStatus() {
  rx_status_.resize(num_interfaces_,
                    TxRxWorkerRx::RxStatusTracker(channels_per_interface_));
  std::vector<RxPacket*> rx_packets(channels_per_interface_);
  for (auto& status : rx_status_) {
    for (auto& new_packet : rx_packets) {
      new_packet = &GetRxPacket();
      AGORA_LOG_TRACE(
          "InitRxStatus[%zu]: Using Packet at location %ld, data location "
          "%ld\n",
          tid_, reinterpret_cast<intptr_t>(new_packet),
          reinterpret_cast<intptr_t>(new_packet->RawPacket()->data_));
    }
    //Allocate memory for each interface / channel
    status.Reset(rx_packets);
  }
}

void TxRxWorkerClientUhd::ResetRxStatus(size_t interface, bool reuse_memory) {
  auto& prev_status = rx_status_.at(interface);

  std::vector<RxPacket*> rx_packets;
  if (reuse_memory) {
    rx_packets = rx_status_.at(interface).GetRxPackets();
  } else {
    for (size_t packets = 0; packets < prev_status.NumChannels(); packets++) {
      rx_packets.emplace_back(&GetRxPacket());
    }
  }
  prev_status.Reset(rx_packets);
}

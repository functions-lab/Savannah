/**
 * @file ue_radio_set.cc
 * @brief Implementation file for the RadioSetUe class
 */

#include "radio_set_ue.h"

#include "logger.h"

RadioSetUe::RadioSetUe(const Config* const cfg, Radio::RadioType radio_type)
    : RadioSet(cfg->SampsPerSymbol()), cfg_(cfg) {

#if defined(USE_PURE_UHD)
  total_radios_ = 1;
#else
  total_radios_ = cfg_->UeNum();
#endif
  
  total_antennas_ = cfg_->UeAntNum();
  std::cout << "Total Number of Client Radios " << cfg_->UeNum() << " with "
            << total_antennas_ << " antennas" << std::endl;

  for (size_t i = 0; i < total_radios_; i++) {
    radios_.emplace_back(Radio::Create(radio_type));
  }

  std::vector<std::thread> radio_threads;
  num_client_radios_initialized_ = 0;
  for (size_t i = 0; i < total_radios_; i++) {
#if defined(THREADED_INIT)
    radio_threads.emplace_back(&RadioSetUe::InitRadio, this, i);
#else
    InitRadio(i);
#endif
  }  // end for (size_t i = 0; i < total_radios_; i++)

#if defined(THREADED_INIT)
  size_t num_checks = 0;
  size_t num_client_radios_init = num_client_radios_initialized_.load();
  while (num_client_radios_init != total_radios_) {
    num_checks++;
    if (num_checks > 1e9) {
      AGORA_LOG_INFO(
          "RadioSetUe: Waiting for radio initialization, %zu of %zu "
          "ready\n",
          num_client_radios_init, total_radios_);
      num_checks = 0;
    }
    num_client_radios_init = num_client_radios_initialized_.load();
  }

  for (auto& init_thread : radio_threads) {
    init_thread.join();
  }
#endif

  for (const auto& radio : radios_) {
    radio->PrintSettings();
  }
  AGORA_LOG_INFO("RadioSetUe: Radio init complete\n");
}

void RadioSetUe::InitRadio(size_t radio_id) {
  
  #if defined(USE_PURE_UHD)
    size_t total_ue_channel;
    if (cfg_->Channel() == "AB") {
      total_ue_channel = cfg_->UeNum() * 2;
    } else {
      total_ue_channel = cfg_->UeNum();
    }
    std::vector<size_t> new_channels(total_ue_channel);

    if (cfg_->Channel() == "AB") {
      for (size_t ii = 0; ii < total_ue_channel; ii++) {
        new_channels[ii] = ii;
      }
    } else if (cfg_->Channel() == "A") {
      for (size_t ii = 0; ii < total_ue_channel; ii++) {
        new_channels[ii] = ii * 2;
      }
    } else {
      for (size_t ii = 0; ii < total_ue_channel; ii++) {
        new_channels[ii] = ii * 2 + 1;
      }
    }
    radios_.at(radio_id)->Init(cfg_, radio_id, cfg_->UeRadioId().at(radio_id),
                             new_channels, false, true);
  #else
    radios_.at(radio_id)->Init(cfg_, radio_id, cfg_->UeRadioId().at(radio_id),
                             Utils::StrToChannels(cfg_->UeChannel()),
                             cfg_->UeHwFramer(), true);
  #endif

  std::vector<double> tx_gains;
  tx_gains.emplace_back(cfg_->ClientTxGainA(radio_id));
  tx_gains.emplace_back(cfg_->ClientTxGainB(radio_id));

  std::vector<double> rx_gains;
  rx_gains.emplace_back(cfg_->ClientRxGainA(radio_id));
  rx_gains.emplace_back(cfg_->ClientRxGainB(radio_id));

  radios_.at(radio_id)->Setup(tx_gains, rx_gains);
  this->num_client_radios_initialized_.fetch_add(1);
}

bool RadioSetUe::RadioStart() {
  // send through the first radio for now
  for (size_t i = 0; i < total_radios_; i++) {
    if (cfg_->UeHwFramer()) {
      radios_.at(i)->ConfigureTddModeUe();
    }
  }
  RadioSet::RadioStart(Radio::kActivateWaitTrigger);
  AGORA_LOG_INFO("RadioSetUe: Radio start complete!\n");
  return true;
}

void RadioSetUe::Go() {
  for (size_t i = 0; i < total_radios_; i++) {
    radios_.at(i)->Trigger();
  }
}
/**
 * @file radio_lib_uhd.cc
 * @brief Implementation file for the RadioSetUhd class.
 */
#include "radio_set_uhd.h"

#include <thread>

#include "logger.h"

// only one BS radio object, since, no emplace_back is needed, and the thread number for BS is also set to be 1
RadioSetUhd::RadioSetUhd(Config* cfg, Radio::RadioType radio_type)
    : RadioSet(cfg->SampsPerSymbol()),
      cfg_(cfg),
      num_radios_initialized_(0),
      num_radios_configured_(0) {
  // load channels
  auto channels = Utils::StrToChannels(cfg_->Channel());

  // for UHD USRP case, there would only be one radio object
  radio_num_ = 1;
  antenna_num_ = cfg_->BsAntNum();
  AGORA_LOG_INFO("BS Radio num is: %d, Antenna num: %d \n", radio_num_,
                 antenna_num_);

  for (size_t i = 0; i < radio_num_; i++) {
    radios_.emplace_back(Radio::Create(radio_type));
  }
  AGORA_LOG_INFO("radio UHD created here \n");
  std::vector<std::thread> init_bs_threads;
  for (size_t i = 0; i < radio_num_; i++) {
#if defined(THREADED_INIT)
    init_bs_threads.emplace_back(&RadioSetUhd::InitRadio, this, i);
#else
    InitRadio(i);
#endif
  }

  // Block until all radios are initialized
  size_t num_checks = 0;
  size_t num_radios_init = num_radios_initialized_.load();
  while (num_radios_init != radio_num_) {
    num_checks++;
    if (num_checks > 1e9) {
      std::printf(
          "RadioSetUhd: Waiting for radio initialization, %zu of %zu "
          "ready\n",
          num_radios_init, radio_num_);
      num_checks = 0;
    }
    num_radios_init = num_radios_initialized_.load();
  }

  for (auto& join_thread : init_bs_threads) {
    join_thread.join();
  }

  std::vector<std::thread> config_bs_threads;
  for (size_t i = 0; i < radio_num_; i++) {
#if defined(THREADED_INIT)
    config_bs_threads.emplace_back(&RadioSetUhd::ConfigureRadio, this, i);
#else
    ConfigureRadio(i);
#endif
  }

  AGORA_LOG_INFO("radio UHD configured here \n");
  num_checks = 0;
  // Block until all radios are configured
  size_t num_radios_config = num_radios_configured_.load();
  while (num_radios_config != radio_num_) {
    num_checks++;
    if (num_checks > 1e9) {
      AGORA_LOG_WARN(
          "RadioSetUhd: Waiting for radio initialization, %zu of %zu "
          "ready\n",
          num_radios_config, radio_num_);
      num_checks = 0;
    }
    num_radios_config = num_radios_configured_.load();
  }
  for (auto& join_thread : config_bs_threads) {
    join_thread.join();
  }
  for (const auto& radio : radios_) {
    radio->PrintSettings();
  }
  AGORA_LOG_INFO("RadioSetUhd init complete!\n");
}

void RadioSetUhd::InitRadio(size_t radio_id) {
  size_t total_rx_channel;

  if (cfg_->Channel() == "AB") {
    total_rx_channel = cfg_->NumRadios() * 2;
  } else {
    total_rx_channel = cfg_->NumRadios();
  }

  std::vector<size_t> new_channels(total_rx_channel);

  if (cfg_->Channel() == "AB") {
    for (size_t ii = 0; ii < total_rx_channel; ii++) {
      new_channels[ii] = ii;
    }
  } else if (cfg_->Channel() == "A") {
    for (size_t ii = 0; ii < total_rx_channel; ii++) {
      new_channels[ii] = ii * 2;
    }
  } else {
    for (size_t ii = 0; ii < total_rx_channel; ii++) {
      new_channels[ii] = ii * 2 + 1;
    }
  }

  std::cout<<"!!!!!!!!!!!!!! here"<<std::endl;
  radios_.at(radio_id)->Init(cfg_, radio_id, cfg_->RadioId().at(radio_id), new_channels, false, false);
  num_radios_initialized_.fetch_add(1);
}

void RadioSetUhd::ConfigureRadio(size_t radio_id) {
  std::vector<double> tx_gains;
  tx_gains.emplace_back(cfg_->TxGainA());
  tx_gains.emplace_back(cfg_->TxGainB());

  std::vector<double> rx_gains;
  rx_gains.emplace_back(cfg_->RxGainA());
  rx_gains.emplace_back(cfg_->RxGainB());

  radios_.at(radio_id)->Setup(tx_gains, rx_gains);
  num_radios_configured_.fetch_add(1);
}

bool RadioSetUhd::RadioStart() {
  for (size_t i = 0; i < radio_num_; i++) {
    //radios_.at(i)->ConfigureTddModeBs(false);
    //radios_.at(i)->SetTimeAtTrigger(0);
  }
  RadioSet::RadioStart(Radio::kActivate);
  return true;
}

//Maybe we start the streaming on all radios at go....
void RadioSetUhd::Go() {}
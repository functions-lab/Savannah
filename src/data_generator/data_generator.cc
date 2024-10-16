/**
 * @file data_generator.cc
 * @brief Data generator to generate binary files as inputs to Agora, sender
 * and correctness tests
 */

#include "data_generator.h"

#include <cstdio>
#include <memory>

#include "comms-lib.h"
#include "crc.h"
#include "datatype_conversion.h"
#include "logger.h"
#include "modulation.h"
#include "phy_ldpc_decoder_5gnr.h"
#include "scrambler.h"

#if defined(USE_ACC100_ENCODE)
#include "concurrent_queue_wrapper.h"
#include "rte_bbdev.h"
#include "rte_bbdev_op.h"
#include "rte_bus_vdev.h"
#define GET_SOCKET(socket_id) (((socket_id) == SOCKET_ID_ANY) ? 0 : (socket_id))
#define MAX_RX_BYTE_SIZE 1500
#define CACHE_SIZE 128
#define NUM_QUEUES 4
#define LCORE_ID 36
#define NUM_ELEMENTS_IN_POOL 2047
#define NUM_ELEMENTS_IN_MEMPOOL 16383
#define DATA_ROOM_SIZE 45488
#define MAX_DEQUEUE_TRIAL 1000000

#endif

static constexpr bool kPrintDebugCSI = false;
static constexpr bool kDebugPrintRxData = false;
static constexpr bool kPrintEncodedBytes = false;
static constexpr bool kPrintDlTxData = false;
static constexpr bool kPrintDlModData = false;
static constexpr bool kPrintUplinkInformationBytes = false;
static constexpr bool kPrintDownlinkInformationBytes = false;

///Output files
static const std::string kUlDataPrefix = "orig_ul_data_";
static const std::string kUlLdpcDataPrefix = "LDPC_orig_ul_data_";
static const std::string kUlLdpcEncodedPrefix = "LDPC_ul_encoded_";
static const std::string kUlLdpcACC100DataPrefix = "LDPC_ACC100_orig_ul_data_";
static const std::string kDlDataPrefix = "orig_dl_data_";
static const std::string kDlLdpcDataPrefix = "LDPC_orig_dl_data_";
static const std::string kRxLdpcPrefix = "LDPC_rx_data_";
static const std::string kDlTxPrefix = "LDPC_dl_tx_data_";
static const std::string kUlScBitsPrefix = "ul_data_b_";

//Utilities?
static float RandFloatFromShort(float min, float max) {
  float rand_val = ((float(rand()) / float(RAND_MAX)) * (max - min)) + min;
  const auto rand_val_short = static_cast<short>(rand_val * kShrtFltConvFactor);
  rand_val = static_cast<float>(rand_val_short) / kShrtFltConvFactor;
  return rand_val;
}

void print_casted_uint8_hex_1(const uint8_t* array, size_t totalByteLength) {
  for (size_t i = 0; i < totalByteLength; i++) {
    // Cast each int8_t to uint8_t and print as two-digit hexadecimal
    printf("%02X ", (uint8_t)array[i]);
  }
  printf("\n");  // Add a newline for readability
}

#if defined(USE_ACC100_ENCODE)
int allocate_buffers_on_socket(struct rte_bbdev_op_data **buffers,
                                             const int len, const int socket) {
  int i;
  *buffers = static_cast<struct rte_bbdev_op_data *>(
      rte_zmalloc_socket(NULL, len, 0, socket));
  if (*buffers == NULL) {
    printf("WARNING: Failed to allocate op_data on socket %d\n", socket);
    /* try to allocate memory on other detected sockets */
    for (i = 0; i < socket; i++) {
      *buffers = static_cast<struct rte_bbdev_op_data *>(
          rte_zmalloc_socket(NULL, len, 0, i));
      if (*buffers != NULL) break;
    }
  }
  return (*buffers == NULL) ? TEST_FAILED : TEST_SUCCESS;
}
#endif

DataGenerator::DataGenerator(Config* cfg, uint64_t seed, Profile profile)
    : cfg_(cfg), seed_(seed), profile_(profile) {
  if (seed != 0) {
    fast_rand_.seed_ = seed;
  }
}

void DataGenerator::DoDataGeneration(const std::string& directory) {
  //Make sure the directory exists
  if (std::filesystem::is_directory(directory) == false) {
    std::filesystem::create_directory(directory);
  }
  srand(time(nullptr));
  std::unique_ptr<DoCRC> crc_obj = std::make_unique<DoCRC>();
  const size_t ul_cb_bytes = cfg_->NumBytesPerCb(Direction::kUplink);
  LDPCconfig ul_ldpc_config = this->cfg_->LdpcConfig(Direction::kUplink);
  const size_t symbol_blocks =
    ul_ldpc_config.NumBlocksInSymbol() * this->cfg_->UeAntNum();
  const size_t num_ul_codeblocks =
      this->cfg_->Frame().NumUlDataSyms() * symbol_blocks;
  AGORA_LOG_SYMBOL("Total number of ul blocks: %zu\n", num_ul_codeblocks);

#if defined(USE_ACC100_ENCODE)
  std::string core_list = std::to_string(LCORE_ID);  // this is hard set to core 36
  const char *rte_argv[] = {"txrx",        "-l",           core_list.c_str(),
                            "--log-level", "lib.eal:info", nullptr};
  int rte_argc = static_cast<int>(sizeof(rte_argv) / sizeof(rte_argv[0])) - 1;

  // Initialize DPDK environment
  int ret = rte_eal_init(rte_argc, const_cast<char **>(rte_argv));
  RtAssert(
      ret >= 0,
      "Failed to initialize DPDK.  Are you running with root permissions?");

  int nb_bbdevs = rte_bbdev_count();
  std::cout << "num bbdevs: " << nb_bbdevs << std::endl;
    if (nb_bbdevs == 0) rte_exit(EXIT_FAILURE, "No bbdevs detected!\n");
  dev_id = 0;
  struct rte_bbdev_info info;
  // rte_bbdev_info_get(dev_id, &info);
  rte_bbdev_intr_enable(dev_id);
  rte_bbdev_info_get(dev_id, &info);

  bbdev_op_pool =
      rte_bbdev_op_pool_create("bbdev_op_pool_enc", RTE_BBDEV_OP_LDPC_ENC,
                               NB_MBUF, CACHE_SIZE , rte_socket_id());
  ret = rte_bbdev_setup_queues(dev_id, NUM_QUEUES, info.socket_id);

  if (ret < 0) {
    printf("rte_bbdev_setup_queues(%u, %u, %d) ret %i\n", dev_id, NUM_QUEUES,
           rte_socket_id(), ret);
  }

  ret = rte_bbdev_intr_enable(dev_id);

  struct rte_bbdev_queue_conf qconf;
  qconf.socket = info.socket_id;
  qconf.queue_size = info.drv.queue_size_lim;
  qconf.op_type = RTE_BBDEV_OP_LDPC_ENC;
  qconf.priority = 0;

  std::cout << "device id is: " << static_cast<int>(dev_id) << std::endl;

  for (int q_id = 0; q_id < NUM_QUEUES; q_id++) {
    /* Configure all queues belonging to this bbdev device */
    ret = rte_bbdev_queue_configure(dev_id, q_id, &qconf);
    if (ret < 0)
      rte_exit(EXIT_FAILURE,
               "ERROR(%d): BBDEV %u queue %u not configured properly\n", ret,
               dev_id, q_id);
  }

  ret = rte_bbdev_start(dev_id);
  int socket_id = GET_SOCKET(info.socket_id);


  ops_mp = rte_bbdev_op_pool_create("RTE_BBDEV_OP_LDPC_ENC_poo",
                                    RTE_BBDEV_OP_LDPC_ENC, NUM_ELEMENTS_IN_POOL, OPS_CACHE_SIZE,
                                    socket_id);
  if (ops_mp == nullptr) {
    std::cerr << "Error: Failed to create memory pool for bbdev operations."
              << std::endl;
  } else {
    std::cout << "Memory pool for bbdev operations created successfully."
              << std::endl;
  }

    in_mbuf_pool = rte_pktmbuf_pool_create("in_pool_0", NUM_ELEMENTS_IN_MEMPOOL, 0, 0, DATA_ROOM_SIZE, 0);
  out_mbuf_pool =
      rte_pktmbuf_pool_create("hard_out_pool_0", NUM_ELEMENTS_IN_MEMPOOL, 0, 0, DATA_ROOM_SIZE, 0);

  if (in_mbuf_pool == nullptr or out_mbuf_pool == nullptr) {
    std::cerr << "Error: Unable to create mbuf pool: "
              << rte_strerror(rte_errno) << std::endl;
  }

  int rte_alloc_ref = rte_bbdev_enc_op_alloc_bulk(ops_mp, ref_enc_op, num_ul_codeblocks);
  if (rte_alloc_ref != TEST_SUCCESS ) {
    rte_exit(EXIT_FAILURE, "Failed to alloc bulk\n");
  }

  const struct rte_bbdev_op_cap *cap = info.drv.capabilities;
  const struct rte_bbdev_op_cap *capabilities = NULL;
  rte_bbdev_info_get(dev_id, &info);
  for (unsigned int i = 0; cap->type != RTE_BBDEV_OP_NONE; ++i, ++cap) {
    std::cout << "cap is: " << cap->type << std::endl;
    if (cap->type == RTE_BBDEV_OP_LDPC_ENC) {
      capabilities = cap;
      std::cout << "capability is being set to: " << capabilities->type
                << std::endl;
      break;
    }
  }

  inputs =
      (struct rte_bbdev_op_data **)malloc(sizeof(struct rte_bbdev_op_data *));
  hard_outputs =
      (struct rte_bbdev_op_data **)malloc(sizeof(struct rte_bbdev_op_data *));

  int ret_socket_in = allocate_buffers_on_socket(
      inputs, 1 * sizeof(struct rte_bbdev_op_data), 0);
  int ret_socket_hard_out = allocate_buffers_on_socket(
      hard_outputs, 1 * sizeof(struct rte_bbdev_op_data), 0);

  if (ret_socket_in != TEST_SUCCESS || ret_socket_hard_out != TEST_SUCCESS) {
    rte_exit(EXIT_FAILURE, "Failed to allocate socket\n");
  }

  // ldpc_llr_decimals = capabilities->cap.ldpc_enc.llr_decimals;
  // ldpc_llr_size = capabilities->cap.ldpc_dec.llr_size;
  // ldpc_cap_flags = capabilities->cap.ldpc_dec.capability_flags;

  min_alignment = info.drv.min_alignment;

  q_m = cfg_->ModOrderBits(Direction::kUplink);
  e = ul_ldpc_config.NumCbCodewLen();

  for (int i = 0; i < num_ul_codeblocks; i++) {
  // ref_dec_op[i]->ldpc_dec.op_flags += RTE_BBDEV_LDPC_ITERATION_STOP_ENABLE;
    ref_enc_op[i]->ldpc_enc.basegraph = (uint8_t)ul_ldpc_config.BaseGraph();
    ref_enc_op[i]->ldpc_enc.z_c = (uint16_t)ul_ldpc_config.ExpansionFactor();
    ref_enc_op[i]->ldpc_enc.n_filler = (uint16_t)0;
    ref_enc_op[i]->ldpc_enc.rv_index = (uint8_t)0;
    ref_enc_op[i]->ldpc_enc.n_cb = (uint16_t)ul_ldpc_config.NumCbCodewLen();
    // std::cout << "n_cb is: " << (uint16_t)ldpc_config.NumCbCodewLen() <<  std::endl;
    ref_enc_op[i]->ldpc_enc.q_m = (uint8_t)q_m;
    ref_enc_op[i]->ldpc_enc.code_block_mode = (uint8_t)1;
    ref_enc_op[i]->ldpc_enc.cb_params.e = (uint32_t)e;
    // std::cout << "e is: " << (uint32_t)e <<  std::endl;
    ref_enc_op[i]->opaque_data = (void *)(uintptr_t)i;
  }
  std::cout << "" << std::endl;
  AGORA_LOG_INFO("rte_pktmbuf_alloc successful\n");

#endif

  // Step 1: Generate the information buffers (MAC Packets) and LDPC-encoded
  // buffers for uplink
  std::vector<std::vector<complex_float>> pre_ifft_data_syms;
  const size_t num_ul_mac_bytes =
      this->cfg_->MacBytesNumPerframe(Direction::kUplink);
  if (num_ul_mac_bytes > 0) {
    std::vector<std::vector<int8_t>> ul_mac_info(cfg_->UeAntNum());
    AGORA_LOG_INFO("Total number of uplink MAC bytes: %zu\n", num_ul_mac_bytes);
    for (size_t ue_id = 0; ue_id < cfg_->UeAntNum(); ue_id++) {
      ul_mac_info.at(ue_id).resize(num_ul_mac_bytes);
      for (size_t pkt_id = 0;
           pkt_id < cfg_->MacPacketsPerframe(Direction::kUplink); pkt_id++) {
        size_t pkt_offset = pkt_id * cfg_->MacPacketLength(Direction::kUplink);
        auto* pkt = reinterpret_cast<MacPacketPacked*>(
            &ul_mac_info.at(ue_id).at(pkt_offset));

        pkt->Set(0, pkt_id, ue_id,
                 cfg_->MacPayloadMaxLength(Direction::kUplink));
        this->GenMacData(pkt, ue_id);
        pkt->Crc((uint16_t)(
            crc_obj->CalculateCrc24(
                pkt->Data(), cfg_->MacPayloadMaxLength(Direction::kUplink)) &
            0xFFFF));
      }
    }

    {
      const std::string filename_input =
          directory + kUlDataPrefix + std::to_string(this->cfg_->OfdmCaNum()) +
          "_ant" + std::to_string(this->cfg_->UeAntNum()) + ".bin";
      AGORA_LOG_INFO("Saving uplink MAC data to %s\n", filename_input.c_str());
      auto* fp_input = std::fopen(filename_input.c_str(), "wb");
      if (fp_input == nullptr) {
        AGORA_LOG_ERROR("Failed to create file %s\n", filename_input.c_str());
        throw std::runtime_error("Failed to create file" + filename_input);
      } else {
        for (size_t i = 0; i < cfg_->UeAntNum(); i++) {
          const auto write_status =
              std::fwrite(reinterpret_cast<uint8_t*>(ul_mac_info.at(i).data()),
                          sizeof(uint8_t), num_ul_mac_bytes, fp_input);
          if (write_status != num_ul_mac_bytes) {
            AGORA_LOG_ERROR("Wrote %zu out of %zu to file %s\n", write_status,
                            num_ul_mac_bytes, filename_input.c_str());
            throw std::runtime_error("Failed to write to file" +
                                     filename_input);
          }
        }
        const auto close_status = std::fclose(fp_input);
        if (close_status != 0) {
          throw std::runtime_error("Failed to close file" + filename_input);
        }
      }

      if (kPrintUplinkInformationBytes) {
        std::printf("Uplink information bytes\n");
        for (size_t n = 0; n < cfg_->UeAntNum(); n++) {
          std::printf("UE %zu\n", n % this->cfg_->UeAntNum());
          for (size_t i = 0; i < num_ul_mac_bytes; i++) {
            std::printf("%u ", static_cast<uint8_t>(ul_mac_info.at(n).at(i)));
          }
          std::printf("\n");
        }
      }
    }

    std::vector<std::vector<int8_t>> ul_information(num_ul_codeblocks);
    std::vector<std::vector<int8_t>> ul_encoded_codewords(num_ul_codeblocks);
    std::vector<std::vector<int8_t>> ul_encoded_codewords_flexRAN(num_ul_codeblocks);
    const size_t encoded_bytes = BitsToBytes(ul_ldpc_config.NumCbCodewLen());
    for (size_t cb = 0; cb < num_ul_codeblocks; cb++) {
      // i : symbol -> ue -> cb (repeat)
      size_t sym_id = cb / (symbol_blocks);
      // ue antenna for code block
      size_t sym_offset = cb % (symbol_blocks);
      size_t ue_id = sym_offset / ul_ldpc_config.NumBlocksInSymbol();
      size_t ue_cb_id = sym_offset % ul_ldpc_config.NumBlocksInSymbol();
      size_t ue_cb_cnt =
          (sym_id * ul_ldpc_config.NumBlocksInSymbol()) + ue_cb_id;

      AGORA_LOG_TRACE(
          "cb %zu -- user %zu -- user block %zu -- user cb id %zu -- input "
          "size %zu, index %zu, total size %zu\n",
          cb, ue_id, ue_cb_id, ue_cb_cnt, ul_cb_bytes, ue_cb_cnt * ul_cb_bytes,
          ul_mac_info.at(ue_id).size());
      int8_t* cb_start = &ul_mac_info.at(ue_id).at(ue_cb_cnt * ul_cb_bytes);
      ul_information.at(cb) =
          std::vector<int8_t>(cb_start, cb_start + ul_cb_bytes);
      ul_encoded_codewords_flexRAN.at(cb) = DataGenerator::GenCodeblock(
          ul_ldpc_config, &ul_information.at(cb).at(0), ul_cb_bytes,
          this->cfg_->ScrambleEnabled());

#if defined(USE_ACC100_ENCODE)
      ul_encoded_codewords.at(cb) = DataGenerator::GenCodeblock_ACC100(
          ul_ldpc_config, &ul_information.at(cb).at(0), ul_cb_bytes,
          this->cfg_->ScrambleEnabled(), cb);
#else
      ul_encoded_codewords.at(cb) = DataGenerator::GenCodeblock(
          ul_ldpc_config, &ul_information.at(cb).at(0), ul_cb_bytes,
          this->cfg_->ScrambleEnabled());
#endif
    }

    // the following generated file is used as a reference to compare BLER.
    {
      std::cout << "directory is: " << directory << std::endl;
      const std::string filename_input =
          directory + kUlLdpcDataPrefix +
          std::to_string(this->cfg_->OfdmCaNum()) + "_ant" +
          std::to_string(this->cfg_->UeAntNum()) + ".bin";
      AGORA_LOG_INFO("Saving raw uplink data (using LDPC) to %s\n",
                     filename_input.c_str());
      auto* fp_input = std::fopen(filename_input.c_str(), "wb");
      if (fp_input == nullptr) {
        AGORA_LOG_ERROR("Failed to create file %s\n", filename_input.c_str());
        throw std::runtime_error("Failed to create file" + filename_input);
      } else {
        for (size_t i = 0; i < num_ul_codeblocks; i++) {
          const auto write_status = std::fwrite(
              reinterpret_cast<uint8_t*>(&ul_information.at(i).at(0)),
              sizeof(uint8_t), ul_cb_bytes, fp_input);
          if (write_status != ul_cb_bytes) {
            AGORA_LOG_ERROR("Wrote %zu out of %zu to file %s\n", write_status,
                            ul_cb_bytes, filename_input.c_str());
            throw std::runtime_error("Failed to write to file" +
                                     filename_input);
          }
        }
        const auto close_status = std::fclose(fp_input);
        if (close_status != 0) {
          throw std::runtime_error("Failed to close file" + filename_input);
        }
      }

      const std::string filename_input_encoded =
          directory + kUlLdpcEncodedPrefix +
          std::to_string(this->cfg_->OfdmCaNum()) + "_ant" +
          std::to_string(this->cfg_->UeAntNum()) + ".bin";
      AGORA_LOG_INFO("Saving encoded uplink data (using LDPC) to %s\n",
                     filename_input_encoded.c_str());
      auto* fp_input_encoded = std::fopen(filename_input_encoded.c_str(), "wb");
      if (fp_input_encoded == nullptr) {
        AGORA_LOG_ERROR("Failed to create file %s\n", filename_input_encoded.c_str());
        throw std::runtime_error("Failed to create file" + filename_input_encoded);
      } else {
        for (size_t i = 0; i < num_ul_codeblocks; i++) {
          const auto write_status = std::fwrite(
              reinterpret_cast<uint8_t*>(&ul_encoded_codewords.at(i).at(0)),
              sizeof(uint8_t), encoded_bytes, fp_input_encoded);
          if (write_status != encoded_bytes) {
            AGORA_LOG_ERROR("Wrote %zu out of %zu to file %s\n", write_status,
                            encoded_bytes, filename_input_encoded.c_str());
            throw std::runtime_error("Failed to write to file" +
                                     filename_input_encoded);
          }
        }
        const auto close_status_encoded = std::fclose(fp_input_encoded);
        if (close_status_encoded != 0) {
          throw std::runtime_error("Failed to close file" + filename_input_encoded);
        }
      }

      if (kPrintUplinkInformationBytes) {
        std::printf("Uplink information bytes\n");
        for (size_t n = 0; n < num_ul_codeblocks; n++) {
          std::printf("Symbol %zu, UE %zu\n", n / this->cfg_->UeAntNum(),
                      n % this->cfg_->UeAntNum());
          for (size_t i = 0; i < ul_cb_bytes; i++) {
            std::printf("%02X ",
                        static_cast<uint8_t>(ul_information.at(n).at(i)));
          }
          std::printf("\n");
        }

        if (kPrintEncodedBytes){
          std::printf("Encoded Uplink information bytes\n");
          for (size_t n = 0; n < num_ul_codeblocks; n++) {
            std::printf("encoded bytes Symbol %zu, UE %zu\n", n / this->cfg_->UeAntNum(),
                        n % this->cfg_->UeAntNum());
            for (size_t i = 0; i < encoded_bytes; i++) {
              std::printf("%02X ",
                          static_cast<uint8_t>(ul_encoded_codewords.at(n).at(i)));
            }
            std::printf("\n");
          }

          std::printf("Encoded Uplink information bytes for FlexRAN is: \n");
          for (size_t n = 0; n < num_ul_codeblocks; n++) {
            std::printf("FlexRAN encoded bytes Symbol %zu, UE %zu\n", n / this->cfg_->UeAntNum(),
                        n % this->cfg_->UeAntNum());
            for (size_t i = 0; i < encoded_bytes; i++) {
              std::printf("%02X ",
                          static_cast<uint8_t>(ul_encoded_codewords_flexRAN.at(n).at(i)));
            }
            std::printf("\n");
          }
        }
      }
    }

// The following are used to generate ACC100 LDPC realted encoding files:
#if defined(USE_ACC100)
    {
      std::cout << "Since using ACC100, generate reference from ACC100"
                << std::endl;
      const std::string filename_input =
          directory + kUlLdpcACC100DataPrefix +
          std::to_string(this->cfg_->OfdmCaNum()) + "_ant" +
          std::to_string(this->cfg_->UeAntNum()) + ".bin";
      AGORA_LOG_INFO("Saving raw uplink data (using ACC100 LDPC) to %s\n",
                     filename_input.c_str());

      auto* fp_input = std::fopen(filename_input.c_str(), "wb");
      if (fp_input == nullptr) {
        AGORA_LOG_ERROR("Failed to create file %s\n", filename_input.c_str());
        throw std::runtime_error("Failed to create file" + filename_input);
      } else {
        for (size_t i = 0; i < num_ul_codeblocks; i++) {
          const auto write_status = std::fwrite(
              reinterpret_cast<uint8_t*>(&ul_information.at(i).at(0)),
              sizeof(uint8_t), ul_cb_bytes, fp_input);
          if (write_status != ul_cb_bytes) {
            AGORA_LOG_ERROR("Wrote %zu out of %zu to file %s\n", write_status,
                            ul_cb_bytes, filename_input.c_str());
            throw std::runtime_error("Failed to write to file" +
                                     filename_input);
          }
        }
        const auto close_status = std::fclose(fp_input);
        if (close_status != 0) {
          throw std::runtime_error("Failed to close file" + filename_input);
        }
      }
    }
#endif

    if (kOutputUlScData) {
      std::vector<std::vector<std::vector<std::vector<std::vector<uint8_t>>>>>
          ul_ofdm_data(
              this->cfg_->UeNum(),
              std::vector<std::vector<std::vector<std::vector<uint8_t>>>>(
                  kOutputFrameNum,
                  std::vector<std::vector<std::vector<uint8_t>>>(
                      this->cfg_->Frame().NumULSyms(),
                      std::vector<std::vector<uint8_t>>(
                          this->cfg_->NumUeChannels(),
                          std::vector<uint8_t>(this->cfg_->OfdmDataNum())))));
      for (size_t n = 0; n < num_ul_codeblocks; n++) {
        const size_t cl_sdr = (n % this->cfg_->UeNum());
        const size_t ul_slot = (n / this->cfg_->UeAntNum()) +
                               this->cfg_->Frame().ClientUlPilotSymbols();
        const size_t cl_sdr_ch =
            (n % this->cfg_->UeAntNum()) % this->cfg_->NumUeChannels();
        std::vector<uint8_t> odfm_symbol(this->cfg_->OfdmDataNum());
        AdaptBitsForMod(
            reinterpret_cast<const uint8_t*>(ul_encoded_codewords.at(n).data()),
            odfm_symbol.data(),
            this->cfg_->LdpcConfig(Direction::kUplink).NumEncodedBytes(),
            this->cfg_->ModOrderBits(Direction::kUplink));
        for (size_t f = 0; f < kOutputFrameNum; f++) {
          ul_ofdm_data.at(cl_sdr).at(f).at(ul_slot).at(cl_sdr_ch) = odfm_symbol;
        }
      }
      for (size_t i = 0; i < this->cfg_->UeNum(); i++) {
        const std::string filename_input =
            directory + kUlScBitsPrefix +
            this->cfg_->Modulation(Direction::kUplink) + "_" +
            std::to_string(this->cfg_->OfdmDataNum()) + "_" +
            std::to_string(this->cfg_->OfdmCaNum()) + "_" +
            std::to_string(kOfdmSymbolPerSlot) + "_" +
            std::to_string(this->cfg_->Frame().NumULSyms()) + "_" +
            std::to_string(kOutputFrameNum) + "_" + this->cfg_->UeChannel() +
            "_" + std::to_string(i) + ".bin";
        AGORA_LOG_INFO("Saving uplink sc bits to %s\n", filename_input.c_str());
        auto* fp_tx_b = std::fopen(filename_input.c_str(), "wb");
        if (fp_tx_b == nullptr) {
          throw std::runtime_error(
              "DataGenerator: Failed to create ul sc bits file");
        }
        for (size_t f = 0; f < kOutputFrameNum; f++) {
          for (size_t u = 0; u < this->cfg_->Frame().NumULSyms(); u++) {
            for (size_t h = 0; h < this->cfg_->NumUeChannels(); h++) {
              const auto write_status = std::fwrite(
                  ul_ofdm_data.at(i).at(f).at(u).at(h).data(), sizeof(uint8_t),
                  this->cfg_->OfdmDataNum(), fp_tx_b);
              if (write_status != this->cfg_->OfdmDataNum()) {
                throw std::runtime_error(
                    "DataGenerator: Failed to write ul sc bits file");
              }
            }
          }
        }
        const auto close_status = std::fclose(fp_tx_b);
        if (close_status != 0) {
          throw std::runtime_error(
              "DataGenerator: Failed to close ul sc bits file");
        }
      }
    }

    // Modulate the encoded codewords
    std::vector<std::vector<complex_float>> ul_modulated_codewords(
        num_ul_codeblocks);
    for (size_t i = 0; i < num_ul_codeblocks; i++) {
      auto ofdm_symbol = DataGenerator::GetModulation(
          &ul_encoded_codewords.at(i)[0], cfg_->ModTable(Direction::kUplink),
          cfg_->LdpcConfig(Direction::kUplink).NumCbCodewLen(),
          cfg_->OfdmDataNum(), cfg_->ModOrderBits(Direction::kUplink));
      ul_modulated_codewords.at(i) = DataGenerator::MapOFDMSymbol(
          cfg_, ofdm_symbol, nullptr, SymbolType::kUL);
    }

    // Place modulated uplink data codewords into central IFFT bins
    AGORA_LOG_INFO("ul_mod_order_bits: %zu\n",
                   cfg_->ModOrderBits(Direction::kUplink));
    AGORA_LOG_INFO("OfdmDataNum(): %zu\n", cfg_->OfdmDataNum());
    AGORA_LOG_INFO("NumBlocksInSymbol(): %zu\n",
                   ul_ldpc_config.NumBlocksInSymbol());
    AGORA_LOG_INFO("NumCbCodewLen(): %u\n", ul_ldpc_config.NumCbCodewLen());
    AGORA_LOG_INFO("NumBlocksInSymbol() input: %f\n",
                   (double(cfg_->OfdmDataNum()) *
                    double(cfg_->ModOrderBits(Direction::kUplink))) /
                       double(ul_ldpc_config.NumCbCodewLen()));
    RtAssert(ul_ldpc_config.NumBlocksInSymbol() == 1,  // TODO: Assumption
             "This version of Agora does not support code block partition");
    pre_ifft_data_syms.resize(this->cfg_->UeAntNum() *
                              this->cfg_->Frame().NumUlDataSyms());
    for (size_t i = 0; i < pre_ifft_data_syms.size(); i++) {
      pre_ifft_data_syms.at(i) = BinForIfft(cfg_, ul_modulated_codewords.at(i));
    }
  }

  // Generate common sounding pilots
  std::vector<complex_float> pilot_fd = this->GetCommonPilotFreqDomain();

  // Generate UE-specific pilots (phase tracking & downlink channel estimation)
  Table<complex_float> ue_specific_pilot = this->GetUeSpecificPilotFreqDomain();

  // Put pilot and data symbols together
  Table<complex_float> tx_data_all_symbols;
  tx_data_all_symbols.Calloc(this->cfg_->Frame().NumTotalSyms(),
                             this->cfg_->UeAntNum() * this->cfg_->OfdmCaNum(),
                             Agora_memory::Alignment_t::kAlign64);

  if (this->cfg_->FreqOrthogonalPilot()) {
    const size_t pilot_sym_idx = this->cfg_->Frame().GetPilotSymbol(0);
    RtAssert(this->cfg_->Frame().NumPilotSyms() == 1,
             "Number of pilot symbols must be 1");
    for (size_t i = 0; i < this->cfg_->UeAntNum(); i++) {
      std::vector<complex_float> pilots_f_ue(
          this->cfg_->OfdmCaNum());  // Zeroed
      size_t step_size =
          this->cfg_->GroupPilotSc() ? this->cfg_->PilotScGroupSize() : 1;
      for (size_t j = this->cfg_->OfdmDataStart();
           j < this->cfg_->OfdmDataStop(); j += step_size) {
        pilots_f_ue.at(i + j) = pilot_fd.at(i + j);
      }
      // Load pilots
      std::memcpy(
          tx_data_all_symbols[pilot_sym_idx] + (i * this->cfg_->OfdmCaNum()),
          &pilots_f_ue.at(0),
          (this->cfg_->OfdmCaNum() * sizeof(complex_float)));
    }
  } else {
    for (size_t i = 0; i < this->cfg_->UeAntNum(); i++) {
      const size_t pilot_sym_idx = this->cfg_->Frame().GetPilotSymbol(i);
      std::memcpy(
          tx_data_all_symbols[pilot_sym_idx] + i * this->cfg_->OfdmCaNum(),
          &pilot_fd.at(0), (this->cfg_->OfdmCaNum() * sizeof(complex_float)));
    }
  }

  // Populate the UL symbols
  for (size_t i = 0; i < this->cfg_->Frame().NumULSyms(); i++) {
    const size_t data_sym_id = this->cfg_->Frame().GetULSymbol(i);
    for (size_t j = 0; j < this->cfg_->UeAntNum(); j++) {
      if (i < this->cfg_->Frame().ClientUlPilotSymbols()) {
        std::memcpy(tx_data_all_symbols[data_sym_id] +
                        (j * this->cfg_->OfdmCaNum()) +
                        this->cfg_->OfdmDataStart(),
                    ue_specific_pilot[j],
                    this->cfg_->OfdmDataNum() * sizeof(complex_float));
      } else {
        const size_t k = i - this->cfg_->Frame().ClientUlPilotSymbols();
        std::memcpy(
            tx_data_all_symbols[data_sym_id] + (j * this->cfg_->OfdmCaNum()),
            &pre_ifft_data_syms.at(k * this->cfg_->UeAntNum() + j).at(0),
            this->cfg_->OfdmCaNum() * sizeof(complex_float));
      }
    }
  }

  // Generate CSI matrix
  Table<complex_float> csi_matrices;
  float sqrt2_norm = 1 / std::sqrt(2);
  csi_matrices.Calloc(this->cfg_->OfdmCaNum(),
                      this->cfg_->UeAntNum() * this->cfg_->BsAntNum(),
                      Agora_memory::Alignment_t::kAlign32);
  for (size_t i = 0; i < (this->cfg_->UeAntNum() * this->cfg_->BsAntNum());
       i++) {
    complex_float csi = {RandFloatFromShort(-1, 1), RandFloatFromShort(-1, 1)};
    for (size_t j = 0; j < this->cfg_->OfdmCaNum(); j++) {
      csi_matrices[j][i].re = csi.re * sqrt2_norm;
      csi_matrices[j][i].im = csi.im * sqrt2_norm;
    }
  }
  arma::arma_rng::set_seed_random();

  // Generate RX data received by base station after going through channels
  Table<complex_float> rx_data_all_symbols;
  rx_data_all_symbols.Calloc(
      this->cfg_->Frame().NumTotalSyms(),
      this->cfg_->SampsPerSymbol() * this->cfg_->BsAntNum(),
      Agora_memory::Alignment_t::kAlign64);
  size_t data_start = this->cfg_->CpLen() + this->cfg_->OfdmTxZeroPrefix();
  for (size_t i = 0; i < this->cfg_->Frame().NumTotalSyms(); i++) {
    arma::cx_fmat mat_input_data(
        reinterpret_cast<arma::cx_float*>(tx_data_all_symbols[i]),
        this->cfg_->OfdmCaNum(), this->cfg_->UeAntNum(), false);
    arma::cx_fmat mat_output(
        reinterpret_cast<arma::cx_float*>(rx_data_all_symbols[i]),
        this->cfg_->SampsPerSymbol(), this->cfg_->BsAntNum(), false);

    for (size_t j = 0; j < this->cfg_->OfdmCaNum(); j++) {
      arma::cx_fmat mat_csi(reinterpret_cast<arma::cx_float*>(csi_matrices[j]),
                            this->cfg_->BsAntNum(), this->cfg_->UeAntNum(),
                            false);
      mat_output.row(j + data_start) = mat_input_data.row(j) * mat_csi.st();
    }
    arma::cx_fmat noise_mat(size(mat_output));
    noise_mat.set_real(arma::randn<arma::fmat>(size(real(mat_output))));
    noise_mat.set_imag(arma::randn<arma::fmat>(size(real(mat_output))));
    mat_output += (noise_mat * this->cfg_->NoiseLevel() * sqrt2_norm);
    for (size_t j = 0; j < this->cfg_->BsAntNum(); j++) {
      auto* this_ofdm_symbol =
          rx_data_all_symbols[i] + j * this->cfg_->SampsPerSymbol() +
          this->cfg_->CpLen() + this->cfg_->OfdmTxZeroPrefix();
      CommsLib::FFTShift(this_ofdm_symbol, this->cfg_->OfdmCaNum());
      CommsLib::IFFT(this_ofdm_symbol, this->cfg_->OfdmCaNum(), false);
    }
  }

  const std::string filename_rx =
      directory + kRxLdpcPrefix + std::to_string(this->cfg_->OfdmCaNum()) +
      "_ant" + std::to_string(this->cfg_->BsAntNum()) + ".bin";
  AGORA_LOG_INFO("Saving rx data to %s\n", filename_rx.c_str());
  auto* fp_rx = std::fopen(filename_rx.c_str(), "wb");
  if (fp_rx == nullptr) {
    AGORA_LOG_ERROR("Failed to create file %s\n", filename_rx.c_str());
    throw std::runtime_error("Failed to create file" + filename_rx);
  } else {
    for (size_t i = 0; i < this->cfg_->Frame().NumTotalSyms(); i++) {
      const auto* ptr = reinterpret_cast<float*>(rx_data_all_symbols[i]);
      const auto write_status = std::fwrite(
          ptr, sizeof(float),
          this->cfg_->SampsPerSymbol() * this->cfg_->BsAntNum() * 2, fp_rx);
      if (write_status !=
          this->cfg_->SampsPerSymbol() * this->cfg_->BsAntNum() * 2) {
        AGORA_LOG_ERROR("Write %zu out of %zu to file %s\n", write_status,
                        num_ul_mac_bytes, filename_rx.c_str());
        throw std::runtime_error("Failed to write to file" + filename_rx);
      }
    }
    const auto close_status = std::fclose(fp_rx);
    if (close_status != 0) {
      throw std::runtime_error("Failed to close file" + filename_rx);
    }
  }

  if (kDebugPrintRxData) {
    std::printf("rx data\n");
    for (size_t i = 0; i < 10; i++) {
      for (size_t j = 0; j < this->cfg_->OfdmCaNum() * this->cfg_->BsAntNum();
           j++) {
        if (j % this->cfg_->OfdmCaNum() == 0) {
          std::printf("\nsymbol %zu ant %zu\n", i, j / this->cfg_->OfdmCaNum());
        }
        std::printf("%.4f+%.4fi ", rx_data_all_symbols[i][j].re,
                    rx_data_all_symbols[i][j].im);
      }
      std::printf("\n");
    }
  }

  /* ------------------------------------------------
   * Generate data for downlink test
   * ------------------------------------------------ */
  const LDPCconfig dl_ldpc_config =
      this->cfg_->LdpcConfig(Direction::kDownlink);
  const size_t dl_cb_bytes = cfg_->NumBytesPerCb(Direction::kDownlink);

  if (this->cfg_->Frame().NumDLSyms() > 0) {
    const size_t num_dl_mac_bytes =
        this->cfg_->MacBytesNumPerframe(Direction::kDownlink);
    std::vector<std::vector<int8_t>> dl_mac_info(cfg_->UeAntNum());
    AGORA_LOG_SYMBOL("Total number of downlink MAC bytes: %zu\n",
                     num_dl_mac_bytes);
    for (size_t ue_id = 0; ue_id < cfg_->UeAntNum(); ue_id++) {
      dl_mac_info[ue_id].resize(num_dl_mac_bytes);
      for (size_t pkt_id = 0;
           pkt_id < cfg_->MacPacketsPerframe(Direction::kDownlink); pkt_id++) {
        size_t pkt_offset =
            pkt_id * cfg_->MacPacketLength(Direction::kDownlink);
        auto* pkt = reinterpret_cast<MacPacketPacked*>(
            &dl_mac_info.at(ue_id).at(pkt_offset));

        pkt->Set(0, pkt_id, ue_id,
                 cfg_->MacPayloadMaxLength(Direction::kDownlink));
        this->GenMacData(pkt, ue_id);
        pkt->Crc((uint16_t)(
            crc_obj->CalculateCrc24(
                pkt->Data(), cfg_->MacPayloadMaxLength(Direction::kDownlink)) &
            0xFFFF));
      }
    }

    {
      const std::string filename_input =
          directory + kDlDataPrefix + std::to_string(this->cfg_->OfdmCaNum()) +
          "_ant" + std::to_string(this->cfg_->UeAntNum()) + ".bin";
      AGORA_LOG_INFO("Saving downlink MAC data to %s\n",
                     filename_input.c_str());
      FILE* fp_input = std::fopen(filename_input.c_str(), "wb");
      if (fp_input == nullptr) {
        AGORA_LOG_ERROR("Failed to create file %s\n", filename_input.c_str());
        throw std::runtime_error("Failed to create file" + filename_input);
      } else {
        for (size_t i = 0; i < cfg_->UeAntNum(); i++) {
          const auto write_status =
              std::fwrite(reinterpret_cast<uint8_t*>(dl_mac_info.at(i).data()),
                          sizeof(uint8_t), num_dl_mac_bytes, fp_input);
          if (write_status != num_dl_mac_bytes) {
            AGORA_LOG_ERROR("Wrote %zu out of %zu to file %s\n", write_status,
                            num_dl_mac_bytes, filename_input.c_str());
            throw std::runtime_error("Failed to write to file" +
                                     filename_input);
          }
        }
        const auto close_status = std::fclose(fp_input);
        if (close_status != 0) {
          throw std::runtime_error("Failed to close file" + filename_input);
        }
      }

      if (kPrintDownlinkInformationBytes) {
        std::printf("Downlink information bytes\n");
        for (size_t n = 0; n < cfg_->UeAntNum(); n++) {
          std::printf("UE %zu\n", n % this->cfg_->UeAntNum());
          for (size_t i = 0; i < num_dl_mac_bytes; i++) {
            std::printf("%u ", static_cast<uint8_t>(dl_mac_info.at(n).at(i)));
          }
          std::printf("\n");
        }
      }
    }

    const size_t symbol_blocks =
        dl_ldpc_config.NumBlocksInSymbol() * this->cfg_->UeAntNum();
    const size_t num_dl_codeblocks =
        this->cfg_->Frame().NumDlDataSyms() * symbol_blocks;
    AGORA_LOG_SYMBOL("Total number of dl data blocks: %zu\n",
                     num_dl_codeblocks);

    std::vector<std::vector<int8_t>> dl_information(num_dl_codeblocks);
    std::vector<std::vector<int8_t>> dl_encoded_codewords(num_dl_codeblocks);
    for (size_t cb = 0; cb < num_dl_codeblocks; cb++) {
      // i : symbol -> ue -> cb (repeat)
      const size_t sym_id = cb / (symbol_blocks);
      // ue antenna for code block
      const size_t sym_offset = cb % (symbol_blocks);
      const size_t ue_id = sym_offset / dl_ldpc_config.NumBlocksInSymbol();
      const size_t ue_cb_id = sym_offset % dl_ldpc_config.NumBlocksInSymbol();
      const size_t ue_cb_cnt =
          (sym_id * dl_ldpc_config.NumBlocksInSymbol()) + ue_cb_id;
      int8_t* cb_start = &dl_mac_info.at(ue_id).at(ue_cb_cnt * dl_cb_bytes);
      dl_information.at(cb) =
          std::vector<int8_t>(cb_start, cb_start + dl_cb_bytes);
      dl_encoded_codewords.at(cb) = DataGenerator::GenCodeblock(
          dl_ldpc_config, &dl_information.at(cb).at(0), dl_cb_bytes,
          this->cfg_->ScrambleEnabled());
    }

    // Modulate the encoded codewords
    std::vector<std::vector<complex_float>> dl_modulated_codewords(
        num_dl_codeblocks);
    for (size_t i = 0; i < num_dl_codeblocks; i++) {
      const size_t sym_offset = i % (symbol_blocks);
      const size_t ue_id = sym_offset / dl_ldpc_config.NumBlocksInSymbol();
      auto ofdm_symbol = DataGenerator::GetModulation(
          &dl_encoded_codewords.at(i)[0], cfg_->ModTable(Direction::kDownlink),
          cfg_->LdpcConfig(Direction::kDownlink).NumCbCodewLen(),
          cfg_->OfdmDataNum(), cfg_->ModOrderBits(Direction::kDownlink));
      dl_modulated_codewords.at(i) = DataGenerator::MapOFDMSymbol(
          cfg_, ofdm_symbol, ue_specific_pilot[ue_id], SymbolType::kDL);
    }

    {
      // Save downlink information bytes to file
      const std::string filename_input =
          directory + kDlLdpcDataPrefix +
          std::to_string(this->cfg_->OfdmCaNum()) + "_ant" +
          std::to_string(this->cfg_->UeAntNum()) + ".bin";
      AGORA_LOG_INFO("Saving raw dl data (using LDPC) to %s\n",
                     filename_input.c_str());
      auto* fp_input = std::fopen(filename_input.c_str(), "wb");
      if (fp_input == nullptr) {
        AGORA_LOG_ERROR("Failed to create file %s\n", filename_input.c_str());
        throw std::runtime_error("Failed to create file" + filename_input);
      } else {
        for (size_t i = 0; i < num_dl_codeblocks; i++) {
          const auto write_status = std::fwrite(
              reinterpret_cast<uint8_t*>(&dl_information.at(i).at(0)),
              sizeof(uint8_t), dl_cb_bytes, fp_input);
          if (write_status != dl_cb_bytes) {
            AGORA_LOG_ERROR("Wrote %zu out of %zu to file %s\n", write_status,
                            dl_cb_bytes, filename_input.c_str());
            throw std::runtime_error("Failed to write to file" +
                                     filename_input);
          }
        }
        const auto close_status = std::fclose(fp_input);
        if (close_status != 0) {
          throw std::runtime_error("Failed to close file" + filename_input);
        }
      }

      if (kPrintDownlinkInformationBytes == true) {
        std::printf("Downlink information bytes\n");
        for (size_t n = 0; n < num_dl_codeblocks; n++) {
          std::printf("Symbol %zu, UE %zu\n", n / this->cfg_->UeAntNum(),
                      n % this->cfg_->UeAntNum());
          for (size_t i = 0; i < dl_cb_bytes; i++) {
            std::printf("%u ",
                        static_cast<unsigned>(dl_information.at(n).at(i)));
          }
          std::printf("\n");
        }
      }
    }

    // Compute precoder
    Table<complex_float> precoder;
    precoder.Calloc(this->cfg_->OfdmCaNum(),
                    this->cfg_->UeAntNum() * this->cfg_->BsAntNum(),
                    Agora_memory::Alignment_t::kAlign32);
    for (size_t i = 0; i < this->cfg_->OfdmCaNum(); i++) {
      arma::cx_fmat mat_input(
          reinterpret_cast<arma::cx_float*>(csi_matrices[i]),
          this->cfg_->BsAntNum(), this->cfg_->UeAntNum(), false);
      arma::cx_fmat mat_output(reinterpret_cast<arma::cx_float*>(precoder[i]),
                               this->cfg_->UeAntNum(), this->cfg_->BsAntNum(),
                               false);
      pinv(mat_output, mat_input, 1e-2, "dc");
    }

    if (kPrintDebugCSI) {
      std::printf("CSI \n");
      // for (size_t i = 0; i < this->cfg_->ofdm_ca_num(); i++)
      for (size_t j = 0; j < this->cfg_->UeAntNum() * this->cfg_->BsAntNum();
           j++) {
        std::printf("%.3f+%.3fi ",
                    csi_matrices[this->cfg_->OfdmDataStart()][j].re,
                    csi_matrices[this->cfg_->OfdmDataStart()][j].im);
      }
      std::printf("\nprecoder \n");
      // for (size_t i = 0; i < this->cfg_->ofdm_ca_num(); i++)
      for (size_t j = 0; j < this->cfg_->UeAntNum() * this->cfg_->BsAntNum();
           j++) {
        std::printf("%.3f+%.3fi ", precoder[this->cfg_->OfdmDataStart()][j].re,
                    precoder[this->cfg_->OfdmDataStart()][j].im);
      }
      std::printf("\n");
    }

    // Prepare downlink data from mod_output
    Table<complex_float> dl_mod_data;
    dl_mod_data.Calloc(this->cfg_->Frame().NumDLSyms(),
                       this->cfg_->OfdmCaNum() * this->cfg_->UeAntNum(),
                       Agora_memory::Alignment_t::kAlign64);
    for (size_t i = 0; i < this->cfg_->Frame().NumDLSyms(); i++) {
      for (size_t j = 0; j < this->cfg_->UeAntNum(); j++) {
        for (size_t sc_id = 0; sc_id < this->cfg_->OfdmDataNum(); sc_id++) {
          complex_float sc_data;
          if ((i < this->cfg_->Frame().ClientDlPilotSymbols()) ||
              (sc_id % this->cfg_->OfdmPilotSpacing() == 0)) {
            sc_data = ue_specific_pilot[j][sc_id];
          } else {
            sc_data =
                dl_modulated_codewords
                    .at(((i - this->cfg_->Frame().ClientDlPilotSymbols()) *
                         this->cfg_->UeAntNum()) +
                        j)
                    .at(sc_id);
          }
          dl_mod_data[i][j * this->cfg_->OfdmCaNum() + sc_id +
                         this->cfg_->OfdmDataStart()] = sc_data;
        }
      }
    }

    if (kPrintDlModData) {
      std::printf("dl mod data \n");
      for (size_t i = 0; i < this->cfg_->Frame().NumDLSyms(); i++) {
        for (size_t k = this->cfg_->OfdmDataStart();
             k < this->cfg_->OfdmDataStart() + this->cfg_->OfdmDataNum(); k++) {
          std::printf("symbol %zu, subcarrier %zu\n", i, k);
          for (size_t j = 0; j < this->cfg_->UeAntNum(); j++) {
            // for (int k = this->cfg_->OfdmDataStart(); k <
            // this->cfg_->OfdmDataStart() + this->cfg_->OfdmDataNum();
            //      k++) {
            std::printf("%.3f+%.3fi ",
                        dl_mod_data[i][j * this->cfg_->OfdmCaNum() + k].re,
                        dl_mod_data[i][j * this->cfg_->OfdmCaNum() + k].im);
          }
          std::printf("\n");
        }
      }
    }

    // Perform precoding and IFFT
    Table<complex_float> dl_ifft_data;
    dl_ifft_data.Calloc(this->cfg_->Frame().NumDLSyms(),
                        this->cfg_->OfdmCaNum() * this->cfg_->BsAntNum(),
                        Agora_memory::Alignment_t::kAlign64);
    Table<short> dl_tx_data;
    dl_tx_data.Calloc(this->cfg_->Frame().NumDLSyms(),
                      2 * this->cfg_->SampsPerSymbol() * this->cfg_->BsAntNum(),
                      Agora_memory::Alignment_t::kAlign64);

    for (size_t i = 0; i < this->cfg_->Frame().NumDLSyms(); i++) {
      arma::cx_fmat mat_input_data(
          reinterpret_cast<arma::cx_float*>(dl_mod_data[i]),
          this->cfg_->OfdmCaNum(), this->cfg_->UeAntNum(), false);

      arma::cx_fmat mat_output(
          reinterpret_cast<arma::cx_float*>(dl_ifft_data[i]),
          this->cfg_->OfdmCaNum(), this->cfg_->BsAntNum(), false);

      for (size_t j = this->cfg_->OfdmDataStart();
           j < this->cfg_->OfdmDataNum() + this->cfg_->OfdmDataStart(); j++) {
        arma::cx_fmat mat_precoder(
            reinterpret_cast<arma::cx_float*>(precoder[j]),
            this->cfg_->UeAntNum(), this->cfg_->BsAntNum(), false);
        mat_precoder /= abs(mat_precoder).max();
        mat_output.row(j) = mat_input_data.row(j) * mat_precoder;

        // std::printf("symbol %d, sc: %d\n", i, j -
        // this->cfg_->ofdm_data_start()); cout << "Precoder: \n" <<
        // mat_precoder
        // << endl; cout << "Data: \n" << mat_input_data.row(j) << endl; cout <<
        // "Precoded data: \n" << mat_output.row(j) << endl;
      }
      for (size_t j = 0; j < this->cfg_->BsAntNum(); j++) {
        complex_float* ptr_ifft = dl_ifft_data[i] + j * this->cfg_->OfdmCaNum();
        CommsLib::FFTShift(ptr_ifft, this->cfg_->OfdmCaNum());
        CommsLib::IFFT(ptr_ifft, this->cfg_->OfdmCaNum(), false);

        short* tx_symbol = dl_tx_data[i] + j * this->cfg_->SampsPerSymbol() * 2;
        std::memset(tx_symbol, 0,
                    sizeof(short) * 2 * this->cfg_->OfdmTxZeroPrefix());
        for (size_t k = 0; k < this->cfg_->OfdmCaNum(); k++) {
          tx_symbol[2 * (k + this->cfg_->CpLen() +
                         this->cfg_->OfdmTxZeroPrefix())] =
              static_cast<short>(kShrtFltConvFactor * ptr_ifft[k].re);
          tx_symbol[2 * (k + this->cfg_->CpLen() +
                         this->cfg_->OfdmTxZeroPrefix()) +
                    1] =
              static_cast<short>(kShrtFltConvFactor * ptr_ifft[k].im);
        }
        for (size_t k = 0; k < (2 * this->cfg_->CpLen()); k++) {
          tx_symbol[2 * this->cfg_->OfdmTxZeroPrefix() + k] =
              tx_symbol[2 * (this->cfg_->OfdmTxZeroPrefix() +
                             this->cfg_->OfdmCaNum()) +
                        k];
        }

        const size_t tx_zero_postfix_offset =
            2 * (this->cfg_->OfdmTxZeroPrefix() + this->cfg_->CpLen() +
                 this->cfg_->OfdmCaNum());
        std::memset(tx_symbol + tx_zero_postfix_offset, 0,
                    sizeof(short) * 2 * this->cfg_->OfdmTxZeroPostfix());
      }
    }

    std::string filename_dl_tx =
        directory + kDlTxPrefix + std::to_string(this->cfg_->OfdmCaNum()) +
        "_ant" + std::to_string(this->cfg_->BsAntNum()) + ".bin";
    AGORA_LOG_INFO("Saving dl tx data to %s\n", filename_dl_tx.c_str());
    auto* fp_dl_tx = std::fopen(filename_dl_tx.c_str(), "wb");
    if (fp_dl_tx == nullptr) {
      AGORA_LOG_ERROR("Failed to create file %s\n", filename_dl_tx.c_str());
      throw std::runtime_error("Failed to create file" + filename_dl_tx);
    } else {
      for (size_t i = 0; i < this->cfg_->Frame().NumDLSyms(); i++) {
        const short* ptr = dl_tx_data[i];
        const auto write_status = std::fwrite(
            ptr, sizeof(short),
            this->cfg_->SampsPerSymbol() * this->cfg_->BsAntNum() * 2,
            fp_dl_tx);
        if (write_status !=
            this->cfg_->SampsPerSymbol() * this->cfg_->BsAntNum() * 2) {
          AGORA_LOG_ERROR(
              "Wrote %zu out of %zu to file %s\n", write_status,
              this->cfg_->SampsPerSymbol() * this->cfg_->BsAntNum() * 2,
              filename_dl_tx.c_str());
          throw std::runtime_error("Failed to write to file" + filename_dl_tx);
        }
      }
      const auto close_status = std::fclose(fp_dl_tx);
      if (close_status != 0) {
        throw std::runtime_error("Failed to close file" + filename_dl_tx);
      }
    }

    if (kPrintDlTxData) {
      std::printf("rx data\n");
      for (size_t i = 0; i < 10; i++) {
        for (size_t j = 0; j < this->cfg_->OfdmCaNum() * this->cfg_->BsAntNum();
             j++) {
          if (j % this->cfg_->OfdmCaNum() == 0) {
            std::printf("symbol %zu ant %zu\n", i, j / this->cfg_->OfdmCaNum());
          }
          // TODO keep and fix or remove
          // std::printf("%d+%di ", dl_tx_data[i][j], dl_tx_data[i][j]);
        }
      }
      std::printf("\n");
    }

    /* Clean Up memory */
    dl_ifft_data.Free();
    dl_tx_data.Free();
    dl_mod_data.Free();
    precoder.Free();
  }

  csi_matrices.Free();
  tx_data_all_symbols.Free();
  rx_data_all_symbols.Free();
  ue_specific_pilot.Free();
}

/**
   * @brief                        Generate random Mac payload bit
   * sequence
   *
   * @param  information           The generated input bit sequence
   * @param  ue_id                 ID of the UE that this codeblock belongs to
   */
void DataGenerator::GenMacData(MacPacketPacked* mac, size_t ue_id) {
  for (size_t i = 0; i < mac->PayloadLength(); i++) {
    if (profile_ == Profile::kRandom) {
      mac->DataPtr()[i] = static_cast<int8_t>(fast_rand_.NextU32());
    } else if (profile_ == Profile::kProfile123) {
      mac->DataPtr()[i] = 1 + (ue_id * 3) + (i % 3);
    }
  }
}

/**
   * @brief                        Generate one raw information bit sequence
   *
   * @param  information           The generated input bit sequence
   * @param  ue_id                 ID of the UE that this codeblock belongs to
   */
void DataGenerator::GenRawData(const LDPCconfig& lc,
                               std::vector<int8_t>& information, size_t ue_id) {
  // const LDPCconfig& lc = cfg_->LdpcConfig(dir);
  information.resize(
      LdpcEncodingInputBufSize(lc.BaseGraph(), lc.ExpansionFactor()));

  for (size_t i = 0; i < lc.NumInputBytes(); i++) {
    if (profile_ == Profile::kRandom) {
      information.at(i) = static_cast<int8_t>(fast_rand_.NextU32());
    } else if (profile_ == Profile::kProfile123) {
      information.at(i) = 1 + (ue_id * 3) + (i % 3);
    }
  }
}

/// Return the frequency-domain pilot symbol with OfdmCaNum complex floats
std::vector<complex_float> DataGenerator::GetCommonPilotFreqDomain() const {
  const std::vector<std::complex<float>> zc_seq = Utils::DoubleToCfloat(
      CommsLib::GetSequence(cfg_->OfdmDataNum(), CommsLib::kLteZadoffChu));

  const std::vector<std::complex<float>> zc_common_pilot =
      CommsLib::SeqCyclicShift(zc_seq, M_PI / 4.0);  // Used in LTE SRS

  std::vector<complex_float> ret(cfg_->OfdmCaNum());  // Zeroed
  for (size_t i = 0; i < cfg_->OfdmDataNum(); i++) {
    ret[i + cfg_->OfdmDataStart()] = {zc_common_pilot[i].real(),
                                      zc_common_pilot[i].imag()};
  }

  return ret;
}

/// Return the user-spepcific frequency-domain pilot symbol with OfdmCaNum complex floats
Table<complex_float> DataGenerator::GetUeSpecificPilotFreqDomain() const {
  Table<complex_float> ue_specific_pilot;
  const std::vector<std::complex<float>> zc_seq =
      Utils::DoubleToCfloat(CommsLib::GetSequence(this->cfg_->OfdmDataNum(),
                                                  CommsLib::kLteZadoffChu));
  const std::vector<std::complex<float>> zc_common_pilot =
      CommsLib::SeqCyclicShift(zc_seq, M_PI / 4.0);  // Used in LTE SRS
  ue_specific_pilot.Malloc(this->cfg_->UeAntNum(), this->cfg_->OfdmDataNum(),
                           Agora_memory::Alignment_t::kAlign64);
  for (size_t i = 0; i < this->cfg_->UeAntNum(); i++) {
    auto zc_ue_pilot_i =
        CommsLib::SeqCyclicShift(zc_seq, i * M_PI / 6.0);  // LTE DMRS
    for (size_t j = 0; j < this->cfg_->OfdmDataNum(); j++) {
      ue_specific_pilot[i][j] = {zc_ue_pilot_i[j].real(),
                                 zc_ue_pilot_i[j].imag()};
    }
  }
  return ue_specific_pilot;
}

void DataGenerator::GetNoisySymbol(
    const std::vector<complex_float>& modulated_symbol,
    std::vector<complex_float>& noisy_symbol, float noise_level) {
  std::default_random_engine generator(seed_);
  std::normal_distribution<double> distribution(0.0, 1.0);
  for (size_t j = 0; j < modulated_symbol.size(); j++) {
    complex_float noise = {
        static_cast<float>(distribution(generator)) * noise_level,
        static_cast<float>(distribution(generator)) * noise_level};
    noisy_symbol.at(j).re = modulated_symbol.at(j).re + noise.re;
    noisy_symbol.at(j).im = modulated_symbol.at(j).im + noise.im;
  }
}

void DataGenerator::GetNoisySymbol(const complex_float* modulated_symbol,
                                   complex_float* noisy_symbol, size_t length,
                                   float noise_level) {
  std::default_random_engine generator(seed_);
  std::normal_distribution<double> distribution(0.0, 1.0);
  for (size_t j = 0; j < length; j++) {
    complex_float noise = {
        static_cast<float>(distribution(generator)) * noise_level,
        static_cast<float>(distribution(generator)) * noise_level};
    noisy_symbol[j].re = modulated_symbol[j].re + noise.re;
    noisy_symbol[j].im = modulated_symbol[j].im + noise.im;
  }
}

/**
   * @brief                        Generate the encoded bit sequence for one
   * code block for the active LDPC configuration from the input bit sequence
   *
   * @param  input_ptr             The input bit sequence to be encoded
   * @param  encoded_codeword      The generated encoded codeword bit sequence
   */
std::vector<int8_t> DataGenerator::GenCodeblock(const LDPCconfig& lc,
                                                const int8_t* input_ptr,
                                                size_t input_size,
                                                bool scramble_enabled) {
  std::vector<int8_t> scramble_buffer(input_ptr, input_ptr + input_size);
  if (scramble_enabled) {
    auto scrambler = std::make_unique<AgoraScrambler::Scrambler>();
    scrambler->Scramble(scramble_buffer.data(), input_size);
  }

  std::vector<int8_t> parity;
  parity.resize(
      LdpcEncodingParityBufSize(lc.BaseGraph(), lc.ExpansionFactor()));

  const size_t encoded_bytes = BitsToBytes(lc.NumCbCodewLen());
  std::vector<int8_t> encoded_codeword(encoded_bytes, 0);

  LdpcEncodeHelper(lc.BaseGraph(), lc.ExpansionFactor(), lc.NumRows(),
                   &encoded_codeword.at(0), &parity.at(0),
                   reinterpret_cast<int8_t*>(scramble_buffer.data()));
  return encoded_codeword;
}

#if defined(USE_ACC100_ENCODE)
/**
   * @brief                        Generate the encoded bit sequence for one
   * code block for the active LDPC configuration from the input bit sequence
   *
   * @param  input_ptr             The input bit sequence to be encoded
   * @param  encoded_codeword      The generated encoded codeword bit sequence
   */
std::vector<int8_t> DataGenerator::GenCodeblock_ACC100(const LDPCconfig& lc,
                                                const int8_t* input_ptr,
                                                size_t input_size,
                                                bool scramble_enabled, size_t enq_index) {

  struct rte_mbuf *m_head;
  struct rte_mbuf *m_head_out;

  std::vector<int8_t> scramble_buffer(input_ptr, input_ptr + input_size);
  if (scramble_enabled) {
    auto scrambler = std::make_unique<AgoraScrambler::Scrambler>();
    scrambler->Scramble(scramble_buffer.data(), input_size);
  }

  std::vector<int8_t> parity;
  parity.resize(
      LdpcEncodingParityBufSize(lc.BaseGraph(), lc.ExpansionFactor()));

  const size_t encoded_bytes = BitsToBytes(lc.NumCbCodewLen());
  std::vector<int8_t> encoded_codeword(encoded_bytes, 0);

  char *data;
  struct rte_bbdev_op_data *bufs = *inputs;

  m_head = rte_pktmbuf_alloc(in_mbuf_pool);

  bufs[0].data = m_head;
  bufs[0].offset = 0;
  bufs[0].length = 0;

  data = rte_pktmbuf_append(m_head, input_size);
  rte_memcpy(data, scramble_buffer.data(), input_size);
  bufs[0].length += input_size;

  rte_bbdev_op_data *bufs_out = *hard_outputs;
  m_head_out = rte_pktmbuf_alloc(out_mbuf_pool);

  bufs_out[0].data = m_head_out;
  bufs_out[0].offset = 0;
  bufs_out[0].length = 0;

  char *data_out = rte_pktmbuf_append(m_head_out, encoded_bytes);
  rte_memcpy(data_out, encoded_codeword.data(), encoded_bytes);
  bufs_out[0].length += encoded_bytes;

  ref_enc_op[enq_index]->ldpc_enc.input = *inputs[0];
  ref_enc_op[enq_index]->ldpc_enc.output = *hard_outputs[0];

  rte_pktmbuf_free(m_head);
  rte_pktmbuf_free(m_head_out);

  enq += rte_bbdev_enqueue_ldpc_enc_ops(0, 0, &ref_enc_op[enq], 1);
  deq += rte_bbdev_dequeue_ldpc_enc_ops(0, 0, &ops_deq[deq], enq - deq);

  int retry_count = 0;

  while (deq < enq && retry_count < MAX_DEQUEUE_TRIAL) {
    deq += rte_bbdev_dequeue_ldpc_enc_ops(0, 0, &ops_deq[deq], enq - deq);
  }

  struct rte_bbdev_op_ldpc_enc *ops_td;
  struct rte_bbdev_op_data *hard_output;
  struct rte_mbuf *temp_m;
  size_t offset = 0;

  ops_td = &ops_deq[enq_index]->ldpc_enc;
  hard_output = &ops_td->output;
  temp_m = hard_output->data;
  uint8_t* temp_data = rte_pktmbuf_mtod(temp_m, uint8_t*);

  if (kPrintUplinkInformationBytes){
    std::cout << "CB size = " << encoded_bytes << " bytes\n";
    std::cout << "Content of the CB (in uint32):\n";
    print_casted_uint8_hex_1(temp_data, encoded_bytes);
    // print_uint32(temp_data, num_bytes_per_cb);
    std::cout << std::endl << std::endl;
  }

  encoded_codeword.assign(temp_data, temp_data + encoded_bytes);
    // LdpcEncodeHelper(lc.BaseGraph(), lc.ExpansionFactor(), lc.NumRows(),
  //                  &encoded_codeword.at(0), &parity.at(0),
  //    
  return encoded_codeword;
}
#endif

/**
   * @brief Return the output of modulating the encoded codeword
   * @param encoded_codeword The encoded LDPC codeword bit sequence
   * @return An array of complex floats with OfdmDataNum() elements
   */
std::vector<complex_float> DataGenerator::GetModulation(
    const int8_t* encoded_codeword, Table<complex_float> mod_table,
    const size_t num_bits, const size_t num_subcarriers,
    const size_t mod_order_bits) {
  std::vector<complex_float> modulated_codeword(num_subcarriers);
  std::vector<uint8_t> mod_input(num_subcarriers, 0);

  AdaptBitsForMod(reinterpret_cast<const uint8_t*>(&encoded_codeword[0]),
                  &mod_input[0], BitsToBytes(num_bits), mod_order_bits);

  for (size_t i = 0; i < num_subcarriers; i++) {
    modulated_codeword[i] = ModSingleUint8(mod_input[i], mod_table);
  }
  return modulated_codeword;
}

std::vector<complex_float> DataGenerator::MapOFDMSymbol(
    Config* cfg, const std::vector<complex_float>& modulated_codeword,
    complex_float* pilot_seq, SymbolType symbol_type) {
  std::vector<complex_float> ofdm_symbol(cfg->OfdmDataNum(), {0, 0});  // Zeroed
  for (size_t i = 0; i < cfg->OfdmDataNum(); i++) {
    if (symbol_type == SymbolType::kUL) {
      if (i < modulated_codeword.size()) {
        ofdm_symbol.at(i) = modulated_codeword.at(i);
      }
    } else if (symbol_type == SymbolType::kDL) {
      if (cfg->IsDataSubcarrier(i) == true) {
        size_t data_idx = cfg->GetOFDMDataIndex(i);
        if (data_idx < modulated_codeword.size()) {
          ofdm_symbol.at(i) = modulated_codeword.at(data_idx);
        }
      } else {
        ofdm_symbol.at(i) = pilot_seq[i];
      }
    } else if (symbol_type == SymbolType::kControl) {
      if (cfg->IsControlSubcarrier(i) == true) {
        size_t ctrl_idx = cfg->GetOFDMCtrlIndex(i);
        if (ctrl_idx < modulated_codeword.size()) {
          ofdm_symbol.at(i) = modulated_codeword.at(ctrl_idx);
        }
      } else {
        ofdm_symbol.at(i) = pilot_seq[i];
      }
    }
  }
  return ofdm_symbol;
}

/**
   * @param modulated_codeword The modulated codeword with OfdmDataNum()
   * elements
   * @brief An array with OfdmDataNum() elements with the OfdmDataNum()
   * modulated elements binned at the center
   */
std::vector<complex_float> DataGenerator::BinForIfft(
    Config* cfg, const std::vector<complex_float>& modulated_codeword,
    bool is_fftshifted) {
  std::vector<complex_float> pre_ifft_symbol(cfg->OfdmCaNum());  // Zeroed
  std::memcpy(&pre_ifft_symbol[cfg->OfdmDataStart()], &modulated_codeword[0],
              cfg->OfdmDataNum() * sizeof(complex_float));

  return is_fftshifted ? CommsLib::FFTShift(pre_ifft_symbol) : pre_ifft_symbol;
}

void DataGenerator::GetNoisySymbol(complex_float* modulated_symbol,
                                   size_t length, float noise_level,
                                   unsigned seed) {
  std::default_random_engine generator(seed);
  std::normal_distribution<double> distribution(0.0, 1.0);
  for (size_t j = 0; j < length; j++) {
    complex_float noise = {
        static_cast<float>(distribution(generator)) * noise_level,
        static_cast<float>(distribution(generator)) * noise_level};
    modulated_symbol[j].re += noise.re;
    modulated_symbol[j].im += noise.im;
  }
}

void DataGenerator::GetDecodedData(int8_t* demoded_data,
                                   uint8_t* decoded_codewords,
                                   const LDPCconfig& ldpc_config,
                                   size_t num_decoded_bytes,
                                   bool scramble_enabled) {
  struct bblib_ldpc_decoder_5gnr_request ldpc_decoder_5gnr_request {};
  struct bblib_ldpc_decoder_5gnr_response ldpc_decoder_5gnr_response {};

  // Decoder setup
  ldpc_decoder_5gnr_request.numChannelLlrs = ldpc_config.NumCbCodewLen();
  ldpc_decoder_5gnr_request.numFillerBits = 0;
  ldpc_decoder_5gnr_request.maxIterations = ldpc_config.MaxDecoderIter();
  ldpc_decoder_5gnr_request.enableEarlyTermination =
      ldpc_config.EarlyTermination();
  ldpc_decoder_5gnr_request.Zc = ldpc_config.ExpansionFactor();
  ldpc_decoder_5gnr_request.baseGraph = ldpc_config.BaseGraph();
  ldpc_decoder_5gnr_request.nRows = ldpc_config.NumRows();
  ldpc_decoder_5gnr_response.numMsgBits = ldpc_config.NumCbLen();
  auto* resp_var_nodes = static_cast<int16_t*>(Agora_memory::PaddedAlignedAlloc(
      Agora_memory::Alignment_t::kAlign64, 1024 * 1024 * sizeof(int16_t)));
  ldpc_decoder_5gnr_response.varNodes = resp_var_nodes;

  ldpc_decoder_5gnr_request.varNodes = demoded_data;
  ldpc_decoder_5gnr_response.compactedMessageBytes = decoded_codewords;
  bblib_ldpc_decoder_5gnr(&ldpc_decoder_5gnr_request,
                          &ldpc_decoder_5gnr_response);
  if (scramble_enabled) {
    auto scrambler = std::make_unique<AgoraScrambler::Scrambler>();
    scrambler->Descramble(decoded_codewords, num_decoded_bytes);
  }
  std::free(resp_var_nodes);
}

void DataGenerator::GetDecodedDataBatch(Table<int8_t>& demoded_data,
                                        Table<uint8_t>& decoded_codewords,
                                        const LDPCconfig& ldpc_config,
                                        size_t num_codeblocks,
                                        size_t num_decoded_bytes,
                                        bool scramble_enabled) {
  struct bblib_ldpc_decoder_5gnr_request ldpc_decoder_5gnr_request {};
  struct bblib_ldpc_decoder_5gnr_response ldpc_decoder_5gnr_response {};

  // Decoder setup
  ldpc_decoder_5gnr_request.numChannelLlrs = ldpc_config.NumCbCodewLen();
  ldpc_decoder_5gnr_request.numFillerBits = 0;
  ldpc_decoder_5gnr_request.maxIterations = ldpc_config.MaxDecoderIter();
  ldpc_decoder_5gnr_request.enableEarlyTermination =
      ldpc_config.EarlyTermination();
  ldpc_decoder_5gnr_request.Zc = ldpc_config.ExpansionFactor();
  ldpc_decoder_5gnr_request.baseGraph = ldpc_config.BaseGraph();
  ldpc_decoder_5gnr_request.nRows = ldpc_config.NumRows();
  ldpc_decoder_5gnr_response.numMsgBits = ldpc_config.NumCbLen();
  auto* resp_var_nodes = static_cast<int16_t*>(Agora_memory::PaddedAlignedAlloc(
      Agora_memory::Alignment_t::kAlign64, 1024 * 1024 * sizeof(int16_t)));
  ldpc_decoder_5gnr_response.varNodes = resp_var_nodes;

  for (size_t i = 0; i < num_codeblocks; i++) {
    ldpc_decoder_5gnr_request.varNodes = demoded_data[i];
    ldpc_decoder_5gnr_response.compactedMessageBytes = decoded_codewords[i];
    bblib_ldpc_decoder_5gnr(&ldpc_decoder_5gnr_request,
                            &ldpc_decoder_5gnr_response);
    if (scramble_enabled) {
      auto scrambler = std::make_unique<AgoraScrambler::Scrambler>();
      scrambler->Descramble(decoded_codewords[i], num_decoded_bytes);
    }
  }
  std::free(resp_var_nodes);
}
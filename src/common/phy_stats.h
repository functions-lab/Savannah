/**
 * @file phy_stats.h
 * @brief Declaration file for the PhyStats class.
 */
#ifndef PHY_STATS_H_
#define PHY_STATS_H_

#include "armadillo"
#include "common_typedef_sdk.h"
#include "config.h"
#include "mat_logger.h"
#include "memory_manage.h"
#include "symbols.h"

class PhyStats {
 public:
  explicit PhyStats(Config* const cfg, Direction dir);
  ~PhyStats();
  void PrintPhyStats();
  void PrintEvmStats(size_t frame_id, const arma::uvec& ue_list);
  void UpdateBitErrors(size_t ue_id, size_t offset, size_t frame_slot,
                       uint8_t tx_byte, uint8_t rx_byte);
  void UpdateDecodedBits(size_t ue_id, size_t offset, size_t frame_slot,
                         size_t new_bits_num);
  void UpdateBlockErrors(size_t ue_id, size_t offset, size_t frame_slot,
                         size_t block_error_count);
  void IncrementDecodedBlocks(size_t ue_id, size_t offset, size_t frame_slot);
  void UpdateUncodedBitErrors(size_t ue_id, size_t offset, size_t mod_bit_size,
                              uint8_t tx_byte, uint8_t rx_byte);
  void UpdateUncodedBits(size_t ue_id, size_t offset, size_t new_bits_num);
  void UpdateEvm(size_t frame_id, size_t data_symbol_id, size_t sc_id,
                 const arma::cx_fvec& eq_vec, const arma::uvec& ue_list);
  void UpdateEvm(size_t frame_id, size_t data_symbol_id, size_t sc_id,
                 size_t tx_ue_id, size_t rx_ue_id, arma::cx_float eq);
  void RecordEvmSnr(size_t frame_id, const arma::uvec& ue_map);
  void RecordDlPilotSnr(size_t frame_id, const arma::uvec& ue_map);
  void RecordDlCsi(size_t frame_id, size_t num_rec_sc,
                   const Table<complex_float>& csi_buffer,
                   const arma::uvec& ue_list);
  void RecordBer(size_t frame_id, const arma::uvec& ue_map);
  void RecordSer(size_t frame_id, const arma::uvec& ue_map);
  void RecordCsiCond(size_t frame_id, size_t num_rec_sc);
  void RecordEvm(size_t frame_id, size_t num_rec_sc, const arma::uvec& ue_map);
  float GetEvmSnr(size_t frame_id, size_t ue_id);
  float GetNoise(size_t frame_id, const arma::uvec& ue_list);
  void ClearEvmBuffer(size_t frame_id);
  void UpdatePilotSnr(size_t frame_id, size_t ue_id, size_t ant_id,
                      complex_float* fft_data);
  void UpdateDlPilotSnr(size_t frame_id, size_t symbol_id, size_t ant_id,
                        complex_float* fft_data);
  void PrintUlSnrStats(size_t frame_id);
  void PrintDlSnrStats(size_t frame_id, const arma::uvec& ue_list);
  void PrintDlSnrStats(size_t frame_id);
  void RecordPilotSnr(size_t frame_id);
  void UpdateCalibPilotSnr(size_t frame_id, size_t calib_sym_id, size_t ant_id,
                           complex_float* fft_data);
  void PrintCalibSnrStats(size_t frame_id);
  void UpdateCsiCond(size_t frame_id, size_t sc_id, float cond);
  void PrintBeamStats(size_t frame_id);
  void UpdateUlCsi(size_t frame_id, size_t sc_id, const arma::cx_fmat& mat_in);
  void UpdateDlCsi(size_t frame_id, size_t sc_id, const arma::cx_fmat& mat_in);
  void UpdateUlBeam(size_t frame_id, size_t sc_id, const arma::cx_fmat& mat_in);
  void UpdateDlBeam(size_t frame_id, size_t sc_id, const arma::cx_fmat& mat_in);
  void UpdateCalibMat(size_t frame_id, size_t sc_id,
                      const arma::cx_fvec& vec_in);

 private:
  Config const* const config_;
  Direction dir_;
  Table<size_t> decoded_bits_count_;
  Table<size_t> bit_error_count_;
  Table<size_t> frame_decoded_bits_;
  Table<size_t> frame_bit_errors_;
  Table<size_t> decoded_blocks_count_;
  Table<size_t> block_error_count_;
  Table<size_t> frame_symbol_errors_;
  Table<size_t> frame_decoded_symbols_;
  Table<size_t> uncoded_bits_count_;
  Table<size_t> uncoded_bit_error_count_;
  Table<float> evm_buffer_;
  Table<float> evm_sc_buffer_;
  Table<float> pilot_snr_;
  Table<float> pilot_rssi_;
  Table<float> pilot_noise_;
  Table<float> dl_pilot_snr_;
  Table<float> dl_pilot_rssi_;
  Table<float> dl_pilot_noise_;
  Table<float> calib_pilot_snr_;
  Table<float> csi_cond_;
  Table<float> calib_;

  arma::cx_fcube gt_cube_;
  size_t num_rx_symbols_;
  size_t num_rxdata_symbols_;

  CsvLog::CsvLogger logger_plt_snr_;
  CsvLog::CsvLogger logger_plt_rssi_;
  CsvLog::CsvLogger logger_plt_noise_;
  CsvLog::CsvLogger logger_bf_snr_;
  CsvLog::CsvLogger logger_bf_rssi_;
  CsvLog::CsvLogger logger_bf_noise_;
  CsvLog::CsvLogger logger_evm_;
  CsvLog::CsvLogger logger_evm_sc_;
  CsvLog::CsvLogger logger_evm_snr_;
  CsvLog::CsvLogger logger_ber_;
  CsvLog::CsvLogger logger_ser_;
  CsvLog::CsvLogger logger_csi_;
  CsvLog::MatLogger logger_calib_;
  CsvLog::MatLogger logger_ul_csi_;
  CsvLog::MatLogger logger_dl_csi_;
  CsvLog::MatLogger logger_ul_beam_;
  CsvLog::MatLogger logger_dl_beam_;
};

#endif  // PHY_STATS_H_

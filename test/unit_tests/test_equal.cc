#include <gtest/gtest.h>
// For some reason, gtest include order matters

#include "comms-lib.h"
#include "config.h"
#include "dodemul.h"
#include "gettime.h"

// set static constexpr bool kExportConstellation = true; at symbol.h to enable
// this unit test. otherwise, the correctness check is not reliable.
// TODO: Test the case that kExportConstellation = false;

// Operator overload for correctness comparison
bool operator==(const complex_float lhs, const complex_float& rhs) {
  return (lhs.re == rhs.re) && (lhs.im == rhs.im);
}

bool operator!=(const complex_float lhs, const complex_float& rhs) {
  // return (lhs.re != rhs.re) || (lhs.im != rhs.im);

  // approx_eq, enable this if you would like to compare between armadillo & MKL
  // the precision here will impact the rate of failure
  float threshold = 0.0001;
  return (fabs(lhs.re - rhs.re) > threshold) ||
         (fabs(lhs.im - rhs.im) > threshold);
}

/******************************************************************************/

/*
 * Original case:
 *   - Used to test the correctness of all special-case implementations.
 *   - The only difference between this version and the Agora's default dodemul
 *     is the usage of arma::sign(), which has been verified with simulation/RRU
 *     mode. It does change the equalization results but won't impact BER/BLER.
 *   - It uses MKL instead of Armadillo for equalization. Thus, the result might
 *     have precision errors.
 */
void equal_org(
    Config* cfg_, Table<complex_float>& data_buffer_,
    Table<complex_float>& equal_buffer_,
    Table<complex_float>& ue_spec_pilot_buffer_,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_beam_matrices_,
    size_t frame_id_, size_t symbol_id_, size_t base_sc_id_) {
  bool kUseSIMDGather = true;
  // ---------------------------------------------------------------------------
  // Class definition of DoDemul
  // ---------------------------------------------------------------------------

  /// Intermediate buffer to gather raw data. Size = subcarriers per cacheline
  /// times number of antennas
  complex_float* data_gather_buffer_;

  // Intermediate buffers for equalized data
  complex_float* equaled_buffer_temp_;
  // complex_float* equaled_buffer_temp_transposed_;
  arma::cx_fmat ue_pilot_data_;
  // int ue_num_simd256_;

  // For efficient phase shift calibration
  static arma::fmat theta_mat;
  static arma::fmat theta_inc;

#if defined(USE_MKL_JIT)
  void* jitter_;
  cgemm_jit_kernel_t mkl_jit_cgemm_;
#endif

  // ---------------------------------------------------------------------------
  // Constructor of DoDemul
  // ---------------------------------------------------------------------------

  data_gather_buffer_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          kSCsPerCacheline * kMaxAntennas * sizeof(complex_float)));
  equaled_buffer_temp_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));
  // equaled_buffer_temp_transposed_ =
  //     static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
  //         Agora_memory::Alignment_t::kAlign64,
  //         cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));

  // phase offset calibration data
  arma::cx_float* ue_pilot_ptr =
      reinterpret_cast<arma::cx_float*>(cfg_->UeSpecificPilot()[0]);
  arma::cx_fmat mat_pilot_data(ue_pilot_ptr, cfg_->OfdmDataNum(),
                               cfg_->UeAntNum(), false);
  ue_pilot_data_ = mat_pilot_data.st();

#if defined(USE_MKL_JIT)
  MKL_Complex8 alpha = {1, 0};
  MKL_Complex8 beta = {0, 0};

  mkl_jit_status_t status =
      mkl_jit_create_cgemm(&jitter_, MKL_COL_MAJOR, MKL_NOTRANS, MKL_NOTRANS,
                           cfg_->SpatialStreamsNum(), 1, cfg_->BsAntNum(),
                           &alpha, cfg_->SpatialStreamsNum(), cfg_->BsAntNum(),
                           &beta, cfg_->SpatialStreamsNum());
  if (MKL_JIT_ERROR == status) {
    std::fprintf(
        stderr,
        "Error: insufficient memory to JIT and store the DGEMM kernel\n");
    throw std::runtime_error(
        "DoDemul: insufficient memory to JIT and store the DGEMM kernel");
  }
  mkl_jit_cgemm_ = mkl_jit_get_cgemm_ptr(jitter_);
#endif

  // ---------------------------------------------------------------------------
  // First part of DoDemul: equalization + phase shift calibration
  // ---------------------------------------------------------------------------

  size_t frame_id = frame_id_;
  size_t symbol_id = symbol_id_;
  size_t base_sc_id = base_sc_id_;

  // ---------------------------------------------------------------------------

  const size_t symbol_idx_ul = cfg_->Frame().GetULSymbolIdx(symbol_id);
  // const size_t data_symbol_idx_ul =
  //     symbol_idx_ul - cfg_->Frame().ClientUlPilotSymbols();
  const size_t total_data_symbol_idx_ul =
      cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
  const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

  const size_t frame_slot = frame_id % kFrameWnd;

  size_t max_sc_ite =
      std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
  assert(max_sc_ite % kSCsPerCacheline == 0);
  // Iterate through cache lines
  for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
    // Step 1: Populate data_gather_buffer as a row-major matrix with
    // kSCsPerCacheline rows and BsAntNum() columns

    // Since kSCsPerCacheline divides demul_block_size and
    // kTransposeBlockSize, all subcarriers (base_sc_id + i) lie in the
    // same partial transpose block.
    const size_t partial_transpose_block_base =
        ((base_sc_id + i) / kTransposeBlockSize) *
        (kTransposeBlockSize * cfg_->BsAntNum());

#ifdef __AVX512F__
    static constexpr size_t kAntNumPerSimd = 8;
#else
    static constexpr size_t kAntNumPerSimd = 4;
#endif

    size_t ant_start = 0;
    if (kUseSIMDGather && kUsePartialTrans &&
        (cfg_->BsAntNum() % kAntNumPerSimd) == 0) {
      // Gather data for all antennas and 8 subcarriers in the same cache
      // line, 1 subcarrier and 4 (AVX2) or 8 (AVX512) ants per iteration
      size_t cur_sc_offset =
          partial_transpose_block_base + (base_sc_id + i) % kTransposeBlockSize;
      const float* src =
          reinterpret_cast<const float*>(&data_buf[cur_sc_offset]);
      float* dst = reinterpret_cast<float*>(data_gather_buffer_);
#ifdef __AVX512F__
      __m512i index = _mm512_setr_epi32(
          0, 1, kTransposeBlockSize * 2, kTransposeBlockSize * 2 + 1,
          kTransposeBlockSize * 4, kTransposeBlockSize * 4 + 1,
          kTransposeBlockSize * 6, kTransposeBlockSize * 6 + 1,
          kTransposeBlockSize * 8, kTransposeBlockSize * 8 + 1,
          kTransposeBlockSize * 10, kTransposeBlockSize * 10 + 1,
          kTransposeBlockSize * 12, kTransposeBlockSize * 12 + 1,
          kTransposeBlockSize * 14, kTransposeBlockSize * 14 + 1);
      for (size_t ant_i = 0; ant_i < cfg_->BsAntNum();
           ant_i += kAntNumPerSimd) {
        for (size_t j = 0; j < kSCsPerCacheline; j++) {
          __m512 data_rx = kTransposeBlockSize == 1
                               ? _mm512_load_ps(&src[j * cfg_->BsAntNum() * 2])
                               : _mm512_i32gather_ps(index, &src[j * 2], 4);

          assert((reinterpret_cast<intptr_t>(&dst[j * cfg_->BsAntNum() * 2]) %
                  (kAntNumPerSimd * sizeof(float) * 2)) == 0);
          assert((reinterpret_cast<intptr_t>(&src[j * cfg_->BsAntNum() * 2]) %
                  (kAntNumPerSimd * sizeof(float) * 2)) == 0);
          _mm512_store_ps(&dst[j * cfg_->BsAntNum() * 2], data_rx);
        }
        src += kAntNumPerSimd * kTransposeBlockSize * 2;
        dst += kAntNumPerSimd * 2;
      }
#else
      __m256i index = _mm256_setr_epi32(
          0, 1, kTransposeBlockSize * 2, kTransposeBlockSize * 2 + 1,
          kTransposeBlockSize * 4, kTransposeBlockSize * 4 + 1,
          kTransposeBlockSize * 6, kTransposeBlockSize * 6 + 1);
      for (size_t ant_i = 0; ant_i < cfg_->BsAntNum();
           ant_i += kAntNumPerSimd) {
        for (size_t j = 0; j < kSCsPerCacheline; j++) {
          assert((reinterpret_cast<intptr_t>(&dst[j * cfg_->BsAntNum() * 2]) %
                  (kAntNumPerSimd * sizeof(float) * 2)) == 0);
          __m256 data_rx = _mm256_i32gather_ps(&src[j * 2], index, 4);
          _mm256_store_ps(&dst[j * cfg_->BsAntNum() * 2], data_rx);
        }
        src += kAntNumPerSimd * kTransposeBlockSize * 2;
        dst += kAntNumPerSimd * 2;
      }
#endif
      // Set the remaining number of antennas for non-SIMD gather
      ant_start = cfg_->BsAntNum() - (cfg_->BsAntNum() % kAntNumPerSimd);
    }
    if (ant_start < cfg_->BsAntNum()) {
      complex_float* dst = data_gather_buffer_ + ant_start;
      for (size_t j = 0; j < kSCsPerCacheline; j++) {
        for (size_t ant_i = ant_start; ant_i < cfg_->BsAntNum(); ant_i++) {
          *dst++ =
              kUsePartialTrans
                  ? data_buf[partial_transpose_block_base +
                             (ant_i * kTransposeBlockSize) +
                             ((base_sc_id + i + j) % kTransposeBlockSize)]
                  : data_buf[ant_i * cfg_->OfdmDataNum() + base_sc_id + i + j];
        }
      }
    }

    // Step 2: For each subcarrier, perform equalization by multiplying the
    // subcarrier's data from each antenna with the subcarrier's precoder
    for (size_t j = 0; j < kSCsPerCacheline; j++) {
      const size_t cur_sc_id = base_sc_id + i + j;

      arma::cx_float* equal_ptr = nullptr;
      if (kExportConstellation) {
        equal_ptr =
            (arma::cx_float*)(&equal_buffer_[total_data_symbol_idx_ul]
                                            [cur_sc_id * cfg_->UeAntNum()]);
      } else {
        equal_ptr =
            (arma::cx_float*)(&equaled_buffer_temp_[(cur_sc_id - base_sc_id) *
                                                    cfg_->UeAntNum()]);
      }
      arma::cx_fmat mat_equaled(equal_ptr, cfg_->UeAntNum(), 1, false);

      arma::cx_float* data_ptr = reinterpret_cast<arma::cx_float*>(
          &data_gather_buffer_[j * cfg_->BsAntNum()]);
      arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
          ul_beam_matrices_[frame_slot][cfg_->GetBeamScId(cur_sc_id)]);

#if defined(USE_MKL_JIT)
      mkl_jit_cgemm_(jitter_, (MKL_Complex8*)ul_beam_ptr,
                     (MKL_Complex8*)data_ptr, (MKL_Complex8*)equal_ptr);
#else
      arma::cx_fmat mat_data(data_ptr, cfg_->BsAntNum(), 1, false);

      arma::cx_fmat mat_ul_beam(ul_beam_ptr, cfg_->UeAntNum(), cfg_->BsAntNum(),
                                false);
      mat_equaled = mat_ul_beam * mat_data;
#endif

      if (symbol_idx_ul <
          cfg_->Frame().ClientUlPilotSymbols()) {  // Calc new phase shift
        if (symbol_idx_ul == 0 && cur_sc_id == 0) {
          // Reset previous frame
          arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
              ue_spec_pilot_buffer_[(frame_id - 1) % kFrameWnd]);
          arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(),
                                        cfg_->Frame().ClientUlPilotSymbols(),
                                        false);
          mat_phase_shift.fill(0);
        }
        arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
            &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                  [symbol_idx_ul * cfg_->UeAntNum()]);
        arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(), 1,
                                      false);
        // arma::cx_fmat shift_sc =
        //     sign(mat_equaled % conj(ue_pilot_data_.col(cur_sc_id)));
        arma::cx_fmat shift_sc =
            mat_equaled % conj(ue_pilot_data_.col(cur_sc_id));
        mat_phase_shift += shift_sc;
      }
      // apply previously calc'ed phase shift to data
      else if (cfg_->Frame().ClientUlPilotSymbols() > 0) {
        arma::cx_float* pilot_corr_ptr = reinterpret_cast<arma::cx_float*>(
            ue_spec_pilot_buffer_[frame_id % kFrameWnd]);
        arma::cx_fmat pilot_corr_mat(pilot_corr_ptr, cfg_->UeAntNum(),
                                     cfg_->Frame().ClientUlPilotSymbols(),
                                     false);
        arma::fmat theta_mat = arg(pilot_corr_mat);
        arma::fmat theta_inc = arma::zeros<arma::fmat>(cfg_->UeAntNum(), 1);
        for (size_t s = 1; s < cfg_->Frame().ClientUlPilotSymbols(); s++) {
          arma::fmat theta_diff = theta_mat.col(s) - theta_mat.col(s - 1);
          theta_inc += theta_diff;
        }
        theta_inc /= (float)std::max(
            1, static_cast<int>(cfg_->Frame().ClientUlPilotSymbols() - 1));
        arma::fmat cur_theta = theta_mat.col(0) + (symbol_idx_ul * theta_inc);
        arma::cx_fmat mat_phase_correct =
            arma::zeros<arma::cx_fmat>(size(cur_theta));
        mat_phase_correct.set_real(cos(-cur_theta));
        mat_phase_correct.set_imag(sin(-cur_theta));
        mat_equaled %= mat_phase_correct;
      }
    }
  }
}

/*
 * Test case:
 *   - Simplify the if-conditions for phase tracking.
 *   - Should identical correctness everywhere as the original implementation.
 */
void equal_ifcond(
    Config* cfg_, Table<complex_float>& data_buffer_,
    Table<complex_float>& equal_buffer_,
    Table<complex_float>& ue_spec_pilot_buffer_,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_beam_matrices_,
    size_t frame_id_, size_t symbol_id_, size_t base_sc_id_) {
  bool kUseSIMDGather = true;
  // ---------------------------------------------------------------------------
  // Class definition of DoDemul
  // ---------------------------------------------------------------------------

  /// Intermediate buffer to gather raw data. Size = subcarriers per cacheline
  /// times number of antennas
  complex_float* data_gather_buffer_;

  // Intermediate buffers for equalized data
  complex_float* equaled_buffer_temp_;
  // complex_float* equaled_buffer_temp_transposed_;
  arma::cx_fmat ue_pilot_data_;
  // int ue_num_simd256_;

  // For efficient phase shift calibration
  static arma::fmat theta_mat;
  static arma::fmat theta_inc;

#if defined(USE_MKL_JIT)
  void* jitter_;
  cgemm_jit_kernel_t mkl_jit_cgemm_;
#endif

  // ---------------------------------------------------------------------------
  // Constructor of DoDemul
  // ---------------------------------------------------------------------------

  data_gather_buffer_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          kSCsPerCacheline * kMaxAntennas * sizeof(complex_float)));
  equaled_buffer_temp_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));
  // equaled_buffer_temp_transposed_ =
  //     static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
  //         Agora_memory::Alignment_t::kAlign64,
  //         cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));

  // phase offset calibration data
  arma::cx_float* ue_pilot_ptr =
      reinterpret_cast<arma::cx_float*>(cfg_->UeSpecificPilot()[0]);
  arma::cx_fmat mat_pilot_data(ue_pilot_ptr, cfg_->OfdmDataNum(),
                               cfg_->UeAntNum(), false);
  ue_pilot_data_ = mat_pilot_data.st();

#if defined(USE_MKL_JIT)
  MKL_Complex8 alpha = {1, 0};
  MKL_Complex8 beta = {0, 0};

  mkl_jit_status_t status =
      mkl_jit_create_cgemm(&jitter_, MKL_COL_MAJOR, MKL_NOTRANS, MKL_NOTRANS,
                           cfg_->SpatialStreamsNum(), 1, cfg_->BsAntNum(),
                           &alpha, cfg_->SpatialStreamsNum(), cfg_->BsAntNum(),
                           &beta, cfg_->SpatialStreamsNum());
  if (MKL_JIT_ERROR == status) {
    std::fprintf(
        stderr,
        "Error: insufficient memory to JIT and store the DGEMM kernel\n");
    throw std::runtime_error(
        "DoDemul: insufficient memory to JIT and store the DGEMM kernel");
  }
  mkl_jit_cgemm_ = mkl_jit_get_cgemm_ptr(jitter_);
#endif

  // ---------------------------------------------------------------------------
  // First part of DoDemul: equalization + phase shift calibration
  // ---------------------------------------------------------------------------

  size_t frame_id = frame_id_;
  size_t symbol_id = symbol_id_;
  size_t base_sc_id = base_sc_id_;

  // ---------------------------------------------------------------------------

  const size_t symbol_idx_ul = cfg_->Frame().GetULSymbolIdx(symbol_id);
  // const size_t data_symbol_idx_ul =
  //     symbol_idx_ul - cfg_->Frame().ClientUlPilotSymbols();
  const size_t total_data_symbol_idx_ul =
      cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
  const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

  const size_t frame_slot = frame_id % kFrameWnd;

  size_t max_sc_ite =
      std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
  assert(max_sc_ite % kSCsPerCacheline == 0);
  // Iterate through cache lines
  for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
    // Step 1: Populate data_gather_buffer as a row-major matrix with
    // kSCsPerCacheline rows and BsAntNum() columns

    // Since kSCsPerCacheline divides demul_block_size and
    // kTransposeBlockSize, all subcarriers (base_sc_id + i) lie in the
    // same partial transpose block.
    const size_t partial_transpose_block_base =
        ((base_sc_id + i) / kTransposeBlockSize) *
        (kTransposeBlockSize * cfg_->BsAntNum());

#ifdef __AVX512F__
    static constexpr size_t kAntNumPerSimd = 8;
#else
    static constexpr size_t kAntNumPerSimd = 4;
#endif

    size_t ant_start = 0;
    if (kUseSIMDGather && kUsePartialTrans &&
        (cfg_->BsAntNum() % kAntNumPerSimd) == 0) {
      // Gather data for all antennas and 8 subcarriers in the same cache
      // line, 1 subcarrier and 4 (AVX2) or 8 (AVX512) ants per iteration
      size_t cur_sc_offset =
          partial_transpose_block_base + (base_sc_id + i) % kTransposeBlockSize;
      const float* src =
          reinterpret_cast<const float*>(&data_buf[cur_sc_offset]);
      float* dst = reinterpret_cast<float*>(data_gather_buffer_);
#ifdef __AVX512F__
      __m512i index = _mm512_setr_epi32(
          0, 1, kTransposeBlockSize * 2, kTransposeBlockSize * 2 + 1,
          kTransposeBlockSize * 4, kTransposeBlockSize * 4 + 1,
          kTransposeBlockSize * 6, kTransposeBlockSize * 6 + 1,
          kTransposeBlockSize * 8, kTransposeBlockSize * 8 + 1,
          kTransposeBlockSize * 10, kTransposeBlockSize * 10 + 1,
          kTransposeBlockSize * 12, kTransposeBlockSize * 12 + 1,
          kTransposeBlockSize * 14, kTransposeBlockSize * 14 + 1);
      for (size_t ant_i = 0; ant_i < cfg_->BsAntNum();
           ant_i += kAntNumPerSimd) {
        for (size_t j = 0; j < kSCsPerCacheline; j++) {
          __m512 data_rx = kTransposeBlockSize == 1
                               ? _mm512_load_ps(&src[j * cfg_->BsAntNum() * 2])
                               : _mm512_i32gather_ps(index, &src[j * 2], 4);

          assert((reinterpret_cast<intptr_t>(&dst[j * cfg_->BsAntNum() * 2]) %
                  (kAntNumPerSimd * sizeof(float) * 2)) == 0);
          assert((reinterpret_cast<intptr_t>(&src[j * cfg_->BsAntNum() * 2]) %
                  (kAntNumPerSimd * sizeof(float) * 2)) == 0);
          _mm512_store_ps(&dst[j * cfg_->BsAntNum() * 2], data_rx);
        }
        src += kAntNumPerSimd * kTransposeBlockSize * 2;
        dst += kAntNumPerSimd * 2;
      }
#else
      __m256i index = _mm256_setr_epi32(
          0, 1, kTransposeBlockSize * 2, kTransposeBlockSize * 2 + 1,
          kTransposeBlockSize * 4, kTransposeBlockSize * 4 + 1,
          kTransposeBlockSize * 6, kTransposeBlockSize * 6 + 1);
      for (size_t ant_i = 0; ant_i < cfg_->BsAntNum();
           ant_i += kAntNumPerSimd) {
        for (size_t j = 0; j < kSCsPerCacheline; j++) {
          assert((reinterpret_cast<intptr_t>(&dst[j * cfg_->BsAntNum() * 2]) %
                  (kAntNumPerSimd * sizeof(float) * 2)) == 0);
          __m256 data_rx = _mm256_i32gather_ps(&src[j * 2], index, 4);
          _mm256_store_ps(&dst[j * cfg_->BsAntNum() * 2], data_rx);
        }
        src += kAntNumPerSimd * kTransposeBlockSize * 2;
        dst += kAntNumPerSimd * 2;
      }
#endif
      // Set the remaining number of antennas for non-SIMD gather
      ant_start = cfg_->BsAntNum() - (cfg_->BsAntNum() % kAntNumPerSimd);
    }
    if (ant_start < cfg_->BsAntNum()) {
      complex_float* dst = data_gather_buffer_ + ant_start;
      for (size_t j = 0; j < kSCsPerCacheline; j++) {
        for (size_t ant_i = ant_start; ant_i < cfg_->BsAntNum(); ant_i++) {
          *dst++ =
              kUsePartialTrans
                  ? data_buf[partial_transpose_block_base +
                             (ant_i * kTransposeBlockSize) +
                             ((base_sc_id + i + j) % kTransposeBlockSize)]
                  : data_buf[ant_i * cfg_->OfdmDataNum() + base_sc_id + i + j];
        }
      }
    }

    // Step 2: For each subcarrier, perform equalization by multiplying the
    // subcarrier's data from each antenna with the subcarrier's precoder
    for (size_t j = 0; j < kSCsPerCacheline; j++) {
      const size_t cur_sc_id = base_sc_id + i + j;

      arma::cx_float* equal_ptr = nullptr;
      if (kExportConstellation) {
        equal_ptr =
            (arma::cx_float*)(&equal_buffer_[total_data_symbol_idx_ul]
                                            [cur_sc_id * cfg_->UeAntNum()]);
      } else {
        equal_ptr =
            (arma::cx_float*)(&equaled_buffer_temp_[(cur_sc_id - base_sc_id) *
                                                    cfg_->UeAntNum()]);
      }
      arma::cx_fmat mat_equaled(equal_ptr, cfg_->UeAntNum(), 1, false);

      arma::cx_float* data_ptr = reinterpret_cast<arma::cx_float*>(
          &data_gather_buffer_[j * cfg_->BsAntNum()]);
      arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
          ul_beam_matrices_[frame_slot][cfg_->GetBeamScId(cur_sc_id)]);

#if defined(USE_MKL_JIT)
      mkl_jit_cgemm_(jitter_, (MKL_Complex8*)ul_beam_ptr,
                     (MKL_Complex8*)data_ptr, (MKL_Complex8*)equal_ptr);
#else
      arma::cx_fmat mat_data(data_ptr, cfg_->BsAntNum(), 1, false);

      arma::cx_fmat mat_ul_beam(ul_beam_ptr, cfg_->UeAntNum(), cfg_->BsAntNum(),
                                false);
      mat_equaled = mat_ul_beam * mat_data;
#endif

      // -----------------------------------------------------------------------

      // Enable phase shift calibration
      if (cfg_->Frame().ClientUlPilotSymbols() > 0) {
        // Calc new phase shift
        if (symbol_idx_ul < cfg_->Frame().ClientUlPilotSymbols()) {
          if (symbol_idx_ul == 0 && cur_sc_id == 0) {
            // Reset previous frame
            arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
                ue_spec_pilot_buffer_[(frame_id - 1) % kFrameWnd]);
            arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(),
                                          cfg_->Frame().ClientUlPilotSymbols(),
                                          false);
            mat_phase_shift.fill(0);
          }
          arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
              &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                    [symbol_idx_ul * cfg_->UeAntNum()]);
          arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(), 1,
                                        false);
          // arma::cx_fmat shift_sc =
          //     sign(mat_equaled % conj(ue_pilot_data_.col(cur_sc_id)));
          arma::cx_fmat shift_sc =
              mat_equaled % conj(ue_pilot_data_.col(cur_sc_id));
          mat_phase_shift += shift_sc;
        }

        if (symbol_idx_ul == cfg_->Frame().ClientUlPilotSymbols() &&
            cur_sc_id == 0) {
          arma::cx_float* pilot_corr_ptr = reinterpret_cast<arma::cx_float*>(
              ue_spec_pilot_buffer_[frame_id % kFrameWnd]);
          arma::cx_fmat pilot_corr_mat(pilot_corr_ptr, cfg_->UeAntNum(),
                                       cfg_->Frame().ClientUlPilotSymbols(),
                                       false);
          theta_mat = arg(pilot_corr_mat);
          theta_inc = theta_mat.col(cfg_->Frame().ClientUlPilotSymbols() - 1) -
                      theta_mat.col(0);
          theta_inc /= (float)std::max(
              1, static_cast<int>(cfg_->Frame().ClientUlPilotSymbols() - 1));
        }

        // apply previously calc'ed phase shift to data
        if (symbol_idx_ul >= cfg_->Frame().ClientUlPilotSymbols()) {
          arma::fmat cur_theta = theta_mat.col(0) + (symbol_idx_ul * theta_inc);
          arma::cx_fmat mat_phase_correct =
              arma::cx_fmat(cos(-cur_theta), sin(-cur_theta));
          mat_equaled %= mat_phase_correct;
        }
      }
    }
  }
}

/*
 * Test case:
 *   - Vectorization for 1x1 SISO.
 *   - THE DEFAULT CASE FOR 1X1 IMPLEMENTATION.
 */
void equal_vec_1x1_complex(
    Config* cfg_, Table<complex_float>& data_buffer_,
    Table<complex_float>& equal_buffer_,
    Table<complex_float>& ue_spec_pilot_buffer_,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_beam_matrices_,
    size_t frame_id_, size_t symbol_id_, size_t base_sc_id_) {
  RtAssert(cfg_->BsAntNum() == 1 && cfg_->UeAntNum() == 1,
           "Correctness is only guaranteed in special case of antenna 1x1!");
  // RtAssert(kUsePartialTrans == false,
  //          "If set kUsePartialTrans = true, the test case might fail (with a probability)");
  RtAssert(kExportConstellation == true,
           "Set kExportConstellation to evaluate the correctness (export "
           "equal_buffer_)");

  // ---------------------------------------------------------------------------
  // Class definition of DoDemul
  // ---------------------------------------------------------------------------

  // Intermediate buffers for equalized data
  complex_float* equaled_buffer_temp_;
  arma::cx_fmat ue_pilot_data_;

  // For efficient phase shift calibration
  static arma::fvec theta_vec;
  static float theta_inc;

  // ---------------------------------------------------------------------------
  // Constructor of DoDemul
  // ---------------------------------------------------------------------------

  equaled_buffer_temp_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));

  // phase offset calibration data
  arma::cx_float* ue_pilot_ptr =
      reinterpret_cast<arma::cx_float*>(cfg_->UeSpecificPilot()[0]);
  arma::cx_fvec vec_pilot_data(ue_pilot_ptr, cfg_->OfdmDataNum(), false);

  // ---------------------------------------------------------------------------
  // First part of DoDemul: equalization + phase shift calibration
  // ---------------------------------------------------------------------------

  size_t frame_id = frame_id_;
  size_t symbol_id = symbol_id_;
  size_t base_sc_id = base_sc_id_;

  // ---------------------------------------------------------------------------

  const size_t symbol_idx_ul = cfg_->Frame().GetULSymbolIdx(symbol_id);
  const size_t total_data_symbol_idx_ul =
      cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
  const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

  const size_t frame_slot = frame_id % kFrameWnd;

  size_t max_sc_ite =
      std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
  assert(max_sc_ite % kSCsPerCacheline == 0);

  // Step 1: Equalization
  arma::cx_float* equal_ptr = nullptr;
  if (kExportConstellation) {
    equal_ptr =
        (arma::cx_float*)(&equal_buffer_[total_data_symbol_idx_ul][base_sc_id]);
  } else {
    equal_ptr = (arma::cx_float*)(&equaled_buffer_temp_[0]);
  }
  arma::cx_fvec vec_equaled(equal_ptr, max_sc_ite, false);

  arma::cx_float* data_ptr = (arma::cx_float*)(&data_buf[base_sc_id]);
  // not consider multi-antenna case (antena offset is omitted)
  arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
      ul_beam_matrices_[frame_slot][0]);  // pick the first element

  arma::cx_fvec vec_ul_beam(max_sc_ite);  // init empty vec
  for (size_t i = 0; i < max_sc_ite; ++i) {
    vec_ul_beam(i) = ul_beam_ptr[cfg_->GetBeamScId(base_sc_id + i)];
  }

#ifdef __AVX512F__
  const complex_float* ptr_data =
      reinterpret_cast<const complex_float*>(data_ptr);
  const complex_float* ptr_ul_beam =
      reinterpret_cast<const complex_float*>(vec_ul_beam.memptr());
  complex_float* ptr_equaled = reinterpret_cast<complex_float*>(equal_ptr);
  for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
    __m512 reg_data = _mm512_loadu_ps(ptr_data + i);
    __m512 reg_ul_beam = _mm512_loadu_ps(ptr_ul_beam + i);
    __m512 reg_equaled =
        CommsLib::M512ComplexCf32Mult(reg_data, reg_ul_beam, false);
    _mm512_storeu_ps(ptr_equaled + i, reg_equaled);
  }
#else
  arma::cx_fvec vec_data(data_ptr, max_sc_ite, false);
  vec_equaled = vec_ul_beam % vec_data;
#endif

  // Step 2: Phase shift calibration

  // Enable phase shift calibration
  if (cfg_->Frame().ClientUlPilotSymbols() > 0) {
    if (symbol_idx_ul == 0 && base_sc_id == 0) {
      // Reset previous frame
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[(frame_id - 1) % kFrameWnd]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(),
                                    cfg_->Frame().ClientUlPilotSymbols(),
                                    false);
      mat_phase_shift.fill(0);
    }

    // Calc new phase shift
    if (symbol_idx_ul < cfg_->Frame().ClientUlPilotSymbols()) {
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                [symbol_idx_ul * cfg_->UeAntNum()]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(), 1,
                                    false);
      // printf("base_sc_id = %ld, end = %ld\n", base_sc_id,
      //        base_sc_id + max_sc_ite - 1);
      arma::cx_fvec vec_ue_pilot_data_ =
          vec_pilot_data.subvec(base_sc_id, base_sc_id + max_sc_ite - 1);

      mat_phase_shift += sum(vec_equaled % conj(vec_ue_pilot_data_));
      // mat_phase_shift += sum(sign(vec_equaled % conj(vec_ue_pilot_data_)));
      // sign should be able to optimize out but the result will be different
    }

    // Calculate the unit phase shift based on the first subcarrier
    // Check the special case condition to avoid reading wrong memory location
    RtAssert(cfg_->UeAntNum() == 1 &&
             cfg_->Frame().ClientUlPilotSymbols() == 2);
    if (symbol_idx_ul == cfg_->Frame().ClientUlPilotSymbols() &&
        base_sc_id == 0) {
      arma::cx_float* pilot_corr_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[frame_id % kFrameWnd]);
      arma::cx_fvec pilot_corr_vec(pilot_corr_ptr,
                                   cfg_->Frame().ClientUlPilotSymbols(), false);
      theta_vec = arg(pilot_corr_vec);
      theta_inc =
          theta_vec(cfg_->Frame().ClientUlPilotSymbols() - 1) - theta_vec(0);
      // theta_inc /= (float)std::max(
      //     1, static_cast<int>(cfg_->Frame().ClientUlPilotSymbols() - 1));
    }

    // Apply previously calc'ed phase shift to data
    if (symbol_idx_ul >= cfg_->Frame().ClientUlPilotSymbols()) {
      float cur_theta_f = theta_vec(0) + (symbol_idx_ul * theta_inc);
      vec_equaled *= arma::cx_float(cos(-cur_theta_f), sin(-cur_theta_f));
    }
  }
}

/*
 * Test case:
 *   - Vectorization for 1x1 SISO.
 *   - Use real value instead of complex value operations.
 *   - Missed the arma::sign() counterpart.
 */
void equal_vec_1x1_real(
    Config* cfg_, Table<complex_float>& data_buffer_,
    Table<complex_float>& equal_buffer_,
    Table<complex_float>& ue_spec_pilot_buffer_,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_beam_matrices_,
    size_t frame_id_, size_t symbol_id_, size_t base_sc_id_) {
  RtAssert(cfg_->BsAntNum() == 1 && cfg_->UeAntNum() == 1,
           "Correctness is only guaranteed in special case of antenna 1x1!");
  // RtAssert(kUsePartialTrans == false,
  //          "If set kUsePartialTrans = true, the test case might fail (with a "
  //          "probability)");
  RtAssert(kExportConstellation == true,
           "Set kExportConstellation to evaluate the correctness"
           " (export equal_buffer_)");

  // ---------------------------------------------------------------------------
  // Class definition of DoDemul
  // ---------------------------------------------------------------------------

  // Intermediate buffers for equalized data
  complex_float* equaled_buffer_temp_;
  arma::cx_fmat ue_pilot_data_;

  // For efficient phase shift calibration
  static arma::fvec theta_vec;
  static float theta_inc;

  // Buffers for real-value operations
  arma::fvec vec_equaled_real;
  arma::fvec vec_equaled_imag;
  arma::fvec vec_data_real;
  arma::fvec vec_data_imag;
  arma::fvec vec_ul_data_real;
  arma::fvec vec_ul_data_imag;
  arma::fvec vec_equaled_real_final;
  arma::fvec vec_equaled_imag_final;

  // ---------------------------------------------------------------------------
  // Constructor of DoDemul
  // ---------------------------------------------------------------------------

  equaled_buffer_temp_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));

  // phase offset calibration data
  arma::cx_float* ue_pilot_ptr =
      reinterpret_cast<arma::cx_float*>(cfg_->UeSpecificPilot()[0]);
  arma::cx_fvec vec_pilot_data(ue_pilot_ptr, cfg_->OfdmDataNum(), false);

  // ---------------------------------------------------------------------------
  // First part of DoDemul: equalization + phase shift calibration
  // ---------------------------------------------------------------------------

  size_t frame_id = frame_id_;
  size_t symbol_id = symbol_id_;
  size_t base_sc_id = base_sc_id_;

  // ---------------------------------------------------------------------------

  const size_t symbol_idx_ul = cfg_->Frame().GetULSymbolIdx(symbol_id);
  const size_t total_data_symbol_idx_ul =
      cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
  const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

  const size_t frame_slot = frame_id % kFrameWnd;

  size_t max_sc_ite =
      std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
  assert(max_sc_ite % kSCsPerCacheline == 0);

  // Step 1: Equalization
  arma::cx_float* equal_ptr = nullptr;
  if (kExportConstellation) {
    equal_ptr =
        (arma::cx_float*)(&equal_buffer_[total_data_symbol_idx_ul][base_sc_id]);
  } else {
    equal_ptr = (arma::cx_float*)(&equaled_buffer_temp_[0]);
  }
  arma::cx_fvec vec_equaled(equal_ptr, max_sc_ite, false);

  arma::cx_float* data_ptr = (arma::cx_float*)(&data_buf[base_sc_id]);
  // not consider multi-antenna case (antena offset is omitted)
  arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
      ul_beam_matrices_[frame_slot][0]);  // pick the first element

  // assuming cfg->BsAntNum() == 1, reducing a dimension
  arma::cx_fvec vec_data(data_ptr, max_sc_ite, false);
  arma::cx_fvec vec_ul_beam(max_sc_ite);  // init empty vec
  for (size_t i = 0; i < max_sc_ite; ++i) {
    vec_ul_beam(i) = ul_beam_ptr[cfg_->GetBeamScId(base_sc_id + i)];
  }
  vec_data_real = arma::real(vec_data);
  vec_data_imag = arma::imag(vec_data);
  vec_ul_data_real = arma::real(vec_ul_beam);
  vec_ul_data_imag = arma::imag(vec_ul_beam);
  vec_equaled_real =
      vec_ul_data_real % vec_data_real - vec_ul_data_imag % vec_data_imag;
  vec_equaled_imag =
      vec_ul_data_real % vec_data_imag + vec_ul_data_imag % vec_data_real;
  // vec_equaled = vec_ul_beam % vec_data;

  // Step 2: Phase shift calibration

  // Enable phase shift calibration
  if (cfg_->Frame().ClientUlPilotSymbols() > 0) {
    if (symbol_idx_ul == 0 && base_sc_id == 0) {
      // Reset previous frame
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[(frame_id - 1) % kFrameWnd]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(),
                                    cfg_->Frame().ClientUlPilotSymbols(),
                                    false);
      mat_phase_shift.fill(0);
    }

    // Calc new phase shift
    if (symbol_idx_ul < cfg_->Frame().ClientUlPilotSymbols()) {
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                [symbol_idx_ul * cfg_->UeAntNum()]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(), 1,
                                    false);
      // printf("base_sc_id = %ld, end = %ld\n", base_sc_id,
      //        base_sc_id + max_sc_ite - 1);
      arma::cx_fvec vec_ue_pilot_data_ =
          vec_pilot_data.subvec(base_sc_id, base_sc_id + max_sc_ite - 1);

      // mat_phase_shift += sum(vec_equaled % conj(vec_ue_pilot_data_));
      arma::fvec vec_ue_pilot_data_real_ = arma::real(vec_ue_pilot_data_);
      arma::fvec vec_ue_pilot_data_imag_ = arma::imag(vec_ue_pilot_data_);
      arma::fmat mat_phase_shift_real = arma::real(mat_phase_shift);
      arma::fmat mat_phase_shift_imag = arma::imag(mat_phase_shift);
      mat_phase_shift_real += sum(vec_equaled_real % vec_ue_pilot_data_real_ +
                                  vec_equaled_imag % vec_ue_pilot_data_imag_);
      mat_phase_shift_imag += sum(vec_equaled_imag % vec_ue_pilot_data_real_ -
                                  vec_equaled_real % vec_ue_pilot_data_imag_);
      mat_phase_shift.set_real(mat_phase_shift_real);
      mat_phase_shift.set_imag(mat_phase_shift_imag);
    }

    // Calculate the unit phase shift based on the first subcarrier
    // Check the special case condition to avoid reading wrong memory location
    RtAssert(cfg_->UeAntNum() == 1 &&
             cfg_->Frame().ClientUlPilotSymbols() == 2);
    if (symbol_idx_ul == cfg_->Frame().ClientUlPilotSymbols() &&
        base_sc_id == 0) {
      arma::cx_float* pilot_corr_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[frame_id % kFrameWnd]);
      arma::cx_fvec pilot_corr_vec(pilot_corr_ptr,
                                   cfg_->Frame().ClientUlPilotSymbols(), false);
      theta_vec = arg(pilot_corr_vec);
      theta_inc =
          theta_vec(cfg_->Frame().ClientUlPilotSymbols() - 1) - theta_vec(0);
      // theta_inc /= (float)std::max(
      //     1, static_cast<int>(cfg_->Frame().ClientUlPilotSymbols() - 1));
    }

    // Apply previously calc'ed phase shift to data
    if (symbol_idx_ul >= cfg_->Frame().ClientUlPilotSymbols()) {
      float cur_theta_f = theta_vec(0) + (symbol_idx_ul * theta_inc);
      // vec_equaled *= arma::cx_float(cos(-cur_theta_f), sin(-cur_theta_f));
      float cos_f = cos(-cur_theta_f);
      float sin_f = sin(-cur_theta_f);
      vec_equaled_real_final =
          vec_equaled_real * cos_f - vec_equaled_imag * sin_f;
      vec_equaled_imag_final =
          vec_equaled_real * sin_f + vec_equaled_imag * cos_f;
      // arma::cx_fvec vec_equaled_new =
      //   arma::cx_fvec(vec_equaled_real_final, vec_equaled_imag_final);
      // EXPECT_TRUE(
      //   arma::approx_equal(vec_equaled, vec_equaled_new, "both", 0.01, 0.01));
      vec_equaled.set_real(vec_equaled_real_final);
      vec_equaled.set_imag(vec_equaled_imag_final);
    } else {
      vec_equaled.set_real(vec_equaled_real);
      vec_equaled.set_imag(vec_equaled_imag);
    }
  }
}

/*
 * Test case:
 *   - Vectorization for 2x2 MIMO.
 *   - THE DEFAULT CASE FOR 2X2 IMPLEMENTATION.
 */
void equal_vec_2x2_complex(
    Config* cfg_, Table<complex_float>& data_buffer_,
    Table<complex_float>& equal_buffer_,
    Table<complex_float>& ue_spec_pilot_buffer_,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_beam_matrices_,
    size_t frame_id_, size_t symbol_id_, size_t base_sc_id_) {
  RtAssert(cfg_->BsAntNum() == 2 && cfg_->UeAntNum() == 2,
           "Correctness is only guaranteed in special case of antenna 2x2!");
  RtAssert(kExportConstellation == true,
           "Set kExportConstellation to evaluate the correctness "
           " (export equal_buffer_)");
  RtAssert(kUsePartialTrans == false,
           "The test case of 2x2 MIMO is not compatible with kUsePartialTrans");

  // ---------------------------------------------------------------------------
  // Class definition of DoDemul
  // ---------------------------------------------------------------------------

  // Intermediate buffers for equalized data
  complex_float* data_gather_buffer_;
  complex_float* equaled_buffer_temp_;
  arma::cx_fmat ue_pilot_data_;

  // For efficient phase shift calibration
  static arma::fmat theta_mat;
  static arma::fmat theta_inc;

  // ---------------------------------------------------------------------------
  // Constructor of DoDemul
  // ---------------------------------------------------------------------------

  data_gather_buffer_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxAntennas * sizeof(complex_float)));

  equaled_buffer_temp_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));

  // phase offset calibration data
  arma::cx_float* ue_pilot_ptr =
      reinterpret_cast<arma::cx_float*>(cfg_->UeSpecificPilot()[0]);
  arma::cx_fmat mat_pilot_data(ue_pilot_ptr, cfg_->OfdmDataNum(),
                               cfg_->UeAntNum(), false);
  ue_pilot_data_ = mat_pilot_data.st();

  // ---------------------------------------------------------------------------
  // First part of DoDemul: equalization + phase shift calibration
  // ---------------------------------------------------------------------------

  size_t frame_id = frame_id_;
  size_t symbol_id = symbol_id_;
  size_t base_sc_id = base_sc_id_;

  // ---------------------------------------------------------------------------

  const size_t symbol_idx_ul = cfg_->Frame().GetULSymbolIdx(symbol_id);
  const size_t total_data_symbol_idx_ul =
      cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
  const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

  const size_t frame_slot = frame_id % kFrameWnd;

  size_t max_sc_ite =
      std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
  assert(max_sc_ite % kSCsPerCacheline == 0);

  // Step 1: Equalization
  arma::cx_float* equal_ptr = nullptr;
  if (kExportConstellation) {
    equal_ptr =
        (arma::cx_float*)(&equal_buffer_[total_data_symbol_idx_ul][base_sc_id]);
  } else {
    equal_ptr = (arma::cx_float*)(&equaled_buffer_temp_[0]);
  }
  arma::cx_fcube cub_equaled(equal_ptr, cfg_->BsAntNum(), 1, max_sc_ite, false);
  // cub_equaled.print("cub_equaled");

#ifdef __AVX512F__
  // Step 0: Prepare pointers
  arma::cx_frowvec vec_equal_0 = arma::zeros<arma::cx_frowvec>(max_sc_ite);
  arma::cx_frowvec vec_equal_1 = arma::zeros<arma::cx_frowvec>(max_sc_ite);
  complex_float* ptr_equal_0 =
      reinterpret_cast<complex_float*>(vec_equal_0.memptr());
  complex_float* ptr_equal_1 =
      reinterpret_cast<complex_float*>(vec_equal_1.memptr());

  complex_float* ul_beam_ptr = ul_beam_matrices_[frame_slot][0];
  const complex_float* ptr_a_1_1 = ul_beam_ptr;
  const complex_float* ptr_a_1_2 = ul_beam_ptr + max_sc_ite;
  const complex_float* ptr_a_2_1 = ul_beam_ptr + 2 * max_sc_ite;
  const complex_float* ptr_a_2_2 = ul_beam_ptr + 3 * max_sc_ite;

  const complex_float* data_ptr = data_buf;
  const complex_float* ptr_b_1 = data_ptr;
  const complex_float* ptr_b_2 = data_ptr + max_sc_ite;

  complex_float* ptr_c_1 = ptr_equal_0;
  complex_float* ptr_c_2 = ptr_equal_1;

  // Step 1: Equalization
  for (size_t sc_idx = 0; sc_idx < max_sc_ite; sc_idx += kSCsPerCacheline) {
    // vec_equal_0 (vec_c_1) = vec_a_1_1 % vec_b_1 + vec_a_1_2 % vec_b_2;
    // vec_equal_1 (vec_c_2) = vec_a_2_1 % vec_b_1 + vec_a_2_2 % vec_b_2;
    __m512 b_1 = _mm512_loadu_ps(ptr_b_1 + sc_idx);
    __m512 b_2 = _mm512_loadu_ps(ptr_b_2 + sc_idx);

    __m512 a_1_1 = _mm512_loadu_ps(ptr_a_1_1 + sc_idx);
    __m512 a_1_2 = _mm512_loadu_ps(ptr_a_1_2 + sc_idx);
    __m512 c_1 = CommsLib::M512ComplexCf32Mult(a_1_1, b_1, false);
    __m512 temp = CommsLib::M512ComplexCf32Mult(a_1_2, b_2, false);
    c_1 = _mm512_add_ps(c_1, temp);
    _mm512_storeu_ps(ptr_c_1 + sc_idx, c_1);

    __m512 a_2_1 = _mm512_loadu_ps(ptr_a_2_1 + sc_idx);
    __m512 a_2_2 = _mm512_loadu_ps(ptr_a_2_2 + sc_idx);
    __m512 c_2 = CommsLib::M512ComplexCf32Mult(a_2_1, b_1, false);
    temp = CommsLib::M512ComplexCf32Mult(a_2_2, b_2, false);
    c_2 = _mm512_add_ps(c_2, temp);
    _mm512_storeu_ps(ptr_c_2 + sc_idx, c_2);
  }
  // delay storing to cub_equaled to avoid frequent avx512-armadillo conversion
#else
  // Step 0: Re-arrange data
  arma::cx_float* data_ptr = (arma::cx_float*)data_buf;
  arma::cx_fvec vec_data_0(data_ptr, max_sc_ite, false);
  arma::cx_fvec vec_data_1(data_ptr + max_sc_ite, max_sc_ite, false);
  arma::cx_fcube cub_data(cfg_->BsAntNum(), 1, max_sc_ite);
  cub_data.tube(0, 0) = vec_data_0;
  cub_data.tube(1, 0) = vec_data_1;

  // cub_data.print("cub_data");
  // arma::cx_fcube cub_ul_beam(cfg_->UeAntNum(), cfg_->BsAntNum(), max_sc_ite);
  // for (size_t i = 0; i < max_sc_ite; ++i) {
  //   arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
  //     ul_beam_matrices_[frame_slot][cfg_->GetBeamScId(base_sc_id + i)]);
  //   arma::cx_fmat mat_ul_beam(ul_beam_ptr,
  //                             cfg_->UeAntNum(), cfg_->BsAntNum(), false);
  //   cub_ul_beam.slice(i) = mat_ul_beam;
  // }
  arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
      ul_beam_matrices_[frame_slot][cfg_->GetBeamScId(base_sc_id)]);
  arma::cx_fcube cub_ul_beam(ul_beam_ptr, cfg_->UeAntNum(), cfg_->BsAntNum(),
                             max_sc_ite, false);

  // Step 1: Equalization
  // for (size_t i = 0; i < max_sc_ite; ++i) {
  //   cub_equaled.slice(i) = cub_ul_beam.slice(i) * cub_data.slice(i);
  // }
  cub_equaled.tube(0, 0) = cub_ul_beam.tube(0, 0) % cub_data.tube(0, 0) +
                           cub_ul_beam.tube(0, 1) % cub_data.tube(1, 0);
  cub_equaled.tube(1, 0) = cub_ul_beam.tube(1, 0) % cub_data.tube(0, 0) +
                           cub_ul_beam.tube(1, 1) % cub_data.tube(1, 0);
#endif

  // Step 2: Phase shift calibration

  // Enable phase shift calibration
  if (cfg_->Frame().ClientUlPilotSymbols() > 0) {
    if (symbol_idx_ul == 0 && base_sc_id == 0) {
      // Reset previous frame
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[(frame_id - 1) % kFrameWnd]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(),
                                    cfg_->Frame().ClientUlPilotSymbols(),
                                    false);
      mat_phase_shift.fill(0);
    }

    // Calc new phase shift
    if (symbol_idx_ul < cfg_->Frame().ClientUlPilotSymbols()) {
#ifdef __AVX512F__
      complex_float* ue_pilot_ptr =
          reinterpret_cast<complex_float*>(cfg_->UeSpecificPilot()[0]);
      complex_float* ptr_ue_pilot_0 = ue_pilot_ptr;
      complex_float* ptr_ue_pilot_1 = ue_pilot_ptr + max_sc_ite;

      __m512 sum_0 = _mm512_setzero_ps();
      __m512 sum_1 = _mm512_setzero_ps();
      for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
        __m512 ue_0 = _mm512_loadu_ps(ptr_ue_pilot_0 + i);
        __m512 eq_0 = _mm512_loadu_ps(ptr_equal_0 + i);
        __m512 temp = CommsLib::M512ComplexCf32Conj(ue_0);
        temp = CommsLib::M512ComplexCf32Mult(temp, eq_0, false);
        sum_0 = _mm512_add_ps(sum_0, temp);

        __m512 ue_1 = _mm512_loadu_ps(ptr_ue_pilot_1 + i);
        __m512 eq_1 = _mm512_loadu_ps(ptr_equal_1 + i);
        temp = CommsLib::M512ComplexCf32Conj(ue_1);
        temp = CommsLib::M512ComplexCf32Mult(temp, eq_1, false);
        sum_1 = _mm512_add_ps(sum_1, temp);
      }

      std::complex<float>* phase_shift_ptr =
          reinterpret_cast<std::complex<float>*>(
              &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                    [symbol_idx_ul * cfg_->UeAntNum()]);
      *phase_shift_ptr += CommsLib::M512ComplexCf32Sum(sum_0);
      *(phase_shift_ptr + 1) += CommsLib::M512ComplexCf32Sum(sum_1);
#else
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                [symbol_idx_ul * cfg_->UeAntNum()]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(), 1,
                                    false);
      arma::cx_fmat mat_ue_pilot_data_ =
          ue_pilot_data_.cols(base_sc_id, base_sc_id + max_sc_ite - 1);

      // if use fvec or fcolvec, then transpose mat_ue_pilot_data_ by
      // mat_ue_pilot_data_.row(0).st()
      arma::cx_frowvec vec_tube_equal_0 = cub_equaled.tube(0, 0);
      arma::cx_frowvec vec_tube_equal_1 = cub_equaled.tube(1, 0);

      mat_phase_shift.col(0).row(0) +=
          arma::sum(vec_tube_equal_0 % arma::conj(mat_ue_pilot_data_.row(0)));
      mat_phase_shift.col(0).row(1) +=
          arma::sum(vec_tube_equal_1 % arma::conj(mat_ue_pilot_data_.row(1)));
      // for (size_t i = 0; i < max_sc_ite; ++i) {
      //   arma::cx_fmat shift_sc =
      //     cub_equaled.slice(i) % arma::conj(mat_ue_pilot_data_.col(i));
      //     // cub_equaled.slice(i) % sign(conj(mat_ue_pilot_data_.col(i)));
      //   mat_phase_shift += shift_sc;
      // }
      // sign should be able to optimize out but the result will be different
#endif
    }

    // Calculate the unit phase shift based on the first subcarrier
    // Check the special case condition to avoid reading wrong memory location
    RtAssert(cfg_->UeAntNum() == 2 &&
             cfg_->Frame().ClientUlPilotSymbols() == 2);
    if (symbol_idx_ul == cfg_->Frame().ClientUlPilotSymbols() &&
        base_sc_id == 0) {
      arma::cx_float* pilot_corr_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[frame_id % kFrameWnd]);
      arma::cx_fmat pilot_corr_mat(pilot_corr_ptr, cfg_->UeAntNum(),
                                   cfg_->Frame().ClientUlPilotSymbols(), false);
      theta_mat = arg(pilot_corr_mat);
      theta_inc = theta_mat.col(cfg_->Frame().ClientUlPilotSymbols() - 1) -
                  theta_mat.col(0);
      // theta_inc /= (float)std::max(
      //     1, static_cast<int>(cfg_->Frame().ClientUlPilotSymbols() - 1));
    }

    // Apply previously calc'ed phase shift to data
    if (symbol_idx_ul >= cfg_->Frame().ClientUlPilotSymbols()) {
      arma::fmat cur_theta = theta_mat.col(0) + (symbol_idx_ul * theta_inc);
      arma::cx_fmat mat_phase_correct =
          arma::cx_fmat(cos(-cur_theta), sin(-cur_theta));

#ifdef __AVX512F__
      __m512 ph_corr_0 = CommsLib::M512ComplexCf32Set1(mat_phase_correct(0, 0));
      __m512 ph_corr_1 = CommsLib::M512ComplexCf32Set1(mat_phase_correct(1, 0));

      // CommsLib::PrintM512ComplexCf32(ph_corr_0);
      // CommsLib::PrintM512ComplexCf32(ph_corr_1);
      for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
        __m512 eq_0 = _mm512_loadu_ps(ptr_equal_0 + i);
        __m512 eq_1 = _mm512_loadu_ps(ptr_equal_1 + i);
        eq_0 = CommsLib::M512ComplexCf32Mult(eq_0, ph_corr_0, false);
        eq_1 = CommsLib::M512ComplexCf32Mult(eq_1, ph_corr_1, false);
        _mm512_storeu_ps(ptr_equal_0 + i, eq_0);
        _mm512_storeu_ps(ptr_equal_1 + i, eq_1);
      }
#else
      cub_equaled.each_slice() %= mat_phase_correct;
#endif
    }
  }

#ifdef __AVX512F__
  // store back to Armadillo matrix
  cub_equaled.tube(0, 0) = vec_equal_0;
  cub_equaled.tube(1, 0) = vec_equal_1;
#endif
}

/*
 * Test case:
 *   - Vectorization for 4x4 MIMO.
 *   - THE DEFAULT CASE FOR 2X2 IMPLEMENTATION.
 */
void equal_vec_4x4_complex(
    Config* cfg_, Table<complex_float>& data_buffer_,
    Table<complex_float>& equal_buffer_,
    Table<complex_float>& ue_spec_pilot_buffer_,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_beam_matrices_,
    size_t frame_id_, size_t symbol_id_, size_t base_sc_id_) {
  RtAssert(cfg_->BsAntNum() == 4 && cfg_->UeAntNum() == 4,
           "Correctness is only guaranteed in special case of antenna 4x4!");
  RtAssert(kExportConstellation == true,
           "Set kExportConstellation to evaluate the correctness "
           " (export equal_buffer_)");
  RtAssert(kUsePartialTrans == false,
           "The test case of 4x4 MIMO is not compatible with kUsePartialTrans");

  // ---------------------------------------------------------------------------
  // Class definition of DoDemul
  // ---------------------------------------------------------------------------

  // Intermediate buffers for equalized data
  complex_float* data_gather_buffer_;
  complex_float* equaled_buffer_temp_;
  arma::cx_fmat ue_pilot_data_;

  // For efficient phase shift calibration
  static arma::fmat theta_mat;
  static arma::fmat theta_inc;

  // ---------------------------------------------------------------------------
  // Constructor of DoDemul
  // ---------------------------------------------------------------------------

  data_gather_buffer_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxAntennas * sizeof(complex_float)));

  equaled_buffer_temp_ =
      static_cast<complex_float*>(Agora_memory::PaddedAlignedAlloc(
          Agora_memory::Alignment_t::kAlign64,
          cfg_->DemulBlockSize() * kMaxUEs * sizeof(complex_float)));

  // phase offset calibration data
  arma::cx_float* ue_pilot_ptr =
      reinterpret_cast<arma::cx_float*>(cfg_->UeSpecificPilot()[0]);
  arma::cx_fmat mat_pilot_data(ue_pilot_ptr, cfg_->OfdmDataNum(),
                               cfg_->UeAntNum(), false);
  ue_pilot_data_ = mat_pilot_data.st();

  // ---------------------------------------------------------------------------
  // First part of DoDemul: equalization + phase shift calibration
  // ---------------------------------------------------------------------------

  size_t frame_id = frame_id_;
  size_t symbol_id = symbol_id_;
  size_t base_sc_id = base_sc_id_;

  // ---------------------------------------------------------------------------

  const size_t symbol_idx_ul = cfg_->Frame().GetULSymbolIdx(symbol_id);
  const size_t total_data_symbol_idx_ul =
      cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
  const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

  const size_t frame_slot = frame_id % kFrameWnd;

  size_t max_sc_ite =
      std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
  assert(max_sc_ite % kSCsPerCacheline == 0);

  arma::cx_float* equal_ptr = nullptr;
  if (kExportConstellation) {
    equal_ptr =
        (arma::cx_float*)(&equal_buffer_[total_data_symbol_idx_ul][base_sc_id]);
  } else {
    equal_ptr = (arma::cx_float*)(&equaled_buffer_temp_[0]);
  }
  arma::cx_fcube cub_equaled(equal_ptr, cfg_->BsAntNum(), 1, max_sc_ite, false);
  // cub_equaled.print("cub_equaled");

#ifdef __AVX512F__
  // Step 0: Prepare pointers
  arma::cx_frowvec vec_equal_0 = arma::zeros<arma::cx_frowvec>(max_sc_ite);
  arma::cx_frowvec vec_equal_1 = arma::zeros<arma::cx_frowvec>(max_sc_ite);
  arma::cx_frowvec vec_equal_2 = arma::zeros<arma::cx_frowvec>(max_sc_ite);
  arma::cx_frowvec vec_equal_3 = arma::zeros<arma::cx_frowvec>(max_sc_ite);
  complex_float* ptr_equal_0 =
      reinterpret_cast<complex_float*>(vec_equal_0.memptr());
  complex_float* ptr_equal_1 =
      reinterpret_cast<complex_float*>(vec_equal_1.memptr());
  complex_float* ptr_equal_2 =
      reinterpret_cast<complex_float*>(vec_equal_2.memptr());
  complex_float* ptr_equal_3 =
      reinterpret_cast<complex_float*>(vec_equal_3.memptr());

  // Prepare operand pointers for core equalization
  complex_float* ul_beam_ptr = ul_beam_matrices_[frame_slot][0];
  const complex_float* ptr_a_1_1 = ul_beam_ptr;
  const complex_float* ptr_a_1_2 = ul_beam_ptr + max_sc_ite;
  const complex_float* ptr_a_1_3 = ul_beam_ptr + 2 * max_sc_ite;
  const complex_float* ptr_a_1_4 = ul_beam_ptr + 3 * max_sc_ite;
  const complex_float* ptr_a_2_1 = ul_beam_ptr + 4 * max_sc_ite;
  const complex_float* ptr_a_2_2 = ul_beam_ptr + 5 * max_sc_ite;
  const complex_float* ptr_a_2_3 = ul_beam_ptr + 6 * max_sc_ite;
  const complex_float* ptr_a_2_4 = ul_beam_ptr + 7 * max_sc_ite;
  const complex_float* ptr_a_3_1 = ul_beam_ptr + 8 * max_sc_ite;
  const complex_float* ptr_a_3_2 = ul_beam_ptr + 9 * max_sc_ite;
  const complex_float* ptr_a_3_3 = ul_beam_ptr + 10 * max_sc_ite;
  const complex_float* ptr_a_3_4 = ul_beam_ptr + 11 * max_sc_ite;
  const complex_float* ptr_a_4_1 = ul_beam_ptr + 12 * max_sc_ite;
  const complex_float* ptr_a_4_2 = ul_beam_ptr + 13 * max_sc_ite;
  const complex_float* ptr_a_4_3 = ul_beam_ptr + 14 * max_sc_ite;
  const complex_float* ptr_a_4_4 = ul_beam_ptr + 15 * max_sc_ite;

  const complex_float* data_ptr = data_buf;
  const complex_float* ptr_b_1 = data_ptr;
  const complex_float* ptr_b_2 = data_ptr + max_sc_ite;
  const complex_float* ptr_b_3 = data_ptr + 2 * max_sc_ite;
  const complex_float* ptr_b_4 = data_ptr + 3 * max_sc_ite;

  complex_float* ptr_c_1 = ptr_equal_0;
  complex_float* ptr_c_2 = ptr_equal_1;
  complex_float* ptr_c_3 = ptr_equal_2;
  complex_float* ptr_c_4 = ptr_equal_3;

  // Step 1: Equalization
  // Each AVX512 register can hold
  //   16 floats = 8 complex floats = 1 kSCsPerCacheline
  for (size_t sc_idx = 0; sc_idx < max_sc_ite; sc_idx += kSCsPerCacheline) {
    __m512 b_1 = _mm512_loadu_ps(ptr_b_1 + sc_idx);
    __m512 b_2 = _mm512_loadu_ps(ptr_b_2 + sc_idx);
    __m512 b_3 = _mm512_loadu_ps(ptr_b_3 + sc_idx);
    __m512 b_4 = _mm512_loadu_ps(ptr_b_4 + sc_idx);

    __m512 a_1_1 = _mm512_loadu_ps(ptr_a_1_1 + sc_idx);
    __m512 a_1_2 = _mm512_loadu_ps(ptr_a_1_2 + sc_idx);
    __m512 a_1_3 = _mm512_loadu_ps(ptr_a_1_3 + sc_idx);
    __m512 a_1_4 = _mm512_loadu_ps(ptr_a_1_4 + sc_idx);
    __m512 temp_1 = CommsLib::M512ComplexCf32Mult(a_1_1, b_1, false);
    __m512 temp_2 = CommsLib::M512ComplexCf32Mult(a_1_2, b_2, false);
    __m512 temp_3 = CommsLib::M512ComplexCf32Mult(a_1_3, b_3, false);
    __m512 temp_4 = CommsLib::M512ComplexCf32Mult(a_1_4, b_4, false);
    temp_1 = _mm512_add_ps(temp_1, temp_2);
    temp_3 = _mm512_add_ps(temp_3, temp_4);
    __m512 c_1 = _mm512_add_ps(temp_1, temp_3);
    _mm512_storeu_ps(ptr_c_1 + sc_idx, c_1);

    __m512 a_2_1 = _mm512_loadu_ps(ptr_a_2_1 + sc_idx);
    __m512 a_2_2 = _mm512_loadu_ps(ptr_a_2_2 + sc_idx);
    __m512 a_2_3 = _mm512_loadu_ps(ptr_a_2_3 + sc_idx);
    __m512 a_2_4 = _mm512_loadu_ps(ptr_a_2_4 + sc_idx);
    temp_1 = CommsLib::M512ComplexCf32Mult(a_2_1, b_1, false);
    temp_2 = CommsLib::M512ComplexCf32Mult(a_2_2, b_2, false);
    temp_3 = CommsLib::M512ComplexCf32Mult(a_2_3, b_3, false);
    temp_4 = CommsLib::M512ComplexCf32Mult(a_2_4, b_4, false);
    temp_1 = _mm512_add_ps(temp_1, temp_2);
    temp_3 = _mm512_add_ps(temp_3, temp_4);
    __m512 c_2 = _mm512_add_ps(temp_1, temp_3);
    _mm512_storeu_ps(ptr_c_2 + sc_idx, c_2);

    __m512 a_3_1 = _mm512_loadu_ps(ptr_a_3_1 + sc_idx);
    __m512 a_3_2 = _mm512_loadu_ps(ptr_a_3_2 + sc_idx);
    __m512 a_3_3 = _mm512_loadu_ps(ptr_a_3_3 + sc_idx);
    __m512 a_3_4 = _mm512_loadu_ps(ptr_a_3_4 + sc_idx);
    temp_1 = CommsLib::M512ComplexCf32Mult(a_3_1, b_1, false);
    temp_2 = CommsLib::M512ComplexCf32Mult(a_3_2, b_2, false);
    temp_3 = CommsLib::M512ComplexCf32Mult(a_3_3, b_3, false);
    temp_4 = CommsLib::M512ComplexCf32Mult(a_3_4, b_4, false);
    temp_1 = _mm512_add_ps(temp_1, temp_2);
    temp_3 = _mm512_add_ps(temp_3, temp_4);
    __m512 c_3 = _mm512_add_ps(temp_1, temp_3);
    _mm512_storeu_ps(ptr_c_3 + sc_idx, c_3);

    __m512 a_4_1 = _mm512_loadu_ps(ptr_a_4_1 + sc_idx);
    __m512 a_4_2 = _mm512_loadu_ps(ptr_a_4_2 + sc_idx);
    __m512 a_4_3 = _mm512_loadu_ps(ptr_a_4_3 + sc_idx);
    __m512 a_4_4 = _mm512_loadu_ps(ptr_a_4_4 + sc_idx);
    temp_1 = CommsLib::M512ComplexCf32Mult(a_4_1, b_1, false);
    temp_2 = CommsLib::M512ComplexCf32Mult(a_4_2, b_2, false);
    temp_3 = CommsLib::M512ComplexCf32Mult(a_4_3, b_3, false);
    temp_4 = CommsLib::M512ComplexCf32Mult(a_4_4, b_4, false);
    temp_1 = _mm512_add_ps(temp_1, temp_2);
    temp_3 = _mm512_add_ps(temp_3, temp_4);
    __m512 c_4 = _mm512_add_ps(temp_1, temp_3);
    _mm512_storeu_ps(ptr_c_4 + sc_idx, c_4);
  }
#else
  // Step 0: Re-arrange data
  arma::cx_float* data_ptr = (arma::cx_float*)data_buf;
  arma::cx_fvec vec_data_0(data_ptr, max_sc_ite, false);
  arma::cx_fvec vec_data_1(data_ptr + max_sc_ite, max_sc_ite, false);
  arma::cx_fvec vec_data_2(data_ptr + 2 * max_sc_ite, max_sc_ite, false);
  arma::cx_fvec vec_data_3(data_ptr + 3 * max_sc_ite, max_sc_ite, false);
  arma::cx_fcube cub_data(cfg_->BsAntNum(), 1, max_sc_ite);
  cub_data.tube(0, 0) = vec_data_0;
  cub_data.tube(1, 0) = vec_data_1;
  cub_data.tube(2, 0) = vec_data_2;
  cub_data.tube(3, 0) = vec_data_3;

  // arma::cx_fcube cub_ul_beam(cfg_->UeAntNum(), cfg_->BsAntNum(), max_sc_ite);
  // for (size_t i = 0; i < max_sc_ite; ++i) {
  //   arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
  //     ul_beam_matrices_[frame_slot][cfg_->GetBeamScId(base_sc_id + i)]);
  //   arma::cx_fmat mat_ul_beam(ul_beam_ptr,
  //                             cfg_->UeAntNum(), cfg_->BsAntNum(), false);
  //   cub_ul_beam.slice(i) = mat_ul_beam;
  // }
  arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
      ul_beam_matrices_[frame_slot][cfg_->GetBeamScId(base_sc_id)]);
  arma::cx_fcube cub_ul_beam(ul_beam_ptr, cfg_->UeAntNum(), cfg_->BsAntNum(),
                             max_sc_ite, false);

  // Step 1: Equalization
  // for (size_t i = 0; i < max_sc_ite; ++i) {
  //   cub_equaled.slice(i) = cub_ul_beam.slice(i) * cub_data.slice(i);
  // }
  cub_equaled.tube(0, 0) = cub_ul_beam.tube(0, 0) % cub_data.tube(0, 0) +
                           cub_ul_beam.tube(0, 1) % cub_data.tube(1, 0) +
                           cub_ul_beam.tube(0, 2) % cub_data.tube(2, 0) +
                           cub_ul_beam.tube(0, 3) % cub_data.tube(3, 0);
  cub_equaled.tube(1, 0) = cub_ul_beam.tube(1, 0) % cub_data.tube(0, 0) +
                           cub_ul_beam.tube(1, 1) % cub_data.tube(1, 0) +
                           cub_ul_beam.tube(1, 2) % cub_data.tube(2, 0) +
                           cub_ul_beam.tube(1, 3) % cub_data.tube(3, 0);
  cub_equaled.tube(2, 0) = cub_ul_beam.tube(2, 0) % cub_data.tube(0, 0) +
                           cub_ul_beam.tube(2, 1) % cub_data.tube(1, 0) +
                           cub_ul_beam.tube(2, 2) % cub_data.tube(2, 0) +
                           cub_ul_beam.tube(2, 3) % cub_data.tube(3, 0);
  cub_equaled.tube(3, 0) = cub_ul_beam.tube(3, 0) % cub_data.tube(0, 0) +
                           cub_ul_beam.tube(3, 1) % cub_data.tube(1, 0) +
                           cub_ul_beam.tube(3, 2) % cub_data.tube(2, 0) +
                           cub_ul_beam.tube(3, 3) % cub_data.tube(3, 0);
#endif

  // Step 2: Phase shift calibration

  // Enable phase shift calibration
  if (cfg_->Frame().ClientUlPilotSymbols() > 0) {
    if (symbol_idx_ul == 0 && base_sc_id == 0) {
      // Reset previous frame
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[(frame_id - 1) % kFrameWnd]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(),
                                    cfg_->Frame().ClientUlPilotSymbols(),
                                    false);
      mat_phase_shift.fill(0);
    }

    // Calc new phase shift
    if (symbol_idx_ul < cfg_->Frame().ClientUlPilotSymbols()) {
#ifdef __AVX512F__
      complex_float* ue_pilot_ptr =
          reinterpret_cast<complex_float*>(cfg_->UeSpecificPilot()[0]);
      complex_float* ptr_ue_pilot_0 = ue_pilot_ptr;
      complex_float* ptr_ue_pilot_1 = ue_pilot_ptr + max_sc_ite;
      complex_float* ptr_ue_pilot_2 = ue_pilot_ptr + 2 * max_sc_ite;
      complex_float* ptr_ue_pilot_3 = ue_pilot_ptr + 3 * max_sc_ite;

      __m512 sum_0 = _mm512_setzero_ps();
      __m512 sum_1 = _mm512_setzero_ps();
      __m512 sum_2 = _mm512_setzero_ps();
      __m512 sum_3 = _mm512_setzero_ps();
      for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
        __m512 ue_0 = _mm512_loadu_ps(ptr_ue_pilot_0 + i);
        __m512 eq_0 = _mm512_loadu_ps(ptr_equal_0 + i);
        __m512 temp = CommsLib::M512ComplexCf32Conj(ue_0);
        temp = CommsLib::M512ComplexCf32Mult(temp, eq_0, false);
        sum_0 = _mm512_add_ps(sum_0, temp);

        __m512 ue_1 = _mm512_loadu_ps(ptr_ue_pilot_1 + i);
        __m512 eq_1 = _mm512_loadu_ps(ptr_equal_1 + i);
        temp = CommsLib::M512ComplexCf32Conj(ue_1);
        temp = CommsLib::M512ComplexCf32Mult(temp, eq_1, false);
        sum_1 = _mm512_add_ps(sum_1, temp);

        __m512 ue_2 = _mm512_loadu_ps(ptr_ue_pilot_2 + i);
        __m512 eq_2 = _mm512_loadu_ps(ptr_equal_2 + i);
        temp = CommsLib::M512ComplexCf32Conj(ue_2);
        temp = CommsLib::M512ComplexCf32Mult(temp, eq_2, false);
        sum_2 = _mm512_add_ps(sum_2, temp);

        __m512 ue_3 = _mm512_loadu_ps(ptr_ue_pilot_3 + i);
        __m512 eq_3 = _mm512_loadu_ps(ptr_equal_3 + i);
        temp = CommsLib::M512ComplexCf32Conj(ue_3);
        temp = CommsLib::M512ComplexCf32Mult(temp, eq_3, false);
        sum_3 = _mm512_add_ps(sum_3, temp);
      }

      std::complex<float>* phase_shift_ptr =
          reinterpret_cast<std::complex<float>*>(
              &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                    [symbol_idx_ul * cfg_->UeAntNum()]);
      *phase_shift_ptr += CommsLib::M512ComplexCf32Sum(sum_0);
      *(phase_shift_ptr + 1) += CommsLib::M512ComplexCf32Sum(sum_1);
      *(phase_shift_ptr + 2) += CommsLib::M512ComplexCf32Sum(sum_2);
      *(phase_shift_ptr + 3) += CommsLib::M512ComplexCf32Sum(sum_3);
#else
      arma::cx_float* phase_shift_ptr = reinterpret_cast<arma::cx_float*>(
          &ue_spec_pilot_buffer_[frame_id % kFrameWnd]
                                [symbol_idx_ul * cfg_->UeAntNum()]);
      arma::cx_fmat mat_phase_shift(phase_shift_ptr, cfg_->UeAntNum(), 1,
                                    false);
      arma::cx_fmat mat_ue_pilot_data_ =
          ue_pilot_data_.cols(base_sc_id, base_sc_id + max_sc_ite - 1);

      // if use fvec or fcolvec, then transpose mat_ue_pilot_data_ by
      // mat_ue_pilot_data_.row(0).st()
      arma::cx_frowvec vec_tube_equal_0 = cub_equaled.tube(0, 0);
      arma::cx_frowvec vec_tube_equal_1 = cub_equaled.tube(1, 0);
      arma::cx_frowvec vec_tube_equal_2 = cub_equaled.tube(2, 0);
      arma::cx_frowvec vec_tube_equal_3 = cub_equaled.tube(3, 0);

      mat_phase_shift.col(0).row(0) +=
          sum(vec_tube_equal_0 % arma::conj(mat_ue_pilot_data_.row(0)));
      mat_phase_shift.col(0).row(1) +=
          sum(vec_tube_equal_1 % arma::conj(mat_ue_pilot_data_.row(1)));
      mat_phase_shift.col(0).row(2) +=
          sum(vec_tube_equal_2 % arma::conj(mat_ue_pilot_data_.row(2)));
      mat_phase_shift.col(0).row(3) +=
          sum(vec_tube_equal_3 % arma::conj(mat_ue_pilot_data_.row(3)));
      // for (size_t i = 0; i < max_sc_ite; ++i) {
      //   arma::cx_fmat shift_sc =
      //     cub_equaled.slice(i) % arma::conj(mat_ue_pilot_data_.col(i));
      //     // cub_equaled.slice(i) % sign(conj(mat_ue_pilot_data_.col(i)));
      //   mat_phase_shift += shift_sc;
      // }
      // sign should be able to optimize out but the result will be different
#endif
    }

    // Calculate the unit phase shift based on the first subcarrier
    // Check the special case condition to avoid reading wrong memory location
    RtAssert(cfg_->UeAntNum() == 4 &&
             cfg_->Frame().ClientUlPilotSymbols() == 2);
    if (symbol_idx_ul == cfg_->Frame().ClientUlPilotSymbols() &&
        base_sc_id == 0) {
      arma::cx_float* pilot_corr_ptr = reinterpret_cast<arma::cx_float*>(
          ue_spec_pilot_buffer_[frame_id % kFrameWnd]);
      arma::cx_fmat pilot_corr_mat(pilot_corr_ptr, cfg_->UeAntNum(),
                                   cfg_->Frame().ClientUlPilotSymbols(), false);
      theta_mat = arg(pilot_corr_mat);
      theta_inc = theta_mat.col(cfg_->Frame().ClientUlPilotSymbols() - 1) -
                  theta_mat.col(0);
      // theta_inc /= (float)std::max(
      //     1, static_cast<int>(cfg_->Frame().ClientUlPilotSymbols() - 1));
    }

    // Apply previously calc'ed phase shift to data
    if (symbol_idx_ul >= cfg_->Frame().ClientUlPilotSymbols()) {
      arma::fmat cur_theta = theta_mat.col(0) + (symbol_idx_ul * theta_inc);
      arma::cx_fmat mat_phase_correct =
          arma::cx_fmat(cos(-cur_theta), sin(-cur_theta));

#ifdef __AVX512F__
      __m512 ph_corr_0 = CommsLib::M512ComplexCf32Set1(mat_phase_correct(0, 0));
      __m512 ph_corr_1 = CommsLib::M512ComplexCf32Set1(mat_phase_correct(1, 0));
      __m512 ph_corr_2 = CommsLib::M512ComplexCf32Set1(mat_phase_correct(2, 0));
      __m512 ph_corr_3 = CommsLib::M512ComplexCf32Set1(mat_phase_correct(3, 0));

      for (size_t i = 0; i < max_sc_ite; i += kSCsPerCacheline) {
        __m512 eq_0 = _mm512_loadu_ps(ptr_equal_0 + i);
        __m512 eq_1 = _mm512_loadu_ps(ptr_equal_1 + i);
        __m512 eq_2 = _mm512_loadu_ps(ptr_equal_2 + i);
        __m512 eq_3 = _mm512_loadu_ps(ptr_equal_3 + i);
        eq_0 = CommsLib::M512ComplexCf32Mult(eq_0, ph_corr_0, false);
        eq_1 = CommsLib::M512ComplexCf32Mult(eq_1, ph_corr_1, false);
        eq_2 = CommsLib::M512ComplexCf32Mult(eq_2, ph_corr_2, false);
        eq_3 = CommsLib::M512ComplexCf32Mult(eq_3, ph_corr_3, false);
        _mm512_storeu_ps(ptr_equal_0 + i, eq_0);
        _mm512_storeu_ps(ptr_equal_1 + i, eq_1);
        _mm512_storeu_ps(ptr_equal_2 + i, eq_2);
        _mm512_storeu_ps(ptr_equal_3 + i, eq_3);
      }
#else
      cub_equaled.each_slice() %= mat_phase_correct;
#endif
    }
  }

#ifdef __AVX512F__
  // store back to Armadillo matrix
  cub_equaled.tube(0, 0) = vec_equal_0;
  cub_equaled.tube(1, 0) = vec_equal_1;
  cub_equaled.tube(2, 0) = vec_equal_2;
  cub_equaled.tube(3, 0) = vec_equal_3;
#endif
}

/******************************************************************************/

/*
 * Select the test case by reading the config.
 */
void equal_test(
    Config* cfg_, Table<complex_float>& data_buffer_,
    Table<complex_float>& equal_buffer_,
    Table<complex_float>& ue_spec_pilot_buffer_,
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float>& ul_beam_matrices_,
    size_t frame_id_, size_t symbol_id_, size_t base_sc_id_) {
  RtAssert(cfg_->BsAntNum() == cfg_->UeAntNum(),
           "Only support square MIMO matrix!");

  if (cfg_->BsAntNum() == 1 && cfg_->UeAntNum() == 1) {
    equal_vec_1x1_complex(cfg_, data_buffer_, equal_buffer_,
                          ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id_,
                          symbol_id_, base_sc_id_);
    // equal_vec_1x1_real(cfg_, data_buffer_, equal_buffer_,
    //   ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id_, symbol_id_,
    //   base_sc_id_);
  }

  if (cfg_->BsAntNum() == 2 && cfg_->UeAntNum() == 2) {
#ifdef __AVX512F__
    // Simulate FFT output: Rearrange beam matrix. Serialize each entry to
    // vectors and write to the memory sequentially.
    size_t frame_slot = frame_id_ % kFrameWnd;
    arma::cx_float* ul_beam_ptr =
        reinterpret_cast<arma::cx_float*>(ul_beam_matrices_[frame_slot][0]);
    arma::cx_fcube cub_ul_beam(ul_beam_ptr, cfg_->UeAntNum(), cfg_->BsAntNum(),
                               cfg_->DemulBlockSize(), false);

    // Allocate memory only
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_seq_2x2;
    ul_beam_matrices_seq_2x2.RandAllocCxFloat(cfg_->BsAntNum() *
                                              cfg_->SpatialStreamsNum());

    // View the memory as plain space and get pointers
    arma::cx_float* ul_beam_seq_ptr = reinterpret_cast<arma::cx_float*>(
        ul_beam_matrices_seq_2x2[frame_slot][0]);
    arma::cx_fvec vec_ul_beam_seq_0(ul_beam_seq_ptr, cfg_->DemulBlockSize(),
                                    false);
    arma::cx_fvec vec_ul_beam_seq_1(ul_beam_seq_ptr + cfg_->DemulBlockSize(),
                                    cfg_->DemulBlockSize(), false);
    arma::cx_fvec vec_ul_beam_seq_2(
        ul_beam_seq_ptr + 2 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_3(
        ul_beam_seq_ptr + 3 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);

    // Copy data from Armadillo matrix to plain space sequentially
    vec_ul_beam_seq_0 = cub_ul_beam.tube(0, 0);
    vec_ul_beam_seq_1 = cub_ul_beam.tube(0, 1);
    vec_ul_beam_seq_2 = cub_ul_beam.tube(1, 0);
    vec_ul_beam_seq_3 = cub_ul_beam.tube(1, 1);

    equal_vec_2x2_complex(cfg_, data_buffer_, equal_buffer_,
                          ue_spec_pilot_buffer_, ul_beam_matrices_seq_2x2,
                          frame_id_, symbol_id_, base_sc_id_);
#else
    equal_vec_2x2_complex(cfg_, data_buffer_, equal_buffer_,
                          ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id_,
                          symbol_id_, base_sc_id_);
#endif
  }

  if (cfg_->BsAntNum() == 4 && cfg_->UeAntNum() == 4) {
#ifdef __AVX512F__
    // Simulate FFT output: Rearrange beam matrix. Serialize each entry to
    // vectors and write to the memory sequentially.
    size_t frame_slot = frame_id_ % kFrameWnd;
    arma::cx_float* ul_beam_ptr =
        reinterpret_cast<arma::cx_float*>(ul_beam_matrices_[frame_slot][0]);
    arma::cx_fcube cub_ul_beam(ul_beam_ptr, cfg_->UeAntNum(), cfg_->BsAntNum(),
                               cfg_->DemulBlockSize(), false);

    // Allocate memory only
    PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_seq_4x4;
    ul_beam_matrices_seq_4x4.RandAllocCxFloat(cfg_->BsAntNum() *
                                              cfg_->SpatialStreamsNum());

    // View the memory as plain space and get pointers
    arma::cx_float* ul_beam_seq_ptr = reinterpret_cast<arma::cx_float*>(
        ul_beam_matrices_seq_4x4[frame_slot][0]);
    arma::cx_fvec vec_ul_beam_seq_0_0(ul_beam_seq_ptr, cfg_->DemulBlockSize(),
                                      false);
    arma::cx_fvec vec_ul_beam_seq_0_1(ul_beam_seq_ptr + cfg_->DemulBlockSize(),
                                      cfg_->DemulBlockSize(), false);
    arma::cx_fvec vec_ul_beam_seq_0_2(
        ul_beam_seq_ptr + 2 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_0_3(
        ul_beam_seq_ptr + 3 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_1_0(
        ul_beam_seq_ptr + 4 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_1_1(
        ul_beam_seq_ptr + 5 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_1_2(
        ul_beam_seq_ptr + 6 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_1_3(
        ul_beam_seq_ptr + 7 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_2_0(
        ul_beam_seq_ptr + 8 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_2_1(
        ul_beam_seq_ptr + 9 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_2_2(
        ul_beam_seq_ptr + 10 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_2_3(
        ul_beam_seq_ptr + 11 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_3_0(
        ul_beam_seq_ptr + 12 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_3_1(
        ul_beam_seq_ptr + 13 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_3_2(
        ul_beam_seq_ptr + 14 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);
    arma::cx_fvec vec_ul_beam_seq_3_3(
        ul_beam_seq_ptr + 15 * cfg_->DemulBlockSize(), cfg_->DemulBlockSize(),
        false);

    // Copy data from Armadillo matrix to plain space sequentially
    vec_ul_beam_seq_0_0 = cub_ul_beam.tube(0, 0);
    vec_ul_beam_seq_0_1 = cub_ul_beam.tube(0, 1);
    vec_ul_beam_seq_0_2 = cub_ul_beam.tube(0, 2);
    vec_ul_beam_seq_0_3 = cub_ul_beam.tube(0, 3);
    vec_ul_beam_seq_1_0 = cub_ul_beam.tube(1, 0);
    vec_ul_beam_seq_1_1 = cub_ul_beam.tube(1, 1);
    vec_ul_beam_seq_1_2 = cub_ul_beam.tube(1, 2);
    vec_ul_beam_seq_1_3 = cub_ul_beam.tube(1, 3);
    vec_ul_beam_seq_2_0 = cub_ul_beam.tube(2, 0);
    vec_ul_beam_seq_2_1 = cub_ul_beam.tube(2, 1);
    vec_ul_beam_seq_2_2 = cub_ul_beam.tube(2, 2);
    vec_ul_beam_seq_2_3 = cub_ul_beam.tube(2, 3);
    vec_ul_beam_seq_3_0 = cub_ul_beam.tube(3, 0);
    vec_ul_beam_seq_3_1 = cub_ul_beam.tube(3, 1);
    vec_ul_beam_seq_3_2 = cub_ul_beam.tube(3, 2);
    vec_ul_beam_seq_3_3 = cub_ul_beam.tube(3, 3);

    equal_vec_4x4_complex(cfg_, data_buffer_, equal_buffer_,
                          ue_spec_pilot_buffer_, ul_beam_matrices_seq_4x4,
                          frame_id_, symbol_id_, base_sc_id_);
#else
    equal_vec_4x4_complex(cfg_, data_buffer_, equal_buffer_,
                          ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id_,
                          symbol_id_, base_sc_id_);
#endif
  }
}

/******************************************************************************/

TEST(TestEqual, OrgSingle) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_;
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // From agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;
  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);
  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------

  equal_org(cfg_.get(), data_buffer_, equal_buffer_, ue_spec_pilot_buffer_,
            ul_beam_matrices_, 0, cfg_->Frame().NumPilotSyms(), 0);
}

TEST(TestEqual, OrgLoop) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_;
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // From agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;
  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);
  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------

  for (size_t frame_id = 0; frame_id <= kFrameWnd; ++frame_id) {
    for (size_t symbol_id = cfg_->Frame().NumPilotSyms();
         symbol_id < cfg_->Frame().NumULSyms(); ++symbol_id) {
      for (size_t base_sc_id = 0; base_sc_id < cfg_->OfdmDataNum();
           base_sc_id += cfg_->DemulBlockSize()) {
        equal_org(cfg_.get(), data_buffer_, equal_buffer_,
                  ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id, symbol_id,
                  base_sc_id);
      }
    }
  }
}

TEST(TestEqual, IfcondSingle) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_;
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // From agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;
  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);
  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------

  equal_ifcond(cfg_.get(), data_buffer_, equal_buffer_, ue_spec_pilot_buffer_,
               ul_beam_matrices_, 0, cfg_->Frame().NumPilotSyms(), 0);
}

TEST(TestEqual, IfcondLoop) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_;
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // From agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;
  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);
  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------

  for (size_t frame_id = 0; frame_id <= kFrameWnd; ++frame_id) {
    for (size_t symbol_id = cfg_->Frame().NumPilotSyms();
         symbol_id < cfg_->Frame().NumULSyms(); ++symbol_id) {
      for (size_t base_sc_id = 0; base_sc_id < cfg_->OfdmDataNum();
           base_sc_id += cfg_->DemulBlockSize()) {
        equal_ifcond(cfg_.get(), data_buffer_, equal_buffer_,
                     ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id,
                     symbol_id, base_sc_id);
      }
    }
  }
}

TEST(TestEqual, TestSingle) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_;
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // From agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;
  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);
  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------

  equal_test(cfg_.get(), data_buffer_, equal_buffer_, ue_spec_pilot_buffer_,
             ul_beam_matrices_, 0, cfg_->Frame().NumPilotSyms(), 0);
}

TEST(TestEqual, TestLoop) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_;
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // From agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;
  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);
  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------

  for (size_t frame_id = 0; frame_id <= kFrameWnd; ++frame_id) {
    for (size_t symbol_id = cfg_->Frame().NumPilotSyms();
         symbol_id < cfg_->Frame().NumULSyms(); ++symbol_id) {
      for (size_t base_sc_id = 0; base_sc_id < cfg_->OfdmDataNum();
           base_sc_id += cfg_->DemulBlockSize()) {
        equal_test(cfg_.get(), data_buffer_, equal_buffer_,
                   ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id,
                   symbol_id, base_sc_id);
      }
    }
  }
}

TEST(TestEqual, CorrectnessSingle) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> equal_buffer_test1_;
  Table<complex_float> equal_buffer_test2_;
  Table<complex_float> ue_spec_pilot_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_test1_;
  Table<complex_float> ue_spec_pilot_buffer_test2_;
  // PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_(
  //   kFrameWnd, cfg_->OfdmDataNum(), cfg_->BsAntNum() * cfg_->SpatialStreamsNum());
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // Similar to agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;

  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);

  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  equal_buffer_test1_ = equal_buffer_;
  equal_buffer_test2_ = equal_buffer_;

  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_test1_ = ue_spec_pilot_buffer_;
  ue_spec_pilot_buffer_test2_ = ue_spec_pilot_buffer_;

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------

  // 1x1 case:
  // Errors arise from symbol_id >= 4 or symbol_id = 0 (pilot should do nothing)
  // The general frame schedule would be PUUU...GGG..., and the doer uses the
  // first three symbols to calculate the phase tracking. Since we are calling
  // the functions individually, starting from 4th symbol the phase tracking
  // values are not calculated properly.
  // 2x2 case: symbol_id = 4
  RtAssert((cfg_->BsAntNum() == 1 && cfg_->UeAntNum() == 1) ||
               (cfg_->BsAntNum() == 2 && cfg_->UeAntNum() == 2) ||
               (cfg_->BsAntNum() == 4 && cfg_->UeAntNum() == 4),
           "Correctness is only guaranteed in special case of antenna"
           " 1x1/2x2/4x4!");
  size_t symbol_id = cfg_->Frame().NumPilotSyms() + 1;  // SISO: 3, 2x2: 4
  size_t frame_id = 0, base_sc_id = 0;

  printf("--------------------------------------------------\n");
  equal_org(cfg_.get(), data_buffer_, equal_buffer_, ue_spec_pilot_buffer_,
            ul_beam_matrices_, frame_id, symbol_id, base_sc_id);
  printf("--------------------------------------------------\n");
  equal_ifcond(cfg_.get(), data_buffer_, equal_buffer_test1_,
               ue_spec_pilot_buffer_test1_, ul_beam_matrices_, frame_id,
               symbol_id, base_sc_id);
  printf("--------------------------------------------------\n");
  equal_test(cfg_.get(), data_buffer_, equal_buffer_test2_,
             ue_spec_pilot_buffer_test2_, ul_beam_matrices_, frame_id,
             symbol_id, base_sc_id);
  printf("--------------------------------------------------\n");

  // ---------------------------------------------------------------------------
  // Test the results
  // ---------------------------------------------------------------------------

  // Debug for single element. Note the func only process the first 64
  // subcarriers (dim2) for the first uplink symbol (dim1).
  // Dim1: the index of uplink symbols. The 0th and 1st uplink symbol is used to
  //       calculate the phase shift, so the 2nd one is the first to be calibrated.
  // Dim2: determined by demul_block_size (64 by default).
  printf("size of equal_buffer_ = %ld x %ld\n", equal_buffer_.Dim1(),
         equal_buffer_.Dim2());
  size_t idx1 = 2, idx2 = 0;
  printf("Test: equal_buffer_[%ld][%ld].re = %f, .im = %f\n", idx1, idx2,
         equal_buffer_[idx1][idx2].re, equal_buffer_[idx1][idx2].im);
  printf("Test: equal_buffer_test1_[%ld][%ld].re = %f, .im = %f\n", idx1, idx2,
         equal_buffer_test1_[idx1][idx2].re,
         equal_buffer_test1_[idx1][idx2].im);
  printf("Test: equal_buffer_test2_[%ld][%ld].re = %f, .im = %f\n", idx1, idx2,
         equal_buffer_test2_[idx1][idx2].re,
         equal_buffer_test2_[idx1][idx2].im);

  // Check if they are the same instance
  EXPECT_FALSE(&equal_buffer_ == &equal_buffer_test1_);
  EXPECT_FALSE(&equal_buffer_ == &equal_buffer_test2_);
  EXPECT_FALSE(&equal_buffer_test1_ == &equal_buffer_test2_);
  EXPECT_TRUE(equal_buffer_ == equal_buffer_test1_);
  EXPECT_TRUE(equal_buffer_ == equal_buffer_test2_);
  EXPECT_TRUE(equal_buffer_test1_ == equal_buffer_test2_);
}

TEST(TestEqual, CorrectnessLoop) {
  auto cfg_ =
      std::make_shared<Config>("files/config/ci/tddconfig-sim-ul-fr2.json");
  cfg_->GenData();

  // ---------------------------------------------------------------------------
  // Prepare buffers
  // ---------------------------------------------------------------------------

  // From agora_buffer.h
  Table<complex_float> data_buffer_;
  Table<complex_float> equal_buffer_;
  Table<complex_float> equal_buffer_test1_;
  Table<complex_float> equal_buffer_test2_;
  Table<complex_float> ue_spec_pilot_buffer_;
  Table<complex_float> ue_spec_pilot_buffer_test1_;
  Table<complex_float> ue_spec_pilot_buffer_test2_;
  // PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_(
  //   kFrameWnd, cfg_->OfdmDataNum(), cfg_->BsAntNum() * cfg_->SpatialStreamsNum());
  PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

  // Similar to agora_buffer.cc
  const size_t task_buffer_symbol_num_ul =
      cfg_->Frame().NumULSyms() * kFrameWnd;

  data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
                                cfg_->OfdmDataNum() * cfg_->BsAntNum(),
                                Agora_memory::Alignment_t::kAlign64);

  equal_buffer_.RandAllocCxFloat(
      task_buffer_symbol_num_ul,
      cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  equal_buffer_test1_ = equal_buffer_;
  equal_buffer_test2_ = equal_buffer_;

  ue_spec_pilot_buffer_.RandAllocCxFloat(
      kFrameWnd,
      cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
      Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_test1_ = ue_spec_pilot_buffer_;
  ue_spec_pilot_buffer_test2_ = ue_spec_pilot_buffer_;

  ul_beam_matrices_.RandAllocCxFloat(cfg_->BsAntNum() *
                                     cfg_->SpatialStreamsNum());

  // ---------------------------------------------------------------------------
  // Run & test
  // ---------------------------------------------------------------------------

  for (size_t frame_id = 0; frame_id <= kFrameWnd; ++frame_id) {
    for (size_t symbol_id = cfg_->Frame().NumPilotSyms();
         symbol_id < cfg_->Frame().NumULSyms(); ++symbol_id) {
      for (size_t base_sc_id = 0; base_sc_id < cfg_->OfdmDataNum();
           base_sc_id += cfg_->DemulBlockSize()) {
        // printf("--------------------------------------------------\n");
        equal_org(cfg_.get(), data_buffer_, equal_buffer_,
                  ue_spec_pilot_buffer_, ul_beam_matrices_, frame_id, symbol_id,
                  base_sc_id);
        // printf("--------------------------------------------------\n");
        equal_ifcond(cfg_.get(), data_buffer_, equal_buffer_test1_,
                     ue_spec_pilot_buffer_test1_, ul_beam_matrices_, frame_id,
                     symbol_id, base_sc_id);
        // printf("--------------------------------------------------\n");
        equal_test(cfg_.get(), data_buffer_, equal_buffer_test2_,
                   ue_spec_pilot_buffer_test2_, ul_beam_matrices_, frame_id,
                   symbol_id, base_sc_id);
        // printf("--------------------------------------------------\n");
        // Check if they are the same instance
        EXPECT_TRUE(&equal_buffer_ != &equal_buffer_test1_ &&
                    &equal_buffer_ != &equal_buffer_test2_ &&
                    &equal_buffer_test1_ != &equal_buffer_test2_);
        EXPECT_TRUE(equal_buffer_ == equal_buffer_test1_)
            << "frame_id = " << frame_id << ", symbol_id = " << symbol_id
            << ", base_sc_id = " << base_sc_id;
        EXPECT_TRUE(equal_buffer_ == equal_buffer_test2_)
            << "frame_id = " << frame_id << ", symbol_id = " << symbol_id
            << ", base_sc_id = " << base_sc_id;
        ASSERT_TRUE(equal_buffer_test1_ == equal_buffer_test2_)
            << "frame_id = " << frame_id << ", symbol_id = " << symbol_id
            << ", base_sc_id = " << base_sc_id;
      }
    }
  }
}

// TEST(TestEqual, MKLvsArma) {

//   auto cfg_ = std::make_shared<Config>(
//       "files/config/ci/tddconfig-sim-ul-fr2.json");
//   cfg_->GenData();
//   size_t frame_id = 0, symbol_id = 1, base_sc_id = 64;

//   // ---------------------------------------------------------------------------

//   // From agora_buffer.h
//   Table<complex_float> data_buffer_;
//   Table<complex_float> equal_buffer_;
//   Table<complex_float> equal_buffer_mkl_;
//   Table<complex_float> equal_buffer_arma_;
//   Table<complex_float> ue_spec_pilot_buffer_;
//   PtrGrid<kFrameWnd, kMaxDataSCs, complex_float> ul_beam_matrices_;

//   // From agora_buffer.cc
//   const size_t task_buffer_symbol_num_ul =
//     cfg_->Frame().NumULSyms() * kFrameWnd;
//   data_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
//                      cfg_->OfdmDataNum() * cfg_->BsAntNum(),
//                      Agora_memory::Alignment_t::kAlign64);
//   equal_buffer_.RandAllocCxFloat(task_buffer_symbol_num_ul,
//                        cfg_->OfdmDataNum() * cfg_->SpatialStreamsNum(),
//                        Agora_memory::Alignment_t::kAlign64);
//   equal_buffer_mkl_ = equal_buffer_;
//   equal_buffer_arma_ = equal_buffer_;
//   ue_spec_pilot_buffer_.RandAllocCxFloat(
//       kFrameWnd,
//       cfg_->Frame().ClientUlPilotSymbols() * cfg_->SpatialStreamsNum(),
//       Agora_memory::Alignment_t::kAlign64);

//   // ---------------------------------------------------------------------------

//   const size_t symbol_idx_ul = cfg_->Frame().GetULSymbolIdx(symbol_id);
//   const size_t data_symbol_idx_ul =
//       symbol_idx_ul - cfg_->Frame().ClientUlPilotSymbols();
//   const size_t total_data_symbol_idx_ul =
//       cfg_->GetTotalDataSymbolIdxUl(frame_id, symbol_idx_ul);
//   const complex_float* data_buf = data_buffer_[total_data_symbol_idx_ul];

//   const size_t frame_slot = frame_id % kFrameWnd;

//   size_t max_sc_ite =
//       std::min(cfg_->DemulBlockSize(), cfg_->OfdmDataNum() - base_sc_id);
//   assert(max_sc_ite % kSCsPerCacheline == 0);

//   // ---------------------------------------------------------------------------
//   // Armadillo elementwise multiplication

//   arma::cx_float* equal_ptr =
//         (arma::cx_float*)(&equal_buffer_arma_[total_data_symbol_idx_ul]
//                                         [base_sc_id * cfg_->UeAntNum()]);
//   arma::cx_fvec vec_equaled(equal_ptr, max_sc_ite, false);

//   arma::cx_float* data_ptr = (arma::cx_float*)(&data_buf[base_sc_id]);
//   // not consider multi-antenna case (antena offset is omitted)
//   // assuming cfg->BsAntNum() == 1, reducing a dimension
//   arma::cx_fvec vec_data(data_ptr, max_sc_ite, false);

//   arma::cx_float* ul_beam_ptr = reinterpret_cast<arma::cx_float*>(
//       ul_beam_matrices_[frame_slot][0]); // pick the first element
//   arma::cx_fvec vec_ul_beam(max_sc_ite); // init empty vec
//   for (size_t i = 0; i < max_sc_ite; ++i) {
//     vec_ul_beam(i) = ul_beam_ptr[cfg_->GetBeamScId(base_sc_id + i)];
//   }

//   vec_equaled = vec_ul_beam % vec_data;

//   // ---------------------------------------------------------------------------
//   // MKL elementwise multiplication

//   arma::cx_float* equal_ptr_mkl =
//         (arma::cx_float*)(&equal_buffer_mkl_[total_data_symbol_idx_ul]
//                                         [base_sc_id * cfg_->UeAntNum()]);

//   vcMul(max_sc_ite, (MKL_Complex8*)ul_beam_ptr,
//         (MKL_Complex8*)data_ptr, (MKL_Complex8*)equal_ptr_mkl);

//   // ---------------------------------------------------------------------------
//   // Test equalness
//   EXPECT_TRUE(equal_buffer_arma_ == equal_buffer_mkl_);
//   EXPECT_FALSE(equal_buffer_ == equal_buffer_arma_);
//   EXPECT_FALSE(equal_buffer_ == equal_buffer_mkl_);
// }

/******************************************************************************/

int main(int argc, char** argv) {
  printf("Running main() from %s\n", __FILE__);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

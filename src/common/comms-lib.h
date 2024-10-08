// Copyright (c) 2018-2020, Rice University
// RENEW OPEN SOURCE LICENSE: http://renew-wireless.org/license

/**
 * @file comms-lib.h
 * @brief Communications Library:
 *    a) Generate pilot/preamble sequences
 *    b) OFDM modulation
 * @author Rahman Doost-Mohamamdy: doost@rice.edu
 *         Oscar Bejarano: obejarano@rice.edu
 */
#ifndef COMMSLIB_H_
#define COMMSLIB_H_

#include <cmath>
#include <complex>
#include <map>
#include <string>
#include <vector>

#include "common_typedef_sdk.h"
#include "immintrin.h"
#include "memory_manage.h"
#include "mkl_dfti.h"

static const std::map<std::string, size_t> kBeamformingStr{
    {"ZF", 0}, {"MMSE", 1}, {"MRC", 2}};

//  38.214  - Table 5.1.3.1-1: MCS index table 1 for PDSCH >
//  Last 3 from < 38.214 - Table 5.1.3.1-2: MCS index table 2 for PDSCH >
/*static const std::map<size_t, std::pair<size_t, size_t>> kMCS = {
    {0, {2, 120}},  {1, {2, 157}},  {2, {2, 193}},  {3, {2, 251}},
    {4, {2, 308}},  {5, {2, 379}},  {6, {2, 449}},  {7, {2, 526}},
    {8, {2, 602}},  {9, {2, 679}},  {10, {4, 340}}, {11, {4, 378}},
    {12, {4, 434}}, {13, {4, 490}}, {14, {4, 553}}, {15, {4, 616}},
    {16, {4, 658}}, {17, {6, 438}}, {18, {6, 466}}, {19, {6, 517}},
    {20, {6, 567}}, {21, {6, 616}}, {22, {6, 666}}, {23, {6, 719}},
    {24, {6, 772}}, {25, {6, 822}}, {26, {6, 873}}, {27, {6, 910}},
    {28, {6, 948}}, {29, {8, 754}}, {30, {8, 797}}, {31, {8, 841}}};*/
static const std::pair<size_t, size_t> kMCS[32u] = {
    {2, 120}, {2, 157}, {2, 193}, {2, 251}, {2, 308}, {2, 379}, {2, 449},
    {2, 526}, {2, 602}, {2, 679}, {4, 340}, {4, 378}, {4, 434}, {4, 490},
    {4, 553}, {4, 616}, {4, 658}, {6, 438}, {6, 466}, {6, 517}, {6, 567},
    {6, 616}, {6, 666}, {6, 719}, {6, 772}, {6, 822}, {6, 873}, {6, 910},
    {6, 948}, {8, 754}, {8, 797}, {8, 841}};

inline size_t GetCodeRate(size_t mcs_index) {
  std::pair mcs = kMCS[mcs_index];
  return std::get<1>(mcs);
}

inline size_t GetModOrderBits(size_t mcs_index) {
  std::pair mcs = kMCS[mcs_index];
  return std::get<0>(mcs);
}

class CommsLib {
 public:
  enum SequenceType {
    kStsSeq,
    kLtsSeq,
    kLtsFSeq,
    kLteZadoffChu,
    kGoldIfft,
    kHadamard
  };

  enum ModulationOrder {
    kBpsk = 1,
    kQpsk = 2,
    kQaM16 = 4,
    kQaM64 = 6,
    kQaM256 = 8
  };

  enum BeamformingAlgorithm { kZF = 0, kMMSE = 1, kMRC = 2 };

  explicit CommsLib(std::string);
  ~CommsLib();

  static std::vector<std::vector<double>> GetSequence(size_t seq_len, int type);
  static std::vector<std::vector<size_t>> GetAvailableMcs();
  static size_t GetMcsIndex(size_t mod_order, size_t code_rate);
  static std::vector<std::complex<float>> Modulate(
      const std::vector<int8_t>& in, int type);

  static std::vector<size_t> GetDataSc(size_t fft_size, size_t data_sc_num,
                                       size_t pilot_sc_offset,
                                       size_t pilot_sc_spacing);
  static std::vector<size_t> GetNullSc(size_t fft_size, size_t data_sc_num);
  static std::vector<std::complex<float>> GetPilotScValue(
      size_t fft_size, size_t data_sc_num, size_t pilot_sc_offset,
      size_t pilot_sc_spacing);
  static std::vector<size_t> GetPilotScIdx(size_t fft_size, size_t data_sc_num,
                                           size_t pilot_sc_offset,
                                           size_t pilot_sc_spacing);

  static MKL_LONG FFT(std::vector<std::complex<float>>& in_out, int fft_size);
  static MKL_LONG IFFT(std::vector<std::complex<float>>& in_out, int fft_size,
                       bool normalize = true);
  static MKL_LONG FFT(complex_float* in_out, int fft_size);
  static MKL_LONG IFFT(complex_float* in_out, int fft_size,
                       bool normalize = true);
  static std::vector<std::complex<float>> FFTShift(
      const std::vector<std::complex<float>>& in);
  static std::vector<complex_float> FFTShift(
      const std::vector<complex_float>& in);

  static void FFTShift(complex_float* inout, complex_float* tmp, int fft_size);
  static void FFTShift(complex_float* inout, int fft_size);

  static float ComputeOfdmSnr(const std::vector<std::complex<float>>& data_t,
                              size_t data_start_index, size_t data_stop_index);
  static size_t FindPilotSeq(const std::vector<std::complex<float>>& iq,
                             const std::vector<std::complex<float>>& pilot,
                             size_t seq_len);
  static int FindLts(const std::vector<std::complex<double>>& iq, int seq_len);
  template <typename T>
  static std::vector<T> Convolve(std::vector<std::complex<T>> const& f,
                                 std::vector<std::complex<T>> const& g);
  template <typename T>
  static std::vector<std::complex<T>> Csign(std::vector<std::complex<T>> iq);
  static void Meshgrid(const std::vector<int>& x_in,
                       const std::vector<int>& y_in,
                       std::vector<std::vector<int>>& x,
                       std::vector<std::vector<int>>& y);
  static inline int Hadamard2(int i, int j) {
    return (__builtin_parity(i & j) != 0 ? -1 : 1);
  }
  static std::vector<float> MagnitudeFft(
      std::vector<std::complex<float>> const& samps,
      std::vector<float> const& win, size_t fft_size);
  static std::vector<float> HannWindowFunction(size_t fft_size);
  static double WindowFunctionPower(std::vector<float> const& win);
  static float FindTone(std::vector<float> const& magnitude, double win_gain,
                        double fft_bin, size_t fft_size,
                        const size_t delta = 10);
  static float MeasureTone(std::vector<std::complex<float>> const& samps,
                           std::vector<float> const& win, double win_gain,
                           double fft_bin, size_t fft_size,
                           const size_t delta = 10);
  static std::vector<std::complex<float>> ComposePartialPilotSym(
      const std::vector<std::complex<float>>& pilot, size_t offset,
      size_t pilot_sc_num, size_t fft_size, size_t data_size, size_t data_start,
      size_t cp_len, bool interleaved_pilot, bool time_domain = true);
  static std::vector<std::complex<float>> SeqCyclicShift(
      const std::vector<std::complex<float>>& in, float alpha);
  static float FindMaxAbs(const complex_float* in, size_t len);
  static float FindMaxAbs(const Table<complex_float>& in, size_t dim1,
                          size_t dim2);
  static float FindMeanAbs(const complex_float* in, size_t len);
  static float FindMeanAbs(const Table<complex_float>& in, size_t dim1,
                           size_t dim2);
  static void Ifft2tx(const complex_float* in, std::complex<short>* out,
                      size_t N, size_t prefix, size_t cp, float scale);
  static float AbsCf(complex_float d) {
    return std::abs(std::complex<float>(d.re, d.im));
  }
  static int FindBeaconAvx(const std::vector<std::complex<float>>& iq,
                           const std::vector<std::complex<float>>& seq,
                           float corr_scale);

  ///Find Beacon with raw samples from the radio
  static ssize_t FindBeaconAvx(const std::complex<int16_t>* iq,
                               const std::vector<std::complex<float>>& seq,
                               size_t sample_window, float corr_scale);

  static std::vector<float> CorrelateAvxS(std::vector<float> const& f,
                                          std::vector<float> const& g);
  static std::vector<float> Abs2Avx(std::vector<std::complex<float>> const& f);
  static std::vector<int32_t> Abs2Avx(
      std::vector<std::complex<int16_t>> const& f);
  static std::vector<std::complex<float>> AutoCorrMultAvx(
      std::vector<std::complex<float>> const& f, const int dly,
      const bool conj = true);
  static std::vector<std::complex<int16_t>> AutoCorrMultAvx(
      std::vector<std::complex<int16_t>> const& f, const int dly,
      const bool conj = true);
  static std::vector<std::complex<float>> CorrelateAvx(
      std::vector<std::complex<float>> const& f,
      std::vector<std::complex<float>> const& g);
  static std::vector<std::complex<float>> ComplexMultAvx(
      std::vector<std::complex<float>> const& f,
      std::vector<std::complex<float>> const& g, const bool conj);
  static std::vector<std::complex<int16_t>> ComplexMultAvx(
      std::vector<std::complex<int16_t>> const& f,
      std::vector<std::complex<int16_t>> const& g, const bool conj);
  static std::vector<std::complex<int16_t>> CorrelateAvx(
      std::vector<std::complex<int16_t>> const& f,
      std::vector<std::complex<int16_t>> const& g);

  static __m256 M256ComplexCf32Mult(__m256 data1, __m256 data2, bool conj);
  static __m256 M256ComplexCf32Reciprocal(__m256 data);
  static __m256 M256ComplexCf32Conj(__m256 data);
  static __m256 M256ComplexCf32Set1(std::complex<float> data);
  static std::complex<float> M256ComplexCf32Sum(__m256 data);
  static bool M256ComplexCf32NearZeros(__m256 data, float threshold);
  static void PrintM256ComplexCf32(__m256 data);
#ifdef __AVX512F__
  static __m512 M512ComplexCf32Mult(__m512 data1, __m512 data2, bool conj);
  static __m512 M512ComplexCf32Reciprocal(__m512 data);
  static __m512 M512ComplexCf32Conj(__m512 data);
  static __m512 M512ComplexCf32Set1(std::complex<float> data);
  static std::complex<float> M512ComplexCf32Sum(__m512 data);
  static bool M512ComplexCf32NearZeros(__m512 data, float threshold);
  static void PrintM512ComplexCf32(__m512 data);
#endif
};

#endif  // COMMSLIB_H_

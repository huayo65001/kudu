// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/common/columnar_serialization.h"

#include <immintrin.h>

#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "kudu/common/zp7.h"
#include "kudu/gutil/cpu.h"
#include "kudu/gutil/port.h"
#include "kudu/util/alignment.h"
#include "kudu/util/bitmap.h"

using std::vector;

namespace kudu {

namespace {

// Utility to write variable bit-length values to a pre-allocated buffer.
//
// This is similar to the BitWriter class in util/bit-stream-utils.h except that
// the other implementation manages growing an underlying 'faststring' rather
// than writing to existing memory.
struct BitWriter {

  // Start writing data to 'dst', but skip over the first 'skip_initial_bits'
  // bits.
  BitWriter(uint8_t* dst, int skip_initial_bits) : dst_(dst) {
    DCHECK_GE(skip_initial_bits, 0);
    dst_ += skip_initial_bits / 8;

    // The "skip" may place us in the middle of a byte. To simplify this,
    // we just position ourselves at the start of that byte and buffer the
    // pre-existing bits, thus positioning ourselves at the right offset.
    int preexisting_bits = skip_initial_bits % 8;
    uint8_t preexisting_val = *dst_ & ((1 << preexisting_bits) - 1);
    Put(preexisting_val, preexisting_bits);
  }

  ~BitWriter() {
    CHECK(flushed_) << "must flush";
  }

  void Put(uint64_t v, int num_bits) {
    DCHECK(!flushed_);
    DCHECK_LE(num_bits, 64);
    buffered_values_ |= v << num_buffered_bits_;
    num_buffered_bits_ += num_bits;

    if (PREDICT_FALSE(num_buffered_bits_ >= 64)) {
      memcpy(dst_, &buffered_values_, 8);
      buffered_values_ = 0;
      num_buffered_bits_ -= 64;
      int shift = num_bits - num_buffered_bits_;
      buffered_values_ = (shift >= 64) ? 0 : v >> shift;
      dst_ += 8;
    }
    DCHECK_LT(num_buffered_bits_, 64);
  }

  void Flush() {
    CHECK(!flushed_) << "must only flush once";
    while (num_buffered_bits_ > 0) {
      *dst_++ = buffered_values_ & 0xff;
      buffered_values_ >>= 8;
      num_buffered_bits_ -= 8;
    }
    flushed_ = true;
  }

  uint8_t* dst_;

  // Accumulated bits that haven't been flushed to the destination buffer yet.
  uint64_t buffered_values_ = 0;

  // The number of accumulated bits in buffered_values_.
  int num_buffered_bits_ = 0;

  bool flushed_ = false;
};

} // anonymous namespace

////////////////////////////////////////////////////////////
// ZeroNullValues
////////////////////////////////////////////////////////////

namespace internal {

namespace {
// Implementation of ZeroNullValues, specialized for a particular type size.
template<int sizeof_type>
ATTRIBUTE_NOINLINE
void ZeroNullValuesImpl(int dst_idx,
                        int n_rows,
                        uint8_t* __restrict__ dst_values_buf,
                        uint8_t* __restrict__ non_null_bitmap) {
  int aligned_dst_idx = KUDU_ALIGN_DOWN(dst_idx, 8);
  int aligned_n_sel = n_rows + (dst_idx - aligned_dst_idx);

  uint8_t* aligned_values_base = dst_values_buf + aligned_dst_idx * sizeof_type;

  // TODO(todd): this code path benefits from the BMI instruction set. We should
  // compile it twice, once with BMI supported.
  ForEachUnsetBit(non_null_bitmap + aligned_dst_idx/8,
                  aligned_n_sel,
                  [&](int position) {
                    // The position here is relative to our aligned bitmap.
                    memset(aligned_values_base + position * sizeof_type, 0, sizeof_type);
                  });
}

} // anonymous namespace

// Zero out any values in 'dst_values_buf' which are indicated as null in 'non_null_bitmap'.
//
// 'n_rows' cells are processed, starting at index 'dst_idx' within the buffers.
// 'sizeof_type' indicates the size of each cell in bytes.
//
// NOTE: this assumes that dst_values_buf and non_null_bitmap are valid for the full range
// of indices [0, dst_idx + n_rows). The implementation may redundantly re-zero cells
// at indexes less than dst_idx.
void ZeroNullValues(int sizeof_type,
                    int dst_idx,
                    int n_rows,
                    uint8_t* dst_values_buf,
                    uint8_t* dst_non_null_bitmap) {
  // Delegate to specialized implementations for each type size.
  // This changes variable-length memsets into inlinable single instructions.
  switch (sizeof_type) {
#define CASE(size)                                                      \
    case size:                                                          \
      ZeroNullValuesImpl<size>(dst_idx, n_rows, dst_values_buf, dst_non_null_bitmap); \
      break;
    CASE(1);
    CASE(2);
    CASE(4);
    CASE(8);
    CASE(16);
#undef CASE
    default:
      LOG(FATAL) << "bad size: " << sizeof_type;
  }
}


////////////////////////////////////////////////////////////
// CopyNonNullBitmap
////////////////////////////////////////////////////////////

namespace {
template<class PextImpl>
void CopyNonNullBitmapImpl(
    const uint8_t* __restrict__ non_null_bitmap,
    const uint8_t* __restrict__ sel_bitmap,
    int dst_idx,
    int n_rows,
    uint8_t* __restrict__ dst_non_null_bitmap) {
  BitWriter bw(dst_non_null_bitmap, dst_idx);

  int num_64bit_words = n_rows / 64;
  for (int i = 0; i < num_64bit_words; i++) {
    uint64_t sel_mask = UnalignedLoad<uint64_t>(sel_bitmap + i * 8);
    int num_bits = __builtin_popcountll(sel_mask);

    uint64_t non_nulls = UnalignedLoad<uint64_t>(non_null_bitmap + i * 8);
    uint64_t extracted = PextImpl::call(non_nulls, sel_mask);
    bw.Put(extracted, num_bits);
  }

  int rem_rows = n_rows % 64;
  non_null_bitmap += num_64bit_words * 8;
  sel_bitmap += num_64bit_words * 8;
  while (rem_rows > 0) {
    uint8_t non_nulls = *non_null_bitmap;
    uint8_t sel_mask = *sel_bitmap;

    uint64_t extracted = PextImpl::call(non_nulls, sel_mask);
    int num_bits = __builtin_popcountl(sel_mask);
    bw.Put(extracted, num_bits);

    sel_bitmap++;
    non_null_bitmap++;
    rem_rows -= 8;
  }
  bw.Flush();
}

struct PextZp7Clmul {
  inline static uint64_t call(uint64_t val, uint64_t mask) {
    return zp7_pext_64_clmul(val, mask);
  }
};
struct PextZp7Simple {
  inline static uint64_t call(uint64_t val, uint64_t mask) {
    return zp7_pext_64_simple(val, mask);
  }
};

#ifdef __x86_64__
struct PextInstruction {
  __attribute__((target("bmi2")))
  inline static uint64_t call(uint64_t val, uint64_t mask) {
#if !defined(__clang__) && defined(__GNUC__) && __GNUC__ < 5
    // GCC <5 doesn't properly handle the _pext_u64 intrinsic inside
    // a function with a specified target attribute. So, use inline
    // assembly instead.
    //
    // Though this assembly works on clang as well, it has two downsides:
    // - the "multiple constraint" 'rm' for 'mask' is supposed to indicate to
    //   the compiler that the mask could either be in memory or in a register,
    //   but clang doesn't support this, and will always spill it to memory
    //   even if the value is already in a register. That results in an extra couple
    //   cycles.
    // - using the intrinsic means that clang optimization passes have some opportunity
    //   to better understand what's going on and make appropriate downstream optimizations.
    uint64_t dst;
    asm ("pextq %[mask], %[val], %[dst]"
        : [dst] "=r" (dst)
        : [val] "r" (val),
          [mask] "rm" (mask));
    return dst;
#else
    return _pext_u64(val, mask);
#endif // compiler check
  }
};
// Explicit instantiation of the template for the PextInstruction case
// allows us to apply the 'bmi2' target attribute for just this version.
template
__attribute__((target("bmi2")))
void CopyNonNullBitmapImpl<PextInstruction>(
    const uint8_t* __restrict__ non_null_bitmap,
    const uint8_t* __restrict__ sel_bitmap,
    int dst_idx,
    int n_rows,
    uint8_t* __restrict__ dst_non_null_bitmap);
#endif // __x86_64__

} // anonymous namespace

// Return a prioritized list of methods that can be used for extracting bits from the non-null
// bitmap.
vector<PextMethod> GetAvailablePextMethods() {
  vector<PextMethod> ret;
#ifdef __x86_64__
  base::CPU cpu;
  // Even though recent AMD chips support pext, it's extremely slow,
  // so only use BMI2 on Intel, and instead use the 'zp7' software
  // implementation on AMD.
  if (cpu.has_bmi2() && cpu.vendor_name() == "GenuineIntel") {
    ret.push_back(PextMethod::kPextInstruction);
  }
  if (cpu.has_pclmulqdq()) {
    ret.push_back(PextMethod::kClmul);
  }
#endif
  ret.push_back(PextMethod::kSimple);
  return ret;
}

PextMethod g_pext_method = GetAvailablePextMethods()[0];

void CopyNonNullBitmap(const uint8_t* non_null_bitmap,
                       const uint8_t* sel_bitmap,
                       int dst_idx,
                       int n_rows,
                       uint8_t* dst_non_null_bitmap) {
  switch (g_pext_method) {
#ifdef __x86_64__
    case PextMethod::kPextInstruction:
      CopyNonNullBitmapImpl<PextInstruction>(
          non_null_bitmap, sel_bitmap, dst_idx, n_rows, dst_non_null_bitmap);
      break;
    case PextMethod::kClmul:
      CopyNonNullBitmapImpl<PextZp7Clmul>(
          non_null_bitmap, sel_bitmap, dst_idx, n_rows, dst_non_null_bitmap);
      break;
#endif
    case PextMethod::kSimple:
      CopyNonNullBitmapImpl<PextZp7Simple>(
          non_null_bitmap,  sel_bitmap, dst_idx, n_rows, dst_non_null_bitmap);
      break;
    default:
      __builtin_unreachable();
  }
}

////////////////////////////////////////////////////////////
// CopySelectedRows
////////////////////////////////////////////////////////////

namespace {
template<int sizeof_type>
ATTRIBUTE_NOINLINE
void CopySelectedRowsImpl(const uint16_t* __restrict__ sel_rows,
                          int n_sel_rows,
                          const uint8_t* __restrict__ src_buf,
                          uint8_t* __restrict__ dst_buf) {
  int rem = n_sel_rows;
  while (rem--) {
    int idx = *sel_rows++;
    memcpy(dst_buf, src_buf + idx * sizeof_type, sizeof_type);
    dst_buf += sizeof_type;
  }
  // TODO(todd): should we zero out nulls first or otherwise avoid
  // copying them?
}

template<int sizeof_type>
ATTRIBUTE_NOINLINE
void CopySelectedRowsImpl(const vector<uint16_t>& sel_rows,
                          const uint8_t* __restrict__ src_buf,
                          uint8_t* __restrict__ dst_buf) {
  CopySelectedRowsImpl<sizeof_type>(sel_rows.data(), sel_rows.size(), src_buf, dst_buf);
}

} // anonymous namespace

void CopySelectedRows(const vector<uint16_t>& sel_rows,
                      int sizeof_type,
                      const uint8_t* __restrict__ src_buf,
                      uint8_t* __restrict__ dst_buf) {
  switch (sizeof_type) {
#define CASE(size)                                            \
    case size:                                                \
      CopySelectedRowsImpl<size>(sel_rows, src_buf, dst_buf); \
      break;
    CASE(1);
    CASE(2);
    CASE(4);
    CASE(8);
    CASE(16);
#undef CASE
    default:
      LOG(FATAL) << "unexpected type size: " << sizeof_type;
  }
}

} // namespace internal

} // namespace kudu
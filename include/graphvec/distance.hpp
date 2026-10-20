// Squared-L2 distance kernels. Same structure as gemm-lab's axpy: one SIMD
// body per ISA (AVX2+FMA, NEON) with a scalar tail, picked at compile time.
#pragma once

#include <cstddef>

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#define GV_AVX2 1
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#define GV_NEON 1
#endif

namespace gv {

inline const char* simd_isa() {
#if GV_AVX2
    return "avx2+fma";
#elif GV_NEON
    return "neon";
#else
    return "scalar";
#endif
}

inline float l2sq_scalar(const float* a, const float* b, size_t d) {
    float s = 0;
    for (size_t i = 0; i < d; ++i) {
        float t = a[i] - b[i];
        s += t * t;
    }
    return s;
}

inline float l2sq(const float* a, const float* b, size_t d) {
#if GV_AVX2
    // Two independent accumulators hide the 4-cycle FMA latency.
    __m256 s0 = _mm256_setzero_ps(), s1 = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 16 <= d; i += 16) {
        __m256 t0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 t1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        s0 = _mm256_fmadd_ps(t0, t0, s0);
        s1 = _mm256_fmadd_ps(t1, t1, s1);
    }
    for (; i + 8 <= d; i += 8) {
        __m256 t = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        s0 = _mm256_fmadd_ps(t, t, s0);
    }
    __m256 s = _mm256_add_ps(s0, s1);
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));
    float r = _mm_cvtss_f32(h);
    return r + l2sq_scalar(a + i, b + i, d - i);
#elif GV_NEON
    float32x4_t s0 = vdupq_n_f32(0), s1 = vdupq_n_f32(0);
    size_t i = 0;
    for (; i + 8 <= d; i += 8) {
        float32x4_t t0 = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
        float32x4_t t1 = vsubq_f32(vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        s0 = vfmaq_f32(s0, t0, t0);
        s1 = vfmaq_f32(s1, t1, t1);
    }
    float r = vaddvq_f32(vaddq_f32(s0, s1));
    return r + l2sq_scalar(a + i, b + i, d - i);
#else
    return l2sq_scalar(a, b, d);
#endif
}

} // namespace gv

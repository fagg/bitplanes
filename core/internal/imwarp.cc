/*
  This file is part of bitplanes.

  bitplanes is free software: you can redistribute it and/or modify
  it under the terms of the Lesser GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  bitplanes is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  Lesser GNU General Public License for more details.

  You should have received a copy of the Lesser GNU General Public License
  along with bitplanes.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bitplanes/core/internal/imwarp.h"
#include "bitplanes/core/internal/intrin.h"
#include "bitplanes/core/homography.h"
#include "bitplanes/utils/error.h"

#include <opencv2/core/core.hpp>
#include <opencv2/core/eigen.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <cassert>
#include <iostream>

namespace bp {

void warpPerspectiveRoi(const cv::Mat& src, cv::Mat& dst, const Matrix33f& T,
                        const cv::Rect& roi)
{
  THROW_ERROR_IF(src.type() != CV_8UC1, "input image must be CV_8UC1");
  dst.create(roi.size(), src.type());

  cv::Mat MM = (cv::Mat_<double>(3,3) <<
                T(0,0), T(0,1), T(0,2),
                T(1,0), T(1,1), T(1,2),
                T(2,0), T(2,1), T(2,2));
  double* M = MM.ptr<double>();

  const int BLOCK_SZ = 32;
  short XY[BLOCK_SZ*BLOCK_SZ*2], A[BLOCK_SZ*BLOCK_SZ];
  int x, y, x1, y1, width = dst.cols, height = dst.rows;

  int bh0 = std::min(BLOCK_SZ/2, height);
  int bw0 = std::min(BLOCK_SZ*BLOCK_SZ/bh0, width);
  bh0 = std::min(BLOCK_SZ*BLOCK_SZ/bw0, height);

  using namespace cv;

  bool haveSSE4_1 = checkHardwareSupport(CV_CPU_SSE4_1);
  __m128d v_M0 = _mm_set1_pd(M[0]);
  __m128d v_M3 = _mm_set1_pd(M[3]);
  __m128d v_M6 = _mm_set1_pd(M[6]);
  __m128d v_intmax = _mm_set1_pd((double)INT_MAX);
  __m128d v_intmin = _mm_set1_pd((double)INT_MIN);
  __m128d v_2 = _mm_set1_pd(2),
          v_zero = _mm_setzero_pd(),
          v_1 = _mm_set1_pd(1),
          v_its = _mm_set1_pd(INTER_TAB_SIZE);
  __m128i v_itsi1 = _mm_set1_epi32(INTER_TAB_SIZE - 1);

  Range range(0, dst.rows);

  for(y = range.start; y < range.end; y += bh0)
  {
    for(x = 0; x < width; x += bw0)
    {
      int bw = std::min( bw0, width - x);
      int bh = std::min( bh0, range.end - y); // height

      Mat _XY(bh, bw, CV_16SC2, XY), matA;
      Mat dpart(dst, Rect(x, y, bw, bh));

      for(y1 = 0; y1 < bh; ++y1)
      {
        short* xy = XY + y1*bw*2;
        double X0 = M[0]*x + M[1]*(y + y1) + M[2];
        double Y0 = M[3]*x + M[4]*(y + y1) + M[5];
        double W0 = M[6]*x + M[7]*(y + y1) + M[8];
        short* alpha = A + y1*bw;
        x1 = 0;
        if(haveSSE4_1)
        {
          __m128d v_X0d = _mm_set1_pd(X0);
          __m128d v_Y0d = _mm_set1_pd(Y0);
          __m128d v_W0 = _mm_set1_pd(W0);
          __m128d v_x1 = _mm_set_pd(1, 0);

          for( ; x1 <= bw - 16; x1 += 16 )
          {
            // 0-3
            __m128i v_X0, v_Y0;
            {
              __m128d v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_X0 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fX0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fX1))));
              v_Y0 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fY0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fY1))));
            }

            // 4-8
            __m128i v_X1, v_Y1;
            {
              __m128d v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_X1 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fX0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fX1))));
              v_Y1 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fY0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fY1))));
            }

            // 8-11
            __m128i v_X2, v_Y2;
            {
              __m128d v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_X2 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fX0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fX1))));
              v_Y2 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fY0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fY1))));
            }

            // 12-15
            __m128i v_X3, v_Y3;
            {
              __m128d v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY0 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_W = _mm_add_pd(_mm_mul_pd(v_M6, v_x1), v_W0);
              v_W = _mm_andnot_pd(_mm_cmpeq_pd(v_W, v_zero), _mm_div_pd(v_its, v_W));
              __m128d v_fX1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_X0d, _mm_mul_pd(v_M0, v_x1)), v_W)));
              __m128d v_fY1 = _mm_max_pd(v_intmin, _mm_min_pd(v_intmax, _mm_mul_pd(_mm_add_pd(v_Y0d, _mm_mul_pd(v_M3, v_x1)), v_W)));
              v_x1 = _mm_add_pd(v_x1, v_2);

              v_X3 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fX0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fX1))));
              v_Y3 = _mm_castps_si128(_mm_movelh_ps(_mm_castsi128_ps(_mm_cvtpd_epi32(v_fY0)),
                                                    _mm_castsi128_ps(_mm_cvtpd_epi32(v_fY1))));
            }

            // store alpha
            __m128i v_alpha0 = _mm_add_epi32(_mm_slli_epi32(_mm_and_si128(v_Y0, v_itsi1), INTER_BITS),
                                             _mm_and_si128(v_X0, v_itsi1));
            __m128i v_alpha1 = _mm_add_epi32(_mm_slli_epi32(_mm_and_si128(v_Y1, v_itsi1), INTER_BITS),
                                             _mm_and_si128(v_X1, v_itsi1));
            _mm_storeu_si128((__m128i *)(alpha + x1), _mm_packs_epi32(v_alpha0, v_alpha1));

            v_alpha0 = _mm_add_epi32(_mm_slli_epi32(_mm_and_si128(v_Y2, v_itsi1), INTER_BITS),
                                     _mm_and_si128(v_X2, v_itsi1));
            v_alpha1 = _mm_add_epi32(_mm_slli_epi32(_mm_and_si128(v_Y3, v_itsi1), INTER_BITS),
                                     _mm_and_si128(v_X3, v_itsi1));
            _mm_storeu_si128((__m128i *)(alpha + x1 + 8), _mm_packs_epi32(v_alpha0, v_alpha1));

            // convert to 16s
            v_X0 = _mm_packs_epi32(_mm_srai_epi32(v_X0, INTER_BITS), _mm_srai_epi32(v_X1, INTER_BITS));
            v_X1 = _mm_packs_epi32(_mm_srai_epi32(v_X2, INTER_BITS), _mm_srai_epi32(v_X3, INTER_BITS));
            v_Y0 = _mm_packs_epi32(_mm_srai_epi32(v_Y0, INTER_BITS), _mm_srai_epi32(v_Y1, INTER_BITS));
            v_Y1 = _mm_packs_epi32(_mm_srai_epi32(v_Y2, INTER_BITS), _mm_srai_epi32(v_Y3, INTER_BITS));

            _mm_interleave_epi16(v_X0, v_X1, v_Y0, v_Y1);

            _mm_storeu_si128((__m128i *)(xy + x1 * 2), v_X0);
            _mm_storeu_si128((__m128i *)(xy + x1 * 2 + 8), v_X1);
            _mm_storeu_si128((__m128i *)(xy + x1 * 2 + 16), v_Y0);
            _mm_storeu_si128((__m128i *)(xy + x1 * 2 + 24), v_Y1);
          }
        } // haveSSE4_1

        for( ; x1 < bw; x1++ )
        {
          double W = W0 + M[6]*x1;
          W = W ? INTER_TAB_SIZE/W : 0;
          double fX = std::max((double)INT_MIN, std::min((double)INT_MAX, (X0 + M[0]*x1)*W));
          double fY = std::max((double)INT_MIN, std::min((double)INT_MAX, (Y0 + M[3]*x1)*W));
          int X = saturate_cast<int>(fX);
          int Y = saturate_cast<int>(fY);

          xy[x1*2] = saturate_cast<short>(X >> INTER_BITS);
          xy[x1*2+1] = saturate_cast<short>(Y >> INTER_BITS);
          alpha[x1] = (short)((Y & (INTER_TAB_SIZE-1))*INTER_TAB_SIZE +
                              (X & (INTER_TAB_SIZE-1)));
        }

        Mat _matA(bh, bw, CV_16U, A);
        remap( src, dpart, _XY, _matA, cv::INTER_LINEAR, cv::BORDER_CONSTANT,
              cv::Scalar(0.0) );
      } // x
    } // y1
  } // y
}

template <class M>
void TransformPoint(const Matrix33f& T, float x, float y, float& xw, float& yw);

template <> inline void
TransformPoint<Translation>(const Matrix33f& T, float x, float y, float& xw, float& yw)
{
  xw = x + T(0,2);
  yw = y + T(1,2);
}

template <> inline void
TransformPoint<Affine>(const Matrix33f& T, float x, float y, float& xw, float& yw)
{
  xw = T(0,0)*x + T(0,1)*y + T(0,2);
  yw = T(1,0)*x + T(1,1)*y + T(1,2);
}

template <> inline void
TransformPoint<Homography>(const Matrix33f& T, float x, float y, float& xw, float& yw)
{
  float w_i = 1.0f / ( T(2,0)*x + T(2,1)*y + T(2,2) );
  xw = w_i * (T(0,0)*x + T(0,1)*y + T(0,2));
  yw = w_i * (T(1,0)*x + T(1,1)*y + T(1,2));
}

template <class Derived> static inline
Eigen::Matrix<
typename Derived::PlainObject::Scalar,
         Derived::PlainObject::RowsAtCompileTime,
Derived::PlainObject::ColsAtCompileTime>
normHomog(const Eigen::MatrixBase<Derived>& x)
{
  static_assert(Derived::PlainObject::RowsAtCompileTime != Eigen::Dynamic &&
                Derived::PlainObject::ColsAtCompileTime == 1,
                "normHomog: input must be a vector of known dimension");

  return x * (1.0f / x[Derived::PlainObject::RowsAtCompileTime-1]);
}


template <class M>
void imwarp(const cv::Mat& src, cv::Mat& dst, const Matrix33f& T,
            const cv::Rect& box, cv::Mat& xmap, cv::Mat& ymap,
            int interp, float offset)
{
  xmap.create(box.size(), CV_32FC1);
  ymap.create(box.size(), CV_32FC1);

  THROW_ERROR_IF( xmap.empty() || ymap.empty(), "Failed to allocate interp maps" );

  //cv::Mat_<float>& xm = (cv::Mat_<float>&) xmap;
  //cv::Mat_<float>& ym = (cv::Mat_<float>&) ymap;
  float* xm_ptr = xmap.ptr<float>();
  float* ym_ptr = ymap.ptr<float>();

  const int x_s = box.x, y_s = box.y;
  for(int y = 0; y < box.height; ++y)
  {
    for(int x = 0; x < box.width; ++x, ++xm_ptr, ++ym_ptr)
    {
      //TransformPoint<M>(T, x + x_s + offset, y + y_s + offset, xm(y,x), ym(y,x));
      //TransformPoint<M>(T, x + x_s + offset, y + y_s + offset, *xm_ptr, *ym_ptr);
      const Eigen::Vector3f pw = normHomog(
          T * Eigen::Vector3f(x+x_s+offset, y+y_s+offset,1.0f) );
      *xm_ptr = pw[0];
      *ym_ptr = pw[1];
    }
  }

  cv::remap(src, dst, xmap, ymap, interp, cv::BORDER_CONSTANT, cv::Scalar(0.0));
}

template void
imwarp<Homography>(const cv::Mat&, cv::Mat&, const Matrix33f&,
                   const cv::Rect&, cv::Mat&, cv::Mat&, int, float);
template void
imwarp<Affine>(const cv::Mat&, cv::Mat&, const Matrix33f&,
               const cv::Rect&, cv::Mat&, cv::Mat&, int, float);
template void
imwarp<Translation>(const cv::Mat&, cv::Mat&, const Matrix33f&,
                    const cv::Rect&, cv::Mat&, cv::Mat&, int, float);

template <class M>
void imwarp(const cv::Mat& src, cv::Mat& dst, const Matrix33f& T, const cv::Rect& roi)
{
  const cv::Mat A = (cv::Mat_<float>(2,3) <<
                     T(0,0), T(0,1), T(0,2),
                     T(1,0), T(1,1), T(1,2));

  cv::warpAffine(src(roi), dst, A, cv::Size(), cv::INTER_LINEAR);
}

template <> void
imwarp<Homography>(const cv::Mat& src, cv::Mat& dst, const Matrix33f& T,
                   const cv::Rect& roi)
{
  const cv::Mat H = (cv::Mat_<float>(3,3) <<
                     T(0,0), T(0,1), T(0,2),
                     T(1,0), T(1,1), T(1,2),
                     T(2,0), T(2,1), T(2,2));

  cv::warpPerspective(src(roi), dst, H, cv::Size(), cv::INTER_LINEAR |
                      cv::WARP_INVERSE_MAP);
}

template void imwarp<Homography>(const cv::Mat&, cv::Mat&, const Matrix33f&, const cv::Rect&);
template void imwarp<Affine>(const cv::Mat&, cv::Mat&, const Matrix33f&, const cv::Rect&);
template void imwarp<Translation>(const cv::Mat&, cv::Mat&, const Matrix33f&, const cv::Rect&);

void imwarp(const cv::Mat& src, cv::Mat& dst, const Matrix33f& T,
            const PointVector& points, const cv::Rect& box,
            cv::Mat& xmap, cv::Mat& ymap, bool is_projective,
            int interp, int border, float border_val)
{
#if 1
  //
  // should set this to zero if there are holes in the points
  // for now, we do this densely
  // TODO for sparse or subsampled points
  //
  xmap.create(box.size(), CV_32FC1);
  ymap.create(box.size(), CV_32FC1);

  assert( !xmap.empty() && !ymap.empty() && "failed to allocate" );
  assert( xmap.isContinuous() && ymap.isContinuous() && "maps must be continous");

  const int x_off = box.x, y_off = box.y;
  const int stride = xmap.cols;
  float* x_map = xmap.ptr<float>();
  float* y_map = ymap.ptr<float>();

  //std::fill_n(x_map, xmap.rows * xmap.cols, 0.0f);
  //std::fill_n(y_map, ymap.rows * ymap.cols, 0.0f);

  for(size_t i = 0; i < points.size(); ++i)
  {
    const Vector3f& p = points[i];
    Vector3f pw = T * p;
    pw *= is_projective ? (1.0f / pw[2]) : 1.0f;

    int y = p.y() - y_off, x = p.x() - x_off, ii = y*stride + x;
    assert( ii >= 0 && ii < xmap.rows*xmap.cols );

    x_map[ii] = pw.x();
    y_map[ii] = pw.y();
  }

  cv::remap(src, dst, xmap, ymap, interp, border, cv::Scalar(border_val));
#else
  cv::Mat M;
  cv::eigen2cv(T, M);
  int flags = interp | cv::WARP_INVERSE_MAP;
  cv::warpPerspective(src(box), dst, M, cv::Size(), flags, border,
                      cv::Scalar(border_val));
#endif
}

#if BITPLANES_HAVE_SSE3

namespace {

static inline __m128 sumv(const __m128& x)
{
  const auto v = _mm_hadd_ps(x, x);
  return _mm_hadd_ps(v, v);
}

} // namespace

int imwarp(const uint8_t* I, int w, int h, const float* P, const float* X,
           const float* I_ref, float* residuals, uint8_t* valid, int N,
           float* I_warped)
{
  int stride = w;
  int i = 0;

  int rounding_mode = _MM_GET_ROUNDING_MODE();
  if(_MM_ROUND_TOWARD_ZERO != rounding_mode) _MM_SET_ROUNDING_MODE(_MM_ROUND_TOWARD_ZERO);

  int flush_mode = _MM_GET_FLUSH_ZERO_MODE();
  if(_MM_FLUSH_ZERO_ON != flush_mode) _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);

  const auto c0 = _mm_load_ps( P + 0 ),
        c1 = _mm_load_ps( P + 4 ),
        c2 = _mm_load_ps( P + 8 ),
        c3 = _mm_load_ps( P + 12 );

  const auto LB = _mm_set1_epi32(-1);                // Lower bound
  const auto UB = _mm_set_epi32(h-1, w-1, h-1, w-1); // Upper bound
  const auto ONES = _mm_set1_ps(1.0f);
  const auto HALF = _mm_set1_ps(0.5f);
  const auto n = N & ~3; // we'll do 4 points at a time

  int num_valid = 0;

  for(i = 0; i < n; i += 4)
  {
    alignas(16) int buf[4];
    __m128 i0, i1, i2, i3;
    {
      __m128 x0, x1, xf;
      __m128i mask, xi;

      {
        auto p = _mm_load_ps(X + 4*i + 0),
             x = _mm_mul_ps(c0, _mm_shuffle_ps(p, p, _MM_SHUFFLE(0,0,0,0))),
             y = _mm_mul_ps(c1, _mm_shuffle_ps(p, p, _MM_SHUFFLE(1,1,1,1))),
             z = _mm_mul_ps(c2, _mm_shuffle_ps(p, p, _MM_SHUFFLE(2,2,2,2)));
        x0 = _mm_add_ps(c3, _mm_add_ps(_mm_add_ps(x, y), z));
      }

      {
        auto p = _mm_load_ps(X + 4*i + 4),
             x = _mm_mul_ps(c0, _mm_shuffle_ps(p, p, _MM_SHUFFLE(0,0,0,0))),
             y = _mm_mul_ps(c1, _mm_shuffle_ps(p, p, _MM_SHUFFLE(1,1,1,1))),
             z = _mm_mul_ps(c2, _mm_shuffle_ps(p, p, _MM_SHUFFLE(2,2,2,2)));
        x1 = _mm_add_ps(c3, _mm_add_ps(_mm_add_ps(x, y), z));
      }

      auto zzzz = _mm_shuffle_ps(x0, x1, _MM_SHUFFLE(2,2,2,2));
      xf = _mm_div_ps(_mm_shuffle_ps(x0, x1, _MM_SHUFFLE(1,0,1,0)), zzzz);
      xi = _mm_cvtps_epi32(_mm_add_ps(xf, HALF));
      mask = _mm_and_si128(_mm_cmpgt_epi32(xi, LB), _mm_cmplt_epi32(xi, UB));
      xi   = _mm_and_si128(mask, xi);

      _mm_store_si128((__m128i*) buf, xi);

      valid[i + 0] = (buf[0] && buf[1]);
      valid[i + 1] = (buf[2] && buf[3]);

      xf = _mm_sub_ps(xf, _mm_cvtepi32_ps(xi));
      auto wx = _mm_sub_ps(ONES, xf);

      auto xx = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(0,0,0,0)),
           yy = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(1,1,1,1));
      yy = _mm_shuffle_ps(yy, yy, _MM_SHUFFLE(2,0,2,0));

      int u0 = buf[0], v0 = buf[1];
      const auto* ii = I + v0*stride + u0;
      auto I0 = static_cast<float>( *ii ),
           I1 = static_cast<float>( *(ii + 1) ),
           I2 = static_cast<float>( *(ii + stride) ),
           I3 = static_cast<float>( *(ii + stride + 1) );

      i0 = sumv( _mm_mul_ps(_mm_mul_ps(xx, yy), _mm_set_ps(I3, I2, I1, I0)) );

      xx = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(2,2,2,2));
      yy = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(3,3,3,3));
      yy = _mm_shuffle_ps(yy, yy, _MM_SHUFFLE(2,0,2,0));

      u0 = buf[2]; v0 = buf[3];
      ii = I + v0*stride + u0;
      I0 = static_cast<float>( *ii );
      I1 = static_cast<float>( *(ii + 1) );
      I2 = static_cast<float>( *(ii + stride) );
      I3 = static_cast<float>( *(ii + stride + 1) );

      i1 = sumv( _mm_mul_ps(_mm_mul_ps(xx, yy), _mm_set_ps(I3, I2, I1, I0)) );
    }

    {
      __m128 x0, x1, xf;
      __m128i mask, xi;

      {
        auto p = _mm_load_ps(X + 4*i + 8),
             x = _mm_mul_ps(c0, _mm_shuffle_ps(p, p, _MM_SHUFFLE(0,0,0,0))),
             y = _mm_mul_ps(c1, _mm_shuffle_ps(p, p, _MM_SHUFFLE(1,1,1,1))),
             z = _mm_mul_ps(c2, _mm_shuffle_ps(p, p, _MM_SHUFFLE(2,2,2,2)));
        x0 = _mm_add_ps(c3, _mm_add_ps(_mm_add_ps(x, y), z));
      }

      {
        auto p = _mm_load_ps(X + 4*i + 12),
             x = _mm_mul_ps(c0, _mm_shuffle_ps(p, p, _MM_SHUFFLE(0,0,0,0))),
             y = _mm_mul_ps(c1, _mm_shuffle_ps(p, p, _MM_SHUFFLE(1,1,1,1))),
             z = _mm_mul_ps(c2, _mm_shuffle_ps(p, p, _MM_SHUFFLE(2,2,2,2)));
        x1 = _mm_add_ps(c3, _mm_add_ps(_mm_add_ps(x, y), z));
      }

      auto zzzz = _mm_shuffle_ps(x0, x1, _MM_SHUFFLE(2,2,2,2));
      xf = _mm_div_ps(_mm_shuffle_ps(x0, x1, _MM_SHUFFLE(1,0,1,0)), zzzz);
      xi = _mm_cvtps_epi32(_mm_add_ps(xf, HALF));
      mask = _mm_and_si128(_mm_cmpgt_epi32(xi, LB), _mm_cmplt_epi32(xi, UB));
      xi   = _mm_and_si128(mask, xi);

      _mm_store_si128((__m128i*) buf, xi);

      valid[i + 2] = (buf[0] && buf[1]);
      valid[i + 3] = (buf[2] && buf[3]);

      xf = _mm_sub_ps(xf, _mm_cvtepi32_ps(xi));
      auto wx = _mm_sub_ps(ONES, xf);

      auto xx = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(0,0,0,0)),
           yy = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(1,1,1,1));
      yy = _mm_shuffle_ps(yy, yy, _MM_SHUFFLE(2,0,2,0));

      int u0 = buf[0], v0 = buf[1];
      const auto* ii = I + v0*stride + u0;
      auto I0 = static_cast<float>( *ii ),
           I1 = static_cast<float>( *(ii + 1) ),
           I2 = static_cast<float>( *(ii + stride) ),
           I3 = static_cast<float>( *(ii + stride + 1) );

      i2 = sumv( _mm_mul_ps(_mm_mul_ps(xx, yy), _mm_set_ps(I3, I2, I1, I0)) );

      xx = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(2,2,2,2));
      yy = _mm_shuffle_ps(wx, xf, _MM_SHUFFLE(3,3,3,3));
      yy = _mm_shuffle_ps(yy, yy, _MM_SHUFFLE(2,0,2,0));

      u0 = buf[2]; v0 = buf[3];
      ii = I + v0*stride + u0;
      I0 = static_cast<float>( *ii );
      I1 = static_cast<float>( *(ii + 1) );
      I2 = static_cast<float>( *(ii + stride) );
      I3 = static_cast<float>( *(ii + stride + 1) );

      i3 = sumv( _mm_mul_ps(_mm_mul_ps(xx, yy), _mm_set_ps(I3, I2, I1, I0)) );
    }

    auto z1 = _mm_shuffle_ps(i0, i1, _MM_SHUFFLE(0,0,0,0)),
         z2 = _mm_shuffle_ps(i2, i3, _MM_SHUFFLE(0,0,0,0)),
         zz = _mm_shuffle_ps(z1, z2, _MM_SHUFFLE(2,0,2,0));

    _mm_store_ps(residuals + i, _mm_sub_ps(_mm_load_ps(I_ref + i), zz));

    if(I_warped)
      _mm_store_ps(I_warped + i, zz);

    num_valid += valid[i + 0] + valid[i + 1] + valid[i + 2] + valid[i + 3];
  }

  typedef Eigen::Map<const Eigen::Vector4f, Eigen::Aligned> Point4Map;
  const auto PP = Eigen::Map<const Eigen::Matrix4f, Eigen::Aligned>(P).block<3,4>(0,0);

  for( ; i < N; ++i)
  {
    Eigen::Vector3f Xw = PP * Point4Map(X + 4*i);
    float z_i = 1.0f / Xw[2];
    float xf = Xw[0] * z_i; // / Xw[2],
    float yf = Xw[1] * z_i; // / Xw[2];
    int xi = static_cast<int>( xf + 0.5f ),
        yi = static_cast<int>( yf + 0.5f );

    valid[i] = (xi >= 0) && (xi < w-1) && (yi >= 0) && (yi < h-1);

    if(valid[i])
    {
      xf -= xi;
      yf -= yi;

      const auto* p0 = I + yi*stride + xi;
      float i0 = static_cast<float>( *p0 ),
            i1 = static_cast<float>( *(p0 + 1) ),
            i2 = static_cast<float>( *(p0 + stride) ),
            i3 = static_cast<float>( *(p0 + stride + 1) ),
            Iw = (1.0f-yf) * ((1.0f-xf)*i0 + xf*i1) +
                yf  * ((1.0f-xf)*i2 + xf*i3);
      residuals[i] = I_ref[i] - Iw;

      if(I_warped)
        I_warped[i] = Iw;

      num_valid += 1;
    }
  }

  if(_MM_ROUND_TOWARD_ZERO != rounding_mode) _MM_SET_ROUNDING_MODE(rounding_mode);
  if(_MM_FLUSH_ZERO_ON != flush_mode) _MM_SET_FLUSH_ZERO_MODE(flush_mode);

  return num_valid;
}

int imwarp3(const uint8_t* I_ptr, int w, int h, const float* H_ptr, const float* X,
            const float* I_ref, float* residuals, uint8_t* valid, int N, float* I_warped)
{
  int stride = w, max_cols = w - 1, max_rows = h - 1;
  int num_valid = 0;

  auto I = [=](int r, int c) { return *(I_ptr + r*stride + c); };

  const Eigen::Map<const Matrix33f> H(H_ptr);
  for(int i = 0; i < N; ++i)
  {
    Vector3f Xw = H * Eigen::Map<const Vector3f>(X + 3*i);
    Xw *= (1.0f / Xw[2]);

    float xf = Xw[0],
          yf = Xw[1];

    int xi = static_cast<int>(xf + 0.5f),
        yi = static_cast<int>(yf + 0.5f);

    xf -= xi;
    yf -= yi;

    if( xi >= 0 && xi < max_cols && yi >= 0 && yi < max_rows ) {
      valid[i] = 1;
      const float wx = 1.0 - xf;
      float Iw = (1.0 - yf) * ( I(yi,   xi)*wx + I(yi,   xi+1)*xf )
          +  yf  * ( I(yi+1, xi)*wx + I(yi+1, xi+1)*xf );

      residuals[i] = I_ref[i] - Iw;

      if(I_warped)
        I_warped[i] = Iw;

      num_valid++;
    } else {
      valid[i] = false;
      residuals[i] = 0.0f;
      if(I_warped)
        I_warped[i] = 0.0f;
    }
  }

  return num_valid;
}

#else

int imwarp(const uint8_t* , int, int, const float*, const float*,
           const float*, float*, uint8_t*, int,
           float*)
{
  THROW_ERROR("simd::imwarp requires SSE3");
}

#endif

} // bp


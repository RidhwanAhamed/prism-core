#pragma once

// Internal DSP primitives for the render path. Everything here is allocation-free and
// branch-cheap: these run per sample on the audio thread (real-time rule 1).

#include <cmath>

namespace prism::pgae::detail {

// One-pole exponential approach toward a target — the native equivalent of Web Audio's
// setTargetAtTime, which the probe used for every smoothed parameter (PGAE §7: gain never
// steps).
struct OnePole {
  double value = 0.0;
  double coeff = 0.0;

  void configure(double tc_seconds, double sample_rate) {
    coeff = 1.0 - std::exp(-1.0 / (tc_seconds * sample_rate));
  }
  double next(double target) {
    value += (target - value) * coeff;
    return value;
  }
};

// RBJ cookbook low-pass biquad (direct form 1). Coefficients are recomputed per small
// block as the smoothed cutoff moves — cheap, and the parameter itself never steps.
struct BiquadLowpass {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

  void set(double cutoff_hz, double q, double sample_rate) {
    const double limited = cutoff_hz < 10.0                 ? 10.0
                           : cutoff_hz > sample_rate * 0.45 ? sample_rate * 0.45
                                                            : cutoff_hz;
    const double w0 = 2.0 * 3.141592653589793 * limited / sample_rate;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    b0 = ((1.0 - cos_w0) / 2.0) / a0;
    b1 = (1.0 - cos_w0) / a0;
    b2 = ((1.0 - cos_w0) / 2.0) / a0;
    a1 = (-2.0 * cos_w0) / a0;
    a2 = (1.0 - alpha) / a0;
  }

  double process(double x) {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
};

// Peak limiter with attack/release envelope follower plus a hard safety clamp: the
// smoothed gain computer does the musical work; the clamp guarantees the ceiling is never
// exceeded no matter what (PGAE §8: the rail wins unconditionally, and it is the last
// stage).
struct PeakLimiter {
  double ceiling = 1.0;
  double attack_coeff = 0.0;
  double release_coeff = 0.0;
  double envelope = 0.0;

  void configure(double ceiling_amp, double attack_s, double release_s, double sample_rate) {
    ceiling = ceiling_amp;
    attack_coeff = 1.0 - std::exp(-1.0 / (attack_s * sample_rate));
    release_coeff = 1.0 - std::exp(-1.0 / (release_s * sample_rate));
  }

  double process(double x) {
    const double magnitude = std::fabs(x);
    envelope += (magnitude - envelope) * (magnitude > envelope ? attack_coeff : release_coeff);
    double y = x;
    if (envelope > ceiling) {
      y = x * (ceiling / envelope);
    }
    if (y > ceiling) {
      y = ceiling;
    } else if (y < -ceiling) {
      y = -ceiling;
    }
    return y;
  }
};

} // namespace prism::pgae::detail

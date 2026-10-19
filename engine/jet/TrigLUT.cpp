#include "TrigLUT.hpp"
#include "FastMath.hpp"
#include <cmath>
#include <limits>

namespace Renderer {

// carts patch: the tables are allocated by the first initializeTrigTables (were static
// arrays: 17 KB of a microcontroller's internal RAM whether a scene exists or not).
int32_t* sin_table = nullptr;
int32_t* tan_table = nullptr;

#if FLOAT_CAMERA_ANGLES
float* float_sin_table = nullptr;
#if FLOAT_TAN_CACHE_SCALE > 1
float* float_tan_table = nullptr;
#endif
#endif

void initializeTrigTables() {
    if (sin_table) return;  // carts patch: constant; made once, by the first scene
    sin_table = new int32_t[ANGLE_MAX];
    tan_table = new int32_t[ANGLE_MAX];
#if FLOAT_CAMERA_ANGLES
    float_sin_table = new float[FLOAT_SIN_ANGLE_MAX];
#if FLOAT_TAN_CACHE_SCALE > 1
    float_tan_table = new float[FLOAT_TAN_ANGLE_MAX];
#endif
#endif
    for (int i = 0; i < ANGLE_MAX; ++i) {
        double radians = i * M_PI / 180.0;
        sin_table[i] = static_cast<int32_t>(std::sin(radians) * FIXED_POINT_SCALE);
        
        // Calculate cos using sin with phase shift
        double cos_val = std::sin((i + 90) * M_PI / 180.0);
        int32_t cos = static_cast<int32_t>(cos_val * FIXED_POINT_SCALE);
        
        // Handle tan() calculation
        if (cos != 0) {
            tan_table[i] = (sin_table[i] * FIXED_POINT_SCALE) / cos;
        } else {
            tan_table[i] = INT32_MAX;
        }
    }

    #if FLOAT_CAMERA_ANGLES
    for (int i = 0; i < FLOAT_SIN_ANGLE_MAX; ++i) {
        double degrees = static_cast<double>(i) / FLOAT_SIN_SUBDIVISIONS;
        double radians = degrees * M_PI / 180.0;
        float_sin_table[i] = static_cast<float>(std::sin(radians));
    }

    #if FLOAT_TAN_CACHE_SCALE > 1
    for (int i = 0; i < FLOAT_TAN_ANGLE_MAX; ++i) {
        double degrees = static_cast<double>(i) / FLOAT_TAN_SUBDIVISIONS;
        double radians = degrees * M_PI / 180.0;
        float cos_val = std::cos(radians);
        if (std::abs(cos_val) > 1e-6) {
            float_tan_table[i] = static_cast<float>(std::sin(radians) / cos_val);
        } else {
            float_tan_table[i] = std::copysign(std::numeric_limits<float>::max(), std::sin(radians));
        }
    }
    #endif
    #endif
}

} // namespace Renderer

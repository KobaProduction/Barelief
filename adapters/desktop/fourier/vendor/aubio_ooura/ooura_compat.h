// SPDX-License-Identifier: GPL-3.0-or-later
// The exact primitive/math selection of the pinned genuine Float32 aubio build.
// This is compile configuration only; vendor arithmetic remains unchanged.
#pragma once
#include <math.h>
#include "ooura_backend_symbols.h"
typedef float smpl_t;
#define COS cosf
#define SIN sinf
#define ATAN atanf

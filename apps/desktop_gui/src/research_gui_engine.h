// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <pipeline/numeric_engine.h>

std::unique_ptr<pipeline::NumericEngine> make_research_gui_engine(pipeline::AudioSourceFactory);

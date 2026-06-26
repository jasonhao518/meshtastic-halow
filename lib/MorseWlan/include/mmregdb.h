/*
 * Compatibility shim for the EdgeZ/Morse static SDK path.
 *
 * Upstream SDK headers define regulatory-database constants and helpers in a
 * separate mmregdb header. In this repository we keep the precompiled archive
 * only, so we re-expose the same API from the local packaged definitions.
 */

#pragma once

#include "mmwlan.h"
#include "mmwlan_regdb.def"

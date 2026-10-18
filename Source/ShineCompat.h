#pragma once
// Shine (the MP3 encoder) is written for GCC; MSVC does not know __attribute__((unused)). Forced into every C file of the build.
#define __attribute__(x)

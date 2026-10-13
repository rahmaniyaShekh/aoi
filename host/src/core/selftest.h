// Shared by the selftest sections.
#pragma once
#include <string>
#include <vector>

namespace aoi::st {

void check(bool ok, const char *name, const std::string &detail = {});
std::string f1(const char *fmt, double a, double b = 0, double c = 0, double d = 0);

void test_voice();     // echo cancellation, noise suppression, level, voice activity, CPU
void test_policy();    // the microphone / talkback output choice
void test_boundary();  // core reaches the OS only through src/platform

// Test signals (48 kHz mono): speech-like syllables and phrases, and music.
std::vector<float> synth_speech(double seconds, unsigned seed, double f0);
std::vector<float> synth_music(double seconds, unsigned seed);

}  // namespace aoi::st

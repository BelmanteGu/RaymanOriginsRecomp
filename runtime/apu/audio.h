#pragma once
#include <cstdint>

// Destino dos frames de áudio do jogo: 256 amostras estéreo intercaladas (float).
using AudioSink = void (*)(const float* stereo, uint32_t frames);
void SetAudioSink(AudioSink sink);

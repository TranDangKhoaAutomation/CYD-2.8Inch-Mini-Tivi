#include "audio_selftest.h"

#include <Arduino.h>
#include <Audio.h>
#include <driver/i2s.h>
#include <math.h>

#include "config.h"

void playBootMaxBeeps(Audio &audio) {
#if USE_EXTERNAL_I2S_DAC
  return;
#else
  constexpr uint32_t sampleRate = 16000;
  constexpr float noteHz[3] = {1760.00f, 2217.46f, 2637.02f}; // A6, C#7, E7
  constexpr uint32_t noteMs = 190;
  constexpr uint32_t gapMs = 65;
  constexpr uint32_t attackMs = 6;
  constexpr uint32_t releaseMs = 30;
  constexpr size_t chunkFrames = 128;
  constexpr float twoPi = 6.28318530718f;
  constexpr float peak = 0.98f;
  uint32_t frames[chunkFrames];
  const i2s_port_t port = (i2s_port_t)audio.getI2sPort();
  uint32_t previousSampleRate = audio.getSampleRate();
  if (previousSampleRate < 8000 || previousSampleRate > 192000) previousSampleRate = sampleRate;

  // Reassert the exact path used by the CYD onboard amplifier. This makes the
  // diagnostic independent of whether a previous decoder/session changed I2S.
  esp_err_t dacErr = i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN); // GPIO26
  esp_err_t clkErr = i2s_set_clk(port, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  esp_err_t startErr = i2s_start(port);
  Serial.printf("[AUDIO][SELFTEST] begin port=%d oldRate=%lu dac=%d clk=%d start=%d\n",
                (int)port, (unsigned long)previousSampleRate, (int)dacErr, (int)clkErr, (int)startErr);
  if (dacErr != ESP_OK || clkErr != ESP_OK || startErr != ESP_OK) {
    Serial.println("[AUDIO][SELFTEST] I2S/DAC setup failed");
    return;
  }

  // Higher notes couple better through the tiny onboard speaker. A small second
  // harmonic adds presence without clipping; the envelope removes clicks.
  for (int note = 0; note < 3; ++note) {
    const uint32_t totalFrames = (sampleRate * noteMs) / 1000u;
    const uint32_t attackFrames = max<uint32_t>(1u, (sampleRate * attackMs) / 1000u);
    const uint32_t releaseFrames = max<uint32_t>(1u, (sampleRate * releaseMs) / 1000u);
    const float phaseStep = twoPi * noteHz[note] / (float)sampleRate;
    float phase = 0.0f;
    uint32_t produced = 0;

    while (produced < totalFrames) {
      const size_t n = min<size_t>(chunkFrames, totalFrames - produced);
      for (size_t i = 0; i < n; ++i) {
        const uint32_t frameIndex = produced + (uint32_t)i;
        float env = 1.0f;
        if (frameIndex < attackFrames) {
          env = (float)frameIndex / (float)attackFrames;
        } else if (frameIndex + releaseFrames >= totalFrames) {
          env = (float)(totalFrames - frameIndex) / (float)releaseFrames;
        }
        env = constrain(env, 0.0f, 1.0f);

        const float wave = 0.86f * sinf(phase) + 0.14f * sinf(phase * 2.0f);
        int32_t signedSample = (int32_t)lroundf(wave * (32767.0f * peak * env));
        signedSample = constrain(signedSample, -32768, 32767);
        const uint16_t dacSample = (uint16_t)(signedSample + 32768);
        frames[i] = ((uint32_t)dacSample << 16) | dacSample;
        phase += phaseStep;
        if (phase >= twoPi) phase -= twoPi;
      }
      size_t written = 0;
      if (i2s_write(port, frames, n * sizeof(uint32_t), &written, portMAX_DELAY) != ESP_OK || written != n * sizeof(uint32_t)) {
        Serial.printf("[AUDIO][SELFTEST] write failed note=%d wrote=%u/%u\n",
                      note, (unsigned)written, (unsigned)(n * sizeof(uint32_t)));
        return;
      }
      produced += (uint32_t)n;
    }

    const uint32_t silenceFrame = 0x80008000u;
    uint32_t remaining = (sampleRate * gapMs) / 1000u;
    while (remaining) {
      const size_t n = min<size_t>(chunkFrames, remaining);
      for (size_t i = 0; i < n; ++i) frames[i] = silenceFrame;
      size_t written = 0;
      if (i2s_write(port, frames, n * sizeof(uint32_t), &written, portMAX_DELAY) != ESP_OK || written != n * sizeof(uint32_t)) {
        Serial.println("[AUDIO][SELFTEST] silence write failed");
        return;
      }
      remaining -= (uint32_t)n;
    }
  }

  // Leave the DAC at silence and restore the decoder's previous clock if needed.
  const uint32_t silenceFrame = 0x80008000u;
  for (size_t i = 0; i < chunkFrames; ++i) frames[i] = silenceFrame;
  size_t written = 0;
  i2s_write(port, frames, sizeof(frames), &written, portMAX_DELAY);
  if (previousSampleRate != sampleRate) {
    i2s_set_clk(port, previousSampleRate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  }
  Serial.println("[AUDIO][SELFTEST] complete 3-note chime");
#endif
}

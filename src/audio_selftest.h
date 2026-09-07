#pragma once

class Audio;

// Three short full-scale tones through the onboard internal-DAC path.
// Intended as a boot-time hardware ceiling check, before the decoder task starts.
void playBootMaxBeeps(Audio &audio);

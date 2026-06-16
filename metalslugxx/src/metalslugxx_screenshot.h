// metalslugxx - ReXGlue Recompiled Project
//
// Guest-output capture: dumps the exact image the game handed to the presenter
// -- i.e. the guest framebuffer *before* the emulator's present-stage scaling
// (bilinear/CAS/FSR) -- to a PNG next to the executable (via lodepng).
//
// This is a graphics-diagnostic tool. The in-game playfield upscale bug we are
// chasing is baked into the resolved/untiled texels long before present-stage
// scaling, so a 1:1 capture of the handed-off buffer answers two things at once:
//   * its true dimensions (320x240 => the emulator does all enlargement;
//     larger => the game already upscaled internally), and
//   * whether the reshuffle is present in the buffer (=> upstream resolve/untile
//     bug) or only appears after present scaling (=> a present-filter issue).
// The dimensions are logged on every capture, so the log alone is informative.

#pragma once

namespace rex {
class Runtime;
}  // namespace rex

namespace metalslugxx {

// Captures the current guest-output framebuffer and writes it to
// `metalslugxx_guest_<timestamp>.png` in the executable folder. Logs the result
// (including the captured dimensions). Safe to call from the UI thread; a no-op
// with a warning if no frame has been presented yet.
void DumpGuestOutput(rex::Runtime* runtime);

}  // namespace metalslugxx

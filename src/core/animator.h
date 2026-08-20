#pragma once

// One easing primitive shared by every animated thing in this project (carousel scroll,
// modal open/close, widget hover/press) instead of each class reimplementing its own
// lerp-toward-target. All-static - there's exactly one piece of global state here (the
// enabled flag and speed multiplier the Settings panel drives), and every eased value in
// the project is owned by whatever CWidget it animates, not by this class.

class CAnimator {
  public:
	// Moves value toward target by a fraction of the remaining distance, framerate-
	// independent via deltaSeconds. rate is roughly "how many times per second the gap
	// closes by ~63%" - higher is snappier. Call once per frame. Snaps straight to target
	// when SetEnabled(false) - the Settings panel's Animations toggle wires here, one
	// place, since every animated value in the project already goes through this. rate is
	// also scaled by SetSpeed's multiplier (the Animation Speed setting) - 1.0 is the
	// speed every rate constant in the project was tuned at.
	static float EaseToward(float value, float target, float rate, float deltaSeconds);

	static void SetEnabled(bool enabled);
	static bool IsEnabled();

	static void SetSpeed(float speed);
	static float GetSpeed();

  private:
	static bool s_bEnabled;
	static float s_flSpeed;
};

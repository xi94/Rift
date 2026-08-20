#include "core/animator.h"

#include <cmath>

bool CAnimator::s_bEnabled = true;
float CAnimator::s_flSpeed = 1.0f;

float CAnimator::EaseToward(float value, float target, float rate, float deltaSeconds)
{
	if (!s_bEnabled) {
		return target;
	}

	const float t = 1.0f - std::exp(-rate * s_flSpeed * deltaSeconds);
	return value + (target - value) * t;
}

void CAnimator::SetEnabled(bool enabled)
{
	s_bEnabled = enabled;
}

bool CAnimator::IsEnabled()
{
	return s_bEnabled;
}

void CAnimator::SetSpeed(float speed)
{
	s_flSpeed = speed;
}

float CAnimator::GetSpeed()
{
	return s_flSpeed;
}

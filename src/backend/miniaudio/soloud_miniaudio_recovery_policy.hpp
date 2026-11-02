#ifndef SOLOUD_MINIAUDIO_RECOVERY_POLICY_H
#define SOLOUD_MINIAUDIO_RECOVERY_POLICY_H

namespace SoLoud
{
	enum class MiniaudioRecoveryTarget
	{
		Selected,
		Default,
		Unavailable,
	};

	template <typename DeviceOpen>
	MiniaudioRecoveryTarget chooseMiniaudioRecoveryTarget(bool hasSelectedDevice, DeviceOpen deviceOpen)
	{
		if (hasSelectedDevice && deviceOpen(true))
			return MiniaudioRecoveryTarget::Selected;
		if (deviceOpen(false))
			return MiniaudioRecoveryTarget::Default;
		return MiniaudioRecoveryTarget::Unavailable;
	}
}

#endif

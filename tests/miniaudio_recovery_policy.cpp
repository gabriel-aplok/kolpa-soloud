#include <cstdio>
#include <vector>

#include "backend/miniaudio/soloud_miniaudio_recovery_policy.hpp"

namespace
{
	bool verify(bool hasSelectedDevice, const std::vector<bool> &openResults,
				const std::vector<bool> &expectedAttempts, SoLoud::MiniaudioRecoveryTarget expectedTarget)
	{
		std::vector<bool> attempts;
		std::size_t resultIndex = 0;
		const auto target = SoLoud::chooseMiniaudioRecoveryTarget(hasSelectedDevice, [&](bool selected)
		{
			attempts.push_back(selected);
			if (resultIndex == openResults.size())
				return false;
			return openResults[resultIndex++];
		});
		return attempts == expectedAttempts && target == expectedTarget;
	}
}

int main()
{
	if (!verify(true, {true}, {true}, SoLoud::MiniaudioRecoveryTarget::Selected) ||
		!verify(true, {false, true}, {true, false}, SoLoud::MiniaudioRecoveryTarget::Default) ||
		!verify(false, {true}, {false}, SoLoud::MiniaudioRecoveryTarget::Default) ||
		!verify(true, {false, false}, {true, false}, SoLoud::MiniaudioRecoveryTarget::Unavailable))
	{
		std::printf("miniaudio recovery fallback policy failed\n");
		return 1;
	}
	std::printf("miniaudio recovery fallback policy ok\n");
	return 0;
}

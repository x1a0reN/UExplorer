#include "CoreSession.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <utility>

namespace UExplorer::Runtime
{

bool TryGenerateCoreSessionId(std::string& sessionId) noexcept
{
	sessionId.clear();
	std::array<std::uint8_t, 16> randomBytes{};
	const NTSTATUS status = BCryptGenRandom(
		nullptr,
		randomBytes.data(),
		static_cast<ULONG>(randomBytes.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (!BCRYPT_SUCCESS(status))
		return false;

	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string generated = "core-";
	generated.reserve(5 + randomBytes.size() * 2);
	for (const std::uint8_t value : randomBytes)
	{
		generated.push_back(kHex[value >> 4]);
		generated.push_back(kHex[value & 0x0F]);
	}
	sessionId = std::move(generated);
	return true;
}

} // namespace UExplorer::Runtime

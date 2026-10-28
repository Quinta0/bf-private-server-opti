#pragma once

#include <cryptopp/filters.h>
#include <cryptopp/hex.h>
#include <cryptopp/osrng.h>
#include <cryptopp/pwdbased.h>
#include <cryptopp/sha.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/*!
* Account and session helpers for internet-facing deployments: identifiers and
* tokens come from the OS CSPRNG (not the gameplay mt19937), and only hashes of
* tokens and device identifiers are stored.
*/
namespace account
{

/*!
* Random bytes from the OS CSPRNG, hex-encoded (lowercase).
* @param byteCount Number of random bytes (output is twice as long).
*/
inline std::string SecureRandomHex(size_t byteCount)
{
	CryptoPP::AutoSeededRandomPool rng;
	std::string bytes(byteCount, '\0');
	rng.GenerateBlock(reinterpret_cast<CryptoPP::byte*>(bytes.data()), bytes.size());

	std::string out;
	CryptoPP::StringSource(bytes, true, new CryptoPP::HexEncoder(new CryptoPP::StringSink(out), false));
	return out;
}

/*!
* Random alphanumeric identifier from the OS CSPRNG (rejection-sampled, so
* every character is uniformly distributed).
* @param length Identifier length.
*/
inline std::string SecureRandomId(size_t length)
{
	static constexpr std::string_view kAlphabet =
		"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	// Largest multiple of the alphabet size that fits in a byte.
	constexpr unsigned kLimit = 256 - (256 % kAlphabet.size());

	CryptoPP::AutoSeededRandomPool rng;
	std::string out;
	out.reserve(length);
	while (out.size() < length)
	{
		const auto b = rng.GenerateByte();
		if (b < kLimit)
			out.push_back(kAlphabet[b % kAlphabet.size()]);
	}
	return out;
}

/*!
* SHA-256 of the input, hex-encoded (lowercase).
*/
inline std::string Sha256Hex(std::string_view input)
{
	CryptoPP::SHA256 hash;
	std::string out;
	CryptoPP::StringSource(
		reinterpret_cast<const CryptoPP::byte*>(input.data()), input.size(), true,
		new CryptoPP::HashFilter(hash, new CryptoPP::HexEncoder(new CryptoPP::StringSink(out), false)));
	return out;
}

/*!
* Comparison whose running time does not depend on where the inputs differ.
*/
inline bool ConstantTimeEquals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); ++i)
		diff |= static_cast<unsigned char>(a[i] ^ b[i]);
	return diff == 0;
}

/*!
* Account transfer code: 10 ASCII digits from the OS CSPRNG, the first never
* '0' so the client's NUMERIC input box cannot drop it as a leading zero.
* Rejection sampling keeps every digit uniform (250 and 252 are the largest
* multiples of 10 and 9 that fit in a byte).
*/
inline std::string NewTransferCode()
{
	CryptoPP::AutoSeededRandomPool rng;
	std::string code;
	code.reserve(10);
	while (code.empty())
	{
		const auto b = rng.GenerateByte();
		if (b < 252)
			code.push_back(static_cast<char>('1' + b % 9));
	}
	while (code.size() < 10)
	{
		const auto b = rng.GenerateByte();
		if (b < 250)
			code.push_back(static_cast<char>('0' + b % 10));
	}
	return code;
}

/*!
* A typed Transfer ID with surrounding ASCII whitespace removed, or nullopt
* unless what remains is exactly 10 digits.
*/
inline std::optional<std::string> NormalizeTransferCode(std::string_view input)
{
	constexpr std::string_view kSpace = " \t\r\n\f\v";
	const auto first = input.find_first_not_of(kSpace);
	if (first == std::string_view::npos)
		return std::nullopt;
	const auto trimmed = input.substr(first, input.find_last_not_of(kSpace) - first + 1);
	if (trimmed.size() != 10)
		return std::nullopt;
	for (const char c : trimmed)
		if (c < '0' || c > '9')
			return std::nullopt;
	return std::string(trimmed);
}

/*!
* What the database keys a transfer code on.  A plain hash is enough: the
* code carries ~33 bits from the CSPRNG and the password, not the code, is
* what an attacker with the database would have to guess.
*/
inline std::string TransferCodeHash(std::string_view code)
{
	return Sha256Hex(std::string("code:") + std::string(code));
}

/*!
* The client's own rule for a new transfer password: 4-12 bytes, each
* [A-Za-z0-9] (TextInputUtils::checkPassword @0x11D521C rejects any byte
* >= 0x80, so byte and character counts agree for a valid password).
*/
inline bool ValidTransferPassword(std::string_view password)
{
	if (password.size() < 4 || password.size() > 12)
		return false;
	for (const char c : password)
	{
		const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (!alnum)
			return false;
	}
	return true;
}

namespace detail
{

constexpr std::string_view kPbkdf2Scheme = "pbkdf2-sha256";
constexpr size_t kPbkdf2SaltBytes = 16;
constexpr size_t kPbkdf2KeyBytes = 32;

inline std::string ToHex(const CryptoPP::byte* data, size_t size)
{
	std::string out;
	CryptoPP::StringSource(data, size, true, new CryptoPP::HexEncoder(new CryptoPP::StringSink(out), false));
	return out;
}

inline std::optional<std::string> FromHex(std::string_view hex, size_t expectedBytes)
{
	if (hex.size() != expectedBytes * 2 || hex.find_first_not_of("0123456789abcdef") != std::string_view::npos)
		return std::nullopt;
	std::string out;
	CryptoPP::StringSource(reinterpret_cast<const CryptoPP::byte*>(hex.data()), hex.size(), true,
		new CryptoPP::HexDecoder(new CryptoPP::StringSink(out)));
	return out;
}

inline std::string DerivePbkdf2(std::string_view password, std::string_view salt, uint32_t iterations)
{
	CryptoPP::byte key[kPbkdf2KeyBytes];
	CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA256>().DeriveKey(
		key, sizeof(key), 0,
		reinterpret_cast<const CryptoPP::byte*>(password.data()), password.size(),
		reinterpret_cast<const CryptoPP::byte*>(salt.data()), salt.size(),
		iterations, 0);
	return ToHex(key, sizeof(key));
}

} // namespace detail

/*!
* PBKDF2-HMAC-SHA256 of a transfer password with a fresh 16-byte salt,
* encoded "pbkdf2-sha256$<iterations>$<salt hex>$<key hex>" so a later change
* of the work factor never invalidates stored hashes.  Slow on purpose: call
* it off the IO loops.
*/
inline std::string HashTransferPassword(std::string_view password, uint32_t iterations)
{
	CryptoPP::AutoSeededRandomPool rng;
	CryptoPP::byte salt[detail::kPbkdf2SaltBytes];
	rng.GenerateBlock(salt, sizeof(salt));
	const std::string saltBytes(reinterpret_cast<const char*>(salt), sizeof(salt));
	return std::string(detail::kPbkdf2Scheme) + "$" + std::to_string(iterations) + "$"
		+ detail::ToHex(salt, sizeof(salt)) + "$" + detail::DerivePbkdf2(password, saltBytes, iterations);
}

/*!
* Checks a password against HashTransferPassword's encoding, re-deriving with
* the stored iteration count.  A malformed encoding never verifies.
*/
inline bool VerifyTransferPassword(std::string_view password, std::string_view encoded)
{
	std::string_view parts[4];
	for (size_t i = 0; i < 4; ++i)
	{
		const auto dollar = encoded.find('$');
		if ((i < 3) == (dollar == std::string_view::npos))
			return false;
		parts[i] = encoded.substr(0, dollar);
		encoded = i < 3 ? encoded.substr(dollar + 1) : std::string_view{};
	}
	uint32_t iterations = 0;
	const auto& it = parts[1];
	if (parts[0] != detail::kPbkdf2Scheme
		|| std::from_chars(it.data(), it.data() + it.size(), iterations).ptr != it.data() + it.size()
		|| iterations == 0)
		return false;
	const auto salt = detail::FromHex(parts[2], detail::kPbkdf2SaltBytes);
	if (!salt || parts[3].size() != detail::kPbkdf2KeyBytes * 2)
		return false;
	return ConstantTimeEquals(detail::DerivePbkdf2(password, *salt, iterations), parts[3]);
}

} // namespace account

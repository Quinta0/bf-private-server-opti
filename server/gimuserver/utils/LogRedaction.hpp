#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace logredact
{

/*!
* Keys whose values must never reach the HTTP dump logs (deploy/log):
*   iN7buP1i  gumi session token (every GME request)
*   LZ2PfNm4  transfer code (TransferCodeIssue reply, Check/Transfer requests)
*   4WSu1irc  transfer password (Issue request and reply, Check/Transfer requests)
* Initialize sends LZ2PfNm4/4WSu1irc empty; empty values are left as they are,
* so a dump still shows whether a field was present and filled.
*/
inline constexpr std::initializer_list<std::string_view> kSecretKeys = { "iN7buP1i", "LZ2PfNm4", "4WSu1irc" };

/*!
* Copy of `json` with the string value of every `"key": "..."` pair whose key
* is in `keys` replaced by "<redacted>" (non-empty values only).  A plain
* linear scan, not std::regex: dumps include multi-megabyte replies, and
* std::regex recurses per character.  Non-string values are left alone.
*/
inline std::string RedactJsonStrings(std::string_view json,
	std::initializer_list<std::string_view> keys = kSecretKeys)
{
	const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

	std::string out;
	out.reserve(json.size());
	size_t pos = 0;
	while (pos < json.size())
	{
		const auto quote = json.find('"', pos);
		if (quote == std::string_view::npos)
			break;

		// The quoted string starting at `quote`; skip escapes.
		size_t end = quote + 1;
		while (end < json.size() && json[end] != '"')
			end += json[end] == '\\' ? 2 : 1;
		if (end >= json.size())
			break;

		const auto token = json.substr(quote + 1, end - quote - 1);
		bool secret = false;
		for (const auto key : keys)
			secret = secret || token == key;

		size_t next = end + 1;
		if (secret)
		{
			// Expect  ws ':' ws '"'  then the value.
			size_t p = next;
			while (p < json.size() && isSpace(json[p]))
				++p;
			if (p < json.size() && json[p] == ':')
			{
				++p;
				while (p < json.size() && isSpace(json[p]))
					++p;
				if (p < json.size() && json[p] == '"')
				{
					size_t valueEnd = p + 1;
					while (valueEnd < json.size() && json[valueEnd] != '"')
						valueEnd += json[valueEnd] == '\\' ? 2 : 1;
					if (valueEnd < json.size())
					{
						out.append(json.substr(pos, p - pos));
						out.append(valueEnd == p + 1 ? "\"\"" : "\"<redacted>\"");
						pos = valueEnd + 1;
						continue;
					}
				}
			}
		}
		out.append(json.substr(pos, next - pos));
		pos = next;
	}
	if (pos < json.size())
		out.append(json.substr(pos));
	return out;
}

} // namespace logredact

#pragma once

#include <glaze/glaze.hpp>

#include <algorithm>
#include <string>

namespace forge
{

/*!
* A glaze read error as one line a balancer can act on: the missing or unknown
* field by name where glaze knows it, else the error and the text around it.
* (glz::format_error's caret drawing spans lines and, on a merged table,
* quotes megabytes of context.)
*/
inline std::string plainReadError(const glz::error_ctx& ec, const std::string& text)
{
	const std::string code(glz::meta<glz::error_code>::keys[static_cast<uint32_t>(ec.ec)]);
	const std::string detail(ec.custom_error_message);
	const auto at = std::min(ec.count, text.size());
	if (ec.ec == glz::error_code::missing_key && !detail.empty())
		return "\"" + detail + "\" is missing.";
	if (ec.ec == glz::error_code::unknown_key && at > 0)
	{
		// glaze stops at the key it does not know (inside it or just past
		// it): the quoted string followed by ':' around that point names it.
		const auto keyAt = [&text](size_t open) -> std::string {
			const auto close = text.find('"', open + 1);
			if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':')
				return {};
			return text.substr(open + 1, close - open - 1);
		};
		const auto quote = text.rfind('"', at - 1);
		std::string key = quote == std::string::npos ? std::string() : keyAt(quote);
		if (key.empty() && quote != std::string::npos && quote > 0)
		{
			const auto open = text.rfind('"', quote - 1);
			if (open != std::string::npos)
				key = keyAt(open);
		}
		if (!key.empty())
			return "\"" + key + "\" is not a field it takes.";
	}
	const auto from = at > 40 ? at - 40 : 0;
	return code + (detail.empty() ? "" : " " + detail) + " near \"" + text.substr(from, std::min<size_t>(60, text.size() - from)) + "\".";
}

} // namespace forge

#pragma once

#include <json/json.h>

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>

/*! Small jsoncpp helpers the Forge loaders share. */
namespace forge
{

/*! One line of JSON, UTF-8 kept as is. */
inline std::string compactJson(const Json::Value& v)
{
	Json::StreamWriterBuilder w;
	w["indentation"] = "";
	w["emitUTF8"] = true;
	return Json::writeString(w, v);
}

/*! An id or key value as text: "18060" for "18060" and 18060 alike (labels, protected ids). */
inline std::string valueText(const Json::Value& v)
{
	if (v.isString())
		return v.asString();
	if (v.isUInt64() && !v.isInt64())
		return std::to_string(v.asUInt64());
	if (v.isInt64())
		return std::to_string(v.asInt64());
	return compactJson(v);
}

/*! A whole number written as a number or as the base tables' quoted text ("18060"). */
inline std::optional<int64_t> wholeNumber(const Json::Value& v)
{
	if (v.isInt64())
		return v.asInt64();
	if (!v.isString())
		return std::nullopt;
	const auto s = v.asString();
	int64_t out = 0;
	const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
	if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size())
		return std::nullopt;
	return out;
}

} // namespace forge

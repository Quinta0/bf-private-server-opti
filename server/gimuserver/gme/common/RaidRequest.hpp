#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// Loose reader for Classic Raid request bodies.
//
// The field requests carry groups that come and go (a pending move in
// 81B4TvEK, client events in QP4cwt7n, a helper row in wc2DiMY1) and the
// client's JsonNode::addParam writes every number as a string
// (CommonUtils::IntToString @0xfdb85c), but a test or a later client build may
// not.  Reading the body as a generic tree and pulling each key as text keeps a
// missing or re-typed group from failing the whole request; the typed
// RaidBareReq still carries the login info.
namespace gme::raid
{

class RaidRequest
{
public:
	explicit RaidRequest(const std::string& json)
	{
		if (glz::read_json(root_, json))
			root_ = glz::generic{};
	}

	/*! Every node of a group, whether the client sent a list or one object. */
	std::vector<const glz::generic*> rows(const char* group) const
	{
		std::vector<const glz::generic*> out;
		if (!root_.is_object() || !root_.contains(group))
			return out;
		const auto& value = root_[group];
		if (value.is_array())
		{
			for (const auto& row : value.get_array())
				if (row.is_object())
					out.push_back(&row);
		}
		else if (value.is_object())
		{
			out.push_back(&value);
		}
		return out;
	}

	/*! The first node of a group, or nullptr. */
	const glz::generic* node(const char* group) const
	{
		const auto all = rows(group);
		return all.empty() ? nullptr : all.front();
	}

	/*! A key of the first node of a group as text ("" when absent). */
	std::string text(const char* group, const char* key) const
	{
		return textOf(node(group), key);
	}

	/*! The raw JSON of a group, for echoing it back untouched ("" when absent). */
	std::string raw(const char* group) const
	{
		if (!root_.is_object() || !root_.contains(group))
			return {};
		std::string out;
		if (glz::write_json(root_[group], out))
			return {};
		return out;
	}

	static std::string textOf(const glz::generic* row, const char* key)
	{
		if (row == nullptr || !row->is_object() || !row->contains(key))
			return {};
		const auto& value = (*row)[key];
		if (value.is_string())
			return value.get<std::string>();
		if (value.is_number())
			return std::to_string(static_cast<int64_t>(value.get<double>()));
		return {};
	}

	static int64_t numberOf(const glz::generic* row, const char* key, int64_t fallback = 0)
	{
		const auto value = textOf(row, key);
		if (value.empty())
			return fallback;
		char* end = nullptr;
		const auto parsed = std::strtoll(value.c_str(), &end, 10);
		return end == value.c_str() ? fallback : parsed;
	}

private:
	glz::generic root_{};
};

} // namespace gme::raid

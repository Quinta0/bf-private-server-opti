#include "App.hpp"
#include "Handlers.hpp"

#include <gimuserver/gme/common/Colosseum.hpp>

// ChallengeArenaResetInfo
//
// By default the server returns GmeError{cmd=Close}, which the
// client interpreted as a connection failure. The connect scene never
// transitioned away, so the game loop re-fired the request every frame.
//
// With this handler returning empty timestamps (instead of an error), the client parses
// a valid response and stores server_time + a local snapshot. needServerRefresh() computes:
//
//     server_time + now() - local_snapshot > daily_cooling_end
//
// Assume fields start at zero. After parsing the empty response, server_time stays 0,
// local_snapshot is set to the current time (via time()), and daily_cooling_end stays 0.
// So the check becomes: 0 + now() - local_snapshot > 0.
//
// Since local_snapshot == the time() value from just after parsing, this is false until
// now() ticks forward to the next second.
//
// To stop the client from retrying every second, we'll set daily_cooling_end to an hour in the
// future.
//
// TODO: figure out the actual intended cooldown time and use that instead.
HANDLEF(ChallengeArenaResetInfo)
{
	using namespace std::chrono;
	using namespace std::chrono_literals;

	::ChallengeArenaResetInfoResp resp{};

	// I've tested this with 5 seconds as well, which causes the client to retry every 6 seconds.
	// This makes sense since its an explicit greater-than check.
	// Using an hour here as a placeholder to prevent the client from constantly retrying and polluting
	// our logs (and our disks :D).
	resp.reset_info.server_time = floor<milliseconds>(system_clock::now());
	resp.reset_info.daily_cooling_end = resp.reset_info.server_time + 1h;

	std::string buffer{};
	const auto& ec = glz::write_json(resp, buffer);
	if (ec) {
		const auto& glze = glz::format_error(ec, buffer);
		LOG_DEBUG << "Gme ChallengeArenaResetInfo Error during JSON writing: " << glze;
		co_return HandleResult::error("Serialization error", glze);
	}


	// The Arena hub (1801 ChallengeArenaSelectScene) sends only this request
	// before its Colosseum banner opens Class Select / Top, which read the
	// Colosseum class rows unchecked: the hub groups ride along here (see
	// gme::colosseum::hubBody).  The two replies share no keys, so the objects
	// are joined as they are.  Nothing here may fail the timer reply.
	try
	{
		::ArenaPlainReq req{};
		glz::context ctx{};
		if (!glz::read<glz::opts{ .error_on_unknown_keys = false }>(req, json, ctx))
		{
			const auto database = theDb();
			const auto identity = (co_await gme::getUserIdentity(database, req.login_info)).nonEmpty();
			const auto hub = co_await gme::colosseum::hubBody(database, identity);
			if (buffer.size() > 2 && hub.size() > 2)
				buffer = buffer.substr(0, buffer.size() - 1) + "," + hub.substr(1);
		}
	}
	catch (const std::exception& ex)
	{
		LOG_WARN << "ChallengeArenaResetInfo: no Colosseum rows: " << ex.what();
	}
	co_return HandleResult::success(buffer);
}
#include "App.hpp"
#include "GmeController.hpp"

#include <gimuserver/gme/common/GameDay.hpp>
#include <gimuserver/gme/common/EventRotation.hpp>
#include <gimuserver/gme/common/SummonRotation.hpp>
#include <gimuserver/forge/ClearLog.hpp>
#include <gimuserver/forge/ForgeStatus.hpp>
#include <gimuserver/forge/Rules.hpp>
#include <gimuserver/forge/StarterPack.hpp>
#include <gimuserver/utils/BfCrypt.hpp>
#include <gimuserver/utils/HuffmanGzip.hpp>

using namespace drogon;

Task<> GmeController::HandleGame(HttpRequestPtr rq, std::function<void(const HttpResponsePtr&)> callback)
{
	auto resp = HttpResponse::newHttpResponse();
	auto req = glz::read_json<GmeAction>(rq->getBody());
	if (!req.has_value())
	{
		LOG_ERROR << "Cannot decode gme request: " << glz::format_error(req.error(), rq->getBody());
		resp->setCloseConnection(true);
		resp->setStatusCode(k400BadRequest);
	}
	else
	{
		auto& gmeReq = req.value();
		if (!gmeReq.body.has_value() || gmeReq.header.id.empty() || gmeReq.error.has_value())
		{
			resp->setCloseConnection(true);
			resp->setStatusCode(k400BadRequest);
		}
		else
		{
			const auto& gmeResp = co_await Handle(rq->session(), gmeReq);
			if (gmeResp.error.has_value())
			{
				auto cmd = gmeResp.error.value().cmd;
				resp->setCloseConnection(cmd == GmeErrorCommand::Close || cmd == GmeErrorCommand::Close2 || cmd == GmeErrorCommand::Close3 ||
					cmd == GmeErrorCommand::LogoutFacebook || cmd == GmeErrorCommand::RaidError);
			}

			std::string buffer{};
			const auto& ec = glz::write_json(gmeResp, buffer);
			if (ec)
			{
				LOG_ERROR << "Cannot create gme response: " << req.value().body.value().body << " error:\n" << glz::format_error(ec, buffer);
				resp->setCloseConnection(true); // kill connection in case of an error
				resp->setStatusCode(k500InternalServerError);
			}
			else
			{
				resp->setStatusCode(k200OK);
				// Gzipped Huffman-only here rather than by drogon (utils/HuffmanGzip.hpp).
				HuffmanGzip::SetBody(rq, resp, std::move(buffer));
			}
		}
	}

	callback(resp);

#if 0
	// NOTE: in a real server this shouldn't happen
	if (m_users.empty())
	{
		UserInfo ui;
		ui.info.userID = HARDCODE_USERID;
		m_users.insert_or_assign(ui.info.userID, ui);
	}
	auto& user = m_users[HARDCODE_USERID];
	
	q.Handle(user, callback, bodyJson);
#endif
}

void GmeController::HandleFeatureCheck(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();

	std::string buffer{};
	const auto& ec = glz::write_json(theServer()->cache().feature(), buffer);

	// Logged like a GME action because whether the CLIENT calls this at all is
	// an open question, and nothing else records it.  The Slots tile enters its
	// video-ad path — which needs `video_ads` and `old_video_ads_slots` to be
	// non-zero — even though this response serves both as unquoted 0, so either
	// the request never arrives or its values are not being applied.  A file in
	// deploy/log/ after a client launch settles which (handbook §7.14).
	//
	// FeatureManager::requestFeatures @0x1C77368 sends it exactly once per
	// launch, from InitializeScene::updateEvent, latching on +0x18 — so expect
	// at most ONE of these per run, early, before the login envelope.
	DumpLog dump{};
	theServer()->tryOpenHttpDumpLog("featureCheck", dump);
	dump << "REQUEST: " << rq->getPath() << "\n";
	dump << "RESPONSE: " << buffer << "\n";

	if (ec)
	{
		LOG_DEBUG << "Cannot serialize featurecheck: " << ec;
		p->setStatusCode(k500InternalServerError);
	}
	else
	{
		p->setStatusCode(k200OK);
		p->setContentTypeCode(CT_APPLICATION_JSON);
		p->setBody(buffer);
	}

	callback(p);
}

void GmeController::HandleServerTime(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_TEXT_HTML);
	p->setBody(std::to_string(trantor::Date::date().secondsSinceEpoch()));
	callback(p);
}

void GmeController::HandleSummonRotation(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_APPLICATION_JSON);
	p->setBody(gme::summonRotationJson(gme::unixNow()));
	callback(p);
}

void GmeController::HandleEventRotation(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_APPLICATION_JSON);
	p->setBody(gme::eventScheduleJson(gme::unixNow()));
	callback(p);
}

void GmeController::HandleSummonUnits(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_APPLICATION_JSON);
	p->setBody(gme::summonUnitsJson(gme::unixNow()));
	callback(p);
}

void GmeController::HandleForgeStatus(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_APPLICATION_JSON);
	// Read on every request: the rules engine updates it while running.
	p->addHeader("Cache-Control", "no-store");
	// The same throttled look at rules.json every game request takes (at most
	// once per 2 s), so an external tool sees a file it just wrote without first
	// needing a player to touch the game.
	forge::rules();
	// Likewise starter.json (a stat, re-read only when it changed).
	(void)forge::starterPack();
	p->setBody(forge::statusJson());
	callback(p);
}

void GmeController::HandleForgeClears(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_APPLICATION_JSON);
	p->addHeader("Cache-Control", "no-store");
	p->setBody(forge::clears::aggregatesJson());
	callback(p);
}

void GmeController::HandleDailyLogin(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	// this is the old daily login system (not used anymore)
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_APPLICATION_JSON);
	// TODO: cache this etc etc
	p->setBody("W3siUFJFU0VOVF9UWVBFIjoiMiIsIlBSRVNFTlRfSUQiOiIwIiwiUFJFU0VOVF9DTlQiOiIyMDAiLCJSRVdBUkQiOmZhbHNlLCJHUk9VUF9UWVBFIjoiMSIsIkRBWVNfTEVGVF9UT19HRU0iOjR9LHsiUFJFU0VOVF9UWVBFIjoiMyIsIlBSRVNFTlRfSUQiOiIwIiwiUFJFU0VOVF9DTlQiOiIxMDAwMCIsIlJFV0FSRCI6ZmFsc2UsIkdST1VQX1RZUEUiOiIxIiwiREFZU19MRUZUX1RPX0dFTSI6NH0seyJQUkVTRU5UX1RZUEUiOiI2IiwiUFJFU0VOVF9JRCI6IjEwMjAzIiwiUFJFU0VOVF9DTlQiOiIxIiwiUkVXQVJEIjpmYWxzZSwiR1JPVVBfVFlQRSI6IjEiLCJEQVlTX0xFRlRfVE9fR0VNIjo0fSx7IlBSRVNFTlRfVFlQRSI6IjYiLCJQUkVTRU5UX0lEIjoiMjAyMDMiLCJQUkVTRU5UX0NOVCI6IjEiLCJSRVdBUkQiOmZhbHNlLCJHUk9VUF9UWVBFIjoiMSIsIkRBWVNfTEVGVF9UT19HRU0iOjR9LHsiUFJFU0VOVF9UWVBFIjoiNiIsIlBSRVNFTlRfSUQiOiIzMDIwMyIsIlBSRVNFTlRfQ05UIjoiMSIsIlJFV0FSRCI6ZmFsc2UsIkdST1VQX1RZUEUiOiIxIiwiREFZU19MRUZUX1RPX0dFTSI6NH0seyJQUkVTRU5UX1RZUEUiOiI2IiwiUFJFU0VOVF9JRCI6IjQwMjAzIiwiUFJFU0VOVF9DTlQiOiIxIiwiUkVXQVJEIjpmYWxzZSwiR1JPVVBfVFlQRSI6IjEiLCJEQVlTX0xFRlRfVE9fR0VNIjo0fSx7IlBSRVNFTlRfVFlQRSI6IjYiLCJQUkVTRU5UX0lEIjoiNTAyMDMiLCJQUkVTRU5UX0NOVCI6IjEiLCJSRVdBUkQiOnRydWUsIkdST1VQX1RZUEUiOiIxIiwiREFZU19MRUZUX1RPX0dFTSI6NH0seyJQUkVTRU5UX1RZUEUiOiI2IiwiUFJFU0VOVF9JRCI6IjYwMTMzIiwiUFJFU0VOVF9DTlQiOiIxIiwiUkVXQVJEIjpmYWxzZSwiR1JPVVBfVFlQRSI6IjEiLCJEQVlTX0xFRlRfVE9fR0VNIjo0fSx7IlBSRVNFTlRfVFlQRSI6IjUiLCJQUkVTRU5UX0lEIjoiMjI0MDAiLCJQUkVTRU5UX0NOVCI6IjEiLCJSRVdBUkQiOmZhbHNlLCJHUk9VUF9UWVBFIjoiMSIsIkRBWVNfTEVGVF9UT19HRU0iOjR9XQ==");
	callback(p);
}

void GmeController::HandleMaintenanceClient(const HttpRequestPtr& rq, std::function<void(const HttpResponsePtr&)>&& callback)
{
	auto p = HttpResponse::newHttpResponse();
	p->setStatusCode(k200OK);
	p->setContentTypeCode(CT_TEXT_HTML);
	callback(p);
}

#pragma once

/**
* Controller for Brave Frontier API
* <p>URL: ios21900.bravefrontier.gumi.sg</p>
*/
class GmeController : public drogon::HttpController<GmeController>
{
public:
	/*!
	* Main handler of game packets.
	* @param[in] rq HTTP request
	* @param[in] callback Callback to send the response
	* @return Asyncronous task to await
	*/
	drogon::Task<> HandleGame(drogon::HttpRequestPtr rq, std::function<void(const drogon::HttpResponsePtr&)> callback);

	/*!
	* Handles the feature check request.
	* @param[in] rq HTTP request
	* @param[in] callback Callback to send the response
	*/
	void HandleFeatureCheck(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* Handles the server time request.
	* @param[in] rq HTTP request
	* @param[in] callback Callback to send the response
	*/
	void HandleServerTime(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* Handles the old daily login bonus.
	* @param[in] rq HTTP request
	* @param[in] callback Callback to send the response
	*/
	void HandleDailyLogin(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* Android maintenance probe. An all-digit body is read as a maintenance
	* start time, so an empty body means "no maintenance".
	*/
	void HandleMaintenanceClient(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* This week's featured summon banners and next week's, as JSON, for the
	* Discord bot's Monday post (gme/common/SummonRotation.hpp).
	*/
	void HandleSummonRotation(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* Every summonable unit with its banners, prices, chances and featured
	* weeks, as JSON for the Discord bot's /unit command.
	*/
	/*!
	* This week's featured event and the Vortex dungeons by weekday, as JSON
	* for the Discord bot's weekly post (gme/common/EventRotation.hpp).
	*/
	void HandleEventRotation(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

		void HandleSummonUnits(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* What the server did with the overlay files and rules: per file
	* its sha256, counts, whether it is live and a plain reason when not.  No
	* paths, contents or secrets, so it is public like summonUnits.php.
	*/
	void HandleForgeStatus(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* The difficulty log, per quest: runs, clears, fails, abandons,
	* clear time and turns, squad strength (forge/ClearLog.hpp).  Aggregates
	* only, nothing that names a player, so it is public like forgeStatus.php.
	*/
	void HandleForgeClears(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	METHOD_LIST_BEGIN
		ADD_METHOD_TO(GmeController::HandleGame, "/bf/gme/action.php", drogon::Post);
		ADD_METHOD_TO(GmeController::HandleFeatureCheck, "/bf/gme/featureCheck.php", drogon::Get);
		ADD_METHOD_TO(GmeController::HandleServerTime, "/bf/gme/action/getServerTime.php", drogon::Get);
		ADD_METHOD_TO(GmeController::HandleDailyLogin, "/bf/gme/action/Daily_login.php", drogon::Post);
		ADD_METHOD_TO(GmeController::HandleMaintenanceClient, "/bf/gme/maintenanceClient.php", drogon::Get);
		ADD_METHOD_TO(GmeController::HandleSummonRotation, "/bf/gme/summonRotation.php", drogon::Get);
		ADD_METHOD_TO(GmeController::HandleSummonUnits, "/bf/gme/summonUnits.php", drogon::Get);
		ADD_METHOD_TO(GmeController::HandleEventRotation, "/bf/gme/eventRotation.php", drogon::Get);
		ADD_METHOD_TO(GmeController::HandleForgeStatus, "/bf/gme/forgeStatus.php", drogon::Get);
		ADD_METHOD_TO(GmeController::HandleForgeClears, "/bf/gme/forgeClears.php", drogon::Get);
	METHOD_LIST_END

private:
	/*!
	* Handles a gme message
	* @param session Session that requested the message
	* @param gme Input message
	* @return A task which will return the output message
	*/
	drogon::Task<GmeAction> Handle(drogon::SessionPtr session, const GmeAction& gme);
};

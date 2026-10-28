#pragma once

/**
* Controller for Gumi Live API registration.
* @note URL: api-sl.gl.gumi.sg
*/
class AccountController : public drogon::HttpController<AccountController>
{
public:
	/*!
	* Handle login request for guest (not facebook) logins.
	* @param[in] rq HTTP request
	* @param[in] callback Callback to send the response
	* @return Asyncronous task to await
	*/
	drogon::Task<> HandleGuest(drogon::HttpRequestPtr rq, std::function<void(const drogon::HttpResponsePtr&)> callback);

	/*!
	* Android LoginScene reports the game user back to gumi Live after Initialize.
	* The client only needs {"status":"successful"}.
	*/
	void HandleGameUserUpdate(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	/*!
	* Android lookup for an existing guest account; "not exist" lets it fall
	* through to the normal guest login.
	*/
	void HandleGuestGet(const drogon::HttpRequestPtr& rq, std::function<void(const drogon::HttpResponsePtr&)>&& callback);

	METHOD_LIST_BEGIN
		ADD_METHOD_TO(AccountController::HandleGuest, "/accounts/guest/login/", drogon::Get);
		ADD_METHOD_TO(AccountController::HandleGameUserUpdate, "/accounts/game_user/update/", drogon::Get);
		ADD_METHOD_TO(AccountController::HandleGuestGet, "/accounts/guest/get/", drogon::Get);
	METHOD_LIST_END
};

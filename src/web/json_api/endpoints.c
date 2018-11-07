
/**
 * @file /magma/web/json_api/endpoints.c
 *
 * @brief The the JSON API interface functions.
 */

#include "magma.h"

static bool_t is_locked(auth_t *auth) {
	return auth->status.locked != AUTH_LOCK_NONE;
}

static chr_t * lock_error_message(auth_t *auth) {

	chr_t *result;

	switch (auth->status.locked) {
		case AUTH_LOCK_INACTIVITY:
			result = "This account has been locked for inactivity.";
			break;
		case AUTH_LOCK_EXPIRED:
			result = "This account has been locked because the subscription expired.";
			break;
		case AUTH_LOCK_ADMIN:
			result = "This account has been administratively locked.";
			break;
		case AUTH_LOCK_ABUSE:
			result = "This account has been locked on suspicion of abuse.";
			break;
		case AUTH_LOCK_USER:
			result = "This account has been locked at the request of the user.";
			break;
		default:
			result = "This account account has been locked.";
			break;
	}

	return result;
}

void api_endpoint_auth(connection_t *con) {

	int_t state;
	uint64_t fails = 0;
	auth_t *auth = NULL;
	json_error_t jansson_err;
	meta_user_t *user = NULL;
	chr_t *username = NULL, *password = NULL;
	stringer_t *choke = NULL, *subnet = NULL, *invalid = NULL;

	if (json_unpack_ex_d(con->http.portal.params, &jansson_err, JSON_STRICT, "{s:s, s:s}", "username", &username, "password", &password) != 0) {
		log_pedantic("Received invalid portal auth request parameters { user = %.*s, errmsg = %s }",
			st_length_int(con->http.session->user->username), st_char_get(con->http.session->user->username),
			jansson_err.text);

		api_error(con, HTTP_ERROR_400, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "Invalid method parameters.");
		return;
	}

	// Store the subnet for tracking login failures. Make the buffer big enough to hold an IPv6 subnet string.
	subnet = con_addr_subnet(con, MANAGEDBUF(256));

	// Generate the authentication choke so only one login attempt is processed per subnet at any given time.
	choke = st_quick(MANAGEDBUF(384), "magma.logins.choke.%.*s", st_length_int(subnet), st_char_get(subnet));

	// Generate the invalid login tracker.
	invalid = st_quick(MANAGEDBUF(384), "magma.logins.invalid.%lu.%.*s", time_datestamp(), st_length_int(subnet), st_char_get(subnet));

	// For now we hard code the maximum number of failed logins.
	if (st_populated(invalid) && cache_increment(invalid, 0, 0, 86400) >= 16) {
		api_error(con, HTTP_ERROR_400, PORTAL_ENDPOINT_ERROR_AUTH, "The maximum number of failed login attempts has been reached. Please try again later.");
		con->protocol.violations++;
		return;
	}

	// Obtain the authentication choke.
	if (lock_get(choke) != 1) {
		api_error(con, HTTP_ERROR_400, PORTAL_ENDPOINT_ERROR_AUTH, "The maximum number of concurrent login attempts has been reached. Please try again later.");
		con->protocol.violations++;
		return;
	}

	// Process the username, and password, and turn them into authentication tokens.
	state = auth_login(NULLER(username), NULLER(password), &auth);

	// Release the authentication lock so another connection may proceed.
	lock_release(choke);

	if (state) {
		if (state < 0) {
			api_error(con, HTTP_ERROR_500, JSON_RPC_2_ERROR_SERVER_INTERNAL, "Internal server error.");
		}
		else {
			api_error(con, HTTP_ERROR_400, PORTAL_ENDPOINT_ERROR_AUTH, "Unable to authenticate with given username and password.");
		}

		log_info("Failed login attempt. { ip = %s / username = %s / protocol = HTTP }", st_char_get(con_addr_presentation(con, MANAGEDBUF(256))),
			username);

		// If we have a valid key, we increment the failed login counter.
		if (st_populated(invalid) && (fails = cache_increment(invalid, 1, 1, 86400)) >= 16) {
			log_info("Subnet banned. { subnet = %s / fails = %lu }", st_char_get(con_addr_subnet(con, MANAGEDBUF(256))), fails);
		}

		con->protocol.violations++;
		return;
	}

	if (is_locked(auth)) {
		api_error(con, HTTP_ERROR_400, PORTAL_ENDPOINT_ERROR_AUTH, lock_error_message(auth));
		auth_free(auth);
		return;
	}

	if ((state = meta_get(auth->usernum, auth->username, auth->seasoning.salt, auth->keys.master, auth->tokens.verification, META_PROTOCOL_JSON, META_GET_NONE, &user))) {
		if (state < 0) {
			api_error(con, HTTP_ERROR_500, JSON_RPC_2_ERROR_SERVER_INTERNAL, "Internal server error.");
		}
		else {
			api_error(con, HTTP_ERROR_400, PORTAL_ENDPOINT_ERROR_AUTH, "Unable to authenticate with given username and password.");
		}

		auth_free(auth);
		return;
	}

	con->http.session->state = SESSION_STATE_AUTHENTICATED;
	con->http.response.cookie = HTTP_COOKIE_SET;
	con->http.session->user = user;

	// There were two successful responses in the original function. I think this one is an extra.
	//portal_endpoint_response(con, "{s:s, s:{s:s, s:s}, s:I}", "jsonrpc", "2.0", "result", "auth", "success", "session",
	//	st_char_get(con->http.session->warden.token), "id",	con->http.portal.id);

	api_response(con, HTTP_OK, "{s:s, s:I}", "jsonrpc", "2.0", "id", con->http.portal.id);

	return;
}

void api_endpoint_register(connection_t *con) {

	placer_t value;
	int_t result = 0;
	uint16_t plan = 1;
	uint64_t usernum = 0;
	int64_t transaction = -1;
	json_error_t jansson_err;
	stringer_t *sanitized = NULL;
	chr_t *holder = NULL, *username = NULL, *password = NULL, *password_verification = NULL;

	// Try parsing the parameters with and without the plan key.
	if (json_unpack_ex_d(con->http.portal.params, &jansson_err, JSON_STRICT, "{s:s, s:s, s:s}", "username", &username,
		"password", &password, "password_verification",	&password_verification) != 0 &&
		json_unpack_ex_d(con->http.portal.params, &jansson_err, JSON_STRICT, "{s:s, s:s, s:s, s:s}", "username", &username,
		"password", &password, "password_verification",	&password_verification, "plan", &holder) != 0) {
		log_pedantic("Received invalid portal register request parameters. { errmsg = %s }", jansson_err.text);
		api_error(con, HTTP_ERROR_400, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "Invalid method parameters.");
		goto out;
	}

	// If a plan was requested, figure out the correct id.
	else if (holder) {

		value = pl_init(holder, ns_length_get(holder));

		if (!st_cmp_ci_eq(&value, PLACER("BASIC", 5))) plan = 1;
		else if (!st_cmp_ci_eq(&value, PLACER("PERSONAL", 8))) plan = 2;
		else if (!st_cmp_ci_eq(&value, PLACER("ENHANCED", 8))) plan = 3;
		else if (!st_cmp_ci_eq(&value, PLACER("PREMIUM", 7))) plan = 4;
		else if (!st_cmp_ci_eq(&value, PLACER("STANDARD", 8))) plan = 5;
		else if (!st_cmp_ci_eq(&value, PLACER("PREMIER", 7))) plan = 6;
		else {
			api_error(con, HTTP_ERROR_400, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "Invalid plan requested.");
			goto out;
		}

	}

	// Sanitize the username, and prepare it for the database insert.
	if (!(sanitized = auth_sanitize_username(NULLER(username)))) {
		api_error(con, HTTP_ERROR_400, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "Invalid username provided.");
		goto out;
	}

	// Start the transaction.
	transaction = tran_start();
	if (transaction == -1) {
		api_error(con, HTTP_ERROR_500, JSON_RPC_2_ERROR_SERVER_INTERNAL, "Internal server error.");
		goto out;
	}

	// Database insert.
	if ((result = register_data_insert_user(con, plan, sanitized, NULLER(password), transaction, &usernum)) != 0) {
		tran_rollback(transaction);
		if (result < 0) {
			api_error(con, HTTP_ERROR_500, JSON_RPC_2_ERROR_SERVER_INTERNAL, "Internal server error.");
		}
		else {
			api_error(con, HTTP_ERROR_500, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "Invalid registration details provided.");
		}
		goto out;
	}

	// Were finally done.
	tran_commit(transaction);

	// And finally, increment the abuse counter.
	register_abuse_increment_history(con);

	api_response(con, HTTP_OK, "{s:s, s:{s:s}, s:I}", "jsonrpc", "2.0", "result", "register", "success", "id", con->http.portal.id);

	out:

	st_cleanup(sanitized);

	return;
}

void api_endpoint_delete_user(connection_t *con) {
	json_error_t jansson_err;
	chr_t *username;
	int64_t num_deleted;
	MYSQL_BIND parameters[1];

	mm_wipe(parameters, sizeof(parameters));

	if (json_unpack_ex_d(con->http.portal.params, &jansson_err, JSON_STRICT, "{s:s}", "username", &username) != 0) {
		log_pedantic(
			"Received invalid portal auth request parameters "
			"{ user = %s, errmsg = %s }",
			st_char_get(con->http.session->user->username),
			jansson_err.text);

		api_error(con, HTTP_ERROR_400, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "Invalid method parameters.");
		// A
		goto out;
	}

	// Key
	parameters[0].buffer_type = MYSQL_TYPE_STRING;
	parameters[0].buffer_length = ns_length_get(username);
	parameters[0].buffer = username;

	num_deleted = stmt_exec_affected(stmts.delete_user, parameters);
	if (0 == num_deleted) {
		api_error(con, HTTP_ERROR_422, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "No such user.");
		goto out;
	}
	if (-1 == num_deleted) {
		api_error(con, HTTP_ERROR_500, JSON_RPC_2_ERROR_SERVER_INTERNAL, "Internal server error.");
	}

	api_response(con, HTTP_OK, "{s:s, s:{s:s}, s:I}", "jsonrpc", "2.0", "result", "delete_user", "success", "id", con->http.portal.id);

	out: return;
}

void api_endpoint_change_password(connection_t *con) {
	json_error_t jansson_err;
	chr_t *password;
	chr_t *new_password;
	chr_t *new_password_verification;

	if (json_unpack_ex_d(con->http.portal.params, &jansson_err, JSON_STRICT, "{s:s, s:s, s:s}", "password", &password, "new_password", &new_password, "new_password_verification",
		&new_password_verification) != 0) {
		log_pedantic(
			"Received invalid portal auth request parameters "
			"{ user = %s, errmsg = %s }",
			st_char_get(con->http.session->user->username),
			jansson_err.text);

		api_error(con, HTTP_ERROR_400, JSON_RPC_2_ERROR_SERVER_METHOD_PARAMS, "Invalid method parameters.");

		goto out;
	}

	/// TODO: - wire up here

	out: return;
}

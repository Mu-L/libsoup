/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */

#include "test-utils.h"
#include "soup-uri-utils-private.h"

static const char *base_uri;
static GMainLoop *loop;

typedef struct {
	/* Explanation of what you should see */
	const char *explanation;

	/* URL to test against */
	const char *url;

	/* Provided passwords, 1 character each. ('1', '2', and '3'
	 * mean the correct passwords for "realm1", "realm2", and
	 * "realm3" respectively. '4' means "use the wrong password".)
	 * The first password (if present) will be used by
	 * authenticate(), and the second (if present) will be used by
	 * reauthenticate().
	 */
	const char *provided;

	/* Whether to pass user and password in the URL or not.
	 */
	gboolean url_auth;

	/* Expected passwords, 1 character each. (As with the provided
	 * passwords, with the addition that '0' means "no
	 * Authorization header expected".) Used to verify that soup
	 * used the password it was supposed to at each step.
	 */
	const char *expected;

	/* What the final status code should be. */
	guint final_status;
} SoupAuthTest;

static SoupAuthTest main_tests[] = {
	{ "No auth available, should fail",
	  "Basic/realm1/", "", FALSE, "0", SOUP_STATUS_UNAUTHORIZED },

	{ "Should fail with no auth, fail again with bad password, and give up",
	  "Basic/realm2/", "4", FALSE, "04", SOUP_STATUS_UNAUTHORIZED },

	{ "Auth provided this time, so should succeed",
	  "Basic/realm1/", "1", FALSE, "01", SOUP_STATUS_OK },

	{ "Now should automatically reuse previous auth",
	  "Basic/realm1/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Subdir should also automatically reuse auth",
	  "Basic/realm1/subdir/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Subdir should retry last auth, but will fail this time",
	  "Basic/realm1/realm2/", "", FALSE, "1", SOUP_STATUS_UNAUTHORIZED },

	{ "Now should use provided auth",
	  "Basic/realm1/realm2/", "2", FALSE, "02", SOUP_STATUS_OK },

	{ "Reusing last auth. Should succeed on first try",
	  "Basic/realm1/realm2/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Reuse will fail, but 2nd try will succeed because it's a known realm",
	  "Basic/realm1/realm2/realm1/", "", FALSE, "21", SOUP_STATUS_OK },

	{ "Should succeed on first try. (Known realm with cached password)",
	  "Basic/realm2/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Fail once, then use typoed password, then use right password",
	  "Basic/realm3/", "43", FALSE, "043", SOUP_STATUS_OK },


	{ "No auth available, should fail",
	  "Digest/realm1/", "", FALSE, "0", SOUP_STATUS_UNAUTHORIZED },

	{ "Should fail with no auth, fail again with bad password, and give up",
	  "Digest/realm2/", "4", FALSE, "04", SOUP_STATUS_UNAUTHORIZED },

	{ "Known realm, auth provided, so should succeed",
	  "Digest/realm1/", "1", FALSE, "01", SOUP_STATUS_OK },

	{ "Now should automatically reuse previous auth",
	  "Digest/realm1/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Subdir should also automatically reuse auth",
	  "Digest/realm1/subdir/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Password provided, should succeed",
	  "Digest/realm2/", "2", FALSE, "02", SOUP_STATUS_OK },

	{ "Should already know correct domain and use provided auth on first try",
	  "Digest/realm1/realm2/", "2", FALSE, "2", SOUP_STATUS_OK },

	{ "Reusing last auth. Should succeed on first try",
	  "Digest/realm1/realm2/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Should succeed on first try because of earlier domain directive",
	  "Digest/realm1/realm2/realm1/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Fail once, then use typoed password, then use right password",
	  "Digest/realm3/", "43", FALSE, "043", SOUP_STATUS_OK },


	{ "Make sure we haven't forgotten anything",
	  "Basic/realm1/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Basic/realm1/realm2/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Basic/realm1/realm2/realm1/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Basic/realm2/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Basic/realm3/", "", FALSE, "3", SOUP_STATUS_OK },


	{ "Make sure we haven't forgotten anything",
	  "Digest/realm1/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Digest/realm1/realm2/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Digest/realm1/realm2/realm1/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Digest/realm2/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Make sure we haven't forgotten anything",
	  "Digest/realm3/", "", FALSE, "3", SOUP_STATUS_OK },

	{ "Now the server will reject the formerly-good password",
	  "Basic/realm1/not/", "1", FALSE, /* should not be used */ "1", SOUP_STATUS_UNAUTHORIZED },

	{ "Make sure we've forgotten it",
	  "Basic/realm1/", "", FALSE, "0", SOUP_STATUS_UNAUTHORIZED },

	{ "Likewise, reject the formerly-good Digest password",
	  "Digest/realm1/not/", "1", FALSE, /* should not be used */ "1", SOUP_STATUS_UNAUTHORIZED },

	{ "Make sure we've forgotten it",
	  "Digest/realm1/", "", FALSE, "0", SOUP_STATUS_UNAUTHORIZED },

	{ "Fail with URI-embedded password, then use right password in the authenticate signal",
	  "Basic/realm3/", "43", TRUE, "43", SOUP_STATUS_OK },

	{ NULL }
};

static const char *auths[] = {
	"no password", "password 1",
	"password 2", "password 3",
	"intentionally wrong password",
};

static int
identify_auth (SoupMessage *msg)
{
	const char *header;
	int num;

	header = soup_message_headers_get_one (soup_message_get_request_headers (msg),
					       "Authorization");
	if (!header)
		return 0;

	if (!g_ascii_strncasecmp (header, "Basic ", 6)) {
		char *token;
		gsize len;

		token = (char *)g_base64_decode (header + 6, &len);
		num = token[len - 1] - '0';
		g_free (token);
	} else {
		const char *user;

		user = strstr (header, "username=\"user");
		if (user)
			num = user[14] - '0';
		else
			num = 0;
	}

	g_assert_cmpint (num, >=, 0);
	g_assert_cmpint (num, <=, 4);

	return num;
}

static void
handler (SoupMessage *msg, gpointer data)
{
	char *expected = data;
	int auth, exp;

	auth = identify_auth (msg);

	debug_printf (1, "  %d %s (using %s)\n",
		      soup_message_get_status (msg), soup_message_get_reason_phrase (msg),
		      auths[auth]);

	if (*expected) {
		exp = *expected - '0';
		soup_test_assert (auth == exp,
				  "expected %s", auths[exp]);
		memmove (expected, expected + 1, strlen (expected));
	} else {
		soup_test_assert (*expected,
				  "expected to be finished");
	}
}

static gboolean
authenticate (SoupMessage  *msg,
	      SoupAuth     *auth,
	      gboolean      retrying,
	      SoupAuthTest *test)
{
	char *username, *password;
	char num;

	if (!test->provided[0])
		return FALSE;
	if (retrying) {
		if (!test->provided[1])
			return FALSE;
		num = test->provided[1];
	} else
		num = test->provided[0];

	username = g_strdup_printf ("user%c", num);
	password = g_strdup_printf ("realm%c", num);
	soup_auth_authenticate (auth, username, password);
	g_free (username);
	g_free (password);

	return TRUE;
}

static void
bug271540_sent (SoupMessage *msg, gpointer data)
{
	int n = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (msg), "#"));
	gboolean *authenticated = data;
	int auth = identify_auth (msg);

	soup_test_assert (*authenticated || !auth,
			  "using auth on message %d before authenticating", n);
	soup_test_assert (!*authenticated || auth,
			  "sent unauthenticated message %d after authenticating", n);
}

static gboolean
bug271540_authenticate (SoupMessage *msg,
			SoupAuth    *auth,
			gboolean     retrying,
			gboolean    *authenticated)
{
	int n = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (msg), "#"));

	if (strcmp (soup_auth_get_scheme_name (auth), "Basic") != 0 ||
	    strcmp (soup_auth_get_realm (auth), "realm1") != 0)
		return FALSE;

	if (!*authenticated) {
		debug_printf (1, "    authenticating message %d\n", n);
		soup_auth_authenticate (auth, "user1", "realm1");
		*authenticated = TRUE;
	} else {
		soup_test_assert (!*authenticated,
				  "asked to authenticate message %d after authenticating", n);
	}

	return TRUE;
}

static void
bug271540_finished (SoupMessage *msg, gpointer data)
{
	int *left = data;

	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	(*left)--;
	if (!*left)
		g_main_loop_quit (loop);
}

static void
do_pipelined_auth_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	gboolean authenticated;
	char *uri;
	int i;

	g_test_bug ("271540");

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);

	authenticated = FALSE;
	uri = g_strconcat (base_uri, "Basic/realm1/", NULL);
	for (i = 0; i < 10; i++) {
		msg = soup_message_new (SOUP_METHOD_GET, uri);
		g_object_set_data (G_OBJECT (msg), "#", GINT_TO_POINTER (i + 1));
		g_signal_connect (msg, "authenticate",
				  G_CALLBACK (bug271540_authenticate), &authenticated);
		g_signal_connect (msg, "wrote_headers",
				  G_CALLBACK (bug271540_sent), &authenticated);

		g_signal_connect (msg, "finished",
				  G_CALLBACK (bug271540_finished), &i);
		soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
		g_object_unref (msg);
	}
	g_free (uri);

	loop = g_main_loop_new (NULL, TRUE);
	g_main_loop_run (loop);
	g_main_loop_unref (loop);

	soup_test_session_abort_unref (session);
}

/* We test two different things here:
 *
 *   1. If we get a 401 response with "WWW-Authenticate: Digest
 *      stale=true...", we should retry and succeed *without* the
 *      session asking for a password again.
 *
 *   2. If we get a successful response with "Authentication-Info:
 *      nextnonce=...", we should update the nonce automatically so as
 *      to avoid getting a stale nonce error on the next request.
 *
 * In our Apache config, /Digest/realm1 and /Digest/realm1/expire are
 * set up to use the same auth info, but only the latter has an
 * AuthDigestNonceLifetime (of 2 seconds). The way nonces work in
 * Apache, a nonce received from /Digest/realm1 will still expire in
 * /Digest/realm1/expire, but it won't issue a nextnonce for a request
 * in /Digest/realm1. This lets us test both behaviors.
 *
 * The expected conversation is:
 *
 * First message
 *   GET /Digest/realm1
 *
 *   401 Unauthorized
 *   WWW-Authenticate: Digest nonce=A
 *
 *   [emit 'authenticate']
 *
 *   GET /Digest/realm1
 *   Authorization: Digest nonce=A
 *
 *   200 OK
 *   [No Authentication-Info]
 *
 * [sleep 2 seconds: nonce A is no longer valid, but we have no
 * way of knowing that]
 *
 * Second message
 *   GET /Digest/realm1/expire/
 *   Authorization: Digest nonce=A
 *
 *   401 Unauthorized
 *   WWW-Authenticate: Digest stale=true nonce=B
 *
 *   GET /Digest/realm1/expire/
 *   Authorization: Digest nonce=B
 *
 *   200 OK
 *   Authentication-Info: nextnonce=C
 *
 * [sleep 1 second]
 *
 * Third message
 *   GET /Digest/realm1/expire/
 *   Authorization: Digest nonce=C
 *   [nonce=B would work here too]
 *
 *   200 OK
 *   Authentication-Info: nextnonce=D
 *
 * [sleep 1 second; nonces B and C are no longer valid, but D is]
 *
 * Fourth message
 *   GET /Digest/realm1/expire/
 *   Authorization: Digest nonce=D
 *
 *   200 OK
 *   Authentication-Info: nextnonce=D
 *
 */

static gboolean
digest_nonce_authenticate (SoupMessage *msg,
			   SoupAuth    *auth,
			   gboolean     retrying)
{
	if (retrying)
		return FALSE;

	if (strcmp (soup_auth_get_scheme_name (auth), "Digest") != 0 ||
	    strcmp (soup_auth_get_realm (auth), "realm1") != 0)
		return FALSE;

	soup_auth_authenticate (auth, "user1", "realm1");

	return TRUE;
}

static void
digest_nonce_unauthorized (SoupMessage *msg, gpointer data)
{
	gboolean *got_401 = data;
	*got_401 = TRUE;
}

static void
do_digest_nonce_test (SoupSession *session,
		      const char *nth, const char *uri, gboolean use_auth_cache,
		      gboolean expect_401, gboolean expect_signal)
{
	SoupMessage *msg;
	gboolean got_401;

	msg = soup_message_new (SOUP_METHOD_GET, uri);
	if (!use_auth_cache)
		soup_message_add_flags (msg, SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE);

	if (expect_signal) {
		g_signal_connect (msg, "authenticate",
				  G_CALLBACK (digest_nonce_authenticate),
				  NULL);
	}
	soup_message_add_status_code_handler (msg, "got_headers",
					      SOUP_STATUS_UNAUTHORIZED,
					      G_CALLBACK (digest_nonce_unauthorized),
					      &got_401);
	got_401 = FALSE;
	soup_test_session_send_message (session, msg);
	soup_test_assert (got_401 == expect_401,
			  "%s request %s a 401 Unauthorized!\n", nth,
			  got_401 ? "got" : "did not get");
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	if (expect_signal) {
		g_signal_handlers_disconnect_by_func (session,
						      G_CALLBACK (digest_nonce_authenticate),
						      NULL);
	}

	g_object_unref (msg);
}

static void
do_digest_expiration_test (void)
{
	SoupSession *session;
	char *uri;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);

	uri = g_strconcat (base_uri, "Digest/realm1/", NULL);
	do_digest_nonce_test (session, "First", uri, TRUE, TRUE, TRUE);
	g_free (uri);
	g_usleep (2 * G_USEC_PER_SEC);
	uri = g_strconcat (base_uri, "Digest/realm1/expire/", NULL);
	do_digest_nonce_test (session, "Second", uri, TRUE, TRUE, FALSE);
	g_usleep (1 * G_USEC_PER_SEC);
	do_digest_nonce_test (session, "Third", uri, TRUE, FALSE, FALSE);
	g_usleep (1 * G_USEC_PER_SEC);
	do_digest_nonce_test (session, "Fourth", uri, TRUE, FALSE, FALSE);
	g_free (uri);

	soup_test_session_abort_unref (session);
}

/* Async auth test. We queue three requests to /Basic/realm1, ensuring
 * that they are sent in order. The first and third ones will be
 * paused from the authentication callback. The second will be allowed
 * to fail. Shortly after the third one requests auth, we'll provide
 * the auth and unpause the two remaining messages, allowing them to
 * succeed.
 */

static gboolean
async_authenticate (SoupMessage *msg,
		    SoupAuth    *auth,
		    gboolean     retrying,
		    SoupAuth   **saved_auth)
{
	int id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (msg), "id"));

	debug_printf (2, "  async_authenticate msg%d\n", id);

	/* The session will try to authenticate msg3 *before* sending
	 * it, because it already knows it's going to need the auth.
	 * Ignore that.
	 */
	if (soup_message_get_status (msg) != SOUP_STATUS_UNAUTHORIZED) {
		debug_printf (2, "    (ignoring)\n");
		return FALSE;
	}

	if (saved_auth)
		*saved_auth = g_object_ref (auth);
	g_main_loop_quit (loop);

	return TRUE;
}

static void
async_finished (SoupMessage *msg,
		gpointer     user_data)
{
	int *remaining = user_data;
	int id = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (msg), "id"));

	debug_printf (2, "  async_finished msg%d\n", id);

	(*remaining)--;
	if (!*remaining)
		g_main_loop_quit (loop);
}

static gboolean
async_authenticate_assert_once (SoupMessage *msg,
                                SoupAuth    *auth,
				gboolean     retrying,
				gboolean    *been_here)
{
	debug_printf (2, "  async_authenticate_assert_once\n");

	soup_test_assert (!*been_here,
			  "async_authenticate_assert_once called twice");
	*been_here = TRUE;

	return FALSE;
}

static gboolean
async_authenticate_assert_once_and_stop (SoupMessage *msg,
					 SoupAuth    *auth,
					 gboolean     retrying,
					 SoupAuth   **saved_auth)
{
	debug_printf (2, "  async_authenticate_assert_once_and_stop\n");

	soup_test_assert (!*saved_auth,
			  "async_authenticate_assert_once called twice");
	*saved_auth = g_object_ref (auth);

	g_main_loop_quit (loop);

	return TRUE;
}

static void
do_async_auth_good_password_test (void)
{
	SoupSession *session;
	SoupMessage *msg1, *msg2, *msg3;
	char *uri;
	SoupAuth *auth = NULL;
	int remaining;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	loop = g_main_loop_new (NULL, TRUE);
	session = soup_test_session_new (NULL);
	uri = g_strconcat (base_uri, "Basic/realm1/", NULL);
	remaining = 0;

	msg1 = soup_message_new ("GET", uri);
	g_object_set_data (G_OBJECT (msg1), "id", GINT_TO_POINTER (1));
	g_signal_connect (msg1, "authenticate",
				    G_CALLBACK (async_authenticate), &auth);
	remaining++;
	g_signal_connect (msg1, "finished",
			  G_CALLBACK (async_finished), &remaining);
	soup_session_send_async (session, msg1, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	g_main_loop_run (loop);

	/* async_authenticate will pause msg1 and quit loop */

	msg2 = soup_message_new ("GET", uri);
	g_object_set_data (G_OBJECT (msg2), "id", GINT_TO_POINTER (2));
	soup_test_session_send_message (session, msg2);

	soup_test_assert_message_status (msg2, SOUP_STATUS_UNAUTHORIZED);

	/* msg2 should be done at this point; assuming everything is
	 * working correctly, the session won't look at it again.
	 */

	msg3 = soup_message_new ("GET", uri);
	g_object_set_data (G_OBJECT (msg3), "id", GINT_TO_POINTER (3));
	g_signal_connect (msg3, "authenticate",
			  G_CALLBACK (async_authenticate), NULL);
	remaining++;
	g_signal_connect (msg3, "finished",
			  G_CALLBACK (async_finished), &remaining);
	soup_session_send_async (session, msg3, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	g_main_loop_run (loop);

	/* async_authenticate will pause msg3 and quit loop */

	/* Now do the auth, and restart */
	if (auth) {
		soup_auth_authenticate (auth, "user1", "realm1");
		g_object_unref (auth);

		g_main_loop_run (loop);

		/* async_finished will quit the loop */
	} else
		soup_test_assert (auth, "msg1 didn't get authenticate signal");

	soup_test_assert_message_status (msg1, SOUP_STATUS_OK);
	soup_test_assert_message_status (msg3, SOUP_STATUS_OK);

	soup_test_session_abort_unref (session);

	g_object_unref (msg1);
	g_object_unref (msg3);
	g_object_unref (msg2);

	g_free (uri);
	g_main_loop_unref (loop);
}

static void
do_async_auth_bad_password_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	guint auth_id;
	char *uri;
	SoupAuth *auth = NULL;
	int remaining;
	gboolean been_there;

	/* Test that giving the wrong password doesn't cause multiple
	 * authenticate signals the second time.
	 */
	g_test_bug ("522601");

	SOUP_TEST_SKIP_IF_NO_APACHE;

	loop = g_main_loop_new (NULL, TRUE);
	session = soup_test_session_new (NULL);
	uri = g_strconcat (base_uri, "Basic/realm1/", NULL);
	remaining = 0;

	msg = soup_message_new ("GET", uri);
	g_object_set_data (G_OBJECT (msg), "id", GINT_TO_POINTER (1));
	auth_id = g_signal_connect (msg, "authenticate",
				    G_CALLBACK (async_authenticate), &auth);
	remaining++;
	g_signal_connect (msg, "finished",
			  G_CALLBACK (async_finished), &remaining);
	soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	g_main_loop_run (loop);
	g_signal_handler_disconnect (msg, auth_id);
	soup_auth_authenticate (auth, "user1", "wrong");
	g_object_unref (auth);

	been_there = FALSE;
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (async_authenticate_assert_once),
			  &been_there);
	g_main_loop_run (loop);

	soup_test_assert (been_there,
			  "authenticate not emitted");

	soup_test_session_abort_unref (session);
	g_object_unref (msg);

	g_free (uri);
	g_main_loop_unref (loop);
}

static void
do_async_auth_no_password_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	char *uri;
	int remaining;
	SoupAuth *auth = NULL;

	/* Test that giving no password doesn't cause multiple
	 * authenticate signals the second time.
	 */
	g_test_bug ("583462");

	SOUP_TEST_SKIP_IF_NO_APACHE;

	loop = g_main_loop_new (NULL, TRUE);
	session = soup_test_session_new (NULL);
	uri = g_strconcat (base_uri, "Basic/realm1/", NULL);
	remaining = 0;

	/* Send a message that doesn't actually authenticate
	 */
	msg = soup_message_new ("GET", uri);
	g_object_set_data (G_OBJECT (msg), "id", GINT_TO_POINTER (1));
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (async_authenticate), &auth);
	remaining++;
	g_signal_connect (msg, "finished",
			  G_CALLBACK (async_finished), &remaining);
	soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	g_main_loop_run (loop);
	soup_auth_cancel (auth);
	g_clear_object (&auth);
	g_main_loop_run (loop);
	g_object_unref(msg);

	/* Now send a second message */
	msg = soup_message_new ("GET", uri);
	g_object_set_data (G_OBJECT (msg), "id", GINT_TO_POINTER (2));
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (async_authenticate_assert_once_and_stop),
			  &auth);
	remaining++;
	g_signal_connect (msg, "finished",
			  G_CALLBACK (async_finished), &remaining);
	soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	g_main_loop_run (loop);
	soup_auth_cancel (auth);
	g_main_loop_run (loop);
	g_object_unref (auth);

	soup_test_session_abort_unref (session);
	g_object_unref (msg);

	g_free (uri);
	g_main_loop_unref (loop);
}

typedef struct {
	SoupAuth *auth;
	GCancellable *cancellable;
} AsyncAuthCancelData;

static gboolean
async_authenticate_cancel_idle (AsyncAuthCancelData *data)
{
	g_cancellable_cancel (data->cancellable);
	return FALSE;
}

static gboolean
async_authenticate_cancel_authenticate (SoupMessage         *msg,
					SoupAuth            *auth,
					gboolean             retrying,
					AsyncAuthCancelData *data)
{
	data->auth = g_object_ref (auth);
	g_idle_add ((GSourceFunc)async_authenticate_cancel_idle, data);

	return TRUE;
}

static void
do_async_auth_cancel_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	char *uri;
	AsyncAuthCancelData data = { NULL, NULL };
	GError *error = NULL;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);
	uri = g_strconcat (base_uri, "Basic/realm1/", NULL);

	msg = soup_message_new ("GET", uri);
	data.cancellable = g_cancellable_new ();
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (async_authenticate_cancel_authenticate),
			  &data);
	g_assert_null (soup_test_session_async_send (session, msg, data.cancellable, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

	g_object_unref (data.auth);
	g_object_unref (data.cancellable);
	g_object_unref (msg);
	g_free (uri);

	soup_test_session_abort_unref (session);
}

static gboolean
sync_bad_password_authenticate (SoupMessage *msg,
                                SoupAuth    *auth,
                                gboolean     retrying)
{
        if (retrying)
                return FALSE;

        soup_auth_authenticate (auth, "user1", "wrong");
        return TRUE;
}

static void
do_sync_auth_bad_password_test (void)
{
        SoupSession *session;
        SoupMessage *msg;
        char *uri;

        SOUP_TEST_SKIP_IF_NO_APACHE;

        session = soup_test_session_new (NULL);
        uri = g_strconcat (base_uri, "Basic/realm1/", NULL);

        msg = soup_message_new ("GET", uri);
        g_free (uri);
        g_signal_connect (msg, "authenticate",
                          G_CALLBACK (sync_bad_password_authenticate),
                          NULL);
        soup_test_session_send_message (session, msg);
        soup_test_assert_message_status (msg, SOUP_STATUS_UNAUTHORIZED);
        g_object_unref (msg);

        soup_test_session_abort_unref (session);
}

typedef struct {
	const char *password;
	struct {
		const char *headers;
		const char *response;
	} round[2];
} SelectAuthData;

static gboolean
select_auth_authenticate (SoupMessage    *msg,
			  SoupAuth       *auth,
			  gboolean        retrying,
			  SelectAuthData *sad)
{
	const char *header, *basic, *digest;
	int round = retrying ? 1 : 0;

	header = soup_message_headers_get_list (soup_message_get_response_headers (msg),
						"WWW-Authenticate");
	basic = strstr (header, "Basic");
	digest = strstr (header, "Digest");
	if (basic && digest) {
		if (basic < digest)
			sad->round[round].headers = "Basic, Digest";
		else
			sad->round[round].headers = "Digest, Basic";
	} else if (basic)
		sad->round[round].headers = "Basic";
	else if (digest)
		sad->round[round].headers = "Digest";

	sad->round[round].response = soup_auth_get_scheme_name (auth);
	if (sad->password && !retrying) {
		soup_auth_authenticate (auth, "user", sad->password);

		return TRUE;
	}

	return FALSE;
}

static void
select_auth_test_one (GUri *uri,
		      gboolean disable_digest, const char *password,
		      const char *first_headers, const char *first_response,
		      const char *second_headers, const char *second_response,
		      guint final_status)
{
	SelectAuthData sad;
	SoupMessage *msg;
	SoupSession *session;

	session = soup_test_session_new (NULL);
	if (disable_digest)
		soup_session_remove_feature_by_type (session, SOUP_TYPE_AUTH_DIGEST);

	msg = soup_message_new_from_uri ("GET", uri);
	g_signal_connect (msg, "authenticate",
                          G_CALLBACK (select_auth_authenticate), &sad);
        memset (&sad, 0, sizeof (sad));
        sad.password = password;
	soup_test_session_send_message (session, msg);

	soup_test_assert (g_strcmp0 (sad.round[0].headers, first_headers) == 0,
			  "Header order wrong: expected %s, got %s",
			  first_headers, sad.round[0].headers);
	soup_test_assert (g_strcmp0 (sad.round[0].response, first_response) == 0,
			  "Selected auth type wrong: expected %s, got %s",
			  first_response, sad.round[0].response);

	soup_test_assert (sad.round[1].headers || !second_headers,
			  "Expected a second round");
	soup_test_assert (!sad.round[1].headers || second_headers,
			  "Didn't expect a second round");
	if (second_headers && second_response) {
		soup_test_assert (g_strcmp0 (sad.round[1].headers, second_headers) == 0,
				  "Second round header order wrong: expected %s, got %s\n",
				  second_headers, sad.round[1].headers);
		soup_test_assert (g_strcmp0 (sad.round[1].response, second_response) == 0,
				  "Second round selected auth type wrong: expected %s, got %s\n",
				  second_response, sad.round[1].response);
	}

	soup_test_assert_message_status (msg, final_status);

	g_object_unref (msg);
	soup_test_session_abort_unref (session);
}

static void
server_callback (SoupServer        *server,
		 SoupServerMessage *msg,
		 const char        *path,
		 GHashTable        *query,
		 gpointer           data)
{
	soup_server_message_set_response (msg, "text/plain",
					  SOUP_MEMORY_STATIC,
					  "OK\r\n", 4);
	soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
}

static gboolean
server_basic_auth_callback (SoupAuthDomain    *auth_domain,
			    SoupServerMessage *msg,
			    const char        *username,
			    const char        *password,
			    gpointer           data)
{
	if (strcmp (username, "user") != 0)
		return FALSE;
	return strcmp (password, "good-basic") == 0;
}

static char *
server_digest_auth_callback (SoupAuthDomain    *auth_domain,
			     SoupServerMessage *msg,
			     const char        *username,
			     gpointer           data)
{
	if (strcmp (username, "user") != 0)
		return NULL;
	return soup_auth_domain_digest_encode_password ("user",
							"auth-test",
							"good");
}

static void
do_select_auth_test (void)
{
	SoupServer *server;
	SoupAuthDomain *basic_auth_domain, *digest_auth_domain;
	GUri *uri;

	g_test_bug ("562339");

	/* It doesn't seem to be possible to configure Apache to serve
	 * multiple auth types for a single URL. So we have to use
	 * SoupServer here. We know that SoupServer handles the server
	 * side of this scenario correctly, because we test it against
	 * curl in server-auth-test.
	 */
	server = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);
	soup_server_add_handler (server, NULL,
				 server_callback, NULL, NULL);
	uri = soup_test_server_get_uri (server, "http", NULL);

	basic_auth_domain = soup_auth_domain_basic_new (
		"realm", "auth-test",
		"auth-callback", server_basic_auth_callback,
		NULL);
        soup_auth_domain_add_path (basic_auth_domain, "/");
	soup_server_add_auth_domain (server, basic_auth_domain);

	digest_auth_domain = soup_auth_domain_digest_new (
		"realm", "auth-test",
		"auth-callback", server_digest_auth_callback,
		NULL);
        soup_auth_domain_add_path (digest_auth_domain, "/");
	soup_server_add_auth_domain (server, digest_auth_domain);

	debug_printf (1, "  Testing with no auth\n");
	select_auth_test_one (uri, FALSE, NULL,
			      "Basic, Digest", "Digest",
			      NULL, NULL,
			      SOUP_STATUS_UNAUTHORIZED);

	debug_printf (1, "  Testing with bad password\n");
	select_auth_test_one (uri, FALSE, "bad",
			      "Basic, Digest", "Digest",
			      "Basic, Digest", "Digest",
			      SOUP_STATUS_UNAUTHORIZED);

	debug_printf (1, "  Testing with good password\n");
	select_auth_test_one (uri, FALSE, "good",
			      "Basic, Digest", "Digest",
			      NULL, NULL,
			      SOUP_STATUS_OK);

	/* Test with Digest disabled in the client. */
	debug_printf (1, "  Testing without Digest with no auth\n");
	select_auth_test_one (uri, TRUE, NULL,
			      "Basic, Digest", "Basic",
			      NULL, NULL,
			      SOUP_STATUS_UNAUTHORIZED);

	debug_printf (1, "  Testing without Digest with bad password\n");
	select_auth_test_one (uri, TRUE, "bad",
			      "Basic, Digest", "Basic",
			      "Basic, Digest", "Basic",
			      SOUP_STATUS_UNAUTHORIZED);

	debug_printf (1, "  Testing without Digest with good password\n");
	select_auth_test_one (uri, TRUE, "good-basic",
			      "Basic, Digest", "Basic",
			      NULL, NULL,
			      SOUP_STATUS_OK);

	/* Now flip the order of the domains, verify that this flips
	 * the order of the headers, and make sure that digest auth
	 * *still* gets used.
	 */

	soup_server_remove_auth_domain (server, basic_auth_domain);
	soup_server_remove_auth_domain (server, digest_auth_domain);
	soup_server_add_auth_domain (server, digest_auth_domain);
	soup_server_add_auth_domain (server, basic_auth_domain);

	debug_printf (1, "  Testing flipped with no auth\n");
	select_auth_test_one (uri, FALSE, NULL,
			      "Digest, Basic", "Digest",
			      NULL, NULL,
			      SOUP_STATUS_UNAUTHORIZED);

	debug_printf (1, "  Testing flipped with bad password\n");
	select_auth_test_one (uri, FALSE, "bad",
			      "Digest, Basic", "Digest",
			      "Digest, Basic", "Digest",
			      SOUP_STATUS_UNAUTHORIZED);

	debug_printf (1, "  Testing flipped with good password\n");
	select_auth_test_one (uri, FALSE, "good",
			      "Digest, Basic", "Digest",
			      NULL, NULL,
			      SOUP_STATUS_OK);

	g_object_unref (basic_auth_domain);
	g_object_unref (digest_auth_domain);
	g_uri_unref (uri);
	soup_test_server_quit_unref (server);
}

static void
sneakily_close_connection (SoupServerMessage *msg,
			   gpointer           user_data)
{
	/* Sneakily close the connection after the response, by
	 * tricking soup-message-io into thinking that had been
	 * the plan all along.
	 */
	soup_message_headers_append (soup_server_message_get_response_headers (msg),
				     "Connection", "close");
}

static void
auth_close_request_started (SoupServer        *server,
			    SoupServerMessage *msg,
			    gpointer           user_data)
{
	g_signal_connect (msg, "wrote-headers",
			  G_CALLBACK (sneakily_close_connection), NULL);
}

typedef struct {
	SoupSession *session;
	SoupMessage *msg;
	SoupAuth *auth;
} AuthCloseData;

static gboolean
auth_close_idle_authenticate (gpointer user_data)
{
	AuthCloseData *acd = user_data;

	soup_auth_authenticate (acd->auth, "user", "good-basic");

	g_object_unref (acd->auth);
	return FALSE;
}

static gboolean
auth_close_authenticate (SoupMessage   *msg,
			 SoupAuth      *auth,
			 gboolean       retrying,
			 AuthCloseData *acd)
{
	acd->auth = g_object_ref (auth);
	g_idle_add (auth_close_idle_authenticate, acd);

	return TRUE;
}

static void
do_auth_close_test (void)
{
	SoupServer *server;
	SoupAuthDomain *basic_auth_domain;
	GUri *uri, *tmp;
	AuthCloseData acd;
	GBytes *body;

	server = soup_test_server_new (SOUP_TEST_SERVER_DEFAULT);
	soup_server_add_handler (server, NULL,
				 server_callback, NULL, NULL);

	uri = soup_test_server_get_uri (server, "http", NULL);
        tmp = g_uri_parse_relative (uri, "/close", SOUP_HTTP_URI_FLAGS, NULL);
        g_uri_unref (uri);
        uri = g_steal_pointer (&tmp);

	basic_auth_domain = soup_auth_domain_basic_new (
		"realm", "auth-test",
		"auth-callback", server_basic_auth_callback,
		NULL);
        soup_auth_domain_add_path (basic_auth_domain, "/");
	soup_server_add_auth_domain (server, basic_auth_domain);
	g_object_unref (basic_auth_domain);

	g_signal_connect (server, "request-started",
			  G_CALLBACK (auth_close_request_started), NULL);

	acd.session = soup_test_session_new (NULL);
	acd.msg = soup_message_new_from_uri ("GET", uri);
	g_signal_connect (acd.msg, "authenticate",
			  G_CALLBACK (auth_close_authenticate), &acd);
	g_uri_unref (uri);
	body = soup_test_session_async_send (acd.session, acd.msg, NULL, NULL);

	soup_test_assert_message_status (acd.msg, SOUP_STATUS_OK);

	g_bytes_unref (body);
	g_object_unref (acd.msg);
	soup_test_session_abort_unref (acd.session);
	soup_test_server_quit_unref (server);
}

static gboolean
infinite_cancel (gpointer session)
{
	soup_session_abort (session);
	return FALSE;
}

static gboolean
infinite_authenticate (SoupMessage *msg,
		       SoupAuth    *auth,
		       gboolean     retrying)
{
	soup_auth_authenticate (auth, "user", "bad");

	return TRUE;
}

static void
do_infinite_auth_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	char *uri;
	int timeout;
	GError *error = NULL;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);
	uri = g_strconcat (base_uri, "Basic/realm1/", NULL);
	msg = soup_message_new ("GET", uri);
	g_signal_connect (msg, "authenticate",
                          G_CALLBACK (infinite_authenticate), NULL);
	g_free (uri);

	timeout = g_timeout_add (500, infinite_cancel, session);
	g_assert_null (soup_session_send (session, msg, NULL, &error));
	g_assert_error (error, SOUP_SESSION_ERROR, SOUP_SESSION_ERROR_TOO_MANY_RESTARTS);
	soup_test_assert_message_status (msg, SOUP_STATUS_UNAUTHORIZED);

	g_source_remove (timeout);
	soup_test_session_abort_unref (session);
	g_clear_error (&error);
	g_object_unref (msg);
}

static void
disappear_request_read (SoupServer        *server,
			SoupServerMessage *msg,
			gpointer           user_data)
{
	SoupMessageHeaders *request_headers;
	SoupMessageHeaders *response_headers;

	request_headers = soup_server_message_get_request_headers (msg);
	response_headers = soup_server_message_get_response_headers (msg);

	/* Remove the WWW-Authenticate header if this was a failed attempt */
	if (soup_message_headers_get_one (request_headers, "Authorization") &&
	    soup_server_message_get_status (msg) == SOUP_STATUS_UNAUTHORIZED)
		soup_message_headers_remove (response_headers, "WWW-Authenticate");
}

static gboolean
disappear_authenticate (SoupMessage *msg,
			SoupAuth    *auth,
			gboolean     retrying,
			int         *counter)
{
	(*counter)++;
	if (!retrying) {
		soup_auth_authenticate (auth, "user", "bad");

		return TRUE;
	}

	return FALSE;
}

static void
do_disappearing_auth_test (void)
{
	SoupServer *server;
	SoupAuthDomain *auth_domain;
	GUri *uri;
	SoupMessage *msg;
	SoupSession *session;
	int counter;
	GBytes *body;

	g_test_bug_base ("https://bugzilla.redhat.com/");
	g_test_bug ("916224");

	server = soup_test_server_new (SOUP_TEST_SERVER_DEFAULT);
	soup_server_add_handler (server, NULL,
				 server_callback, NULL, NULL);
	uri = soup_test_server_get_uri (server, "http", NULL);

	auth_domain = soup_auth_domain_basic_new (
						  "realm", "auth-test",
						  "auth-callback", server_basic_auth_callback,
						  NULL);
        soup_auth_domain_add_path (auth_domain, "/");
	soup_server_add_auth_domain (server, auth_domain);
	g_signal_connect (server, "request-read",
			  G_CALLBACK (disappear_request_read), NULL);

	session = soup_test_session_new (NULL);

	counter = 0;
	msg = soup_message_new_from_uri ("GET", uri);
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (disappear_authenticate), &counter);

	body = soup_test_session_async_send (session, msg, NULL, NULL);

	soup_test_assert (counter <= 2,
			  "Got stuck in loop");
	soup_test_assert_message_status (msg, SOUP_STATUS_UNAUTHORIZED);

	g_bytes_unref (body);
	g_object_unref (msg);
	soup_test_session_abort_unref (session);

	g_object_unref (auth_domain);
	g_uri_unref (uri);
	soup_test_server_quit_unref (server);
}

static SoupAuthTest relogin_tests[] = {
	{ "Auth provided via URL, should succeed",
	  "Basic/realm12/", "1", TRUE, "01", SOUP_STATUS_OK },

	{ "Now should automatically reuse previous auth",
	  "Basic/realm12/", "", FALSE, "1", SOUP_STATUS_OK },

	{ "Different auth provided via URL for the same realm, should succeed",
	  "Basic/realm12/", "2", TRUE, "2", SOUP_STATUS_OK },

	{ "Subdir should also automatically reuse auth",
	  "Basic/realm12/subdir/", "", FALSE, "2", SOUP_STATUS_OK },

	{ "Should fail with no auth",
	  "Basic/realm12/", "4", TRUE, "4", SOUP_STATUS_UNAUTHORIZED },

	{ "Make sure we've forgotten it",
	  "Basic/realm12/", "", FALSE, "0", SOUP_STATUS_UNAUTHORIZED },

	{ "Should fail with no auth, fail again with bad password, and give up",
	  "Basic/realm12/", "3", FALSE, "03", SOUP_STATUS_UNAUTHORIZED },

	{ NULL }
};

/* https://bugzilla.gnome.org/show_bug.cgi?id=755617 */
static SoupAuthTest basic_root_pspace_test[] = {
	{ "Auth provided via URL, should succeed",
	  "BasicRoot", "1", TRUE, "01", SOUP_STATUS_OK },

	{ "Parent dir should automatically reuse auth",
	  "/", "1", FALSE, "1", SOUP_STATUS_OK },

	{ NULL }
};

static void
do_batch_tests (gconstpointer data)
{
	const SoupAuthTest *current_tests = data;
	SoupSession *session;
	SoupMessage *msg;
	char *expected, *uristr;
	GUri *base;
	int i;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);
	base = g_uri_parse (base_uri, SOUP_HTTP_URI_FLAGS, NULL);

	for (i = 0; current_tests[i].url; i++) {
		GUri *soup_uri = g_uri_parse_relative (base, current_tests[i].url, SOUP_HTTP_URI_FLAGS, NULL);

		debug_printf (1, "Test %d: %s\n", i + 1, current_tests[i].explanation);

		if (current_tests[i].url_auth) {
			gchar *username = g_strdup_printf ("user%c", current_tests[i].provided[0]);
			gchar *password = g_strdup_printf ("realm%c", current_tests[i].provided[0]);
                        GUri *tmp = soup_uri_copy (soup_uri, SOUP_URI_USER, username, SOUP_URI_PASSWORD, password, SOUP_URI_NONE);
                        g_uri_unref (soup_uri);
                        soup_uri = tmp;
			g_free (username);
			g_free (password);
		}

		msg = soup_message_new_from_uri (SOUP_METHOD_GET, soup_uri);
		g_uri_unref (soup_uri);
		if (!msg) {
			g_printerr ("auth-test: Could not parse URI\n");
			exit (1);
		}

		uristr = g_uri_to_string (soup_message_get_uri (msg));
		debug_printf (1, "  GET %s\n", uristr);
		g_free (uristr);

		expected = g_strdup (current_tests[i].expected);
		soup_message_add_status_code_handler (
			msg, "got_headers", SOUP_STATUS_UNAUTHORIZED,
			G_CALLBACK (handler), expected);
		soup_message_add_status_code_handler (
			msg, "got_headers", SOUP_STATUS_OK,
			G_CALLBACK (handler), expected);

		g_signal_connect (msg, "authenticate",
				  G_CALLBACK (authenticate),
				  (gpointer)&current_tests[i]);
		soup_test_session_send_message (session, msg);

		soup_test_assert_message_status (msg, current_tests[i].final_status);
		soup_test_assert (!*expected,
				  "expected %d more round(s)\n",
				  (int)strlen (expected));

		g_free (expected);
		debug_printf (1, "\n");

		g_object_unref (msg);
	}
	g_uri_unref (base);

	soup_test_session_abort_unref (session);
}

static void
do_clear_credentials_test (void)
{
	SoupSession *session;
	SoupAuthManager *manager;
	char *uri;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);

	uri = g_strconcat (base_uri, "Digest/realm1/", NULL);
	do_digest_nonce_test (session, "First", uri, TRUE, TRUE, TRUE);

	manager = SOUP_AUTH_MANAGER (soup_session_get_feature (session, SOUP_TYPE_AUTH_MANAGER));
	soup_auth_manager_clear_cached_credentials (manager);

	do_digest_nonce_test (session, "Second", uri, TRUE, TRUE, TRUE);
	g_free (uri);

	soup_test_session_abort_unref (session);
}

static void
do_message_do_not_use_auth_cache_test (void)
{
	SoupSession *session;
	SoupAuthManager *manager;
	SoupMessage *msg;
	GUri *soup_uri, *auth_uri;
	char *uri;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);

	uri = g_strconcat (base_uri, "Digest/realm1/", NULL);

	/* First check that cached credentials are not used */
	do_digest_nonce_test (session, "First", uri, TRUE, TRUE, TRUE);
	do_digest_nonce_test (session, "Second", uri, TRUE, FALSE, FALSE);
	do_digest_nonce_test (session, "Third", uri, FALSE, TRUE, TRUE);

	/* Passing credentials in the URI should always authenticate
	 * no matter whether the cache is used or not
	 */
	soup_uri = g_uri_parse (uri, SOUP_HTTP_URI_FLAGS, NULL);
        auth_uri = soup_uri_copy (soup_uri, SOUP_URI_USER, "user1", SOUP_URI_PASSWORD, "realm1", SOUP_URI_NONE);

	msg = soup_message_new_from_uri (SOUP_METHOD_GET, auth_uri);
	soup_message_add_flags (msg, SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE);
	soup_test_session_send_message (session, msg);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	g_object_unref (msg);
	g_uri_unref (soup_uri);
        g_uri_unref (auth_uri);

	manager = SOUP_AUTH_MANAGER (soup_session_get_feature (session, SOUP_TYPE_AUTH_MANAGER));

	soup_auth_manager_clear_cached_credentials (manager);

	/* Now check that credentials are not stored */
	do_digest_nonce_test (session, "First", uri, FALSE, TRUE, TRUE);
	do_digest_nonce_test (session, "Second", uri, TRUE, TRUE, TRUE);
	do_digest_nonce_test (session, "Third", uri, TRUE, FALSE, FALSE);

	/* Credentials were stored for uri, but if we set SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE flag,
	 * and we don't have the authenticate signal, it should respond with 401
	 */
	msg = soup_message_new (SOUP_METHOD_GET, uri);
	soup_message_add_flags (msg, SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE);
	soup_test_session_send_message (session, msg);
	soup_test_assert_message_status (msg, SOUP_STATUS_UNAUTHORIZED);
	g_object_unref (msg);
	g_free (uri);

	soup_test_session_abort_unref (session);
}

static gboolean
async_no_auth_cache_authenticate (SoupMessage *msg,
				  SoupAuth    *auth,
				  gboolean     retrying,
				  SoupAuth   **auth_out)
{
	debug_printf (1, "  async_no_auth_cache_authenticate\n");

	*auth_out = g_object_ref (auth);
	g_main_loop_quit (loop);

	return TRUE;
}

static void
async_no_auth_cache_finished (SoupMessage *msg, gpointer user_data)
{
	debug_printf (1, "  async_no_auth_cache_finished\n");

	g_main_loop_quit (loop);
}

static void
do_async_message_do_not_use_auth_cache_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	char *uri;
	SoupAuth *auth = NULL;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	loop = g_main_loop_new (NULL, TRUE);
	session = soup_test_session_new (NULL);
	uri = g_strconcat (base_uri, "Basic/realm1/", NULL);

	msg = soup_message_new ("GET", uri);
	g_free (uri);
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (async_no_auth_cache_authenticate), &auth);
	soup_message_add_flags (msg, SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE);
	g_signal_connect (msg, "finished",
			  G_CALLBACK (async_no_auth_cache_finished), NULL);
	soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
	g_main_loop_run (loop);

	soup_test_assert_message_status (msg, SOUP_STATUS_UNAUTHORIZED);

	soup_test_assert (auth, "msg didn't get authenticate signal");
	soup_auth_authenticate (auth, "user1", "realm1");
	g_object_unref (auth);

	g_main_loop_run (loop);

	soup_test_assert_message_status (msg, SOUP_STATUS_OK);

	soup_test_session_abort_unref (session);
	g_object_unref (msg);
	g_main_loop_unref (loop);
}

static gboolean
has_authorization_header_authenticate (SoupMessage *msg,
				       SoupAuth    *auth,
				       gboolean     retrying,
				       SoupAuth   **saved_auth)
{
	soup_auth_authenticate (auth, "user1", "realm1");
	*saved_auth = g_object_ref (auth);

	return TRUE;
}

static void
has_authorization_header_authenticate_assert (SoupMessage *msg,
					      SoupAuth    *auth,
					      gboolean     retrying)
{
	soup_test_assert (FALSE, "authenticate emitted unexpectedly");
}

static void
do_message_has_authorization_header_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	SoupAuthManager *manager;
	SoupAuth *auth = NULL;
	char *token;
	char *uri;

	g_test_bug ("775882");

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);
	uri = g_strconcat (base_uri, "Digest/realm1/", NULL);

	msg = soup_message_new ("GET", uri);
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (has_authorization_header_authenticate), &auth);
	soup_test_session_send_message (session, msg);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	soup_test_assert (SOUP_IS_AUTH (auth), "Expected a SoupAuth");
	token = soup_auth_get_authorization (auth, msg);
	g_object_unref (auth);
	g_object_unref (msg);

	manager = SOUP_AUTH_MANAGER (soup_session_get_feature (session, SOUP_TYPE_AUTH_MANAGER));
	soup_auth_manager_clear_cached_credentials (manager);

	msg = soup_message_new ("GET", uri);
	soup_message_headers_replace (soup_message_get_request_headers (msg), "Authorization", token);
	g_signal_connect (msg, "authenticate",
			  G_CALLBACK (has_authorization_header_authenticate_assert),
			  NULL);
	soup_test_session_send_message (session, msg);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	g_object_unref (msg);

	/* Check that we can also provide our own Authorization header when not using credentials cache. */
	soup_auth_manager_clear_cached_credentials (manager);
	msg = soup_message_new ("GET", uri);
	soup_message_headers_replace (soup_message_get_request_headers (msg), "Authorization", token);
	soup_message_add_flags (msg, SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE);
	soup_test_session_send_message (session, msg);
	soup_test_assert_message_status (msg, SOUP_STATUS_OK);
	g_object_unref (msg);
	g_free (token);

	g_free (uri);
	soup_test_session_abort_unref (session);
}

static gboolean
cancel_after_retry_authenticate (SoupMessage  *msg,
                                 SoupAuth     *auth,
                                 gboolean      retrying,
                                 GCancellable *cancellable)
{
        if (retrying) {
                g_cancellable_cancel (cancellable);

		return FALSE;
	}

	soup_auth_authenticate (auth, "user1", "wrong");

	return TRUE;
}

static void
request_send_cb (SoupSession  *session,
                 GAsyncResult *result,
                 GMainLoop    *loop)
{
        GInputStream *stream;
        GError *error = NULL;

        stream = soup_session_send_finish (session, result, &error);
        g_assert_null (stream);
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

        g_main_loop_quit (loop);
}

static void
do_cancel_after_retry_test (void)
{
        SoupSession *session;
        SoupMessage *msg;
        char *uri;
	GCancellable *cancellable;
        GMainLoop *loop;

        SOUP_TEST_SKIP_IF_NO_APACHE;

        session = soup_test_session_new (NULL);
        cancellable = g_cancellable_new ();

        loop = g_main_loop_new (NULL, FALSE);

        uri = g_strconcat (base_uri, "Digest/realm1/", NULL);
        msg = soup_message_new ("GET", uri);
	g_signal_connect (msg, "authenticate",
                          G_CALLBACK (cancel_after_retry_authenticate),
                          cancellable);
        soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, cancellable,
				 (GAsyncReadyCallback)request_send_cb, loop);
        g_main_loop_run (loop);

        g_object_unref (msg);
        g_object_unref (cancellable);
        g_free (uri);
        soup_test_session_abort_unref (session);
        g_main_loop_unref (loop);
}

static gboolean
cancel_on_authenticate (SoupMessage  *msg,
                        SoupAuth     *auth,
                        gboolean      retrying)
{
        soup_auth_cancel (auth);

        return TRUE;
}

static void
do_cancel_on_authenticate (void)
{
        SoupSession *session;
        SoupMessage *msg;
        char *uri;

        SOUP_TEST_SKIP_IF_NO_APACHE;

        session = soup_test_session_new (NULL);

        loop = g_main_loop_new (NULL, FALSE);

        uri = g_strconcat (base_uri, "Digest/realm1/", NULL);
        msg = soup_message_new ("GET", uri);
        g_signal_connect (msg, "authenticate",
                          G_CALLBACK (cancel_on_authenticate),
                          NULL);
        g_signal_connect (msg, "finished",
                          G_CALLBACK (async_no_auth_cache_finished), NULL);
        soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
        g_main_loop_run (loop);

        soup_test_assert_message_status (msg, SOUP_STATUS_UNAUTHORIZED);

        g_object_unref (msg);
        g_free (uri);
        soup_test_session_abort_unref (session);
        g_main_loop_unref (loop);
}

static void
do_cancel_request_on_authenticate (void)
{
        SoupSession *session;
        SoupMessage *msg;
        GCancellable *cancellable;
        SoupAuth *auth = NULL;
        char *uri;

        SOUP_TEST_SKIP_IF_NO_APACHE;

        session = soup_test_session_new (NULL);
        loop = g_main_loop_new (NULL, FALSE);

        uri = g_strconcat (base_uri, "Digest/realm1/", NULL);
        msg = soup_message_new ("GET", uri);
        g_signal_connect (msg, "authenticate",
                          G_CALLBACK (async_authenticate),
                          &auth);
        g_signal_connect (msg, "finished",
                          G_CALLBACK (async_no_auth_cache_finished), NULL);
        cancellable = g_cancellable_new ();
        soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, cancellable, NULL, NULL);
        g_main_loop_run (loop);

        soup_test_assert_message_status (msg, SOUP_STATUS_UNAUTHORIZED);
        g_cancellable_cancel (cancellable);
        g_main_loop_run (loop);

        soup_auth_cancel (auth);

        while (g_main_context_pending (NULL))
                g_main_context_iteration (NULL, FALSE);

        g_object_unref (auth);
        g_object_unref (cancellable);
        g_object_unref (msg);
        g_free (uri);
        soup_test_session_abort_unref (session);
        g_main_loop_unref (loop);
}

static const struct {
	const char *url;
	guint status;
} uri_tests[] = {
	{ "http://user1:realm1@127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_OK },
	{ "http://user1:wrong@127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_UNAUTHORIZED },
	{ "http://user1@127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_UNAUTHORIZED },
	{ "http://user5:realm1@127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_UNAUTHORIZED },
	{ "http://127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_UNAUTHORIZED },
	{ "http://user4@127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_OK },
	{ "http://user4:@127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_OK },
	{ "http://user4:wrong@127.0.0.1:47524/Digest/realm1/", SOUP_STATUS_UNAUTHORIZED },
};

static void
do_auth_uri_test (void)
{
	SoupSession *session;
	int i;

	SOUP_TEST_SKIP_IF_NO_APACHE;

	session = soup_test_session_new (NULL);

	for (i = 0; i < G_N_ELEMENTS (uri_tests); i++) {
		SoupMessage *msg;

		msg = soup_message_new (SOUP_METHOD_GET, uri_tests[i].url);
		soup_message_add_flags (msg, SOUP_MESSAGE_DO_NOT_USE_AUTH_CACHE);
		soup_test_session_send_message (session, msg);
		soup_test_assert_message_status (msg, uri_tests[i].status);
		g_object_unref (msg);
	}

	soup_test_session_abort_unref (session);
}

static void
on_request_read (SoupServer        *server,
                 SoupServerMessage *msg,
                 gpointer           user_data)
{
        SoupMessageHeaders *response_headers = soup_server_message_get_response_headers (msg);
        char *old_header = g_strdup (soup_message_headers_get_one (response_headers, "WWW-Authenticate"));
        if (old_header) {
                /* These must be in order to ensure libsoup passes over the invalid one. */
                soup_message_headers_replace (response_headers, "WWW-Authenticate",
                                "Digest realm=\"auth-test\", nonce=\"0000000000001\", qop=\"auth\", algorithm=FAKE");
                soup_message_headers_append (response_headers, "WWW-Authenticate", old_header);
                g_free (old_header);
        }
}

static gboolean
on_digest_authenticate (SoupMessage *msg,
                        SoupAuth    *auth,
                        gboolean     retrying,
                        gpointer     user_data)
{
        g_assert_false (retrying);
        soup_auth_authenticate (auth, "user", "good");
        return TRUE;
}

static void
do_multiple_digest_algorithms (void)
{
        SoupSession *session;
        SoupMessage *msg;
        SoupServer *server;
        SoupAuthDomain *digest_auth_domain;
        gint status;
        GUri *uri;

       	server = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);
	soup_server_add_handler (server, NULL,
				 server_callback, NULL, NULL);
	uri = soup_test_server_get_uri (server, "http", NULL);

        /* Add one real authentication option, later we will add
           a fake one with an unsupported algorithm. */
	digest_auth_domain = soup_auth_domain_digest_new (
		"realm", "auth-test",
		"auth-callback", server_digest_auth_callback,
		NULL);
        soup_auth_domain_add_path (digest_auth_domain, "/");
	soup_server_add_auth_domain (server, digest_auth_domain);
        g_object_unref (digest_auth_domain);

        /* We wait for the message to come in and will add a header. */
        g_signal_connect (server, "request-read",
                          G_CALLBACK (on_request_read),
                          NULL);

        session = soup_test_session_new (NULL);
        msg = soup_message_new_from_uri ("GET", uri);
        g_signal_connect (msg, "authenticate",
                          G_CALLBACK (on_digest_authenticate),
                          NULL);

        status = soup_test_session_send_message (session, msg);

        g_assert_cmpint (status, ==, SOUP_STATUS_OK);
	g_uri_unref (uri);
	soup_test_server_quit_unref (server);
}

static void
on_request_read_for_missing_params (SoupServer        *server,
                                      SoupServerMessage *msg,
                                      gpointer           user_data)
{
        const char *auth_header = user_data;
        SoupMessageHeaders *response_headers = soup_server_message_get_response_headers (msg);
        soup_message_headers_replace (response_headers, "WWW-Authenticate", auth_header);
}

static void
do_missing_params_test (gconstpointer auth_header)
{
        SoupSession *session;
        SoupMessage *msg;
        SoupServer *server;
        SoupAuthDomain *digest_auth_domain;
        gint status;
        GUri *uri;

        server = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);
	soup_server_add_handler (server, NULL,
				 server_callback, NULL, NULL);
	uri = soup_test_server_get_uri (server, "http", NULL);

	digest_auth_domain = soup_auth_domain_digest_new (
		"realm", "auth-test",
		"auth-callback", server_digest_auth_callback,
		NULL);
        soup_auth_domain_add_path (digest_auth_domain, "/");
	soup_server_add_auth_domain (server, digest_auth_domain);
        g_object_unref (digest_auth_domain);

        g_signal_connect (server, "request-read",
                          G_CALLBACK (on_request_read_for_missing_params),
                          (gpointer)auth_header);

        session = soup_test_session_new (NULL);
        msg = soup_message_new_from_uri ("GET", uri);
        g_signal_connect (msg, "authenticate",
                          G_CALLBACK (on_digest_authenticate),
                          NULL);

        status = soup_test_session_send_message (session, msg);

        g_assert_cmpint (status, ==, SOUP_STATUS_UNAUTHORIZED);
	g_uri_unref (uri);
	soup_test_server_quit_unref (server);
}

static void
redirect_server_callback (SoupServer        *server,
                          SoupServerMessage *msg,
                          const char        *path,
                          GHashTable        *query,
                          gpointer           user_data)
{
    static gboolean redirected = FALSE;

    if (!redirected) {
        char *redirect_uri = g_uri_to_string (user_data);
        soup_server_message_set_redirect (msg, SOUP_STATUS_MOVED_PERMANENTLY, redirect_uri);
        g_free (redirect_uri);
        redirected = TRUE;
        return;
    }

    g_assert_cmpstr ("This code", ==, "should not be reached");
}

static gboolean
auth_for_redirect_callback (SoupMessage *msg, SoupAuth *auth, gboolean retrying, gpointer user_data)
{
    GUri *known_server_uri = user_data;

    if (!soup_uri_host_equal (known_server_uri, soup_message_get_uri (msg)))
        return FALSE;

    soup_auth_authenticate (auth, "user", "good-basic");

    return TRUE;
}

static void
do_strip_on_crossorigin_redirect (void)
{
    SoupSession *session;
    SoupMessage *msg;
    SoupServer *server1, *server2;
    SoupAuthDomain *auth_domain;
    GUri *uri;
    gint status;

    server1 = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);
    server2 = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);

    /* Both servers have the same credentials. */
    auth_domain = soup_auth_domain_basic_new ("realm", "auth-test", "auth-callback", server_basic_auth_callback, NULL);
    soup_auth_domain_add_path (auth_domain, "/");
    soup_server_add_auth_domain (server1, auth_domain);
    soup_server_add_auth_domain (server2, auth_domain);
    g_object_unref (auth_domain);

    /* Server 1 asks for auth, then redirects to Server 2. */
    soup_server_add_handler (server1, NULL,
                    redirect_server_callback,
                   soup_test_server_get_uri (server2, "http", NULL), (GDestroyNotify)g_uri_unref);
    /* Server 2 requires auth. */
    soup_server_add_handler (server2, NULL, server_callback, NULL, NULL);

    session = soup_test_session_new (NULL);
    uri = soup_test_server_get_uri (server1, "http", NULL);
    msg = soup_message_new_from_uri ("GET", uri);
    /* The client only sends credentials for the host it knows. */
    g_signal_connect (msg, "authenticate", G_CALLBACK (auth_for_redirect_callback), uri);

    status = soup_test_session_send_message (session, msg);

    g_assert_cmpint (status, ==, SOUP_STATUS_UNAUTHORIZED);

    g_uri_unref (uri);
    soup_test_server_quit_unref (server1);
    soup_test_server_quit_unref (server2);
}

int
main (int argc, char **argv)
{
	int ret;

	test_init (argc, argv, NULL);
	apache_init ();

	base_uri = "http://127.0.0.1:47524/";

	g_test_add_data_func ("/auth/main-tests", main_tests, do_batch_tests);
	g_test_add_data_func ("/auth/relogin-tests", relogin_tests, do_batch_tests);
	g_test_add_data_func ("/auth/basic-root-pspec-test", basic_root_pspace_test, do_batch_tests);
	g_test_add_func ("/auth/pipelined-auth", do_pipelined_auth_test);
	g_test_add_func ("/auth/digest-expiration", do_digest_expiration_test);
	g_test_add_func ("/auth/async-auth/good-password", do_async_auth_good_password_test);
	g_test_add_func ("/auth/async-auth/bad-password", do_async_auth_bad_password_test);
	g_test_add_func ("/auth/async-auth/no-password", do_async_auth_no_password_test);
	g_test_add_func ("/auth/async-auth/cancel", do_async_auth_cancel_test);
        g_test_add_func ("/auth/sync-auth/bad-password", do_sync_auth_bad_password_test);
	g_test_add_func ("/auth/select-auth", do_select_auth_test);
	g_test_add_func ("/auth/auth-close", do_auth_close_test);
	g_test_add_func ("/auth/infinite-auth", do_infinite_auth_test);
	g_test_add_func ("/auth/disappearing-auth", do_disappearing_auth_test);
	g_test_add_func ("/auth/clear-credentials", do_clear_credentials_test);
	g_test_add_func ("/auth/message-do-not-use-auth-cache", do_message_do_not_use_auth_cache_test);
	g_test_add_func ("/auth/async-message-do-not-use-auth-cache", do_async_message_do_not_use_auth_cache_test);
	g_test_add_func ("/auth/authorization-header-request", do_message_has_authorization_header_test);
	g_test_add_func ("/auth/cancel-after-retry", do_cancel_after_retry_test);
	g_test_add_func ("/auth/cancel-on-authenticate", do_cancel_on_authenticate);
	g_test_add_func ("/auth/auth-uri", do_auth_uri_test);
        g_test_add_func ("/auth/cancel-request-on-authenticate", do_cancel_request_on_authenticate);
        g_test_add_func ("/auth/multiple-algorithms", do_multiple_digest_algorithms);
        g_test_add_func ("/auth/strip-on-crossorigin-redirect", do_strip_on_crossorigin_redirect);
        g_test_add_data_func ("/auth/missing-params/realm", "Digest qop=\"auth\"", do_missing_params_test);
        g_test_add_data_func ("/auth/missing-params/nonce", "Digest realm=\"auth-test\", qop=\"auth,auth-int\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"", do_missing_params_test);
        g_test_add_data_func ("/auth/missing-params/nonce-md5-sess", "Digest realm=\"auth-test\", qop=\"auth,auth-int\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\" algorithm=\"MD5-sess\"", do_missing_params_test);
        g_test_add_data_func ("/auth/missing-params/nonce-and-qop", "Digest realm=\"auth-test\"", do_missing_params_test);

	ret = g_test_run ();

	test_cleanup ();
	return ret;
}

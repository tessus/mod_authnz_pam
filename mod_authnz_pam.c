
/*
 * Copyright 2014--2022 Jan Pazdziora
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <security/pam_appl.h>

#include "apr_general.h"
#include "apr_strings.h"
#include "apr_md5.h"

#include "ap_config.h"
#include "ap_provider.h"
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "http_request.h"

#include "mod_auth.h"

#ifdef APLOG_USE_MODULE
#define SHOW_MODULE ""
#else
#define SHOW_MODULE "mod_authnz_pam: "
#endif

typedef struct {
	char * pam_service;
	char * expired_redirect_url;
	int expired_redirect_status;
} authnz_pam_config_rec;

static void * create_dir_conf(apr_pool_t * pool, char * dir) {
	authnz_pam_config_rec * cfg = apr_pcalloc(pool, sizeof(authnz_pam_config_rec));
	return cfg;
}

static const char * set_redirect_and_status(cmd_parms * cmd, void * conf_void, const char * url, const char * status) {
	authnz_pam_config_rec * cfg = (authnz_pam_config_rec *) conf_void;
        if (cfg) {
                cfg->expired_redirect_url = apr_pstrdup(cmd->pool, url);
		cfg->expired_redirect_status = HTTP_SEE_OTHER;
                if (status) {
                        cfg->expired_redirect_status = apr_atoi64(status);
                        if (cfg->expired_redirect_status == 0) {
				ap_log_error(APLOG_MARK, APLOG_WARNING, 0, cmd->server,
					SHOW_MODULE "AuthPAMExpiredRedirect status has to be a number, setting to %d",
					HTTP_SEE_OTHER);
				cfg->expired_redirect_status = HTTP_SEE_OTHER;
			} else if (cfg->expired_redirect_status < 300 || cfg->expired_redirect_status > 399) {
				ap_log_error(APLOG_MARK, APLOG_WARNING, 0, cmd->server,
					SHOW_MODULE "AuthPAMExpiredRedirect status has to be in the 3xx range, setting to %d",
					HTTP_SEE_OTHER);
				cfg->expired_redirect_status = HTTP_SEE_OTHER;
			}
		}
        }
        return NULL;
}

static const command_rec authnz_pam_cmds[] = {
	AP_INIT_TAKE1("AuthPAMService", ap_set_string_slot,
		(void *)APR_OFFSETOF(authnz_pam_config_rec, pam_service),
		OR_AUTHCFG, "PAM service to authenticate against"),
	AP_INIT_TAKE12("AuthPAMExpiredRedirect", set_redirect_and_status,
		NULL,
		ACCESS_CONF|OR_AUTHCFG, "URL (and optional status) to redirect to should user have expired credentials"),
	{NULL}
};

static int pam_authenticate_conv(int num_msg, const struct pam_message ** msg, struct pam_response ** resp, void * appdata_ptr) {
	struct pam_response * response = NULL;
	if (!msg || !resp || !appdata_ptr)
		return PAM_CONV_ERR;
	if (!(response = malloc(num_msg * sizeof(struct pam_response))))
		return PAM_CONV_ERR;
	int i;
	for (i = 0; i < num_msg; i++) {
		response[i].resp = 0;
		response[i].resp_retcode = 0;
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF) {
			if (i == 0) {
				response[i].resp = strdup(appdata_ptr);
			} else {
				response[i].resp = NULL;
			}
		} else {
			free(response);
			return PAM_CONV_ERR;
		}
	}
	* resp = response;
	return PAM_SUCCESS;
}

#if AP_MODULE_MAGIC_AT_LEAST(20111025,1)
#else
#include <stdio.h>
#include "apr_lib.h"
static const char * ap_escape_urlencoded(apr_pool_t * pool, const char * buffer) {
	char * copy = apr_palloc(pool, 3 * strlen(buffer) + 1);
	char * p = copy;
	while (*buffer) {
		if (!apr_isalnum(*buffer) && !strchr(".-*_ ", *buffer)) {
			*p++ = '%';
			p += snprintf(p, 3, "%02x", *buffer);
		} else if (*buffer == ' ') {
			*p++ = '+';
		} else {
			*p++ = *buffer;
		}
		buffer++;
	}
	*p = '\0';
	return copy;
}
#endif


static const char * format_location(request_rec * r, const char * url, const char *login) {
	const char * out = "";
	const char * p = url;
	const char * append = NULL;
	while (*p) {
		if (*p == '%') {
			if (*(p + 1) == '%') {
				append = "%";
			} else if (*(p + 1) == 's') {
				append = ap_construct_url(r->pool, r->uri, r);
				if (r->args) {
					append = apr_pstrcat(r->pool, append, "?", r->args, NULL);
				}
			} else if (*(p + 1) == 'u') {
				append = login;
			}
		}
		if (append) {
			char * prefix = "";
			if (p != url) {
				prefix = apr_pstrndup(r->pool, url, p - url);
			}
			out = apr_pstrcat(r->pool, out, prefix, ap_escape_urlencoded(r->pool, append), NULL);
			p++;
			url = p + 1;
			append = NULL;
		}
		p++;
	}
	if (p != url) {
		out = apr_pstrcat(r->pool, out, url, NULL);
	}
	return out;
}

module AP_MODULE_DECLARE_DATA authnz_pam_module;

#if AP_MODULE_MAGIC_AT_LEAST(20100625,0)
static APR_OPTIONAL_FN_TYPE(ap_authn_cache_store) *authn_cache_store = NULL;

// copied from socache implementations of dbm and dbd @ http://svn.eu.apache.org/viewvc?view=revision&revision=957072
static void opt_retr(void) {
	authn_cache_store = APR_RETRIEVE_OPTIONAL_FN(ap_authn_cache_store);
}

void store_password_to_cache(request_rec * r, const char * login, const char * password) {
	if (!(authn_cache_store && login && password)) {
		return;
	}
	unsigned char salt[16];
	char hash[61];
	if (apr_generate_random_bytes(salt, sizeof(salt)) != APR_SUCCESS) {
		ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
			SHOW_MODULE "apr_generate_random_bytes failed, will not cache password");
		return;
	}
	if (apr_bcrypt_encode(password, 5, salt, sizeof(salt), hash, sizeof(hash)) != APR_SUCCESS) {
		ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r,
			SHOW_MODULE "apr_bcrypt_encode failed, will not cache password");
		return;
	}
	authn_cache_store(r, "PAM", login, NULL, hash);
}
#endif

#define _REMOTE_USER_ENV_NAME "REMOTE_USER"
#define _EXTERNAL_AUTH_ERROR_ENV_NAME "EXTERNAL_AUTH_ERROR"
#define _PAM_STEP_AUTH 1
#define _PAM_STEP_ACCOUNT 2
#define _PAM_STEP_ALL 3
static authn_status pam_authenticate_with_login_password(request_rec * r, const char * pam_service,
	const char * login, const char * password, int steps) {
	pam_handle_t * pamh = NULL;
	struct pam_conv pam_conversation = { &pam_authenticate_conv, (void *) password };
	const char * stage = "PAM transaction failed for service";
	const char * param = pam_service;
	int ret;
	ret = pam_start(pam_service, login, &pam_conversation, &pamh);
	if (ret == PAM_SUCCESS) {
#if AP_MODULE_MAGIC_AT_LEAST(20120211,56)
		const char * remote_host_or_ip = ap_get_useragent_host(r, REMOTE_NAME, NULL);
#else
		const char * remote_host_or_ip = ap_get_remote_host(r->connection, r->per_dir_config, REMOTE_NAME, NULL);
#endif
		if (remote_host_or_ip) {
			stage = "PAM pam_set_item PAM_RHOST failed for service";
			ret = pam_set_item(pamh, PAM_RHOST, remote_host_or_ip);
		}
	}
	if (ret == PAM_SUCCESS) {
		if (steps & _PAM_STEP_AUTH) {
			param = login;
			stage = "PAM authentication failed for user";
			ret = pam_authenticate(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK);
		}
		if ((ret == PAM_SUCCESS) && (steps & _PAM_STEP_ACCOUNT)) {
			param = login;
			stage = "PAM account validation failed for user";
			ret = pam_acct_mgmt(pamh, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK);
			if (ret == PAM_NEW_AUTHTOK_REQD) {
				authnz_pam_config_rec * conf = ap_get_module_config(r->per_dir_config, &authnz_pam_module);
				if (conf && conf->expired_redirect_url) {
					ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
						SHOW_MODULE "PAM_NEW_AUTHTOK_REQD: redirect to [%s] using [%d]",
						conf->expired_redirect_url, conf->expired_redirect_status);
					apr_table_addn(r->headers_out, "Location", format_location(r, conf->expired_redirect_url, login));
					r->status = conf->expired_redirect_status;
					ap_send_error_response(r, 0);
					return AUTH_DENIED;
				}
			}
		}
	}
	if (ret != PAM_SUCCESS) {
		const char * strerr = pam_strerror(pamh, ret);
		ap_log_rerror(APLOG_MARK, APLOG_WARNING, 0, r, SHOW_MODULE "%s %s: %s", stage, param, strerr);
		apr_table_setn(r->subprocess_env, _EXTERNAL_AUTH_ERROR_ENV_NAME, apr_pstrdup(r->pool, strerr));
		pam_end(pamh, ret);
		return AUTH_DENIED;
	}
	apr_table_setn(r->subprocess_env, _REMOTE_USER_ENV_NAME, login);
	r->user = apr_pstrdup(r->pool, login);
	ap_log_rerror(APLOG_MARK, APLOG_INFO, 0, r, SHOW_MODULE "PAM authentication passed for user %s", login);
	pam_end(pamh, ret);
#if AP_MODULE_MAGIC_AT_LEAST(20100625,0)
	if (steps & _PAM_STEP_AUTH) {
		store_password_to_cache(r, login, password);
	}
#endif
	return AUTH_GRANTED;
}

APR_DECLARE_OPTIONAL_FN(authn_status, pam_authenticate_with_login_password,
	(request_rec * r, const char * pam_service,
	const char * login, const char * password, int steps));

static authn_status pam_auth_account(request_rec * r, const char * login, const char * password) {
	authnz_pam_config_rec * conf = ap_get_module_config(r->per_dir_config, &authnz_pam_module);

	if (!conf->pam_service) {
		return AUTH_GENERAL_ERROR;
	}

	return pam_authenticate_with_login_password(r, conf->pam_service, login, password, _PAM_STEP_ALL);
}

static const authn_provider authn_pam_provider = {
	&pam_auth_account,
};

#ifdef AUTHN_PROVIDER_VERSION
static authz_status check_user_access(request_rec * r, const char * require_args, const void * parsed_require_args) {
	if (!r->user) {
		return AUTHZ_DENIED_NO_USER;
	}

	const char * pam_service = ap_getword_conf(r->pool, &require_args);
	if (pam_service && pam_service[0]) {
		authn_status ret = pam_authenticate_with_login_password(r, pam_service, r->user, NULL, _PAM_STEP_ACCOUNT);
		if (ret == AUTH_GRANTED) {
			return AUTHZ_GRANTED;
		}
	}
	return AUTHZ_DENIED;
}
static const authz_provider authz_pam_provider = {
	&check_user_access,
        NULL,
};
#else
static int check_user_access(request_rec * r) {
	int m = r->method_number;
	const apr_array_header_t * reqs_arr = ap_requires(r);
	if (! reqs_arr) {
		return DECLINED;
	}
	require_line * reqs = (require_line *)reqs_arr->elts;
	int x;
	for (x = 0; x < reqs_arr->nelts; x++) {
		if (!(reqs[x].method_mask & (AP_METHOD_BIT << m))) {
			continue;
		}
		const char * t = reqs[x].requirement;
		const char * w = ap_getword_white(r->pool, &t);
		if (!strcasecmp(w, "pam-account")) {
			const char * pam_service = ap_getword_conf(r->pool, &t);
			if (pam_service && strlen(pam_service)) {
				authn_status ret = pam_authenticate_with_login_password(r, pam_service, r->user, NULL, _PAM_STEP_ACCOUNT);
				if (ret == AUTH_GRANTED) {
					return OK;
				}
			}
		}
	}
	return DECLINED;
}
#endif

static void register_hooks(apr_pool_t * p) {
#ifdef AUTHN_PROVIDER_VERSION
	ap_register_auth_provider(p, AUTHN_PROVIDER_GROUP, "PAM", AUTHN_PROVIDER_VERSION, &authn_pam_provider, AP_AUTH_INTERNAL_PER_CONF);
	ap_register_auth_provider(p, AUTHZ_PROVIDER_GROUP, "pam-account", AUTHZ_PROVIDER_VERSION, &authz_pam_provider, AP_AUTH_INTERNAL_PER_CONF);
#else
	ap_register_provider(p, AUTHN_PROVIDER_GROUP, "PAM", "0", &authn_pam_provider);
	ap_hook_auth_checker(check_user_access, NULL, NULL, APR_HOOK_MIDDLE);
#endif
	APR_REGISTER_OPTIONAL_FN(pam_authenticate_with_login_password);
#if AP_MODULE_MAGIC_AT_LEAST(20100625,0)
	ap_hook_optional_fn_retrieve(opt_retr, NULL, NULL, APR_HOOK_MIDDLE);
#endif
}

#ifdef AP_DECLARE_MODULE
AP_DECLARE_MODULE(authnz_pam)
#else
module AP_MODULE_DECLARE_DATA authnz_pam_module
#endif
	= {
	STANDARD20_MODULE_STUFF,
	create_dir_conf,	/* Per-directory configuration handler */
	NULL,			/* Merge handler for per-directory configurations */
	NULL,			/* Per-server configuration handler */
	NULL,			/* Merge handler for per-server configurations */
	authnz_pam_cmds,	/* Any directives we may have for httpd */
	register_hooks		/* Our hook registering function */
};


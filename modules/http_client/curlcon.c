/*
 * Copyright (C) 2015 Olle E. Johansson, Edvina AB
 *
 * Based on code from sqlops and htable by Elena-Ramona:
 * Copyright (C) 2008 Elena-Ramona Modroiu (asipto.com)
 *
 * This file is part of kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*! \file
 * \brief  Kamailio http_client :: Connectoin handling
 * \ingroup http_client
 */

#include <curl/curl.h>

#include "../../hashes.h"
#include "../../dprint.h"
#include "../../parser/parse_param.h"
#include "../../usr_avp.h"
#include "http_client.h"
#include "curlcon.h"

#define KEYVALUE_TYPE_NONE	0
#define KEYVALUE_TYPE_PARAMS	1


curl_con_t *_curl_con_root = NULL;

/* Forward declaration */
curl_con_t *curl_init_con(str *name);

/*! Count the number of connections 
 */
unsigned int curl_connection_count()
{
	unsigned int i = 0;
	curl_con_t *cc;
	cc = _curl_con_root;
	while(cc)
	{
		i++;
		cc = cc->next;
	}
	return i;
}


/*! Find CURL connection by name
 */
curl_con_t* curl_get_connection(str *name)
{
	curl_con_t *cc;
	unsigned int conid;

	conid = core_case_hash(name, 0, 0);
	LM_DBG("curl_get_connection looking for curlcon: [%.*s] ID %u\n", name->len, name->s, conid);

	cc = _curl_con_root;
	while(cc)
	{
		if(conid==cc->conid && cc->name.len==name->len && strncmp(cc->name.s, name->s, name->len)==0) {
			return cc;
		}
		cc = cc->next;
	}
	LM_DBG("curl_get_connection no success in looking for curlcon: [%.*s]\n", name->len, name->s);
	return NULL;
}


/*! Parse the curlcon module parameter
 *
 *	Syntax:
 *		name => proto://user:password@server/url/url
 *		name => proto://server/url/url
 *		name => proto://server/url/url;param=value;param=value
 *
 *		the url is very much like CURLs syntax
 *		the url is a base url where you can add local address
 *	Parameters
 *		httpredirect
 *		timeout
 *		useragent
 *		failover
 *		maxdatasize
 *		verifypeer
 *		verifyhost
 *
 */
int curl_parse_param(char *val)
{
	str name	= STR_NULL;;
	str schema	= STR_NULL;
	str url		= STR_NULL;
	str username	= STR_NULL;
	str password	= STR_NULL;
	str params	= STR_NULL;
	str failover	= STR_NULL;

	str client_cert  = default_tls_clientcert;
	str client_key   = default_tls_clientkey;
	str ciphersuites = default_cipher_suite_list;
	str useragent    = default_useragent;

	unsigned int maxdatasize = default_maxdatasize;
	unsigned int timeout	= default_connection_timeout;
	unsigned int http_follow_redirect = default_http_follow_redirect;
	unsigned int verify_peer = default_tls_verify_peer;
	unsigned int verify_host = default_tls_verify_host;
	unsigned int sslversion = default_tls_version;

	str in;
	char *p;
	char *u;
	param_t *conparams = NULL;
	curl_con_t *cc;

	LM_INFO("curl modparam parsing starting\n");
	LM_DBG("modparam curlcon: %s\n", val);

	/* parse: name=>http_url*/
	in.s = val;
	in.len = strlen(in.s);
	p = in.s;

	/* Skip white space */
	while(p < in.s+in.len && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) {
		p++;
	}
	if(p > in.s+in.len || *p=='\0') {
		goto error;
	}

	/* This is the connection name */
	name.s = p;
	/* Skip to whitespace */
	while(p < in.s + in.len)
	{
		if(*p=='=' || *p==' ' || *p=='\t' || *p=='\n' || *p=='\r') {
			break;
		}
		p++;
	}
	if(p > in.s+in.len || *p=='\0') {
		goto error;
	}
	name.len = p - name.s;
	if(*p != '=')
	{
		/* Skip whitespace */
		while(p<in.s+in.len && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) {
			p++;
		}
		if(p>in.s+in.len || *p=='\0' || *p!='=') {
			goto error;
		}
	}
	p++;
	if(*p != '>') {
		goto error;
	}
	p++;
	/* Skip white space again */
	while(p < in.s+in.len && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) {
		p++;
	}
	schema.s = p;
	/* Skip to colon ':' */
	while(p < in.s + in.len)
	{
		if(*p == ':') {
			break;
		}
		p++;
	}
	if(*p != ':') {
		goto error;
	}
	schema.len = p - schema.s;
	p++;	/* Skip the colon */
	/* Skip two slashes */
	if(*p != '/') {
		goto error;
	}
	p++;
	if(*p != '/') {
		goto error;
	}
	p++;
	/* We are now at the first character after :// */
	url.s = p;
	url.len = in.len + (int)(in.s - p);
	u = p;

	/* Now check if there is a @ character. If so, we need to parse the username
	   and password */
	/* Skip to at-sign '@' */
	while(p < in.s + in.len)
	{
		if(*p == '@') {
			break;
		}
		p++;
	}
	if (*p == '@') {
		/* We have a username and possibly password - parse them out */
		username.s = u;
		while (u < p) {
			if (*u == ':') {
				break;
			}
			u++;
		}
		username.len = u - username.s;

		/* We either have a : or a @ */
		if (*u == ':') {
			u++;
			/* Go look for password */
			password.s = u;
			while (u < p) {
				u++;
			}
			password.len = u - password.s;
		}
		p++;	/* Skip the at sign */
		url.s = p;
		url.len = in.len + (int)(in.s - p);
	}
	/* Reset P to beginning of URL and look for parameters - starting with ; */
	p = url.s;
	/* Skip to ';' or end of string */
	while(p < url.s + url.len)
	{
		if(*p == ';') {
			/* Cut off URL at the ; */
			url.len = (int)(p - url.s);
			break;
		}
		p++;
	}
	if (*p == ';') {
		/* We have parameters */
		str tok;
		param_t *pit = NULL;

		/* Adjust the URL length */

		p++;		/* Skip the ; */
		params.s = p;
		params.len = in.len + (int) (in.s - p);
		param_hooks_t phooks;

		if (parse_params(&params, CLASS_ANY, &phooks, &conparams) < 0)
                {
                        LM_ERR("CURL failed parsing curlcon parameters value\n");
                        goto error;
                }

		/* Have parameters */
		for (pit = conparams; pit; pit=pit->next)
		{
			tok = pit->body;
			if(pit->name.len==12 && strncmp(pit->name.s, "httpredirect", 12)==0) {
				if(str2int(&tok, &http_follow_redirect) != 0) {
					/* Bad value */
					LM_DBG("curl connection [%.*s]: httpredirect bad value. Using default\n", name.len, name.s);
					http_follow_redirect = default_http_follow_redirect;
				}
				if (http_follow_redirect != 0 && http_follow_redirect != 1) {
					LM_DBG("curl connection [%.*s]: httpredirect bad value. Using default\n", name.len, name.s);
					http_follow_redirect = default_http_follow_redirect;
				}
				LM_DBG("curl [%.*s] - httpredirect [%d]\n", pit->name.len, pit->name.s, http_follow_redirect);
			} else if(pit->name.len==7 && strncmp(pit->name.s, "timeout", 7)==0) {
				if(str2int(&tok, &timeout)!=0) {
					/* Bad timeout */
					LM_DBG("curl connection [%.*s]: timeout bad value. Using default\n", name.len, name.s);
					timeout = default_connection_timeout;
				}
				LM_DBG("curl [%.*s] - timeout [%d]\n", pit->name.len, pit->name.s, timeout);
			} else if(pit->name.len==9 && strncmp(pit->name.s, "useragent", 9)==0) {
				useragent = tok;
				LM_DBG("curl [%.*s] - useragent [%.*s]\n", pit->name.len, pit->name.s,
						useragent.len, useragent.s);
			} else if(pit->name.len==8 && strncmp(pit->name.s, "failover", 8)==0) {
				failover = tok;
				LM_DBG("curl [%.*s] - failover [%.*s]\n", pit->name.len, pit->name.s,
						failover.len, failover.s);
			} else if(pit->name.len==11 && strncmp(pit->name.s, "maxdatasize", 11)==0) {
				if(str2int(&tok, &maxdatasize)!=0) {
					/* Bad timeout */
					LM_DBG("curl connection [%.*s]: timeout bad value. Using default\n", name.len, name.s);
					maxdatasize = default_maxdatasize;
				}
				LM_DBG("curl [%.*s] - timeout [%d]\n", pit->name.len, pit->name.s, maxdatasize);
			} else if(pit->name.len==10 && strncmp(pit->name.s, "verifypeer", 10)==0) {
				if(str2int(&tok, &verify_peer)!=0) {
					/* Bad integer */
					LM_DBG("curl connection [%.*s]: verifypeer bad value. Using default\n", name.len, name.s);
					verify_peer = default_tls_verify_peer;
				}
				if (verify_peer != 0 && verify_peer != 1) {
					LM_DBG("curl connection [%.*s]: verifypeer bad value. Using default\n", name.len, name.s);
					verify_peer = default_tls_verify_peer;
				}
				LM_DBG("curl [%.*s] - verifypeer [%d]\n", pit->name.len, pit->name.s, verify_peer);
			} else if(pit->name.len==10 && strncmp(pit->name.s, "verifyhost", 10)==0) {
				if(str2int(&tok, &verify_host)!=0) {
					/* Bad integer */
					LM_DBG("curl connection [%.*s]: verifyhost bad value. Using default\n", name.len, name.s);
					verify_host = default_tls_verify_host;
				}
				LM_DBG("curl [%.*s] - verifyhost [%d]\n", pit->name.len, pit->name.s, verify_host);
			} else if(pit->name.len==10 && strncmp(pit->name.s, "sslversion", 10)==0) {
				if(str2int(&tok, &sslversion)!=0) {
					/* Bad integer */
					LM_DBG("curl connection [%.*s]: sslversion bad value. Using default\n", name.len, name.s);
					sslversion = default_tls_version;
				}
				if (sslversion >= CURL_SSLVERSION_LAST) {
					LM_DBG("curl connection [%.*s]: sslversion bad value. Using default\n", name.len, name.s);
					sslversion = default_tls_version;
				}
				LM_DBG("curl [%.*s] - sslversion [%d]\n", pit->name.len, pit->name.s, sslversion);
			} else if(pit->name.len==10 && strncmp(pit->name.s, "clientcert", 10)==0) {
				client_cert = tok;
				LM_DBG("curl [%.*s] - clientcert [%.*s]\n", pit->name.len, pit->name.s,
						client_cert.len, client_cert.s);
			} else if(pit->name.len==9 && strncmp(pit->name.s, "clientkey", 9)==0) {
				client_key = tok;
				LM_DBG("curl [%.*s] - clientkey [%.*s]\n", pit->name.len, pit->name.s,
						client_key.len, client_key.s);
			} else if(pit->name.len==12 && strncmp(pit->name.s, "ciphersuites", 12)==0) {
				ciphersuites = tok;
				LM_DBG("curl [%.*s] - ciphersuites [%.*s]\n", pit->name.len, pit->name.s,
						ciphersuites.len, ciphersuites.s);
			} else {
				LM_ERR("curl Unknown parameter [%.*s] \n", pit->name.len, pit->name.s);
			}
		}
	}

	/* The URL ends either with nothing or parameters. Parameters start with ; */

	if(conparams != NULL) {
		free_params(conparams);
	}

	cc =  curl_init_con(&name);
	if (cc == NULL) {
		return -1;
	}

	cc->username = username.s ? as_asciiz(&username) : NULL;
	cc->password = password.s ? as_asciiz(&password) : NULL;
	cc->schema = schema;
	cc->failover = failover;
	cc->useragent = as_asciiz(&useragent);
	cc->url = url;
	cc->clientcert = client_cert.s ? as_asciiz(&client_cert) : NULL;
	cc->clientkey = client_key.s ? as_asciiz(&client_key) : NULL;
	cc->ciphersuites = ciphersuites.s ? as_asciiz(&ciphersuites) : NULL;
	cc->sslversion = sslversion;
	cc->verify_peer = verify_peer;
	cc->verify_host = verify_host;
	cc->timeout = timeout;
	cc->maxdatasize = maxdatasize;
	cc->http_follow_redirect = http_follow_redirect;

	LM_DBG("cname: [%.*s] url: [%.*s] username [%s] password [%s] failover [%.*s] timeout [%d] useragent [%s] maxdatasize [%d]\n", 
			name.len, name.s, cc->url.len, cc->url.s, cc->username ? cc->username : "", cc->password ? cc->password : "",
			cc->failover.len, cc->failover.s, cc->timeout, cc->useragent, cc->maxdatasize);
	LM_DBG("cname: [%.*s] client_cert [%s] client_key [%s] ciphersuites [%s] sslversion [%d] verify_peer [%d] verify_host [%d]\n",
			name.len, name.s, cc->clientcert, cc->clientkey, cc->ciphersuites, cc->sslversion, cc->verify_peer, cc->verify_host);

	return 0;

error:
	LM_ERR("invalid curl parameter [%.*s] at [%d]\n", in.len, in.s, (int)(p-in.s));

	if(conparams != NULL) {
		free_params(conparams);
	}
	return -1;
}

/*! Init connection structure and place it in structure
 */
curl_con_t *curl_init_con(str *name)
{
	curl_con_t *cc;
	unsigned int conid;

	conid = core_case_hash(name, 0, 0);
	LM_DBG("curl_init_con curlcon: [%.*s] ID %u\n", name->len, name->s, conid);

	cc = _curl_con_root;
	while(cc)
	{
		if(conid==cc->conid && cc->name.len == name->len
				&& strncmp(cc->name.s, name->s, name->len)==0)
		{
			LM_ERR("duplicate Curl connection name\n");
			return NULL;
		}
		cc = cc->next;
	}

	cc = (curl_con_t*) pkg_malloc(sizeof(curl_con_t));
	if(cc == NULL)
	{
		LM_ERR("no pkg memory\n");
		return NULL;
	}
	memset(cc, 0, sizeof(curl_con_t));
	cc->next = _curl_con_root;
	cc->conid = conid;
	_curl_con_root = cc;
	cc->name = *name;

	LM_INFO("CURL: Added connection [%.*s]\n", name->len, name->s);
	return cc;
}
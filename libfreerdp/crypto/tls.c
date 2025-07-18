/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Transport Layer Security
 *
 * Copyright 2011-2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2023 Armin Novak <anovak@thincast.com>
 * Copyright 2023 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *		 http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include "../core/settings.h"

#include <winpr/assert.h>
#include <string.h>
#include <errno.h>

#include <winpr/crt.h>
#include <winpr/winpr.h>
#include <winpr/string.h>
#include <winpr/sspi.h>
#include <winpr/ssl.h>
#include <winpr/json.h>

#include <winpr/stream.h>
#include <freerdp/utils/ringbuffer.h>

#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/certificate_data.h>
#include <freerdp/utils/helpers.h>

#include <freerdp/log.h>
#include "../crypto/tls.h"
#include "../core/tcp.h"

#include "opensslcompat.h"
#include "certificate.h"
#include "privatekey.h"

#ifdef WINPR_HAVE_POLL_H
#include <poll.h>
#endif

#ifdef FREERDP_HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#define TAG FREERDP_TAG("crypto")

/**
 * Earlier Microsoft iOS RDP clients have sent a null or even double null
 * terminated hostname in the SNI TLS extension.
 * If the length indicator does not equal the hostname strlen OpenSSL
 * will abort (see openssl:ssl/t1_lib.c).
 * Here is a tcpdump segment of Microsoft Remote Desktop Client Version
 * 8.1.7 running on an iPhone 4 with iOS 7.1.2 showing the transmitted
 * SNI hostname TLV blob when connection to server "abcd":
 * 00                  name_type 0x00 (host_name)
 * 00 06               length_in_bytes 0x0006
 * 61 62 63 64 00 00   host_name "abcd\0\0"
 *
 * Currently the only (runtime) workaround is setting an openssl tls
 * extension debug callback that sets the SSL context's servername_done
 * to 1 which effectively disables the parsing of that extension type.
 *
 * Nowadays this workaround is not required anymore but still can be
 * activated by adding the following define:
 *
 * #define MICROSOFT_IOS_SNI_BUG
 */

typedef struct
{
	SSL* ssl;
	CRITICAL_SECTION lock;
} BIO_RDP_TLS;

static int tls_verify_certificate(rdpTls* tls, const rdpCertificate* cert, const char* hostname,
                                  UINT16 port);
static void tls_print_certificate_name_mismatch_error(const char* hostname, UINT16 port,
                                                      const char* common_name, char** alt_names,
                                                      size_t alt_names_count);
static void tls_print_new_certificate_warn(rdpCertificateStore* store, const char* hostname,
                                           UINT16 port, const char* fingerprint);
static void tls_print_certificate_error(rdpCertificateStore* store, rdpCertificateData* stored_data,
                                        const char* hostname, UINT16 port, const char* fingerprint);

static void free_tls_public_key(rdpTls* tls)
{
	WINPR_ASSERT(tls);
	free(tls->PublicKey);
	tls->PublicKey = NULL;
	tls->PublicKeyLength = 0;
}

static void free_tls_bindings(rdpTls* tls)
{
	WINPR_ASSERT(tls);

	if (tls->Bindings)
		free(tls->Bindings->Bindings);

	free(tls->Bindings);
	tls->Bindings = NULL;
}

static int bio_rdp_tls_write(BIO* bio, const char* buf, int size)
{
	int error = 0;
	int status = 0;
	BIO_RDP_TLS* tls = (BIO_RDP_TLS*)BIO_get_data(bio);

	if (!buf || !tls)
		return 0;

	BIO_clear_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_READ | BIO_FLAGS_IO_SPECIAL);
	EnterCriticalSection(&tls->lock);
	status = SSL_write(tls->ssl, buf, size);
	error = SSL_get_error(tls->ssl, status);
	LeaveCriticalSection(&tls->lock);

	if (status <= 0)
	{
		switch (error)
		{
			case SSL_ERROR_NONE:
				BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_WANT_WRITE:
				BIO_set_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_WANT_READ:
				BIO_set_flags(bio, BIO_FLAGS_READ | BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_WANT_X509_LOOKUP:
				BIO_set_flags(bio, BIO_FLAGS_IO_SPECIAL);
				BIO_set_retry_reason(bio, BIO_RR_SSL_X509_LOOKUP);
				break;

			case SSL_ERROR_WANT_CONNECT:
				BIO_set_flags(bio, BIO_FLAGS_IO_SPECIAL);
				BIO_set_retry_reason(bio, BIO_RR_CONNECT);
				break;

			case SSL_ERROR_SYSCALL:
				BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_SSL:
				BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
				break;
			default:
				break;
		}
	}

	return status;
}

static int bio_rdp_tls_read(BIO* bio, char* buf, int size)
{
	int error = 0;
	int status = 0;
	BIO_RDP_TLS* tls = (BIO_RDP_TLS*)BIO_get_data(bio);

	if (!buf || !tls)
		return 0;

	BIO_clear_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_READ | BIO_FLAGS_IO_SPECIAL);
	EnterCriticalSection(&tls->lock);
	status = SSL_read(tls->ssl, buf, size);
	error = SSL_get_error(tls->ssl, status);
	LeaveCriticalSection(&tls->lock);

	if (status <= 0)
	{

		switch (error)
		{
			case SSL_ERROR_NONE:
				BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_WANT_READ:
				BIO_set_flags(bio, BIO_FLAGS_READ | BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_WANT_WRITE:
				BIO_set_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_WANT_X509_LOOKUP:
				BIO_set_flags(bio, BIO_FLAGS_IO_SPECIAL);
				BIO_set_retry_reason(bio, BIO_RR_SSL_X509_LOOKUP);
				break;

			case SSL_ERROR_WANT_ACCEPT:
				BIO_set_flags(bio, BIO_FLAGS_IO_SPECIAL);
				BIO_set_retry_reason(bio, BIO_RR_ACCEPT);
				break;

			case SSL_ERROR_WANT_CONNECT:
				BIO_set_flags(bio, BIO_FLAGS_IO_SPECIAL);
				BIO_set_retry_reason(bio, BIO_RR_CONNECT);
				break;

			case SSL_ERROR_SSL:
				BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_ZERO_RETURN:
				BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
				break;

			case SSL_ERROR_SYSCALL:
				BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
				break;
			default:
				break;
		}
	}

#ifdef FREERDP_HAVE_VALGRIND_MEMCHECK_H

	if (status > 0)
	{
		VALGRIND_MAKE_MEM_DEFINED(buf, status);
	}

#endif
	return status;
}

static int bio_rdp_tls_puts(BIO* bio, const char* str)
{
	if (!str)
		return 0;

	const size_t size = strnlen(str, INT_MAX + 1UL);
	if (size > INT_MAX)
		return -1;
	ERR_clear_error();
	return BIO_write(bio, str, (int)size);
}

static int bio_rdp_tls_gets(WINPR_ATTR_UNUSED BIO* bio, WINPR_ATTR_UNUSED char* str,
                            WINPR_ATTR_UNUSED int size)
{
	return 1;
}

static long bio_rdp_tls_ctrl(BIO* bio, int cmd, long num, void* ptr)
{
	BIO* ssl_rbio = NULL;
	BIO* ssl_wbio = NULL;
	BIO* next_bio = NULL;
	long status = -1;
	BIO_RDP_TLS* tls = (BIO_RDP_TLS*)BIO_get_data(bio);

	if (!tls)
		return 0;

	if (!tls->ssl && (cmd != BIO_C_SET_SSL))
		return 0;

	next_bio = BIO_next(bio);
	ssl_rbio = tls->ssl ? SSL_get_rbio(tls->ssl) : NULL;
	ssl_wbio = tls->ssl ? SSL_get_wbio(tls->ssl) : NULL;

	switch (cmd)
	{
		case BIO_CTRL_RESET:
			SSL_shutdown(tls->ssl);

			if (SSL_in_connect_init(tls->ssl))
				SSL_set_connect_state(tls->ssl);
			else if (SSL_in_accept_init(tls->ssl))
				SSL_set_accept_state(tls->ssl);

			SSL_clear(tls->ssl);

			if (next_bio)
				status = BIO_ctrl(next_bio, cmd, num, ptr);
			else if (ssl_rbio)
				status = BIO_ctrl(ssl_rbio, cmd, num, ptr);
			else
				status = 1;

			break;

		case BIO_C_GET_FD:
			status = BIO_ctrl(ssl_rbio, cmd, num, ptr);
			break;

		case BIO_CTRL_INFO:
			status = 0;
			break;

		case BIO_CTRL_SET_CALLBACK:
			status = 0;
			break;

		case BIO_CTRL_GET_CALLBACK:
			/* The OpenSSL API is horrible here:
			 * we get a function pointer returned and have to cast it to ULONG_PTR
			 * to return the value to the caller.
			 *
			 * This, of course, is something compilers warn about. So silence it by casting */
			{
				void* vptr = WINPR_FUNC_PTR_CAST(SSL_get_info_callback(tls->ssl), void*);
				*((void**)ptr) = vptr;
				status = 1;
			}
			break;

		case BIO_C_SSL_MODE:
			if (num)
				SSL_set_connect_state(tls->ssl);
			else
				SSL_set_accept_state(tls->ssl);

			status = 1;
			break;

		case BIO_CTRL_GET_CLOSE:
			status = BIO_get_shutdown(bio);
			break;

		case BIO_CTRL_SET_CLOSE:
			BIO_set_shutdown(bio, (int)num);
			status = 1;
			break;

		case BIO_CTRL_WPENDING:
			status = BIO_ctrl(ssl_wbio, cmd, num, ptr);
			break;

		case BIO_CTRL_PENDING:
			status = SSL_pending(tls->ssl);

			if (status == 0)
				status = BIO_pending(ssl_rbio);

			break;

		case BIO_CTRL_FLUSH:
			BIO_clear_retry_flags(bio);
			status = BIO_ctrl(ssl_wbio, cmd, num, ptr);
			if (status != 1)
				WLog_DBG(TAG, "BIO_ctrl returned %d", status);
			BIO_copy_next_retry(bio);
			status = 1;
			break;

		case BIO_CTRL_PUSH:
			if (next_bio && (next_bio != ssl_rbio))
			{
#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
				SSL_set_bio(tls->ssl, next_bio, next_bio);
				CRYPTO_add(&(bio->next_bio->references), 1, CRYPTO_LOCK_BIO);
#else
				/*
				 * We are going to pass ownership of next to the SSL object...but
				 * we don't own a reference to pass yet - so up ref
				 */
				BIO_up_ref(next_bio);
				SSL_set_bio(tls->ssl, next_bio, next_bio);
#endif
			}

			status = 1;
			break;

		case BIO_CTRL_POP:

			/* Only detach if we are the BIO explicitly being popped */
			if (bio == ptr)
			{
				if (ssl_rbio != ssl_wbio)
					BIO_free_all(ssl_wbio);

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)

				if (next_bio)
					CRYPTO_add(&(bio->next_bio->references), -1, CRYPTO_LOCK_BIO);

				tls->ssl->wbio = tls->ssl->rbio = NULL;
#else
				/* OpenSSL 1.1: This will also clear the reference we obtained during push */
				SSL_set_bio(tls->ssl, NULL, NULL);
#endif
			}

			status = 1;
			break;

		case BIO_C_GET_SSL:
			if (ptr)
			{
				*((SSL**)ptr) = tls->ssl;
				status = 1;
			}

			break;

		case BIO_C_SET_SSL:
			BIO_set_shutdown(bio, (int)num);

			if (ptr)
			{
				tls->ssl = (SSL*)ptr;
				ssl_rbio = SSL_get_rbio(tls->ssl);
			}

			if (ssl_rbio)
			{
				if (next_bio)
					BIO_push(ssl_rbio, next_bio);

				BIO_set_next(bio, ssl_rbio);
#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    (defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x2070000fL)
				CRYPTO_add(&(ssl_rbio->references), 1, CRYPTO_LOCK_BIO);
#else
				BIO_up_ref(ssl_rbio);
#endif
			}

			BIO_set_init(bio, 1);
			status = 1;
			break;

		case BIO_C_DO_STATE_MACHINE:
			BIO_clear_flags(bio, BIO_FLAGS_READ | BIO_FLAGS_WRITE | BIO_FLAGS_IO_SPECIAL);
			BIO_set_retry_reason(bio, 0);
			status = SSL_do_handshake(tls->ssl);

			if (status <= 0)
			{
				const int err = (status < INT32_MIN) ? INT32_MIN : (int)status;
				switch (SSL_get_error(tls->ssl, err))
				{
					case SSL_ERROR_WANT_READ:
						BIO_set_flags(bio, BIO_FLAGS_READ | BIO_FLAGS_SHOULD_RETRY);
						break;

					case SSL_ERROR_WANT_WRITE:
						BIO_set_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_SHOULD_RETRY);
						break;

					case SSL_ERROR_WANT_CONNECT:
						BIO_set_flags(bio, BIO_FLAGS_IO_SPECIAL | BIO_FLAGS_SHOULD_RETRY);
						BIO_set_retry_reason(bio, BIO_get_retry_reason(next_bio));
						break;

					default:
						BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
						break;
				}
			}

			break;

		default:
			status = BIO_ctrl(ssl_rbio, cmd, num, ptr);
			break;
	}

	return status;
}

static int bio_rdp_tls_new(BIO* bio)
{
	BIO_RDP_TLS* tls = NULL;
	BIO_set_flags(bio, BIO_FLAGS_SHOULD_RETRY);

	if (!(tls = calloc(1, sizeof(BIO_RDP_TLS))))
		return 0;

	InitializeCriticalSectionAndSpinCount(&tls->lock, 4000);
	BIO_set_data(bio, (void*)tls);
	return 1;
}

static int bio_rdp_tls_free(BIO* bio)
{
	BIO_RDP_TLS* tls = NULL;

	if (!bio)
		return 0;

	tls = (BIO_RDP_TLS*)BIO_get_data(bio);

	if (!tls)
		return 0;

	BIO_set_data(bio, NULL);
	if (BIO_get_shutdown(bio))
	{
		if (BIO_get_init(bio) && tls->ssl)
		{
			SSL_shutdown(tls->ssl);
			SSL_free(tls->ssl);
		}

		BIO_set_init(bio, 0);
		BIO_set_flags(bio, 0);
	}

	DeleteCriticalSection(&tls->lock);
	free(tls);

	return 1;
}

static long bio_rdp_tls_callback_ctrl(BIO* bio, int cmd, bio_info_cb* fp)
{
	long status = 0;

	if (!bio)
		return 0;

	BIO_RDP_TLS* tls = (BIO_RDP_TLS*)BIO_get_data(bio);

	if (!tls)
		return 0;

	switch (cmd)
	{
		case BIO_CTRL_SET_CALLBACK:
		{
			typedef void (*fkt_t)(const SSL*, int, int);

			/* Documented since https://www.openssl.org/docs/man1.1.1/man3/BIO_set_callback.html
			 * the argument is not really of type bio_info_cb* and must be cast
			 * to the required type */

			fkt_t fkt = WINPR_FUNC_PTR_CAST(fp, fkt_t);
			SSL_set_info_callback(tls->ssl, fkt);
			status = 1;
		}
		break;

		default:
			status = BIO_callback_ctrl(SSL_get_rbio(tls->ssl), cmd, fp);
			break;
	}

	return status;
}

#define BIO_TYPE_RDP_TLS 68

static BIO_METHOD* BIO_s_rdp_tls(void)
{
	static BIO_METHOD* bio_methods = NULL;

	if (bio_methods == NULL)
	{
		if (!(bio_methods = BIO_meth_new(BIO_TYPE_RDP_TLS, "RdpTls")))
			return NULL;

		BIO_meth_set_write(bio_methods, bio_rdp_tls_write);
		BIO_meth_set_read(bio_methods, bio_rdp_tls_read);
		BIO_meth_set_puts(bio_methods, bio_rdp_tls_puts);
		BIO_meth_set_gets(bio_methods, bio_rdp_tls_gets);
		BIO_meth_set_ctrl(bio_methods, bio_rdp_tls_ctrl);
		BIO_meth_set_create(bio_methods, bio_rdp_tls_new);
		BIO_meth_set_destroy(bio_methods, bio_rdp_tls_free);
		BIO_meth_set_callback_ctrl(bio_methods, bio_rdp_tls_callback_ctrl);
	}

	return bio_methods;
}

static BIO* BIO_new_rdp_tls(SSL_CTX* ctx, int client)
{
	BIO* bio = NULL;
	SSL* ssl = NULL;
	bio = BIO_new(BIO_s_rdp_tls());

	if (!bio)
		return NULL;

	ssl = SSL_new(ctx);

	if (!ssl)
	{
		BIO_free_all(bio);
		return NULL;
	}

	if (client)
		SSL_set_connect_state(ssl);
	else
		SSL_set_accept_state(ssl);

	BIO_set_ssl(bio, ssl, BIO_CLOSE);
	return bio;
}

static rdpCertificate* tls_get_certificate(rdpTls* tls, BOOL peer)
{
	X509* remote_cert = NULL;

	if (peer)
		remote_cert = SSL_get_peer_certificate(tls->ssl);
	else
		remote_cert = X509_dup(SSL_get_certificate(tls->ssl));

	if (!remote_cert)
	{
		WLog_ERR(TAG, "failed to get the server TLS certificate");
		return NULL;
	}

	/* Get the peer's chain. If it does not exist, we're setting NULL (clean data either way) */
	STACK_OF(X509)* chain = SSL_get_peer_cert_chain(tls->ssl);
	rdpCertificate* cert = freerdp_certificate_new_from_x509(remote_cert, chain);
	X509_free(remote_cert);

	return cert;
}

static const char* tls_get_server_name(rdpTls* tls)
{
	return tls->serverName ? tls->serverName : tls->hostname;
}

#define TLS_SERVER_END_POINT "tls-server-end-point:"

static SecPkgContext_Bindings* tls_get_channel_bindings(const rdpCertificate* cert)
{
	size_t CertificateHashLength = 0;
	BYTE* ChannelBindingToken = NULL;
	SEC_CHANNEL_BINDINGS* ChannelBindings = NULL;
	const size_t PrefixLength = strnlen(TLS_SERVER_END_POINT, ARRAYSIZE(TLS_SERVER_END_POINT));

	WINPR_ASSERT(cert);

	/* See https://www.rfc-editor.org/rfc/rfc5929 for details about hashes */
	WINPR_MD_TYPE alg = freerdp_certificate_get_signature_alg(cert);
	const char* hash = NULL;
	switch (alg)
	{

		case WINPR_MD_MD5:
		case WINPR_MD_SHA1:
			hash = winpr_md_type_to_string(WINPR_MD_SHA256);
			break;
		default:
			hash = winpr_md_type_to_string(alg);
			break;
	}
	if (!hash)
		return NULL;

	char* CertificateHash = freerdp_certificate_get_hash(cert, hash, &CertificateHashLength);
	if (!CertificateHash)
		return NULL;

	const size_t ChannelBindingTokenLength = PrefixLength + CertificateHashLength;
	SecPkgContext_Bindings* ContextBindings = calloc(1, sizeof(SecPkgContext_Bindings));

	if (!ContextBindings)
		goto out_free;

	const size_t slen = sizeof(SEC_CHANNEL_BINDINGS) + ChannelBindingTokenLength;
	if (slen > UINT32_MAX)
		goto out_free;

	ContextBindings->BindingsLength = (UINT32)slen;
	ChannelBindings = (SEC_CHANNEL_BINDINGS*)calloc(1, ContextBindings->BindingsLength);

	if (!ChannelBindings)
		goto out_free;

	ContextBindings->Bindings = ChannelBindings;
	ChannelBindings->cbApplicationDataLength = (UINT32)ChannelBindingTokenLength;
	ChannelBindings->dwApplicationDataOffset = sizeof(SEC_CHANNEL_BINDINGS);
	ChannelBindingToken = &((BYTE*)ChannelBindings)[ChannelBindings->dwApplicationDataOffset];
	memcpy(ChannelBindingToken, TLS_SERVER_END_POINT, PrefixLength);
	memcpy(ChannelBindingToken + PrefixLength, CertificateHash, CertificateHashLength);
	free(CertificateHash);
	return ContextBindings;
out_free:
	free(CertificateHash);
	free(ContextBindings);
	return NULL;
}

static INIT_ONCE secrets_file_idx_once = INIT_ONCE_STATIC_INIT;
static int secrets_file_idx = -1;

static BOOL CALLBACK secrets_file_init_cb(WINPR_ATTR_UNUSED PINIT_ONCE once,
                                          WINPR_ATTR_UNUSED PVOID param,
                                          WINPR_ATTR_UNUSED PVOID* context)
{
	secrets_file_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);

	return (secrets_file_idx != -1);
}

static void SSLCTX_keylog_cb(const SSL* ssl, const char* line)
{
	char* dfile = NULL;

	if (secrets_file_idx == -1)
		return;

	dfile = SSL_get_ex_data(ssl, secrets_file_idx);
	if (dfile)
	{
		FILE* f = winpr_fopen(dfile, "a+");
		if (f)
		{
			(void)fwrite(line, strlen(line), 1, f);
			(void)fwrite("\n", 1, 1, f);
			(void)fclose(f);
		}
	}
}

static void tls_reset(rdpTls* tls)
{
	WINPR_ASSERT(tls);

	if (tls->ctx)
	{
		SSL_CTX_free(tls->ctx);
		tls->ctx = NULL;
	}

	/* tls->underlying is a stacked BIO under tls->bio.
	 * BIO_free_all will free recursively. */
	if (tls->bio)
		BIO_free_all(tls->bio);
	else if (tls->underlying)
		BIO_free_all(tls->underlying);
	tls->bio = NULL;
	tls->underlying = NULL;

	free_tls_public_key(tls);
	free_tls_bindings(tls);
}

#if OPENSSL_VERSION_NUMBER >= 0x010000000L
static BOOL tls_prepare(rdpTls* tls, BIO* underlying, const SSL_METHOD* method, int options,
                        BOOL clientMode)
#else
static BOOL tls_prepare(rdpTls* tls, BIO* underlying, SSL_METHOD* method, int options,
                        BOOL clientMode)
#endif
{
	WINPR_ASSERT(tls);

	rdpSettings* settings = tls->context->settings;
	WINPR_ASSERT(settings);

	tls_reset(tls);
	tls->ctx = SSL_CTX_new(method);

	tls->underlying = underlying;

	if (!tls->ctx)
	{
		WLog_ERR(TAG, "SSL_CTX_new failed");
		return FALSE;
	}

	SSL_CTX_set_mode(tls->ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_ENABLE_PARTIAL_WRITE);
	SSL_CTX_set_options(tls->ctx, WINPR_ASSERTING_INT_CAST(uint64_t, options));
	SSL_CTX_set_read_ahead(tls->ctx, 1);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	UINT16 version = freerdp_settings_get_uint16(settings, FreeRDP_TLSMinVersion);
	if (!SSL_CTX_set_min_proto_version(tls->ctx, version))
	{
		WLog_ERR(TAG, "SSL_CTX_set_min_proto_version %s failed", version);
		return FALSE;
	}
	version = freerdp_settings_get_uint16(settings, FreeRDP_TLSMaxVersion);
	if (!SSL_CTX_set_max_proto_version(tls->ctx, version))
	{
		WLog_ERR(TAG, "SSL_CTX_set_max_proto_version %s failed", version);
		return FALSE;
	}
#endif
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
	SSL_CTX_set_security_level(tls->ctx, WINPR_ASSERTING_INT_CAST(int, settings->TlsSecLevel));
#endif

	if (settings->AllowedTlsCiphers)
	{
		if (!SSL_CTX_set_cipher_list(tls->ctx, settings->AllowedTlsCiphers))
		{
			WLog_ERR(TAG, "SSL_CTX_set_cipher_list %s failed", settings->AllowedTlsCiphers);
			return FALSE;
		}
	}

	tls->bio = BIO_new_rdp_tls(tls->ctx, clientMode);

	if (BIO_get_ssl(tls->bio, &tls->ssl) < 0)
	{
		WLog_ERR(TAG, "unable to retrieve the SSL of the connection");
		return FALSE;
	}

	if (settings->TlsSecretsFile)
	{
#if OPENSSL_VERSION_NUMBER >= 0x10101000L
		InitOnceExecuteOnce(&secrets_file_idx_once, secrets_file_init_cb, NULL, NULL);

		if (secrets_file_idx != -1)
		{
			SSL_set_ex_data(tls->ssl, secrets_file_idx, settings->TlsSecretsFile);
			SSL_CTX_set_keylog_callback(tls->ctx, SSLCTX_keylog_cb);
		}
#else
		WLog_WARN(TAG, "Key-Logging not available - requires OpenSSL 1.1.1 or higher");
#endif
	}

	BIO_push(tls->bio, underlying);
	return TRUE;
}

static void
adjustSslOptions(WINPR_ATTR_UNUSED int* options) // NOLINT(readability-non-const-parameter)
{
	WINPR_ASSERT(options);
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	*options |= SSL_OP_NO_SSLv2;
	*options |= SSL_OP_NO_SSLv3;
#endif
}

const SSL_METHOD* freerdp_tls_get_ssl_method(BOOL isDtls, BOOL isClient)
{
	if (isClient)
	{
		if (isDtls)
			return DTLS_client_method();
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
		return SSLv23_client_method();
#else
		return TLS_client_method();
#endif
	}

	if (isDtls)
		return DTLS_server_method();

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	return SSLv23_server_method();
#else
	return TLS_server_method();
#endif
}

TlsHandshakeResult freerdp_tls_connect_ex(rdpTls* tls, BIO* underlying, const SSL_METHOD* methods)
{
	WINPR_ASSERT(tls);

	int options = 0;
	/**
	 * SSL_OP_NO_COMPRESSION:
	 *
	 * The Microsoft RDP server does not advertise support
	 * for TLS compression, but alternative servers may support it.
	 * This was observed between early versions of the FreeRDP server
	 * and the FreeRDP client, and caused major performance issues,
	 * which is why we're disabling it.
	 */
#ifdef SSL_OP_NO_COMPRESSION
	options |= SSL_OP_NO_COMPRESSION;
#endif
	/**
	 * SSL_OP_TLS_BLOCK_PADDING_BUG:
	 *
	 * The Microsoft RDP server does *not* support TLS padding.
	 * It absolutely needs to be disabled otherwise it won't work.
	 */
	options |= SSL_OP_TLS_BLOCK_PADDING_BUG;
	/**
	 * SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS:
	 *
	 * Just like TLS padding, the Microsoft RDP server does not
	 * support empty fragments. This needs to be disabled.
	 */
	options |= SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;

	tls->isClientMode = TRUE;
	adjustSslOptions(&options);

	if (!tls_prepare(tls, underlying, methods, options, TRUE))
		return 0;

#if !defined(OPENSSL_NO_TLSEXT)
	const char* str = tls_get_server_name(tls);
	void* ptr = WINPR_CAST_CONST_PTR_AWAY(str, void*);
	SSL_set_tlsext_host_name(tls->ssl, ptr);
#endif

	return freerdp_tls_handshake(tls);
}

static int bio_err_print(const char* str, size_t len, void* u)
{
	wLog* log = u;
	WLog_Print(log, WLOG_ERROR, "[BIO_do_handshake] %s [%" PRIuz "]", str, len);
	return 0;
}

TlsHandshakeResult freerdp_tls_handshake(rdpTls* tls)
{
	TlsHandshakeResult ret = TLS_HANDSHAKE_ERROR;

	WINPR_ASSERT(tls);
	const long status = BIO_do_handshake(tls->bio);
	if (status != 1)
	{
		if (!BIO_should_retry(tls->bio))
		{
			wLog* log = WLog_Get(TAG);
			WLog_Print(log, WLOG_ERROR, "BIO_do_handshake failed");
			ERR_print_errors_cb(bio_err_print, log);
			return TLS_HANDSHAKE_ERROR;
		}

		return TLS_HANDSHAKE_CONTINUE;
	}

	int verify_status = 0;
	rdpCertificate* cert = tls_get_certificate(tls, tls->isClientMode);

	if (!cert)
	{
		WLog_ERR(TAG, "tls_get_certificate failed to return the server certificate.");
		return TLS_HANDSHAKE_ERROR;
	}

	do
	{
		free_tls_bindings(tls);
		tls->Bindings = tls_get_channel_bindings(cert);
		if (!tls->Bindings)
		{
			WLog_ERR(TAG, "unable to retrieve bindings");
			break;
		}

		free_tls_public_key(tls);
		if (!freerdp_certificate_get_public_key(cert, &tls->PublicKey, &tls->PublicKeyLength))
		{
			WLog_ERR(TAG,
			         "freerdp_certificate_get_public_key failed to return the server public key.");
			break;
		}

		/* server-side NLA needs public keys (keys from us, the server) but no certificate verify */
		ret = TLS_HANDSHAKE_SUCCESS;

		if (tls->isClientMode)
		{
			WINPR_ASSERT(tls->port <= UINT16_MAX);
			verify_status =
			    tls_verify_certificate(tls, cert, tls_get_server_name(tls), (UINT16)tls->port);

			if (verify_status < 1)
			{
				WLog_ERR(TAG, "certificate not trusted, aborting.");
				freerdp_tls_send_alert(tls);
				ret = TLS_HANDSHAKE_VERIFY_ERROR;
			}
		}
	} while (0);

	freerdp_certificate_free(cert);
	return ret;
}

static int pollAndHandshake(rdpTls* tls)
{
	WINPR_ASSERT(tls);

	do
	{
		HANDLE events[] = { freerdp_abort_event(tls->context), NULL };
		DWORD status = 0;
		if (BIO_get_event(tls->bio, &events[1]) < 0)
		{
			WLog_ERR(TAG, "unable to retrieve BIO associated event");
			return -1;
		}

		if (!events[1])
		{
			WLog_ERR(TAG, "unable to retrieve BIO event");
			return -1;
		}

		status = WaitForMultipleObjectsEx(ARRAYSIZE(events), events, FALSE, INFINITE, TRUE);
		switch (status)
		{
			case WAIT_OBJECT_0 + 1:
				break;
			case WAIT_OBJECT_0:
				WLog_DBG(TAG, "Abort event set, cancel connect");
				return -1;
			case WAIT_TIMEOUT:
			case WAIT_IO_COMPLETION:
				continue;
			default:
				WLog_ERR(TAG, "error during WaitForSingleObject(): 0x%08" PRIX32 "", status);
				return -1;
		}

		TlsHandshakeResult result = freerdp_tls_handshake(tls);
		switch (result)
		{
			case TLS_HANDSHAKE_CONTINUE:
				break;
			case TLS_HANDSHAKE_SUCCESS:
				return 1;
			case TLS_HANDSHAKE_ERROR:
			case TLS_HANDSHAKE_VERIFY_ERROR:
			default:
				return -1;
		}
	} while (TRUE);
}

int freerdp_tls_connect(rdpTls* tls, BIO* underlying)
{
	const SSL_METHOD* method = freerdp_tls_get_ssl_method(FALSE, TRUE);

	WINPR_ASSERT(tls);
	TlsHandshakeResult result = freerdp_tls_connect_ex(tls, underlying, method);
	switch (result)
	{
		case TLS_HANDSHAKE_SUCCESS:
			return 1;
		case TLS_HANDSHAKE_CONTINUE:
			break;
		case TLS_HANDSHAKE_ERROR:
		case TLS_HANDSHAKE_VERIFY_ERROR:
			return -1;
		default:
			return -1;
	}

	return pollAndHandshake(tls);
}

#if defined(MICROSOFT_IOS_SNI_BUG) && !defined(OPENSSL_NO_TLSEXT) && \
    !defined(LIBRESSL_VERSION_NUMBER)
static void tls_openssl_tlsext_debug_callback(SSL* s, int client_server, int type,
                                              unsigned char* data, int len, void* arg)
{
	if (type == TLSEXT_TYPE_server_name)
	{
		WLog_DBG(TAG, "Client uses SNI (extension disabled)");
		s->servername_done = 2;
	}
}
#endif

BOOL freerdp_tls_accept(rdpTls* tls, BIO* underlying, rdpSettings* settings)
{
	WINPR_ASSERT(tls);
	TlsHandshakeResult res =
	    freerdp_tls_accept_ex(tls, underlying, settings, freerdp_tls_get_ssl_method(FALSE, FALSE));
	switch (res)
	{
		case TLS_HANDSHAKE_SUCCESS:
			return TRUE;
		case TLS_HANDSHAKE_CONTINUE:
			break;
		case TLS_HANDSHAKE_ERROR:
		case TLS_HANDSHAKE_VERIFY_ERROR:
		default:
			return FALSE;
	}

	return pollAndHandshake(tls) > 0;
}

TlsHandshakeResult freerdp_tls_accept_ex(rdpTls* tls, BIO* underlying, rdpSettings* settings,
                                         const SSL_METHOD* methods)
{
	WINPR_ASSERT(tls);

	int options = 0;
	int status = 0;

	/**
	 * SSL_OP_NO_SSLv2:
	 *
	 * We only want SSLv3 and TLSv1, so disable SSLv2.
	 * SSLv3 is used by, eg. Microsoft RDC for Mac OS X.
	 */
	options |= SSL_OP_NO_SSLv2;
	/**
	 * SSL_OP_NO_COMPRESSION:
	 *
	 * The Microsoft RDP server does not advertise support
	 * for TLS compression, but alternative servers may support it.
	 * This was observed between early versions of the FreeRDP server
	 * and the FreeRDP client, and caused major performance issues,
	 * which is why we're disabling it.
	 */
#ifdef SSL_OP_NO_COMPRESSION
	options |= SSL_OP_NO_COMPRESSION;
#endif
	/**
	 * SSL_OP_TLS_BLOCK_PADDING_BUG:
	 *
	 * The Microsoft RDP server does *not* support TLS padding.
	 * It absolutely needs to be disabled otherwise it won't work.
	 */
	options |= SSL_OP_TLS_BLOCK_PADDING_BUG;
	/**
	 * SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS:
	 *
	 * Just like TLS padding, the Microsoft RDP server does not
	 * support empty fragments. This needs to be disabled.
	 */
	options |= SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;

	/**
	 * SSL_OP_NO_RENEGOTIATION
	 *
	 * Disable SSL client site renegotiation.
	 */

#if (OPENSSL_VERSION_NUMBER >= 0x10101000L) && (OPENSSL_VERSION_NUMBER < 0x30000000L) && \
    !defined(LIBRESSL_VERSION_NUMBER)
	options |= SSL_OP_NO_RENEGOTIATION;
#endif

	if (!tls_prepare(tls, underlying, methods, options, FALSE))
		return TLS_HANDSHAKE_ERROR;

	const rdpPrivateKey* key = freerdp_settings_get_pointer(settings, FreeRDP_RdpServerRsaKey);
	if (!key)
	{
		WLog_ERR(TAG, "invalid private key");
		return TLS_HANDSHAKE_ERROR;
	}

	EVP_PKEY* privkey = freerdp_key_get_evp_pkey(key);
	if (!privkey)
	{
		WLog_ERR(TAG, "invalid private key");
		return TLS_HANDSHAKE_ERROR;
	}

	status = SSL_use_PrivateKey(tls->ssl, privkey);
	/* The local reference to the private key will anyway go out of
	 * scope; so the reference count should be decremented weither
	 * SSL_use_PrivateKey succeeds or fails.
	 */
	EVP_PKEY_free(privkey);

	if (status <= 0)
	{
		WLog_ERR(TAG, "SSL_CTX_use_PrivateKey_file failed");
		return TLS_HANDSHAKE_ERROR;
	}

	rdpCertificate* cert =
	    freerdp_settings_get_pointer_writable(settings, FreeRDP_RdpServerCertificate);
	if (!cert)
	{
		WLog_ERR(TAG, "invalid certificate");
		return TLS_HANDSHAKE_ERROR;
	}

	status = SSL_use_certificate(tls->ssl, freerdp_certificate_get_x509(cert));

	if (status <= 0)
	{
		WLog_ERR(TAG, "SSL_use_certificate_file failed");
		return TLS_HANDSHAKE_ERROR;
	}

#if defined(MICROSOFT_IOS_SNI_BUG) && !defined(OPENSSL_NO_TLSEXT) && \
    !defined(LIBRESSL_VERSION_NUMBER)
	SSL_set_tlsext_debug_callback(tls->ssl, tls_openssl_tlsext_debug_callback);
#endif

	return freerdp_tls_handshake(tls);
}

BOOL freerdp_tls_send_alert(rdpTls* tls)
{
	WINPR_ASSERT(tls);

	if (!tls)
		return FALSE;

	if (!tls->ssl)
		return TRUE;

	/**
	 * FIXME: The following code does not work on OpenSSL > 1.1.0 because the
	 *        SSL struct is opaqe now
	 */
#if (!defined(LIBRESSL_VERSION_NUMBER) && (OPENSSL_VERSION_NUMBER < 0x10100000L)) || \
    (defined(LIBRESSL_VERSION_NUMBER) && (LIBRESSL_VERSION_NUMBER <= 0x2080300fL))

	if (tls->alertDescription != TLS_ALERT_DESCRIPTION_CLOSE_NOTIFY)
	{
		/**
		 * OpenSSL doesn't really expose an API for sending a TLS alert manually.
		 *
		 * The following code disables the sending of the default "close notify"
		 * and then proceeds to force sending a custom TLS alert before shutting down.
		 *
		 * Manually sending a TLS alert is necessary in certain cases,
		 * like when server-side NLA results in an authentication failure.
		 */
		SSL_SESSION* ssl_session = SSL_get_session(tls->ssl);
		SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(tls->ssl);
		SSL_set_quiet_shutdown(tls->ssl, 1);

		if ((tls->alertLevel == TLS_ALERT_LEVEL_FATAL) && (ssl_session))
			SSL_CTX_remove_session(ssl_ctx, ssl_session);

		tls->ssl->s3->alert_dispatch = 1;
		tls->ssl->s3->send_alert[0] = tls->alertLevel;
		tls->ssl->s3->send_alert[1] = tls->alertDescription;

		if (tls->ssl->s3->wbuf.left == 0)
			tls->ssl->method->ssl_dispatch_alert(tls->ssl);
	}

#endif
	return TRUE;
}

int freerdp_tls_write_all(rdpTls* tls, const BYTE* data, size_t length)
{
	WINPR_ASSERT(tls);
	size_t offset = 0;
	BIO* bio = tls->bio;

	if (length > INT32_MAX)
		return -1;

	while (offset < length)
	{
		ERR_clear_error();
		const int status = BIO_write(bio, &data[offset], (int)(length - offset));

		if (status > 0)
			offset += (size_t)status;
		else
		{
			if (!BIO_should_retry(bio))
				return -1;

			if (BIO_write_blocked(bio))
			{
				const long rc = BIO_wait_write(bio, 100);
				if (rc < 0)
					return -1;
			}
			else if (BIO_read_blocked(bio))
				return -2; /* Abort write, there is data that must be read */
			else
				USleep(100);
		}
	}

	return (int)length;
}

int freerdp_tls_set_alert_code(rdpTls* tls, int level, int description)
{
	WINPR_ASSERT(tls);
	tls->alertLevel = level;
	tls->alertDescription = description;
	return 0;
}

static BOOL tls_match_hostname(const char* pattern, const size_t pattern_length,
                               const char* hostname)
{
	if (strlen(hostname) == pattern_length)
	{
		if (_strnicmp(hostname, pattern, pattern_length) == 0)
			return TRUE;
	}

	if ((pattern_length > 2) && (pattern[0] == '*') && (pattern[1] == '.') &&
	    ((strlen(hostname)) >= pattern_length))
	{
		const char* check_hostname = &hostname[strlen(hostname) - pattern_length + 1];

		if (_strnicmp(check_hostname, &pattern[1], pattern_length - 1) == 0)
		{
			return TRUE;
		}
	}

	return FALSE;
}

static BOOL is_redirected(rdpTls* tls)
{
	rdpSettings* settings = tls->context->settings;

	if (LB_NOREDIRECT & settings->RedirectionFlags)
		return FALSE;

	return settings->RedirectionFlags != 0;
}

static BOOL is_accepted(rdpTls* tls, const rdpCertificate* cert)
{
	WINPR_ASSERT(tls);
	WINPR_ASSERT(tls->context);
	WINPR_ASSERT(cert);
	rdpSettings* settings = tls->context->settings;
	WINPR_ASSERT(settings);

	FreeRDP_Settings_Keys_String keyAccepted = FreeRDP_AcceptedCert;
	FreeRDP_Settings_Keys_UInt32 keyLength = FreeRDP_AcceptedCertLength;

	if (tls->isGatewayTransport)
	{
		keyAccepted = FreeRDP_GatewayAcceptedCert;
		keyLength = FreeRDP_GatewayAcceptedCertLength;
	}
	else if (is_redirected(tls))
	{
		keyAccepted = FreeRDP_RedirectionAcceptedCert;
		keyLength = FreeRDP_RedirectionAcceptedCertLength;
	}

	const char* AcceptedKey = freerdp_settings_get_string(settings, keyAccepted);
	const UINT32 AcceptedKeyLength = freerdp_settings_get_uint32(settings, keyLength);

	if ((AcceptedKeyLength > 0) && AcceptedKey)
	{
		BOOL accepted = FALSE;
		size_t pemLength = 0;
		char* pem = freerdp_certificate_get_pem_ex(cert, &pemLength, FALSE);
		if (pem && (AcceptedKeyLength == pemLength))
		{
			if (memcmp(AcceptedKey, pem, AcceptedKeyLength) == 0)
				accepted = TRUE;
		}
		free(pem);
		if (accepted)
			return TRUE;
	}

	(void)freerdp_settings_set_string(settings, keyAccepted, NULL);
	(void)freerdp_settings_set_uint32(settings, keyLength, 0);

	return FALSE;
}

static BOOL compare_fingerprint(const char* fp, const char* hash, const rdpCertificate* cert,
                                BOOL separator)
{
	BOOL equal = 0;
	char* strhash = NULL;

	WINPR_ASSERT(fp);
	WINPR_ASSERT(hash);
	WINPR_ASSERT(cert);

	strhash = freerdp_certificate_get_fingerprint_by_hash_ex(cert, hash, separator);
	if (!strhash)
		return FALSE;

	equal = (_stricmp(strhash, fp) == 0);
	free(strhash);
	return equal;
}

static BOOL compare_fingerprint_all(const char* fp, const char* hash, const rdpCertificate* cert)
{
	WINPR_ASSERT(fp);
	WINPR_ASSERT(hash);
	WINPR_ASSERT(cert);
	if (compare_fingerprint(fp, hash, cert, FALSE))
		return TRUE;
	if (compare_fingerprint(fp, hash, cert, TRUE))
		return TRUE;
	return FALSE;
}

static BOOL is_accepted_fingerprint(const rdpCertificate* cert,
                                    const char* CertificateAcceptedFingerprints)
{
	WINPR_ASSERT(cert);

	BOOL rc = FALSE;
	if (CertificateAcceptedFingerprints)
	{
		char* context = NULL;
		char* copy = _strdup(CertificateAcceptedFingerprints);
		char* cur = strtok_s(copy, ",", &context);
		while (cur)
		{
			char* subcontext = NULL;
			const char* h = strtok_s(cur, ":", &subcontext);

			if (!h)
				goto next;

			const char* fp = h + strlen(h) + 1;
			if (compare_fingerprint_all(fp, h, cert))
			{
				rc = TRUE;
				break;
			}
		next:
			cur = strtok_s(NULL, ",", &context);
		}
		free(copy);
	}

	return rc;
}

static BOOL accept_cert(rdpTls* tls, const rdpCertificate* cert)
{
	WINPR_ASSERT(tls);
	WINPR_ASSERT(tls->context);
	WINPR_ASSERT(cert);

	FreeRDP_Settings_Keys_String id = FreeRDP_AcceptedCert;
	FreeRDP_Settings_Keys_UInt32 lid = FreeRDP_AcceptedCertLength;

	rdpSettings* settings = tls->context->settings;
	WINPR_ASSERT(settings);

	if (tls->isGatewayTransport)
	{
		id = FreeRDP_GatewayAcceptedCert;
		lid = FreeRDP_GatewayAcceptedCertLength;
	}
	else if (is_redirected(tls))
	{
		id = FreeRDP_RedirectionAcceptedCert;
		lid = FreeRDP_RedirectionAcceptedCertLength;
	}

	size_t pemLength = 0;
	char* pem = freerdp_certificate_get_pem_ex(cert, &pemLength, FALSE);
	BOOL rc = FALSE;
	if (pemLength <= UINT32_MAX)
	{
		if (freerdp_settings_set_string_len(settings, id, pem, pemLength))
			rc = freerdp_settings_set_uint32(settings, lid, (UINT32)pemLength);
	}
	free(pem);
	return rc;
}

static BOOL tls_extract_full_pem(const rdpCertificate* cert, BYTE** PublicKey,
                                 size_t* PublicKeyLength)
{
	if (!cert || !PublicKey)
		return FALSE;
	*PublicKey = (BYTE*)freerdp_certificate_get_pem(cert, PublicKeyLength);
	return *PublicKey != NULL;
}

static int tls_config_parse_bool(WINPR_JSON* json, const char* opt)
{
	WINPR_JSON* val = WINPR_JSON_GetObjectItem(json, opt);
	if (!val || !WINPR_JSON_IsBool(val))
		return -1;

	if (WINPR_JSON_IsTrue(val))
		return 1;
	return 0;
}

static int tls_config_check_allowed_hashed(const char* configfile, const rdpCertificate* cert,
                                           WINPR_JSON* json)
{
	WINPR_ASSERT(configfile);
	WINPR_ASSERT(cert);
	WINPR_ASSERT(json);

	WINPR_JSON* db = WINPR_JSON_GetObjectItem(json, "certificate-db");
	if (!db || !WINPR_JSON_IsArray(db))
		return 0;

	for (size_t x = 0; x < WINPR_JSON_GetArraySize(db); x++)
	{
		WINPR_JSON* cur = WINPR_JSON_GetArrayItem(db, x);
		if (!cur || !WINPR_JSON_IsObject(cur))
		{
			WLog_WARN(TAG,
			          "[%s] invalid certificate-db entry at position %" PRIuz ": not a JSON object",
			          configfile, x);
			continue;
		}

		WINPR_JSON* key = WINPR_JSON_GetObjectItem(cur, "type");
		if (!key || !WINPR_JSON_IsString(key))
		{
			WLog_WARN(TAG,
			          "[%s] invalid certificate-db entry at position %" PRIuz
			          ": invalid 'type' element, expected type string",
			          configfile, x);
			continue;
		}
		WINPR_JSON* val = WINPR_JSON_GetObjectItem(cur, "hash");
		if (!val || !WINPR_JSON_IsString(val))
		{
			WLog_WARN(TAG,
			          "[%s] invalid certificate-db entry at position %" PRIuz
			          ": invalid 'hash' element, expected type string",
			          configfile, x);
			continue;
		}

		const char* skey = WINPR_JSON_GetStringValue(key);
		const char* sval = WINPR_JSON_GetStringValue(val);

		char* hash = freerdp_certificate_get_fingerprint_by_hash_ex(cert, skey, FALSE);
		if (!hash)
		{
			WLog_WARN(TAG,
			          "[%s] invalid certificate-db entry at position %" PRIuz
			          ": hash type '%s' not supported by certificate",
			          configfile, x, skey);
			continue;
		}

		const int cmp = _stricmp(hash, sval);
		free(hash);

		if (cmp == 0)
			return 1;
	}

	return 0;
}

static int tls_config_check_certificate(const rdpCertificate* cert, BOOL* pAllowUserconfig)
{
	WINPR_ASSERT(cert);
	WINPR_ASSERT(pAllowUserconfig);

	int rc = 0;
	const char configfile[] = "certificates.json";
	WINPR_JSON* json = freerdp_GetJSONConfigFile(TRUE, configfile);

	if (!json)
	{
		WLog_DBG(TAG, "No or no valid configuration file for certificate handling, asking user");
		goto fail;
	}

	if (tls_config_parse_bool(json, "deny") > 0)
	{
		WLog_WARN(TAG, "[%s] certificate denied by configuration", configfile);
		rc = -1;
		goto fail;
	}

	if (tls_config_parse_bool(json, "ignore") > 0)
	{
		WLog_WARN(TAG, "[%s] certificate ignored by configuration", configfile);
		rc = 1;
		goto fail;
	}

	if (tls_config_check_allowed_hashed(configfile, cert, json) > 0)
	{
		WLog_WARN(TAG, "[%s] certificate manually accepted by configuration", configfile);
		rc = 1;
		goto fail;
	}

	if (tls_config_parse_bool(json, "deny-userconfig") > 0)
	{
		WLog_WARN(TAG, "[%s] configuration denies user to accept certificates", configfile);
		rc = -1;
		goto fail;
	}

fail:

	*pAllowUserconfig = (rc == 0);
	WINPR_JSON_Delete(json);
	return rc;
}

int tls_verify_certificate(rdpTls* tls, const rdpCertificate* cert, const char* hostname,
                           UINT16 port)
{
	int match = 0;
	size_t length = 0;
	BOOL certificate_status = 0;
	char* common_name = NULL;
	size_t common_name_length = 0;
	char** dns_names = 0;
	size_t dns_names_count = 0;
	size_t* dns_names_lengths = NULL;
	int verification_status = -1;
	BOOL hostname_match = FALSE;
	rdpCertificateData* certificate_data = NULL;
	BYTE* pemCert = NULL;
	DWORD flags = VERIFY_CERT_FLAG_NONE;

	WINPR_ASSERT(tls);
	WINPR_ASSERT(tls->context);

	freerdp* instance = tls->context->instance;
	WINPR_ASSERT(instance);

	if (freerdp_shall_disconnect_context(instance->context))
		return -1;

	if (!tls_extract_full_pem(cert, &pemCert, &length))
		goto end;

	/* Check, if we already accepted this key. */
	if (is_accepted(tls, cert))
	{
		verification_status = 1;
		goto end;
	}

	if (is_accepted_fingerprint(cert, tls->context->settings->CertificateAcceptedFingerprints))
	{
		verification_status = 1;
		goto end;
	}

	if (tls->isGatewayTransport || is_redirected(tls))
		flags |= VERIFY_CERT_FLAG_LEGACY;

	if (tls->isGatewayTransport)
		flags |= VERIFY_CERT_FLAG_GATEWAY;

	if (is_redirected(tls))
		flags |= VERIFY_CERT_FLAG_REDIRECT;

	/* Certificate management is done by the application */
	if (tls->context->settings->ExternalCertificateManagement)
	{
		if (instance->VerifyX509Certificate)
			verification_status =
			    instance->VerifyX509Certificate(instance, pemCert, length, hostname, port, flags);
		else
			WLog_ERR(TAG, "No VerifyX509Certificate callback registered!");

		if (verification_status > 0)
			accept_cert(tls, cert);
		else if (verification_status < 0)
		{
			WLog_ERR(TAG, "VerifyX509Certificate failed: (length = %" PRIuz ") status: [%d] %s",
			         length, verification_status, pemCert);
			goto end;
		}
	}
	/* ignore certificate verification if user explicitly required it (discouraged) */
	else if (freerdp_settings_get_bool(tls->context->settings, FreeRDP_IgnoreCertificate))
	{
		WLog_WARN(TAG, "[DANGER] Certificate not checked, /cert:ignore in use.");
		WLog_WARN(TAG, "[DANGER] This prevents MITM attacks from being detected!");
		WLog_WARN(TAG,
		          "[DANGER] Avoid using this unless in a secure LAN (=no internet) environment");
		verification_status = 1; /* success! */
	}
	else if (!tls->isGatewayTransport && (tls->context->settings->AuthenticationLevel == 0))
		verification_status = 1; /* success! */
	else
	{
		/* if user explicitly specified a certificate name, use it instead of the hostname */
		if (!tls->isGatewayTransport && tls->context->settings->CertificateName)
			hostname = tls->context->settings->CertificateName;

		/* attempt verification using OpenSSL and the ~/.freerdp/certs certificate store */
		certificate_status = freerdp_certificate_verify(
		    cert, freerdp_certificate_store_get_certs_path(tls->certificate_store));
		/* verify certificate name match */
		certificate_data = freerdp_certificate_data_new(hostname, port, cert);
		if (!certificate_data)
			goto end;
		/* extra common name and alternative names */
		common_name = freerdp_certificate_get_common_name(cert, &common_name_length);
		dns_names = freerdp_certificate_get_dns_names(cert, &dns_names_count, &dns_names_lengths);

		/* compare against common name */

		if (common_name)
		{
			if (tls_match_hostname(common_name, common_name_length, hostname))
				hostname_match = TRUE;
		}

		/* compare against alternative names */

		if (dns_names)
		{
			for (size_t index = 0; index < dns_names_count; index++)
			{
				if (tls_match_hostname(dns_names[index], dns_names_lengths[index], hostname))
				{
					hostname_match = TRUE;
					break;
				}
			}
		}

		/* if the certificate is valid and the certificate name matches, verification succeeds
		 */
		if (certificate_status && hostname_match)
			verification_status = 1; /* success! */

		if (!hostname_match)
			flags |= VERIFY_CERT_FLAG_MISMATCH;

		BOOL allowUserconfig = TRUE;
		if (!certificate_status || !hostname_match)
			verification_status = tls_config_check_certificate(cert, &allowUserconfig);

		/* verification could not succeed with OpenSSL, use known_hosts file and prompt user for
		 * manual verification */
		if (allowUserconfig && (!certificate_status || !hostname_match))
		{
			DWORD accept_certificate = 0;
			size_t pem_length = 0;
			char* issuer = freerdp_certificate_get_issuer(cert);
			char* subject = freerdp_certificate_get_subject(cert);
			char* pem = freerdp_certificate_get_pem(cert, &pem_length);

			if (!pem)
				goto end;

			/* search for matching entry in known_hosts file */
			match =
			    freerdp_certificate_store_contains_data(tls->certificate_store, certificate_data);

			if (match == 1)
			{
				/* no entry was found in known_hosts file, prompt user for manual verification
				 */
				if (!hostname_match)
					tls_print_certificate_name_mismatch_error(hostname, port, common_name,
					                                          dns_names, dns_names_count);

				{
					char* efp = freerdp_certificate_get_fingerprint(cert);
					tls_print_new_certificate_warn(tls->certificate_store, hostname, port, efp);
					free(efp);
				}

				/* Automatically accept certificate on first use */
				if (tls->context->settings->AutoAcceptCertificate)
				{
					WLog_INFO(TAG, "No certificate stored, automatically accepting.");
					accept_certificate = 1;
				}
				else if (tls->context->settings->AutoDenyCertificate)
				{
					WLog_INFO(TAG, "No certificate stored, automatically denying.");
					accept_certificate = 0;
				}
				else if (instance->VerifyX509Certificate)
				{
					int rc = instance->VerifyX509Certificate(instance, pemCert, pem_length,
					                                         hostname, port, flags);

					if (rc == 1)
						accept_certificate = 1;
					else if (rc > 1)
						accept_certificate = 2;
					else
						accept_certificate = 0;
				}
				else if (instance->VerifyCertificateEx)
				{
					const BOOL use_pem = freerdp_settings_get_bool(
					    tls->context->settings, FreeRDP_CertificateCallbackPreferPEM);
					char* fp = NULL;
					DWORD cflags = flags;
					if (use_pem)
					{
						cflags |= VERIFY_CERT_FLAG_FP_IS_PEM;
						fp = pem;
					}
					else
						fp = freerdp_certificate_get_fingerprint(cert);
					accept_certificate = instance->VerifyCertificateEx(
					    instance, hostname, port, common_name, subject, issuer, fp, cflags);
					if (!use_pem)
						free(fp);
				}
#if defined(WITH_FREERDP_DEPRECATED)
				else if (instance->VerifyCertificate)
				{
					char* fp = freerdp_certificate_get_fingerprint(cert);

					WLog_WARN(TAG, "The VerifyCertificate callback is deprecated, migrate your "
					               "application to VerifyCertificateEx");
					accept_certificate = instance->VerifyCertificate(instance, common_name, subject,
					                                                 issuer, fp, !hostname_match);
					free(fp);
				}
#endif
			}
			else if (match == -1)
			{
				rdpCertificateData* stored_data =
				    freerdp_certificate_store_load_data(tls->certificate_store, hostname, port);
				/* entry was found in known_hosts file, but fingerprint does not match. ask user
				 * to use it */
				{
					char* efp = freerdp_certificate_get_fingerprint(cert);
					tls_print_certificate_error(tls->certificate_store, stored_data, hostname, port,
					                            efp);
					free(efp);
				}

				if (!stored_data)
					WLog_WARN(TAG, "Failed to get certificate entry for %s:%" PRIu16 "", hostname,
					          port);

				if (tls->context->settings->AutoDenyCertificate)
				{
					WLog_INFO(TAG, "No certificate stored, automatically denying.");
					accept_certificate = 0;
				}
				else if (instance->VerifyX509Certificate)
				{
					const int rc =
					    instance->VerifyX509Certificate(instance, pemCert, pem_length, hostname,
					                                    port, flags | VERIFY_CERT_FLAG_CHANGED);

					if (rc == 1)
						accept_certificate = 1;
					else if (rc > 1)
						accept_certificate = 2;
					else
						accept_certificate = 0;
				}
				else if (instance->VerifyChangedCertificateEx)
				{
					DWORD cflags = flags | VERIFY_CERT_FLAG_CHANGED;
					const char* old_subject = freerdp_certificate_data_get_subject(stored_data);
					const char* old_issuer = freerdp_certificate_data_get_issuer(stored_data);
					const char* old_fp = freerdp_certificate_data_get_fingerprint(stored_data);
					const char* old_pem = freerdp_certificate_data_get_pem(stored_data);
					const BOOL fpIsAllocated =
					    !old_pem ||
					    !freerdp_settings_get_bool(tls->context->settings,
					                               FreeRDP_CertificateCallbackPreferPEM);
					char* fp = NULL;
					if (!fpIsAllocated)
					{
						cflags |= VERIFY_CERT_FLAG_FP_IS_PEM;
						fp = pem;
						old_fp = old_pem;
					}
					else
					{
						fp = freerdp_certificate_get_fingerprint(cert);
					}
					accept_certificate = instance->VerifyChangedCertificateEx(
					    instance, hostname, port, common_name, subject, issuer, fp, old_subject,
					    old_issuer, old_fp, cflags);
					if (fpIsAllocated)
						free(fp);
				}
#if defined(WITH_FREERDP_DEPRECATED)
				else if (instance->VerifyChangedCertificate)
				{
					char* fp = freerdp_certificate_get_fingerprint(cert);
					const char* old_subject = freerdp_certificate_data_get_subject(stored_data);
					const char* old_issuer = freerdp_certificate_data_get_issuer(stored_data);
					const char* old_fingerprint =
					    freerdp_certificate_data_get_fingerprint(stored_data);

					WLog_WARN(TAG, "The VerifyChangedCertificate callback is deprecated, migrate "
					               "your application to VerifyChangedCertificateEx");
					accept_certificate = instance->VerifyChangedCertificate(
					    instance, common_name, subject, issuer, fp, old_subject, old_issuer,
					    old_fingerprint);
					free(fp);
				}
#endif

				freerdp_certificate_data_free(stored_data);
			}
			else if (match == 0)
				accept_certificate = 2; /* success! */

			/* Save certificate or do a simple accept / reject */
			switch (accept_certificate)
			{
				case 1:

					/* user accepted certificate, add entry in known_hosts file */
					verification_status = freerdp_certificate_store_save_data(
					                          tls->certificate_store, certificate_data)
					                          ? 1
					                          : -1;
					break;

				case 2:
					/* user did accept temporaty, do not add to known hosts file */
					verification_status = 1;
					break;

				default:
					/* user did not accept, abort and do not add entry in known_hosts file */
					verification_status = -1; /* failure! */
					break;
			}

			free(issuer);
			free(subject);
			free(pem);
		}

		if (verification_status > 0)
			accept_cert(tls, cert);
	}

end:
	freerdp_certificate_data_free(certificate_data);
	free(common_name);
	freerdp_certificate_free_dns_names(dns_names_count, dns_names_lengths, dns_names);
	free(pemCert);
	return verification_status;
}

void tls_print_new_certificate_warn(rdpCertificateStore* store, const char* hostname, UINT16 port,
                                    const char* fingerprint)
{
	char* path = freerdp_certificate_store_get_cert_path(store, hostname, port);

	WLog_ERR(TAG, "The host key for %s:%" PRIu16 " has changed", hostname, port);
	WLog_ERR(TAG, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	WLog_ERR(TAG, "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @");
	WLog_ERR(TAG, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	WLog_ERR(TAG, "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!");
	WLog_ERR(TAG, "Someone could be eavesdropping on you right now (man-in-the-middle attack)!");
	WLog_ERR(TAG, "It is also possible that a host key has just been changed.");
	WLog_ERR(TAG, "The fingerprint for the host key sent by the remote host is %s", fingerprint);
	WLog_ERR(TAG, "Please contact your system administrator.");
	WLog_ERR(TAG, "Add correct host key in %s to get rid of this message.", path);
	WLog_ERR(TAG, "Host key for %s has changed and you have requested strict checking.", hostname);
	WLog_ERR(TAG, "Host key verification failed.");

	free(path);
}

void tls_print_certificate_error(rdpCertificateStore* store,
                                 WINPR_ATTR_UNUSED rdpCertificateData* stored_data,
                                 const char* hostname, UINT16 port, const char* fingerprint)
{
	char* path = freerdp_certificate_store_get_cert_path(store, hostname, port);

	WLog_ERR(TAG, "New host key for %s:%" PRIu16, hostname, port);
	WLog_ERR(TAG, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	WLog_ERR(TAG, "@    WARNING: NEW HOST IDENTIFICATION!     @");
	WLog_ERR(TAG, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");

	WLog_ERR(TAG, "The fingerprint for the host key sent by the remote host is %s", fingerprint);
	WLog_ERR(TAG, "Please contact your system administrator.");
	WLog_ERR(TAG, "Add correct host key in %s to get rid of this message.", path);

	free(path);
}

void tls_print_certificate_name_mismatch_error(const char* hostname, UINT16 port,
                                               const char* common_name, char** alt_names,
                                               size_t alt_names_count)
{
	WINPR_ASSERT(NULL != hostname);
	WLog_ERR(TAG, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	WLog_ERR(TAG, "@           WARNING: CERTIFICATE NAME MISMATCH!           @");
	WLog_ERR(TAG, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
	WLog_ERR(TAG, "The hostname used for this connection (%s:%" PRIu16 ") ", hostname, port);
	WLog_ERR(TAG, "does not match %s given in the certificate:",
	         alt_names_count < 1 ? "the name" : "any of the names");
	WLog_ERR(TAG, "Common Name (CN):");
	WLog_ERR(TAG, "\t%s", common_name ? common_name : "no CN found in certificate");

	if (alt_names_count > 0)
	{
		WINPR_ASSERT(NULL != alt_names);
		WLog_ERR(TAG, "Alternative names:");

		for (size_t index = 0; index < alt_names_count; index++)
		{
			WINPR_ASSERT(alt_names[index]);
			WLog_ERR(TAG, "\t %s", alt_names[index]);
		}
	}

	WLog_ERR(TAG, "A valid certificate for the wrong name should NOT be trusted!");
}

rdpTls* freerdp_tls_new(rdpContext* context)
{
	rdpTls* tls = NULL;
	tls = (rdpTls*)calloc(1, sizeof(rdpTls));

	if (!tls)
		return NULL;

	tls->context = context;

	if (!freerdp_settings_get_bool(tls->context->settings, FreeRDP_ServerMode))
	{
		tls->certificate_store = freerdp_certificate_store_new(tls->context->settings);

		if (!tls->certificate_store)
			goto out_free;
	}

	tls->alertLevel = TLS_ALERT_LEVEL_WARNING;
	tls->alertDescription = TLS_ALERT_DESCRIPTION_CLOSE_NOTIFY;
	return tls;
out_free:
	free(tls);
	return NULL;
}

void freerdp_tls_free(rdpTls* tls)
{
	if (!tls)
		return;

	tls_reset(tls);

	if (tls->certificate_store)
	{
		freerdp_certificate_store_free(tls->certificate_store);
		tls->certificate_store = NULL;
	}

	free(tls);
}

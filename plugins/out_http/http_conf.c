/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2024 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_kv.h>
#include <fluent-bit/flb_record_accessor.h>

#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
#include <fluent-bit/flb_aws_credentials.h>
#endif
#endif
#include "http.h"
#include "http_conf.h"

struct flb_out_http *flb_http_conf_create(struct flb_output_instance *ins,
                                          struct flb_config *config)
{
    int ret;
    int ulen;
    int io_flags = 0;
    char *protocol = NULL;
    char *host = NULL;
    char *port = NULL;
    char *uri = NULL;
    char *tmp_uri = NULL;
    const char *tmp;
    struct flb_upstream *upstream;
    struct flb_out_http *ctx = NULL;

    /* Allocate plugin context */
    ctx = flb_calloc(1, sizeof(struct flb_out_http));
    if (!ctx) {
        flb_errno();
        return NULL;
    }
    ctx->ins = ins;

    ret = flb_output_config_map_set(ins, (void *) ctx);
    if (ret == -1) {
        flb_free(ctx);
        return NULL;
    }

    if (ctx->headers_key && !ctx->body_key) {
        flb_plg_error(ctx->ins, "when setting headers_key, body_key is also required");
        flb_free(ctx);
        return NULL;
    }

    if (ctx->body_key && !ctx->headers_key) {
        flb_plg_error(ctx->ins, "when setting body_key, headers_key is also required");
        flb_free(ctx);
        return NULL;
    }

    if (ctx->body_key && ctx->headers_key) {
        ctx->body_ra = flb_ra_create(ctx->body_key, FLB_FALSE);
        if (!ctx->body_ra) {
            flb_plg_error(ctx->ins, "failed to allocate body record accessor");
            flb_free(ctx);
            return NULL;
        }

        ctx->headers_ra = flb_ra_create(ctx->headers_key, FLB_FALSE);
        if (!ctx->headers_ra) {
            flb_plg_error(ctx->ins, "failed to allocate headers record accessor");
            flb_free(ctx);
            return NULL;
        }
    }

    /*
     * Check if a Proxy have been set, if so the Upstream manager will use
     * the Proxy end-point and then we let the HTTP client know about it, so
     * it can adjust the HTTP requests.
     */
    tmp = flb_output_get_property("proxy", ins);
    if (tmp) {
        ret = flb_utils_url_split(tmp, &protocol, &host, &port, &uri);
        if (ret == -1) {
            flb_plg_error(ctx->ins, "could not parse proxy parameter: '%s'", tmp);
            flb_free(ctx);
            return NULL;
        }

        ctx->proxy_host = host;
        ctx->proxy_port = atoi(port);
        ctx->proxy = tmp;
        flb_free(protocol);
        flb_free(port);
        flb_free(uri);
        uri = NULL;
    }
    else {
        flb_output_net_default("127.0.0.1", 80, ins);
    }

    /* Check if AWS SigV4 authentication is enabled */
#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
    if (ctx->has_aws_auth) {
        ctx->aws_service = flb_output_get_property(FLB_HTTP_AWS_CREDENTIAL_PREFIX
                                                   "service", ctx->ins);
        if (!ctx->aws_service) {
            flb_plg_error(ins, "aws_auth option requires " FLB_HTTP_AWS_CREDENTIAL_PREFIX
                          "service to be set");
            flb_free(ctx);
            return NULL;
        }

        ctx->aws_provider = flb_managed_chain_provider_create(
            ins,
            config,
            FLB_HTTP_AWS_CREDENTIAL_PREFIX,
            NULL,
            flb_aws_client_generator()
        );
        if (!ctx->aws_provider) {
            flb_plg_error(ins, "failed to create aws credential provider for sigv4 auth");
            flb_free(ctx);
            return NULL;
        }

        /* If managed provider creation succeeds, then region key is present */
        ctx->aws_region = flb_output_get_property(FLB_HTTP_AWS_CREDENTIAL_PREFIX
                                                  "region", ctx->ins);
    }
#endif /* !FLB_HAVE_AWS */
#endif /* !FLB_HAVE_SIGNV4 */

    /* Check if SSL/TLS is enabled */
#ifdef FLB_HAVE_TLS
    if (ins->use_tls == FLB_TRUE) {
        io_flags = FLB_IO_TLS;
    }
    else {
        io_flags = FLB_IO_TCP;
    }
#else
    io_flags = FLB_IO_TCP;
#endif

    if (ins->host.ipv6 == FLB_TRUE) {
        io_flags |= FLB_IO_IPV6;
    }

    if (ctx->proxy) {
        flb_plg_trace(ctx->ins, "Upstream Proxy=%s:%i",
                      ctx->proxy_host, ctx->proxy_port);
        upstream = flb_upstream_create(config,
                                       ctx->proxy_host,
                                       ctx->proxy_port,
                                       io_flags, ins->tls);
    }
    else {
        upstream = flb_upstream_create(config,
                                       ins->host.name,
                                       ins->host.port,
                                       io_flags, ins->tls);
    }

    if (!upstream) {
        flb_free(ctx);
        return NULL;
    }

    if (ins->host.uri) {
        uri = flb_strdup(ins->host.uri->full);
    }
    else {
        tmp = flb_output_get_property("uri", ins);
        if (tmp) {
            uri = flb_strdup(tmp);
        }
    }

    if (!uri) {
        uri = flb_strdup("/");
    }
    else if (uri[0] != '/') {
        ulen = strlen(uri);
        tmp_uri = flb_malloc(ulen + 2);
        tmp_uri[0] = '/';
        memcpy(tmp_uri + 1, uri, ulen);
        tmp_uri[ulen + 1] = '\0';
        flb_free(uri);
        uri = tmp_uri;
    }

    /* Output format */
    ctx->out_format = FLB_PACK_JSON_FORMAT_NONE;
    if (ctx->format) {
        if (strcasecmp(ctx->format, "gelf") == 0) {
            ctx->out_format = FLB_HTTP_OUT_GELF;
        }
        else if (strcasecmp(ctx->format, "msgpack") == 0) {
            ctx->out_format = FLB_HTTP_OUT_MSGPACK;
        }
        else {
            ret = flb_pack_to_json_format_type(ctx->format);
            if (ret == -1) {
                flb_plg_error(ctx->ins, "unrecognized 'format' option. "
                              "Using 'msgpack'");
            }
            else {
                ctx->out_format = ret;
            }
        }
    }

    /* Date key */
    ctx->date_key = ctx->json_date_key;
    tmp = flb_output_get_property("json_date_key", ins);
    if (tmp) {
        /* Just check if we have to disable it */
        if (flb_utils_bool(tmp) == FLB_FALSE) {
            ctx->date_key = NULL;
        }
    }

    /* Date format for JSON output */
    ctx->json_date_format = FLB_PACK_JSON_DATE_DOUBLE;
    tmp = flb_output_get_property("json_date_format", ins);
    if (tmp) {
        ret = flb_pack_to_json_date_type(tmp);
        if (ret == -1) {
            flb_plg_error(ctx->ins, "unrecognized 'json_date_format' option. "
                          "Using 'double'.");
        }
        else {
            ctx->json_date_format = ret;
        }
    }

    /* Compress (gzip) */
    tmp = flb_output_get_property("compress", ins);
    ctx->compress_gzip = FLB_FALSE;
    if (tmp) {
        if (strcasecmp(tmp, "gzip") == 0) {
            ctx->compress_gzip = FLB_TRUE;
        }
        else if (strcasecmp(tmp, "snappy") == 0) {
            ctx->compress_snappy = FLB_TRUE;
        }
        else if (strcasecmp(tmp, "zstd") == 0) {
            ctx->compress_zstd = FLB_TRUE;
        }
        else {
            flb_plg_error(ctx->ins, "invalid compress option '%s'", tmp);
            flb_free(ctx);
            return NULL;
        }
    }

    ctx->u = upstream;
    ctx->uri = uri;
    ctx->host = ins->host.name;
    ctx->port = ins->host.port;

    /* Set instance flags into upstream */
    flb_output_upstream_set(ctx->u, ins);

    return ctx;
}

void flb_http_conf_destroy(struct flb_out_http *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->body_ra && ctx->headers_ra) {
        flb_ra_destroy(ctx->body_ra);
        flb_ra_destroy(ctx->headers_ra);
    }

    if (ctx->u) {
        flb_upstream_destroy(ctx->u);
    }

#ifdef FLB_HAVE_SIGNV4
#ifdef FLB_HAVE_AWS
    if (ctx->aws_provider) {
        flb_aws_provider_destroy(ctx->aws_provider);
    }
#endif
#endif

    flb_free(ctx->proxy_host);
    flb_free(ctx->uri);
    flb_free(ctx);
}

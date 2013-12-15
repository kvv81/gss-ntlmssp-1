/*
   Copyright (C) 2013 Simo Sorce <simo@samba.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include <endian.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gssapi_ntlmssp.h"
#include "gss_ntlmssp.h"

/* each integer in the export format is a little endian integer */
#pragma pack(push, 1)
struct relmem {
    uint16_t ptr;
    uint16_t len;
};

struct export_name {
    uint8_t type;
    struct relmem domain;
    struct relmem name;
};

struct export_keys {
    struct relmem sign_key;
    struct relmem seal_key;
    struct relmem rc4_state;
    uint32_t seq_num;
};

struct export_ctx {
    uint16_t version;   /* 0x00 0x01 */
    uint8_t role;
    uint8_t stage;

    struct relmem workstation;

    struct relmem nego_msg;
    struct relmem chal_msg;
    struct relmem auth_msg;

    struct export_name source;
    struct export_name target;

    uint8_t server_chal[8];

    uint32_t gss_flags;
    uint32_t neg_flags;

    struct relmem exported_session_key;
    struct export_keys send;
    struct export_keys recv;

    uint8_t established;
    uint64_t expration_time;

    uint8_t data[];
};
#pragma pack(pop)

#define EXP_CTX_CLIENT 1
#define EXP_CTX_SERVER 2
#define EXP_CTX_DOMSRV 3
#define EXP_CTX_DOMCTR 4
#define EXP_STG_INIT 1
#define EXP_STG_NEGO 2
#define EXP_STG_CHAL 3
#define EXP_STG_AUTH 4
#define EXP_STG_DONE 5
#define EXP_NAME_NONE 0
#define EXP_NAME_ANON 1
#define EXP_NAME_USER 2
#define EXP_NAME_SERV 3

#define INC_EXP_SIZE 0x001000 /* 4K */
#define MAX_EXP_SIZE 0x100000 /* 1M */

#define NEW_SIZE(s, n) \
    ((((s) + (n) + (INC_EXP_SIZE-1)) / INC_EXP_SIZE) * INC_EXP_SIZE)

struct export_ctx_state {
    struct export_ctx *exp;
    size_t exp_size;
    size_t exp_len;
    size_t exp_ptr;
};

static int export_data_buffer(struct export_ctx_state *state,
                              void *data, size_t length,
                              struct relmem *rm)
{
    void *tmp;
    size_t avail = state->exp_size - state->exp_len;
    size_t new_size;

    if (length > avail) {
        new_size = NEW_SIZE(state->exp_size, (length - avail));
        if ((new_size < state->exp_size) || new_size > MAX_EXP_SIZE) {
            return ENOMEM;
        }
        tmp = realloc(state->exp, new_size);
        if (!tmp) {
            return ENOMEM;
        }
        state->exp = (struct export_ctx *)tmp;
        state->exp_size = new_size;
        avail = state->exp_size - state->exp_len;
    }

    memcpy(&state->exp->data[state->exp_ptr], data, length);
    rm->ptr = state->exp_ptr;
    rm->len = length;
    state->exp_ptr += length;
    state->exp_len += length;
    return 0;
}

static int export_name(struct export_ctx_state *state,
                       struct gssntlm_name *name,
                       struct export_name *exp_name)
{
    int ret;

    switch (name->type) {
    case GSSNTLM_NAME_NULL:
        memset(exp_name, 0, sizeof(struct export_name));
        return 0;
    case GSSNTLM_NAME_ANON:
        memset(exp_name, 0, sizeof(struct export_name));
        exp_name->type = EXP_NAME_ANON;
        return 0;
    case GSSNTLM_NAME_USER:
        exp_name->type = EXP_NAME_USER;
        if (name->data.user.domain) {
            ret = export_data_buffer(state, name->data.user.domain,
                                     strlen(name->data.user.domain) + 1,
                                     &exp_name->domain);
            if (ret) {
                return ret;
            }
        } else {
            exp_name->domain.ptr = 0;
            exp_name->domain.len = 0;
        }
        if (name->data.user.name) {
            ret = export_data_buffer(state, name->data.user.name,
                                     strlen(name->data.user.name) + 1,
                                     &exp_name->name);
            if (ret) {
                return ret;
            }
        } else {
            exp_name->name.ptr = 0;
            exp_name->name.len = 0;
        }
        return 0;
    case GSSNTLM_NAME_SERVER:
        exp_name->type = EXP_NAME_SERV;
        exp_name->domain.ptr = 0;
        exp_name->domain.len = 0;
        if (name->data.server.name) {
            ret = export_data_buffer(state, name->data.server.name,
                                     strlen(name->data.server.name) + 1,
                                     &exp_name->name);
            if (ret) {
                return ret;
            }
        } else {
            exp_name->name.ptr = 0;
            exp_name->name.len = 0;
        }
        return 0;
    }
    return EINVAL;
}

static int export_keys(struct export_ctx_state *state,
                       struct gssntlm_signseal *keys,
                       struct export_keys *exp_keys)
{
    uint8_t buf[258];
    struct ntlm_buffer out = { .data=buf, .length=sizeof(buf) };
    int ret;

    if (keys->sign_key.length > 0) {
        ret = export_data_buffer(state,
                                 keys->sign_key.data,
                                 keys->sign_key.length,
                                 &exp_keys->sign_key);
        if (ret) return ret;
    } else {
        exp_keys->sign_key.ptr = 0;
        exp_keys->sign_key.len = 0;
    }

    if (keys->seal_key.length > 0) {
        ret = export_data_buffer(state,
                                 keys->seal_key.data,
                                 keys->seal_key.length,
                                 &exp_keys->seal_key);
        if (ret) return ret;
    } else {
        exp_keys->seal_key.ptr = 0;
        exp_keys->seal_key.len = 0;
    }

    if (keys->seal_handle) {
        ret = RC4_EXPORT(keys->seal_handle, &out);
        if (ret) return ret;
        ret = export_data_buffer(state, buf, sizeof(buf),
                                 &exp_keys->rc4_state);
        safezero(buf, sizeof(buf));
        if (ret) return ret;
    } else {
        exp_keys->rc4_state.ptr = 0;
        exp_keys->rc4_state.len = 0;
    }

    exp_keys->seq_num = htole32(keys->seq_num);

    return 0;
}

uint32_t gssntlm_export_sec_context(uint32_t *minor_status,
                                    gss_ctx_id_t *context_handle,
                                    gss_buffer_t interprocess_token)
{
    struct gssntlm_ctx *ctx;
    struct export_ctx_state state;
    uint64_t expiration;
    int ret;

    if (context_handle == NULL) {
        return GSS_S_CALL_INACCESSIBLE_READ;
    }

    if (interprocess_token == NULL) {
        return GSS_S_CALL_INACCESSIBLE_WRITE;
    }

    ctx = (struct gssntlm_ctx *)*context_handle;
    if (ctx == NULL) return GSS_S_NO_CONTEXT;

    if (ctx->expiration_time && ctx->expiration_time < time(NULL)) {
        return GSS_S_CONTEXT_EXPIRED;
    }

    *minor_status = 0;

    /* serialize context */
    state.exp_size = NEW_SIZE(0, sizeof(struct export_ctx));
    state.exp = malloc(state.exp_size);
    if (!state.exp) {
        *minor_status = ENOMEM;
        return GSS_S_FAILURE;
    }
    state.exp_len = (void *)state.exp->data - (void *)state.exp;
    state.exp_ptr = 0;

    state.exp->version = htole16(1);

    switch(ctx->role) {
    case GSSNTLM_CLIENT:
        state.exp->role = EXP_CTX_CLIENT;
        break;
    case GSSNTLM_SERVER:
        state.exp->role = EXP_CTX_SERVER;
        break;
    case GSSNTLM_DOMAIN_SERVER:
        state.exp->role = EXP_CTX_DOMSRV;
        break;
    case GSSNTLM_DOMAIN_CONTROLLER:
        state.exp->role = EXP_CTX_DOMCTR;
        break;
    }

    switch(ctx->stage) {
    case NTLMSSP_STAGE_INIT:
        state.exp->stage = EXP_STG_INIT;
        break;
    case NTLMSSP_STAGE_NEGOTIATE:
        state.exp->stage = EXP_STG_NEGO;
        break;
    case NTLMSSP_STAGE_CHALLENGE:
        state.exp->stage = EXP_STG_CHAL;
        break;
    case NTLMSSP_STAGE_AUTHENTICATE:
        state.exp->stage = EXP_STG_AUTH;
        break;
    case NTLMSSP_STAGE_DONE:
        state.exp->stage = EXP_STG_DONE;
        break;
    }

    if (!ctx->workstation) {
        state.exp->workstation.ptr = 0;
        state.exp->workstation.len = 0;
    } else {
        ret = export_data_buffer(&state, ctx->workstation,
                                 strlen(ctx->workstation) + 1,
                                 &state.exp->workstation);
        if (ret) goto done;
    }

    if (ctx->nego_msg.length > 0) {
        ret = export_data_buffer(&state,
                                 ctx->nego_msg.data,
                                 ctx->nego_msg.length,
                                 &state.exp->nego_msg);
        if (ret) goto done;
    } else {
        state.exp->nego_msg.ptr = 0;
        state.exp->nego_msg.len = 0;
    }

    if (ctx->chal_msg.length > 0) {
        ret = export_data_buffer(&state,
                                 ctx->chal_msg.data,
                                 ctx->chal_msg.length,
                                 &state.exp->chal_msg);
        if (ret) goto done;
    } else {
        state.exp->chal_msg.ptr = 0;
        state.exp->chal_msg.len = 0;
    }

    if (ctx->auth_msg.length > 0) {
        ret = export_data_buffer(&state,
                                 ctx->auth_msg.data,
                                 ctx->auth_msg.length,
                                 &state.exp->auth_msg);
        if (ret) goto done;
    } else {
        state.exp->auth_msg.ptr = 0;
        state.exp->auth_msg.len = 0;
    }

    ret = export_name(&state, &ctx->source_name, &state.exp->source);
    if (ret) goto done;

    ret = export_name(&state, &ctx->target_name, &state.exp->target);
    if (ret) goto done;

    memcpy(state.exp->server_chal, ctx->server_chal, 8);

    state.exp->gss_flags = htole32(ctx->gss_flags);
    state.exp->neg_flags = htole32(ctx->neg_flags);

    ret = export_data_buffer(&state,
                             ctx->exported_session_key.data,
                             ctx->exported_session_key.length,
                             &state.exp->exported_session_key);
    if (ret) goto done;

    ret = export_keys(&state, &ctx->send, &state.exp->send);
    if (ret) goto done;

    ret = export_keys(&state, &ctx->recv, &state.exp->recv);
    if (ret) goto done;

    state.exp->established = ctx->established ? 1 : 0;

    expiration = ctx->expiration_time;
    state.exp->expration_time = htole64(expiration);

    ret = 0;

done:
    if (ret) {
        *minor_status = ret;
        free(state.exp);
        return GSS_S_FAILURE;
    } else {
        uint32_t min;
        interprocess_token->value = state.exp;
        interprocess_token->length = state.exp_len;

        /* Invalidate the current context once successfully exported */
        gssntlm_delete_sec_context(&min, context_handle, NULL);

        return GSS_S_COMPLETE;
    }
}

uint32_t gssntlm_import_sec_context(uint32_t *minor_status,
                                    gss_buffer_t interprocess_token,
                                    gss_ctx_id_t *context_handle)
{
    return GSS_S_UNAVAILABLE;
}

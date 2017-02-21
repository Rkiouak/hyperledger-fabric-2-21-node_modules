/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/lib/channel/http_server_filter.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <string.h>
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/percent_encoding.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/transport/static_metadata.h"

#define EXPECTED_CONTENT_TYPE "application/grpc"
#define EXPECTED_CONTENT_TYPE_LENGTH sizeof(EXPECTED_CONTENT_TYPE) - 1

extern int grpc_http_trace;

typedef struct call_data {
  uint8_t seen_path;
  uint8_t seen_method;
  uint8_t sent_status;
  uint8_t seen_scheme;
  uint8_t seen_te_trailers;
  uint8_t seen_authority;
  uint8_t seen_payload_bin;
  grpc_linked_mdelem status;
  grpc_linked_mdelem content_type;

  /* flag to ensure payload_bin is delivered only once */
  uint8_t payload_bin_delivered;

  grpc_metadata_batch *recv_initial_metadata;
  bool *recv_idempotent_request;
  bool *recv_cacheable_request;
  /** Closure to call when finished with the hs_on_recv hook */
  grpc_closure *on_done_recv;
  /** Closure to call when we retrieve read message from the payload-bin header
   */
  grpc_closure *recv_message_ready;
  grpc_closure *on_complete;
  grpc_byte_stream **pp_recv_message;
  grpc_slice_buffer read_slice_buffer;
  grpc_slice_buffer_stream read_stream;

  /** Receive closures are chained: we inject this closure as the on_done_recv
      up-call on transport_op, and remember to call our on_done_recv member
      after handling it. */
  grpc_closure hs_on_recv;
  grpc_closure hs_on_complete;
  grpc_closure hs_recv_message_ready;
} call_data;

typedef struct channel_data { uint8_t unused; } channel_data;

static grpc_mdelem *server_filter_outgoing_metadata(grpc_exec_ctx *exec_ctx,
                                                    void *user_data,
                                                    grpc_mdelem *md) {
  if (md->key == GRPC_MDSTR_GRPC_MESSAGE) {
    grpc_slice pct_encoded_msg = grpc_percent_encode_slice(
        md->value->slice, grpc_compatible_percent_encoding_unreserved_bytes);
    if (grpc_slice_is_equivalent(pct_encoded_msg, md->value->slice)) {
      grpc_slice_unref_internal(exec_ctx, pct_encoded_msg);
      return md;
    } else {
      return grpc_mdelem_from_metadata_strings(
          exec_ctx, GRPC_MDSTR_GRPC_MESSAGE,
          grpc_mdstr_from_slice(exec_ctx, pct_encoded_msg));
    }
  } else {
    return md;
  }
}

static grpc_mdelem *server_filter(grpc_exec_ctx *exec_ctx, void *user_data,
                                  grpc_mdelem *md) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;

  /* Check if it is one of the headers we care about. */
  if (md == GRPC_MDELEM_TE_TRAILERS || md == GRPC_MDELEM_METHOD_POST ||
      md == GRPC_MDELEM_METHOD_PUT || md == GRPC_MDELEM_METHOD_GET ||
      md == GRPC_MDELEM_SCHEME_HTTP || md == GRPC_MDELEM_SCHEME_HTTPS ||
      md == GRPC_MDELEM_CONTENT_TYPE_APPLICATION_SLASH_GRPC) {
    /* swallow it */
    if (md == GRPC_MDELEM_METHOD_POST) {
      calld->seen_method = 1;
      *calld->recv_idempotent_request = false;
      *calld->recv_cacheable_request = false;
    } else if (md == GRPC_MDELEM_METHOD_PUT) {
      calld->seen_method = 1;
      *calld->recv_idempotent_request = true;
    } else if (md == GRPC_MDELEM_METHOD_GET) {
      calld->seen_method = 1;
      *calld->recv_cacheable_request = true;
    } else if (md->key == GRPC_MDSTR_SCHEME) {
      calld->seen_scheme = 1;
    } else if (md == GRPC_MDELEM_TE_TRAILERS) {
      calld->seen_te_trailers = 1;
    }
    /* TODO(klempner): Track that we've seen all the headers we should
       require */
    return NULL;
  } else if (md->key == GRPC_MDSTR_CONTENT_TYPE) {
    const char *value_str = grpc_mdstr_as_c_string(md->value);
    if (strncmp(value_str, EXPECTED_CONTENT_TYPE,
                EXPECTED_CONTENT_TYPE_LENGTH) == 0 &&
        (value_str[EXPECTED_CONTENT_TYPE_LENGTH] == '+' ||
         value_str[EXPECTED_CONTENT_TYPE_LENGTH] == ';')) {
      /* Although the C implementation doesn't (currently) generate them,
         any custom +-suffix is explicitly valid. */
      /* TODO(klempner): We should consider preallocating common values such
         as +proto or +json, or at least stashing them if we see them. */
      /* TODO(klempner): Should we be surfacing this to application code? */
    } else {
      /* TODO(klempner): We're currently allowing this, but we shouldn't
         see it without a proxy so log for now. */
      gpr_log(GPR_INFO, "Unexpected content-type '%s'", value_str);
    }
    return NULL;
  } else if (md->key == GRPC_MDSTR_TE || md->key == GRPC_MDSTR_METHOD ||
             md->key == GRPC_MDSTR_SCHEME) {
    gpr_log(GPR_ERROR, "Invalid %s: header: '%s'",
            grpc_mdstr_as_c_string(md->key), grpc_mdstr_as_c_string(md->value));
    /* swallow it and error everything out. */
    /* TODO(klempner): We ought to generate more descriptive error messages
       on the wire here. */
    grpc_call_element_send_cancel(exec_ctx, elem);
    return NULL;
  } else if (md->key == GRPC_MDSTR_PATH) {
    if (calld->seen_path) {
      gpr_log(GPR_ERROR, "Received :path twice");
      return NULL;
    }
    calld->seen_path = 1;
    return md;
  } else if (md->key == GRPC_MDSTR_AUTHORITY) {
    calld->seen_authority = 1;
    return md;
  } else if (md->key == GRPC_MDSTR_HOST) {
    /* translate host to :authority since :authority may be
       omitted */
    grpc_mdelem *authority = grpc_mdelem_from_metadata_strings(
        exec_ctx, GRPC_MDSTR_AUTHORITY, GRPC_MDSTR_REF(md->value));
    calld->seen_authority = 1;
    return authority;
  } else if (md->key == GRPC_MDSTR_GRPC_PAYLOAD_BIN) {
    /* Retrieve the payload from the value of the 'grpc-internal-payload-bin'
       header field */
    calld->seen_payload_bin = 1;
    grpc_slice_buffer_add(&calld->read_slice_buffer,
                          grpc_slice_ref_internal(md->value->slice));
    grpc_slice_buffer_stream_init(&calld->read_stream,
                                  &calld->read_slice_buffer, 0);
    return NULL;
  } else {
    return md;
  }
}

static void hs_on_recv(grpc_exec_ctx *exec_ctx, void *user_data,
                       grpc_error *err) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;
  if (err == GRPC_ERROR_NONE) {
    grpc_metadata_batch_filter(exec_ctx, calld->recv_initial_metadata,
                               server_filter, elem);
    /* Have we seen the required http2 transport headers?
       (:method, :scheme, content-type, with :path and :authority covered
       at the channel level right now) */
    if (calld->seen_method && calld->seen_scheme && calld->seen_te_trailers &&
        calld->seen_path && calld->seen_authority) {
      /* do nothing */
    } else {
      err = GRPC_ERROR_CREATE("Bad incoming HTTP headers");
      if (!calld->seen_path) {
        err = grpc_error_add_child(err,
                                   GRPC_ERROR_CREATE("Missing :path header"));
      }
      if (!calld->seen_authority) {
        err = grpc_error_add_child(
            err, GRPC_ERROR_CREATE("Missing :authority header"));
      }
      if (!calld->seen_method) {
        err = grpc_error_add_child(err,
                                   GRPC_ERROR_CREATE("Missing :method header"));
      }
      if (!calld->seen_scheme) {
        err = grpc_error_add_child(err,
                                   GRPC_ERROR_CREATE("Missing :scheme header"));
      }
      if (!calld->seen_te_trailers) {
        err = grpc_error_add_child(
            err, GRPC_ERROR_CREATE("Missing te: trailers header"));
      }
      /* Error this call out */
      if (grpc_http_trace) {
        const char *error_str = grpc_error_string(err);
        gpr_log(GPR_ERROR, "Invalid http2 headers: %s", error_str);
        grpc_error_free_string(error_str);
      }
      grpc_call_element_send_cancel(exec_ctx, elem);
    }
  } else {
    GRPC_ERROR_REF(err);
  }
  calld->on_done_recv->cb(exec_ctx, calld->on_done_recv->cb_arg, err);
  GRPC_ERROR_UNREF(err);
}

static void hs_on_complete(grpc_exec_ctx *exec_ctx, void *user_data,
                           grpc_error *err) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;
  /* Call recv_message_ready if we got the payload via the header field */
  if (calld->seen_payload_bin && calld->recv_message_ready != NULL) {
    *calld->pp_recv_message = calld->payload_bin_delivered
                                  ? NULL
                                  : (grpc_byte_stream *)&calld->read_stream;
    calld->recv_message_ready->cb(exec_ctx, calld->recv_message_ready->cb_arg,
                                  err);
    calld->recv_message_ready = NULL;
    calld->payload_bin_delivered = true;
  }
  calld->on_complete->cb(exec_ctx, calld->on_complete->cb_arg, err);
}

static void hs_recv_message_ready(grpc_exec_ctx *exec_ctx, void *user_data,
                                  grpc_error *err) {
  grpc_call_element *elem = user_data;
  call_data *calld = elem->call_data;
  if (calld->seen_payload_bin) {
    /* do nothing. This is probably a GET request, and payload will be returned
    in hs_on_complete callback. */
  } else {
    calld->recv_message_ready->cb(exec_ctx, calld->recv_message_ready->cb_arg,
                                  err);
  }
}

static void hs_mutate_op(grpc_exec_ctx *exec_ctx, grpc_call_element *elem,
                         grpc_transport_stream_op *op) {
  /* grab pointers to our data from the call element */
  call_data *calld = elem->call_data;

  if (op->send_initial_metadata != NULL && !calld->sent_status) {
    calld->sent_status = 1;
    grpc_metadata_batch_add_head(op->send_initial_metadata, &calld->status,
                                 GRPC_MDELEM_STATUS_200);
    grpc_metadata_batch_add_tail(
        op->send_initial_metadata, &calld->content_type,
        GRPC_MDELEM_CONTENT_TYPE_APPLICATION_SLASH_GRPC);
  }

  if (op->recv_initial_metadata) {
    /* substitute our callback for the higher callback */
    GPR_ASSERT(op->recv_idempotent_request != NULL);
    GPR_ASSERT(op->recv_cacheable_request != NULL);
    calld->recv_initial_metadata = op->recv_initial_metadata;
    calld->recv_idempotent_request = op->recv_idempotent_request;
    calld->recv_cacheable_request = op->recv_cacheable_request;
    calld->on_done_recv = op->recv_initial_metadata_ready;
    op->recv_initial_metadata_ready = &calld->hs_on_recv;
  }

  if (op->recv_message) {
    calld->recv_message_ready = op->recv_message_ready;
    calld->pp_recv_message = op->recv_message;
    if (op->recv_message_ready) {
      op->recv_message_ready = &calld->hs_recv_message_ready;
    }
    if (op->on_complete) {
      calld->on_complete = op->on_complete;
      op->on_complete = &calld->hs_on_complete;
    }
  }

  if (op->send_trailing_metadata) {
    grpc_metadata_batch_filter(exec_ctx, op->send_trailing_metadata,
                               server_filter_outgoing_metadata, elem);
  }
}

static void hs_start_transport_op(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem,
                                  grpc_transport_stream_op *op) {
  GRPC_CALL_LOG_OP(GPR_INFO, elem, op);
  GPR_TIMER_BEGIN("hs_start_transport_op", 0);
  hs_mutate_op(exec_ctx, elem, op);
  grpc_call_next_op(exec_ctx, elem, op);
  GPR_TIMER_END("hs_start_transport_op", 0);
}

/* Constructor for call_data */
static grpc_error *init_call_elem(grpc_exec_ctx *exec_ctx,
                                  grpc_call_element *elem,
                                  grpc_call_element_args *args) {
  /* grab pointers to our data from the call element */
  call_data *calld = elem->call_data;
  /* initialize members */
  memset(calld, 0, sizeof(*calld));
  grpc_closure_init(&calld->hs_on_recv, hs_on_recv, elem,
                    grpc_schedule_on_exec_ctx);
  grpc_closure_init(&calld->hs_on_complete, hs_on_complete, elem,
                    grpc_schedule_on_exec_ctx);
  grpc_closure_init(&calld->hs_recv_message_ready, hs_recv_message_ready, elem,
                    grpc_schedule_on_exec_ctx);
  grpc_slice_buffer_init(&calld->read_slice_buffer);
  return GRPC_ERROR_NONE;
}

/* Destructor for call_data */
static void destroy_call_elem(grpc_exec_ctx *exec_ctx, grpc_call_element *elem,
                              const grpc_call_final_info *final_info,
                              void *ignored) {
  call_data *calld = elem->call_data;
  grpc_slice_buffer_destroy_internal(exec_ctx, &calld->read_slice_buffer);
}

/* Constructor for channel_data */
static grpc_error *init_channel_elem(grpc_exec_ctx *exec_ctx,
                                     grpc_channel_element *elem,
                                     grpc_channel_element_args *args) {
  GPR_ASSERT(!args->is_last);
  return GRPC_ERROR_NONE;
}

/* Destructor for channel data */
static void destroy_channel_elem(grpc_exec_ctx *exec_ctx,
                                 grpc_channel_element *elem) {}

const grpc_channel_filter grpc_http_server_filter = {
    hs_start_transport_op,
    grpc_channel_next_op,
    sizeof(call_data),
    init_call_elem,
    grpc_call_stack_ignore_set_pollset_or_pollset_set,
    destroy_call_elem,
    sizeof(channel_data),
    init_channel_elem,
    destroy_channel_elem,
    grpc_call_next_get_peer,
    grpc_channel_next_get_info,
    "http-server"};
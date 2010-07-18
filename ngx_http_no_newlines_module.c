/*
 * This NginX module strips the HTML being served by NginX of all newline
 * ('\n', '\r') characters, thereby saving on the bandwidth
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* A context to store the current state of processing. */
typedef struct {
    unsigned char state;
} ngx_http_no_newlines_ctx_t;


/* Enum defining states required */
typedef enum {
    state_text = 0,
    state_abort,
    state_tag_pre
} ngx_http_no_newlines_state_e;

/* Header filter */
static ngx_int_t ngx_http_no_newlines_header_filter (ngx_http_request_t *r);
/* Body filter */
static ngx_int_t ngx_http_no_newlines_body_filter (ngx_http_request_t *r,
                                                   ngx_chain_t *in);
/* Init function for filter installation */
static ngx_int_t ngx_http_no_newlines_filter_init (ngx_conf_t *cf);

/* Worker functions */
static void ngx_http_no_newlines_strip_buffer (ngx_buf_t *buffer,
                                               ngx_http_no_newlines_ctx_t *ctx);
static void ngx_http_no_newlines_handle_tags (u_char *reader,
                                              u_char *writer,
                                              ngx_http_no_newlines_ctx_t *ctx);

static ngx_int_t is_tag_pre (u_char *reader);
static void ngx_http_no_newlines_ignore_preformatted_text (u_char *reader,
                                                           u_char *writer,
                                                           ngx_http_no_newlines_ctx_t *ctx);


/* Module directives */
static ngx_command_t  ngx_http_no_newlines_commands[] = {
    { ngx_string ("no_newlines"),
      NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS,
      NULL,
      0,
      0,
      NULL },

    ngx_null_command
};


/* The Module Context - for managing the configurations */
static ngx_http_module_t  ngx_http_no_newlines_module_ctx = {
    NULL,                             /* preconfiguration */
    ngx_http_no_newlines_filter_init, /* postconfiguration */

    NULL,                             /* create main configuration */
    NULL,                             /* init main configuration */

    NULL,                             /* create server configuration */
    NULL,                             /* merge server configuration */

    NULL,                             /* create location configuration */
    NULL                              /* merge location configuration */
};


/* The Module Definition - the master control */
ngx_module_t ngx_http_no_newlines_module = {
    NGX_MODULE_V1,
    &ngx_http_no_newlines_module_ctx, /* module context */
    ngx_http_no_newlines_commands,    /* module directives */
    NGX_HTTP_MODULE,                  /* module type */
    NULL,                             /* init master */
    NULL,                             /* init module */
    NULL,                             /* init process */
    NULL,                             /* init thread */
    NULL,                             /* exit thread */
    NULL,                             /* exit process */
    NULL,                             /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


/* Function definitions start here */
static ngx_int_t ngx_http_no_newlines_filter_init (ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_no_newlines_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_no_newlines_body_filter;

    return NGX_OK;
}


static ngx_int_t ngx_http_no_newlines_header_filter (ngx_http_request_t *r)
{
    ngx_http_no_newlines_ctx_t   *ctx;

    // step 1: decide whether to operate
    if ((r->headers_out.status != NGX_HTTP_OK &&
         r->headers_out.status != NGX_HTTP_FORBIDDEN &&
         r->headers_out.status != NGX_HTTP_NOT_FOUND) ||
        r->header_only ||
        r->headers_out.content_type.len == 0 ||
        (r->headers_out.content_encoding &&
         r->headers_out.content_encoding->value.len)) {
        // No need to filter
        return ngx_http_next_header_filter(r);
    }

    if (ngx_strncasecmp(r->headers_out.content_type.data, (u_char *)"text/html", sizeof("text/html" - 1)) != 0) {
        // No need to filter
        return ngx_http_next_header_filter(r);
    }

    /* step 2: operate on the header */
    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_no_newlines_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_no_newlines_module);

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    r->main_filter_need_in_memory = 1; //What does this do? -VM

    /* step 3: call the next filter */
    return ngx_http_next_header_filter(r);
}

static ngx_int_t ngx_http_no_newlines_body_filter (ngx_http_request_t *r,
                                                   ngx_chain_t *in)
{
    ngx_http_no_newlines_ctx_t *ctx;
    ngx_chain_t *chain_link;

    /* Get the current context */
    ctx = ngx_http_get_module_ctx (r, ngx_http_no_newlines_module);

    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    /* Set initial state to text */
    ctx->state = state_text;

    /* For each buffer in the chain link, remove all the newlines */
    for (chain_link = in; chain_link; chain_link = chain_link->next) {
        ngx_http_no_newlines_strip_buffer (chain_link->buf, ctx);
    }

    /* Pass the chain to the next output filter */
    return ngx_http_next_body_filter(r, in);
}

static void ngx_http_no_newlines_strip_buffer (ngx_buf_t *buffer,
                                               ngx_http_no_newlines_ctx_t *ctx)
{
    u_char *reader;
    u_char *writer;

    for (writer = buffer->pos, reader = buffer->pos; reader < buffer->last; reader++) {
        switch(ctx->state) {
        case state_text:
            switch(*reader) {
            case '\r':
            case '\n':
                continue;
            case '<':
                ngx_http_no_newlines_handle_tags (reader, writer, ctx);
                break;
            default:
                break;
            }
            break;

        case state_tag_pre:
            //ignore newlines while we are displaying pre-formatted text
            ngx_http_no_newlines_ignore_preformatted_text (reader, writer, ctx);
            break;

        case state_abort:
        default:
            break;
        }

        *writer++ = *reader;
    }
    buffer->last = writer;
}

static void ngx_http_no_newlines_handle_tags (u_char *reader,
                                              u_char *writer,
                                              ngx_http_no_newlines_ctx_t *ctx)
{
    u_char *t = NULL;
    int i = 0;

    *writer++ = *reader++; //Write the opening angle and move on.
    t = reader;
    if (is_tag_pre (t)) {
        ctx->state = state_tag_pre;
        //Write into writer and bring it to closing angle.
        for (i = 0; i < 3; i++) {
            *writer++ = *reader++;
        }
    }
}

static ngx_int_t is_tag_pre (u_char *reader)
{
    if (ngx_strncasecmp (reader, (u_char *)"pre", sizeof ("pre" - 1)) == 0) {
        return 1;
    }
    return 0;
}


static void ngx_http_no_newlines_ignore_preformatted_text (u_char *reader,
                                                           u_char *writer,
                                                           ngx_http_no_newlines_ctx_t *ctx)
{
    u_char *t = NULL;
    int i = 0;

    do {
        while (*reader != '/') {
            *writer++ = *reader++;
        }
        *writer++ = *reader++; //write the front-slash and move on.
        t = reader;
    } while (is_tag_pre (t) != 1);

    //We have found the pre-tag. Write to writer and switch states.
    for (i = 0; i < 3; i++) {
        *writer++ = *reader++; //Bring it to closing angle.
    }

    ctx->state = state_text;
}


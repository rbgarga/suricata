/**Copyright (c) 2009 Open Information Security Foundation
 *
 * \author Gurvinder Singh <gurvindersinghdahiya@gmail.com>
 *
 * Stream size for the engine.
 */

#include <pcre.h>
#include <string.h>

#include "eidps.h"
#include "stream-tcp.h"
#include "util-unittest.h"
#include "detect.h"
#include "flow.h"
#include "detect-stream_size.h"
#include "stream-tcp-private.h"

/** XXX GS define it properly!!
 * \brief Regex for parsing our flow options
 */
#define PARSE_REGEX  "^\\s*([^\\s,]+)\\s*,\\s*([^\\s,]+)\\s*,([0-9]+)\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectStreamSizeMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
int DetectStreamSizeSetup (DetectEngineCtx *, Signature *, SigMatch *, char *);
void DetectStreamSizeFree(void *);
void DetectStreamSizeRegisterTests(void);

void DetectStreamSizeRegister(void) {
    sigmatch_table[DETECT_STREAM_SIZE].name = "stream_size";
    sigmatch_table[DETECT_STREAM_SIZE].Match = DetectStreamSizeMatch;
    sigmatch_table[DETECT_STREAM_SIZE].Setup = DetectStreamSizeSetup;
    sigmatch_table[DETECT_STREAM_SIZE].Free = DetectStreamSizeFree;
    sigmatch_table[DETECT_STREAM_SIZE].RegisterTests = DetectStreamSizeRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if (parse_regex == NULL) {
        printf("pcre compile of \"%s\" failed at offset %" PRId32 ": %s\n", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if (eb != NULL) {
        printf("pcre study failed: %s\n", eb);
        goto error;
    }
    return;

error:
    if (parse_regex != NULL) free(parse_regex);
    if (parse_regex_study != NULL) free(parse_regex_study);
    return;
}


static int DetectStreamSizeCompare (uint32_t diff, uint32_t stream_size, uint8_t mode) {

    int ret = 0;
    switch(mode) {
            case DETECTSSIZE_LT:
                if (diff < stream_size)
                    ret = 1;
                break;
            case DETECTSSIZE_LEQ:
                if (diff <= stream_size)
                    ret = 1;
                break;
            case DETECTSSIZE_EQ:
                if (diff == stream_size)
                    ret = 1;
                break;
            case DETECTSSIZE_NEQ:
                if (diff != stream_size)
                    ret = 1;
                break;
            case DETECTSSIZE_GEQ:
                if (diff >= stream_size)
                    ret = 1;
                break;
            case DETECTSSIZE_GT:
                if (diff > stream_size)
                    ret = 1;
                break;
    }

    return ret;
}

/**
 * \brief This function is used to match Stream size rule option on a packet with those passed via stream_size:
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectStreamSizeData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int DetectStreamSizeMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *m) {

    int ret = 0;
    DetectStreamSizeData *sd = (DetectStreamSizeData *) m->ctx;

    if (p->ip4h == NULL)
        return ret;
    if (sd == NULL)
        printf("hello\n");
    uint32_t csdiff = 0;
    uint32_t ssdiff = 0;
    TcpSession *ssn = (TcpSession *)p->flow->stream;

    if (ssn != NULL) {
        csdiff = ssn->client.next_seq - ssn->client.last_ack;
        ssdiff = ssn->server.next_seq - ssn->server.last_ack;
    } else
        return ret;

    if (sd->flags & STREAM_SIZE_SERVER) {
        ret = DetectStreamSizeCompare(ssdiff, sd->ssize, sd->mode);
    } else if (sd->flags & STREAM_SIZE_CLIENT) {
        ret = DetectStreamSizeCompare(csdiff, sd->ssize, sd->mode);
    } else if (sd->flags & STREAM_SIZE_BOTH) {
        if (DetectStreamSizeCompare(ssdiff, sd->ssize, sd->mode) && DetectStreamSizeCompare(csdiff, sd->ssize, sd->mode))
            ret = 1;
    } else if (sd->flags & STREAM_SIZE_EITHER) {
        if (DetectStreamSizeCompare(ssdiff, sd->ssize, sd->mode) || DetectStreamSizeCompare(csdiff, sd->ssize, sd->mode))
            ret = 1;
    }

    return ret;
}

DetectStreamSizeData *DetectStreamSizeParse (char *streamstr) {

    DetectStreamSizeData *sd = NULL;
    char *arg = NULL;
    char *value = NULL;
    char *mode = NULL;
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study, streamstr, strlen(streamstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret != 3) {
        printf("DetectStreamSizeSetup: parse error, ret %" PRId32 "\n", ret);
        goto error;
    }

    const char *str_ptr;

    res = pcre_get_substring((char *)streamstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
    if (res < 0) {
        printf("DetectStreamSizeSetup: pcre_get_substring failed\n");
        goto error;
    }
    arg = (char *)str_ptr;

    res = pcre_get_substring((char *)streamstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
    if (res < 0) {
        printf("DetectDsizeSetup: pcre_get_substring failed\n");
        goto error;
    }
    mode = (char *)str_ptr;

    res = pcre_get_substring((char *)streamstr, ov, MAX_SUBSTRINGS, 3, &str_ptr);
    if (res < 0) {
        printf("DetectDsizeSetup: pcre_get_substring failed\n");
        goto error;
    }
    value = (char *)str_ptr;

    sd = malloc(sizeof(DetectStreamSizeData));
    if (sd == NULL) {
        printf("DetectStreamSizeSetup malloc failed\n");
        goto error;
    }
    sd->ssize = 0;
    sd->flags = 0;

    if(mode[0] == '<') sd->mode = DETECTSSIZE_LT;
    else if(strcmp("<=", mode) == 0) sd->mode = DETECTSSIZE_LEQ;
    else if (mode[0] == '>') sd->mode = DETECTSSIZE_GT;
    else if(strcmp(">=", mode)) sd->mode = DETECTSSIZE_GEQ;
    else if(strcmp("!=", mode)) sd->mode = DETECTSSIZE_NEQ;
    else sd->mode = DETECTSSIZE_EQ;

    /* set the value */
    sd->ssize = (uint16_t)atoi(value);

    if (strcmp(arg, "server") == 0) {
        if (sd->flags & STREAM_SIZE_SERVER) {
            printf("DetectFlowParse error STREAM_SIZE_SERVER flag is already set \n");
            goto error;
        }
        sd->flags |= STREAM_SIZE_SERVER;

    } else if (strcmp(arg, "client") == 0) {

        if (sd->flags & STREAM_SIZE_CLIENT) {
            printf("DetectFlowParse error STREAM_SIZE_CLIENT flag is already set \n");
            goto error;
        }
        sd->flags |= STREAM_SIZE_CLIENT;

    } else if ((strcmp(arg, "both") == 0)) {

        if (sd->flags & STREAM_SIZE_SERVER || sd->flags & STREAM_SIZE_CLIENT) {
            printf("DetectFlowParse error STREAM_SIZE_SERVER or STREAM_SIZE_CLIENT flag is already set \n");
            goto error;
        }
        sd->flags |= STREAM_SIZE_BOTH;
    } else if (strcmp(arg, "either") == 0) {

        if (sd->flags & STREAM_SIZE_SERVER || sd->flags & STREAM_SIZE_CLIENT) {
            printf("DetectFlowParse error STREAM_SIZE_SERVER or STREAM_SIZE_CLIENT flag is already set \n");
            goto error;
        }
        sd->flags |= STREAM_SIZE_EITHER;

    }

    if (mode != NULL) free(mode);
    if (arg != NULL) free(arg);
    if (value != NULL) free(value);
    return sd;

error:
    if (mode != NULL) free(mode);
    if (arg != NULL) free(arg);
    if (value != NULL) free(value);
    if (sd != NULL) DetectStreamSizeFree(sd);

    return NULL;
}

/**
 * \brief this function is used to add the parsed stream size data into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param m pointer to the Current SigMatch
 * \param streamstr pointer to the user provided stream size options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectStreamSizeSetup (DetectEngineCtx *de_ctx, Signature *s, SigMatch *m, char *streamstr) {

    DetectStreamSizeData *sd = NULL;
    SigMatch *sm = NULL;

    sd = DetectStreamSizeParse(streamstr);
    if (sd == NULL)
        goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_STREAM_SIZE;
    sm->ctx = (void *)sd;

    SigMatchAppend(s,m,sm);

    return 0;

error:
    if (sd != NULL) DetectStreamSizeFree(sd);
    if (sm != NULL) free(sm);
    return -1;
}

/**
 * \brief this function will free memory associated with DetectStreamSizeData
 *
 * \param ptr pointer to DetectStreamSizeData
 */
void DetectStreamSizeFree(void *ptr) {
    DetectStreamSizeData *sd = (DetectStreamSizeData *)ptr;
    free(sd);
}

static int DetectStreamSizeParseTest01 (void) {
    int result = 0;
    DetectStreamSizeData *sd = NULL;
    sd = DetectStreamSizeParse("server,<,6");
    if (sd != NULL) {
        if (sd->flags & STREAM_SIZE_SERVER && sd->mode == DETECTSSIZE_LT && sd->ssize == 6)
            result = 1;
        DetectStreamSizeFree(sd);
    }

    return result;
}

static int DetectStreamSizeParseTest02 (void) {
    int result = 1;
    DetectStreamSizeData *sd = NULL;
    sd = DetectStreamSizeParse("invalidoption,<,6");
    if (sd != NULL) {
        printf("expected: NULL got 0x%02X %" PRId16 ": ",sd->flags, sd->ssize);
        result = 0;
        DetectStreamSizeFree(sd);
    }

    return result;
}

static int DetectStreamSizeParseTest03 (void) {

    int result = 0;
    DetectStreamSizeData *sd = NULL;
    TcpSession ssn;
    ThreadVars tv;
    DetectEngineThreadCtx dtx;
    Packet p;
    Signature s;
    SigMatch sm;
    TcpStream client;
    Flow f;
    IPV4Hdr ip4h;

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&tv, 0, sizeof(ThreadVars));
    memset(&dtx, 0, sizeof(DetectEngineThreadCtx));
    memset(&p, 0, sizeof(Packet));
    memset(&s, 0, sizeof(Signature));
    memset(&sm, 0, sizeof(SigMatch));
    memset(&client, 0, sizeof(TcpStream));
    memset(&f, 0, sizeof(Flow));
    memset(&ip4h, 0, sizeof(IPV4Hdr));

    sd = DetectStreamSizeParse("client,>,8");
    if (sd != NULL) {
        if (!(sd->flags & STREAM_SIZE_CLIENT) && sd->mode != DETECTSSIZE_GT && sd->ssize != 8)
            return 0;
    }

    client.last_ack = 20;
    client.next_seq = 30;
    ssn.client = client;
    f.stream = &ssn;
    p.flow = &f;
    p.ip4h = &ip4h;
    sm.ctx = sd;

    //result = DetectStreamSizeMatch(&tv, &dtx, &p, &s, &sm);

    return result;
}

static int DetectStreamSizeParseTest04 (void) {

    int result = 0;
    DetectStreamSizeData *sd = NULL;
    TcpSession ssn;
    ThreadVars tv;
    DetectEngineThreadCtx dtx;
    Packet p;
    Signature s;
    SigMatch sm;
    TcpStream client;
    Flow f;
    IPV4Hdr ip4h;

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&tv, 0, sizeof(ThreadVars));
    memset(&dtx, 0, sizeof(DetectEngineThreadCtx));
    memset(&p, 0, sizeof(Packet));
    memset(&s, 0, sizeof(Signature));
    memset(&sm, 0, sizeof(SigMatch));
    memset(&client, 0, sizeof(TcpStream));
    memset(&f, 0, sizeof(Flow));
    memset(&ip4h, 0, sizeof(IPV4Hdr));

    sd = DetectStreamSizeParse("client,>,8");
    if (sd != NULL) {
        if (!(sd->flags & STREAM_SIZE_CLIENT) && sd->mode != DETECTSSIZE_GT && sd->ssize != 8)
            return 0;
    }

    client.last_ack = 20;
    client.next_seq = 28;
    ssn.client = client;
    f.stream = &ssn;
    p.flow = &f;
    p.ip4h = &ip4h;
    sm.ctx = sd;

    //if (!DetectStreamSizeMatch(&tv, &dtx, &p, &s, &sm))
        result = 1;

    return result;
}

void DetectStreamSizeRegisterTests(void) {

    UtRegisterTest("DetectStreamSizeParseTest01", DetectStreamSizeParseTest01, 1);
    UtRegisterTest("DetectStreamSizeParseTest02", DetectStreamSizeParseTest02, 1);
    UtRegisterTest("DetectStreamSizeParseTest03", DetectStreamSizeParseTest03, 1);
    UtRegisterTest("DetectStreamSizeParseTest04", DetectStreamSizeParseTest04, 1);
}
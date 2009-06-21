/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2008-2009 Joshua Haberman.  See LICENSE for details.
 */

#define INLINE
#include "upb_parse.h"

#include <assert.h>
#include <string.h>
#include "descriptor.h"

/* Branch prediction hints for GCC. */
#ifdef __GNUC__
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#else
#define likely(x)       (x)
#define unlikely(x)     (x)
#endif

#define CHECK(func) do { \
  upb_status_t status = func; \
  if(status != UPB_STATUS_OK) return status; \
  } while (0)

/* Lowest-level functions -- these read integers from the input buffer.
 * To avoid branches, none of these do bounds checking.  So we force clients
 * to overallocate their buffers by >=9 bytes. */

static upb_status_t get_v_uint64_t(void *restrict *buf,
                                   uint64_t *restrict val)
{
  uint8_t *ptr = *buf, b;
  uint32_t part0 = 0, part1 = 0, part2 = 0;

  /* From the original proto2 implementation. */
  b = *(ptr++); part0  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
  b = *(ptr++); part0 |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
  b = *(ptr++); part0 |= (b & 0x7F) << 14; if (!(b & 0x80)) goto done;
  b = *(ptr++); part0 |= (b & 0x7F) << 21; if (!(b & 0x80)) goto done;
  b = *(ptr++); part1  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
  b = *(ptr++); part1 |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
  b = *(ptr++); part1 |= (b & 0x7F) << 14; if (!(b & 0x80)) goto done;
  b = *(ptr++); part1 |= (b & 0x7F) << 21; if (!(b & 0x80)) goto done;
  b = *(ptr++); part2  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
  b = *(ptr++); part2 |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
  return UPB_ERROR_UNTERMINATED_VARINT;

done:
  *buf = ptr;
  *val = (uint64_t)part0 | ((uint64_t)part1 << 28) | ((uint64_t)part2 << 56);
  return UPB_STATUS_OK;
}

static upb_status_t skip_v_uint64_t(void **buf)
{
  uint8_t *ptr = *buf, b;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  b = *(ptr++); if (!(b & 0x80)) goto done;
  return UPB_ERROR_UNTERMINATED_VARINT;

done:
  *buf = (uint8_t*)ptr;
  return UPB_STATUS_OK;
}

static upb_status_t get_v_uint32_t(void *restrict *buf,
                                   uint32_t *restrict val)
{
  uint8_t *ptr = *buf, b;
  uint32_t result;

  /* From the original proto2 implementation. */
  b = *(ptr++); result  = (b & 0x7F)      ; if (!(b & 0x80)) goto done;
  b = *(ptr++); result |= (b & 0x7F) <<  7; if (!(b & 0x80)) goto done;
  b = *(ptr++); result |= (b & 0x7F) << 14; if (!(b & 0x80)) goto done;
  b = *(ptr++); result |= (b & 0x7F) << 21; if (!(b & 0x80)) goto done;
  b = *(ptr++); result  = (b & 0x7F) << 28; if (!(b & 0x80)) goto done;
  return UPB_ERROR_UNTERMINATED_VARINT;

done:
  *buf = ptr;
  *val = result;
  return UPB_STATUS_OK;
}

#define SHL(val, bits) ((uint32_t)val << bits)
static upb_status_t get_f_uint32_t(void *restrict *buf,
                                   uint32_t *restrict val)
{
  uint8_t *b = *buf;
#if UPB_UNALIGNED_READS_OK
  *val = *(uint32_t*)b;
#else
  *val = SHL(b[0], 0) | SHL(b[1], 8) | SHL(b[2], 16) | SHL(b[3], 24);
#endif
  b += sizeof(uint32_t);
  *buf = b;
  return UPB_STATUS_OK;
}
#undef SHL

static upb_status_t skip_f_uint32_t(void **buf)
{
  *buf = (char*)*buf + sizeof(uint32_t);
  return UPB_STATUS_OK;
}

static upb_status_t get_f_uint64_t(void *restrict *buf,
                                   uint64_t *restrict val)
{
#if UPB_UNALIGNED_READS_OK
  *val = *(uint64_t*)*buf;
  *buf = (char*)*buf + sizeof(uint64_t);
#else
  uint32_t lo32, hi32;
  get_f_uint32_t(buf, &lo32);
  get_f_uint32_t(buf, &hi32);
  *val = lo32 | ((uint64_t)hi32 << 32);
#endif
  return UPB_STATUS_OK;
}

static upb_status_t skip_f_uint64_t(void **buf)
{
  *buf = (char*)*buf + sizeof(uint64_t);
  return UPB_STATUS_OK;
}

static int32_t zz_decode_32(uint32_t n) { return (n >> 1) ^ -(int32_t)(n & 1); }
static int64_t zz_decode_64(uint64_t n) { return (n >> 1) ^ -(int64_t)(n & 1); }

/* Functions for reading wire values and converting them to values.  These
 * are generated with macros because they follow a higly consistent pattern. */

#define WVTOV(type, wire_t, val_t) \
  static void wvtov_ ## type(wire_t s, val_t *d)

#define GET(type, v_or_f, wire_t, val_t, member_name) \
  static upb_status_t get_ ## type(void **buf, union upb_value *d) { \
    wire_t tmp; \
    CHECK(get_ ## v_or_f ## _ ## wire_t(buf, &tmp)); \
    wvtov_ ## type(tmp, &d->member_name); \
    return UPB_STATUS_OK; \
  }

#define T(type, v_or_f, wire_t, val_t, member_name) \
  WVTOV(type, wire_t, val_t);  /* prototype for GET below */ \
  GET(type, v_or_f, wire_t, val_t, member_name) \
  WVTOV(type, wire_t, val_t)

T(DOUBLE,   f, uint64_t, double,   _double) { memcpy(d, &s, sizeof(double)); }
T(FLOAT,    f, uint32_t, float,    _float)  { memcpy(d, &s, sizeof(float));  }
T(INT32,    v, uint32_t, int32_t,  int32)   { *d = (int32_t)s;               }
T(INT64,    v, uint64_t, int64_t,  int64)   { *d = (int64_t)s;               }
T(UINT32,   v, uint32_t, uint32_t, uint32)  { *d = s;                        }
T(UINT64,   v, uint64_t, uint64_t, uint64)  { *d = s;                        }
T(SINT32,   v, uint32_t, int32_t,  int32)   { *d = zz_decode_32(s);          }
T(SINT64,   v, uint64_t, int64_t,  int64)   { *d = zz_decode_64(s);          }
T(FIXED32,  f, uint32_t, uint32_t, uint32)  { *d = s;                        }
T(FIXED64,  f, uint64_t, uint64_t, uint64)  { *d = s;                        }
T(SFIXED32, f, uint32_t, int32_t,  int32)   { *d = (int32_t)s;               }
T(SFIXED64, f, uint64_t, int64_t,  int64)   { *d = (int64_t)s;               }
T(BOOL,     v, uint32_t, bool,     _bool)   { *d = (bool)s;                  }
T(ENUM,     v, uint32_t, int32_t,  int32)   { *d = (int32_t)s;               }
#undef WVTOV
#undef GET
#undef T

upb_wire_type_t upb_expected_wire_types[] = {
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_DOUBLE] = UPB_WIRE_TYPE_64BIT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_FLOAT] = UPB_WIRE_TYPE_32BIT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_INT64] = UPB_WIRE_TYPE_VARINT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_UINT64] = UPB_WIRE_TYPE_VARINT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_INT32] = UPB_WIRE_TYPE_VARINT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_FIXED64] = UPB_WIRE_TYPE_64BIT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_FIXED32] = UPB_WIRE_TYPE_32BIT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_BOOL] = UPB_WIRE_TYPE_VARINT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_STRING] = UPB_WIRE_TYPE_DELIMITED,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_BYTES] = UPB_WIRE_TYPE_DELIMITED,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_GROUP] = -1,  /* TODO */
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_MESSAGE] = UPB_WIRE_TYPE_DELIMITED,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_UINT32] = UPB_WIRE_TYPE_VARINT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_ENUM] = UPB_WIRE_TYPE_VARINT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_SFIXED32] = UPB_WIRE_TYPE_32BIT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_SFIXED64] = UPB_WIRE_TYPE_64BIT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_SINT32] = UPB_WIRE_TYPE_VARINT,
  [GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_SINT64] = UPB_WIRE_TYPE_VARINT,
};

upb_status_t upb_parse_tag(void **buf, struct upb_tag *tag)
{
  uint32_t tag_int;
  CHECK(get_v_uint32_t(buf, &tag_int));
  tag->wire_type    = (upb_wire_type_t)(tag_int & 0x07);
  tag->field_number = tag_int >> 3;
  return UPB_STATUS_OK;
}

upb_status_t upb_parse_wire_value(void *buf, size_t *offset,
                                  upb_wire_type_t wt,
                                  union upb_wire_value *wv)
{
#define READ(expr) CHECK(expr); *offset += ((char*)b-(char*)buf)
  void *b = buf;
  switch(wt) {
    case UPB_WIRE_TYPE_VARINT: READ(get_v_uint64_t(&b, &wv->varint)); break;
    case UPB_WIRE_TYPE_64BIT: READ(get_f_uint64_t(&b, &wv->_64bit)); break;
    case UPB_WIRE_TYPE_32BIT: READ(get_f_uint32_t(&b, &wv->_32bit)); break;
    case UPB_WIRE_TYPE_DELIMITED:
      READ(get_v_uint32_t(&b, &wv->_32bit));
      size_t new_offset = *offset + wv->_32bit;
      if (new_offset < *offset) return UPB_ERROR_OVERFLOW;
      *offset += new_offset;
      break;
    case UPB_WIRE_TYPE_START_GROUP:
    case UPB_WIRE_TYPE_END_GROUP: break;
  }
  return UPB_STATUS_OK;
}

upb_status_t upb_skip_wire_value(void *buf, size_t *offset,
                                 upb_wire_type_t wt)
{
  void *b = buf;
  switch(wt) {
    case UPB_WIRE_TYPE_VARINT: READ(skip_v_uint64_t(&b)); break;
    case UPB_WIRE_TYPE_64BIT: READ(skip_f_uint64_t(&b)); break;
    case UPB_WIRE_TYPE_32BIT: READ(skip_f_uint32_t(&b)); break;
    case UPB_WIRE_TYPE_DELIMITED: {
      /* Have to get (not skip) the length to skip the bytes. */
      uint32_t len;
      READ(get_v_uint32_t(&b, &len));
      size_t new_offset = *offset + len;
      if (new_offset < *offset) return UPB_ERROR_OVERFLOW;
      *offset += new_offset;
      break;
    }
    case UPB_WIRE_TYPE_START_GROUP: /* TODO: skip to matching end group. */
    case UPB_WIRE_TYPE_END_GROUP: break;
  }
  return UPB_STATUS_OK;
#undef READ
}

upb_status_t upb_parse_value(void **b, upb_field_type_t ft,
                             union upb_value *v)
{
#define CASE(t) \
  case GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_ ## t: return get_ ## t(b, v);
  switch(ft) {
    CASE(DOUBLE) CASE(FLOAT) CASE(INT64) CASE(UINT64) CASE(INT32) CASE(FIXED64)
    CASE(FIXED32) CASE(BOOL) CASE(UINT32) CASE(ENUM) CASE(SFIXED32)
    CASE(SFIXED64) CASE(SINT32) CASE(SINT64)
    case GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_BYTES:
    case GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_STRING:
    case GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_MESSAGE:
      return get_UINT32(b, v);
    default: return 0;  /* Including GROUP -- groups have no value. */
  }
#undef CASE
}

static void pop_stack_frame(struct upb_parse_state *s)
{
  s->submsg_end_cb(s);
  s->top--;
  s->top = (struct upb_parse_stack_frame*)((char*)s->top - s->udata_size);
}

static upb_status_t push_stack_frame(struct upb_parse_state *s, size_t end,
                                     void *user_field_desc)
{
  s->top++;
  s->top = (struct upb_parse_stack_frame*)((char*)s->top + s->udata_size);
  if(unlikely(s->top > s->limit)) return UPB_ERROR_STACK_OVERFLOW;
  s->top->end_offset = end;
  s->submsg_start_cb(s, user_field_desc);
  return UPB_STATUS_OK;
}

upb_status_t upb_parse(struct upb_parse_state *s, void *buf, size_t len,
                       size_t *read)
{
  size_t start_offset = s->offset;
  size_t end_offset = start_offset + len;
  while(!s->done && s->offset < end_offset) {
    while(s->offset >= s->top->end_offset) pop_stack_frame(s);
    while(s->packed_end_offset > s->offset) {
      /* Parse a packed field entry. */
    }

    struct upb_tag tag;
    void *b = buf;
    CHECK(upb_parse_tag(&b, &tag));
    int tag_bytes = ((char*)b - (char*)buf);
    s->offset += tag_bytes;
    buf = b;
    if(unlikely(tag.wire_type == UPB_WIRE_TYPE_END_GROUP)) {
      if(unlikely(s->top->end_offset != 0)) return UPB_ERROR_SPURIOUS_END_GROUP;
      pop_stack_frame(s);
      continue;
    }

    void *user_field_desc;
    upb_field_type_t ft = s->tag_cb(s, &tag, &user_field_desc);
    if(ft == 0) {
      CHECK(upb_skip_wire_value(b, &s->offset, tag.wire_type));
    } else if(ft == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_GROUP) {
      /* No length specified, an "end group" tag will mark the end. */
      push_stack_frame(s, 0, user_field_desc);
    } else {
      /* For all other cases we parse the next value. */
      union upb_value v;
      CHECK(upb_parse_value(&b, ft, &v));
      if(ft == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_MESSAGE) {
        /* The value we parsed is the length of the submessage. */
        push_stack_frame(s, s->offset + v.delim_len, user_field_desc);
      } else if(ft == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_STRING ||
                ft == GOOGLE_PROTOBUF_FIELDDESCRIPTORPROTO_TYPE_BYTES) {
        s->value_cb(s, &v, b, user_field_desc);
        b = (char*)b + v.delim_len;
      } else if(tag.wire_type == UPB_WIRE_TYPE_DELIMITED) {
        /* Delimited data which is not a string, bytes, or a submessage.
         * It must be a packed array. */
        s->packed_type = ft;
        s->packed_end_offset = s->offset + v.delim_len;
      } else {
        /* The common case: a simple value. */
        CHECK(upb_parse_value(&b, ft, &v));
        s->value_cb(s, &v, b, user_field_desc);
      }
    }
  }
  *read = s->offset - start_offset;
  return UPB_STATUS_OK;
}

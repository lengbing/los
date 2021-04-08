#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>


#ifdef _WIN32
#define LUA_MOD_EXPORT __declspec(dllexport)
#else
#define LUA_MOD_EXPORT extern
#endif

#define ENDIAN_LE 1
#define ENDIAN_BE 2

#define LOS_ETYPE -1
#define LOS_ESIGN -2
#define LOS_EBUF  -3
#define LOS_ESRC  -4
#define LOS_ESTR  -5
#define LOS_EFMT  -6

#define SIGN_FLT    0xf0
#define SIGN_INT1   0xf1
#define SIGN_INT2   0xf2
#define SIGN_INT4   0xf3
#define SIGN_INT8   0xf4
#define SIGN_STR1   0xf5
#define SIGN_STR2   0xf6
#define SIGN_STR4   0xf7
#define SIGN_NIL    0xf8
#define SIGN_FALSE  0xf9
#define SIGN_TRUE   0xfa
#define SIGN_TBLBEG 0xfb
#define SIGN_TBLSEP 0xfc
#define SIGN_TBLEND 0xfd
#define SIGN_SHRSTR 0xc0
#define MASK_SHRINT 0xc0
#define MASK_SHRSTR 0xe0

#define IS_SHRINT(v) (((v) & MASK_SHRINT) != 0xc0)
#define IS_SHRSTR(v) (((v) & MASK_SHRSTR) == 0xc0)

#define swap16(x) ((uint16_t)( \
    (((uint16_t)(x) & 0xff00U) >> 8) | \
    (((uint16_t)(x) & 0x00ffU) << 8)))

#define swap32(x) ((uint32_t)( \
    (((uint32_t)(x) & 0xff000000U) >> 24) | \
    (((uint32_t)(x) & 0x00ff0000U) >>  8) | \
    (((uint32_t)(x) & 0x0000ff00U) <<  8) | \
    (((uint32_t)(x) & 0x000000ffU) << 24)))

#define swap64(x) ((uint64_t)( \
    (((uint64_t)(x) & 0xff00000000000000ULL) >> 56) | \
    (((uint64_t)(x) & 0x00ff000000000000ULL) >> 40) | \
    (((uint64_t)(x) & 0x0000ff0000000000ULL) >> 24) | \
    (((uint64_t)(x) & 0x000000ff00000000ULL) >>  8) | \
    (((uint64_t)(x) & 0x00000000ff000000ULL) <<  8) | \
    (((uint64_t)(x) & 0x0000000000ff0000ULL) << 24) | \
    (((uint64_t)(x) & 0x000000000000ff00ULL) << 40) | \
    (((uint64_t)(x) & 0x00000000000000ffULL) << 56)))

typedef union ucast
{
    double   f;
    int64_t  i64;
    uint64_t u64;
    int32_t  i32[2];
    uint32_t u32[2];
    int16_t  i16[4];
    uint16_t u16[4];
    int8_t   i8[8];
    uint8_t  u8[8];
} ucast;

#define los_try(E) do {          \
    int err = setjmp(E);         \
    if (err != 0) {              \
        lua_pushinteger(L, err); \
        return 1;                \
    }                            \
} while (0)

#define los_throw(E, err) longjmp(E, err)

#define checkbuflen(len, need, err) do {if ((len) < (need)) {los_throw(E, err);}} while (0)
#define checkdestlen(len, need) checkbuflen(len, need, LOS_EBUF)
#define checksrclen(len, need) checkbuflen(len, need, LOS_ESRC)

static lua_CFunction str_format = NULL;


static size_t dump(jmp_buf E, lua_State* L, luaL_Buffer* B)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        luaL_addchar(B, SIGN_NIL);
        return 1;
    }
    case LUA_TBOOLEAN: {
        luaL_addchar(B, lua_toboolean(L, -1) ? SIGN_TRUE : SIGN_FALSE);
        return 1;
    }
    case LUA_TNUMBER: {
        if (lua_isinteger(L, -1)) {
            int64_t v = lua_tointeger(L, -1);
            if (-63 <= v && v <= 127) {
                int8_t i = (int8_t)v;
                luaL_addchar(B, i);
                return 1;
            }
            else if (INT8_MIN <= v && v <= INT8_MAX) {
                int8_t i = (int8_t)v;
                luaL_addchar(B, SIGN_INT1);
                luaL_addchar(B, i);
                return 2;
            }
            else if (INT16_MIN <= v && v <= INT16_MAX) {
                int16_t i = (int16_t)v;
                luaL_addchar(B, SIGN_INT2);
                luaL_addlstring(B, (const char*)&i, 2);
                return 3;
            }
            else if (INT32_MIN <= v && v <= INT32_MAX) {
                int32_t i = (int32_t)v;
                luaL_addchar(B, SIGN_INT4);
                luaL_addlstring(B, (const char*)&i, 4);
                return 5;
            }
            else {
                luaL_addchar(B, SIGN_INT8);
                luaL_addlstring(B, (const char*)&v, 8);
                return 9;
            }
        }
        else {
            double v = lua_tonumber(L, -1);
            luaL_addchar(B, SIGN_FLT);
            luaL_addlstring(B, (const char*)&v, 8);
            return 9;
        }
    }
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, -1, &len);
        size_t size = len;
        if (len <= 31) {
            uint8_t c = (uint8_t)len;
            c |= SIGN_SHRSTR;
            luaL_addchar(B, c);
            size += 1;
        }
        else if (len <= UINT8_MAX) {
            uint8_t c = (uint8_t)len;
            luaL_addchar(B, SIGN_STR1);
            luaL_addchar(B, c);
            size += 2;
        }
        else if (len <= UINT16_MAX) {
            luaL_addchar(B, SIGN_STR2);
            luaL_addlstring(B, (const char*)&len, 2);
            size += 3;
        }
        else if (len <= UINT32_MAX) {
            luaL_addchar(B, SIGN_STR4);
            luaL_addlstring(B, (const char*)&len, 4);
            size += 5;
        }
        else {
            los_throw(E, LOS_ESTR);
        }
        luaL_addlstring(B, s, len);
        return size;
    }
    case LUA_TTABLE: {
        luaL_addchar(B, SIGN_TBLBEG);
        size_t size = 1;
        size_t len = lua_rawlen(L, -1);
        size_t numnil = 0;
        for (size_t i = 1; i <= len; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TNIL) {
                ++numnil;
            }
            else {
                for (size_t j = 0; j < numnil; ++j) {
                    luaL_addchar(B, SIGN_NIL);
                }
                size += numnil;
                numnil = 0;
                size += dump(E, L, B);
            }
            lua_pop(L, 1);
        }
        luaL_addchar(B, SIGN_TBLSEP);
        ++size;
        int top = lua_gettop(L);
        if (len > 0) {
            size_t lastkey = len - numnil;
            lua_pushinteger(L, lastkey);
        }
        else {
            lua_pushnil(L);
        }
        while (lua_next(L, -2)) {
            size += dump(E, L, B);
            lua_pop(L, 1);
            size += dump(E, L, B);
        }
        lua_settop(L, top);
        luaL_addchar(B, SIGN_TBLEND);
        ++size;
        return size;
    }
    default: {
        los_throw(E, LOS_ETYPE);
    }
    }
    return 0;
}


static size_t dump_x(jmp_buf E, lua_State* L, luaL_Buffer* B)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        luaL_addchar(B, SIGN_NIL);
        return 1;
    }
    case LUA_TBOOLEAN: {
        luaL_addchar(B, lua_toboolean(L, -1) ? SIGN_TRUE : SIGN_FALSE);
        return 1;
    }
    case LUA_TNUMBER: {
        if (lua_isinteger(L, -1)) {
            int64_t v = lua_tointeger(L, -1);
            if (-63 <= v && v <= 127) {
                int8_t i = (int8_t)v;
                luaL_addchar(B, i);
                return 1;
            }
            else if (INT8_MIN <= v && v <= INT8_MAX) {
                int8_t i = (int8_t)v;
                luaL_addchar(B, SIGN_INT1);
                luaL_addchar(B, i);
                return 2;
            }
            else if (INT16_MIN <= v && v <= INT16_MAX) {
                int16_t i = (int16_t)v;
                i = swap16(i);
                luaL_addchar(B, SIGN_INT2);
                luaL_addlstring(B, (const char*)&i, 2);
                return 3;
            }
            else if (INT32_MIN <= v && v <= INT32_MAX) {
                int32_t i = (int32_t)v;
                i = swap32(i);
                luaL_addchar(B, SIGN_INT4);
                luaL_addlstring(B, (const char*)&i, 4);
                return 5;
            }
            else {
                v = swap64(v);
                luaL_addchar(B, SIGN_INT8);
                luaL_addlstring(B, (const char*)&v, 8);
                return 9;
            }
        }
        else {
            double v = lua_tonumber(L, -1);
            int64_t i = *(int64_t*)&v;
            i = swap64(i);
            luaL_addchar(B, SIGN_FLT);
            luaL_addlstring(B, (const char*)&i, 8);
        }
    }
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, -1, &len);
        size_t size = len;
        if (len <= 31) {
            uint8_t c = (uint8_t)len;
            c |= SIGN_SHRSTR;
            luaL_addchar(B, c);
            size += 1;
        }
        else if (len <= UINT8_MAX) {
            uint8_t c = (uint8_t)len;
            luaL_addchar(B, SIGN_STR1);
            luaL_addchar(B, c);
            size += 2;
        }
        else if (len <= UINT16_MAX) {
            uint16_t i = (uint16_t)len;
            i = swap16(i);
            luaL_addchar(B, SIGN_STR2);
            luaL_addlstring(B, (const char*)&i, 2);
            size += 3;
        }
        else if (len <= UINT32_MAX) {
            uint32_t i = (uint32_t)len;
            i = swap32(i);
            luaL_addchar(B, SIGN_STR4);
            luaL_addlstring(B, (const char*)&i, 4);
            size += 5;
        }
        else {
            los_throw(E, LOS_ESTR);
        }
        luaL_addlstring(B, s, len);
        return size;
    }
    case LUA_TTABLE: {
        luaL_addchar(B, SIGN_TBLBEG);
        size_t size = 1;
        size_t len = lua_rawlen(L, -1);
        size_t numnil = 0;
        for (size_t i = 1; i <= len; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TNIL) {
                ++numnil;
            }
            else {
                for (size_t j = 0; j < numnil; ++j) {
                    luaL_addchar(B, SIGN_NIL);
                }
                size += numnil;
                numnil = 0;
                size += dump_x(E, L, B);
            }
            lua_pop(L, 1);
        }
        luaL_addchar(B, SIGN_TBLSEP);
        ++size;
        int top = lua_gettop(L);
        if (len > 0) {
            size_t lastkey = len - numnil;
            lua_pushinteger(L, lastkey);
        }
        else {
            lua_pushnil(L);
        }
        while (lua_next(L, -2)) {
            size += dump_x(E, L, B);
            lua_pop(L, 1);
            size += dump_x(E, L, B);
        }
        lua_settop(L, top);
        luaL_addchar(B, SIGN_TBLEND);
        ++size;
        return size;
    }
    default: {
        los_throw(E, LOS_ETYPE);
    }
    }
    return 0;
}


static size_t dumpbuf(jmp_buf E, lua_State* L, char* B, size_t buflen)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        checkdestlen(buflen, 1);
        B[0] = SIGN_NIL;
        return 1;
    }
    case LUA_TBOOLEAN: {
        checkdestlen(buflen, 1);
        B[0] = lua_toboolean(L, -1) ? SIGN_TRUE : SIGN_FALSE;
        return 1;
    }
    case LUA_TNUMBER: {
        if (lua_isinteger(L, -1)) {
            int64_t v = lua_tointeger(L, -1);
            if (-63 <= v && v <= 127) {
                checkdestlen(buflen, 1);
                int8_t i = (int8_t)v;
                B[0] = i;
                return 1;
            }
            else if (INT8_MIN <= v && v <= INT8_MAX) {
                checkdestlen(buflen, 2);
                int8_t i = (int8_t)v;
                B[0] = SIGN_INT1;
                B[1] = i;
                return 2;
            }
            else if (INT16_MIN <= v && v <= INT16_MAX) {
                checkdestlen(buflen, 3);
                int16_t i = (int16_t)v;
                B[0] = SIGN_INT2;
                B[1] = *(char*)&i;
                B[2] = *((char*)&i + 1);
                return 3;
            }
            else if (INT32_MIN <= v && v <= INT32_MAX) {
                checkdestlen(buflen, 5);
                int32_t i = (int32_t)v;
                B[0] = SIGN_INT4;
                B[1] = *(char*)&i;
                B[2] = *((char*)&i + 1);
                B[3] = *((char*)&i + 2);
                B[4] = *((char*)&i + 3);
                return 5;
            }
            else {
                checkdestlen(buflen, 9);
                B[0] = SIGN_INT8;
                B[1] = *(char*)&v;
                B[2] = *((char*)&v + 1);
                B[3] = *((char*)&v + 2);
                B[4] = *((char*)&v + 3);
                B[5] = *((char*)&v + 4);
                B[6] = *((char*)&v + 5);
                B[7] = *((char*)&v + 6);
                B[8] = *((char*)&v + 7);
                return 9;
            }
        }
        else {
            checkdestlen(buflen, 9);
            double v = lua_tonumber(L, -1);
            B[0] = SIGN_FLT;
            B[1] = *(char*)&v;
            B[2] = *((char*)&v + 1);
            B[3] = *((char*)&v + 2);
            B[4] = *((char*)&v + 3);
            B[5] = *((char*)&v + 4);
            B[6] = *((char*)&v + 5);
            B[7] = *((char*)&v + 6);
            B[8] = *((char*)&v + 7);
            return 9;
        }
    }
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, -1, &len);
        if (len <= 31) {
            checkdestlen(buflen, 1 + len);
            uint8_t c = (uint8_t)len;
            c |= SIGN_SHRSTR;
            B[0] = c;
            memcpy(B + 1, s, len);
            return 1 + len;
        }
        else if (len <= UINT8_MAX) {
            checkdestlen(buflen, 2 + len);
            uint8_t c = (uint8_t)len;
            B[0] = SIGN_STR1;
            B[1] = c;
            memcpy(B + 2, s, len);
            return 2 + len;
        }
        else if (len <= UINT16_MAX) {
            checkdestlen(buflen, 3 + len);
            uint16_t i = (uint16_t)len;
            B[0] = SIGN_STR2;
            B[1] = *(char*)&i;
            B[2] = *((char*)&i + 1);
            memcpy(B + 3, s, len);
            return 3 + len;
        }
        else if (len <= UINT32_MAX) {
            checkdestlen(buflen, 5 + len);
            uint32_t i = (uint32_t)len;
            B[0] = SIGN_STR4;
            B[1] = *(char*)&i;
            B[2] = *((char*)&i + 1);
            B[3] = *((char*)&i + 2);
            B[4] = *((char*)&i + 3);
            memcpy(B + 5, s, len);
            return 5 + len;
        }
        else {
            los_throw(E, LOS_ESTR);
        }
        return 0;
    }
    case LUA_TTABLE: {
        checkdestlen(buflen, 1);
        B[0] = SIGN_TBLBEG;
        size_t size = 1;
        size_t len = lua_rawlen(L, -1);
        size_t numnil = 0;
        for (size_t i = 1; i <= len; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TNIL) {
                ++numnil;
            }
            else {
                checkdestlen(buflen, size + numnil);
                for (size_t j = 0; j < numnil; ++j) {
                    B[size + j] = SIGN_NIL;
                }
                size += numnil;
                numnil = 0;
                size += dumpbuf(E, L, B + size, buflen - size);
            }
            lua_pop(L, 1);
        }
        checkdestlen(buflen, size + 1);
        B[size] = SIGN_TBLSEP;
        ++size;
        int top = lua_gettop(L);
        if (len > 0) {
            size_t lastkey = len - numnil;
            lua_pushinteger(L, lastkey);
        }
        else {
            lua_pushnil(L);
        }
        while (lua_next(L, -2)) {
            size += dumpbuf(E, L, B + size, buflen - size);
            lua_pop(L, 1);
            size += dumpbuf(E, L, B + size, buflen - size);
        }
        lua_settop(L, top);
        checkdestlen(buflen, size + 1);
        B[size] = SIGN_TBLEND;
        ++size;
        return size;
    }
    default: {
        los_throw(E, LOS_ETYPE);
    }
    }
    return 0;
}


static size_t dumpbuf_x(jmp_buf E, lua_State* L, char* B, size_t buflen)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        checkdestlen(buflen, 1);
        B[0] = SIGN_NIL;
        return 1;
    }
    case LUA_TBOOLEAN: {
        checkdestlen(buflen, 1);
        B[0] = lua_toboolean(L, -1) ? SIGN_TRUE : SIGN_FALSE;
        return 1;
    }
    case LUA_TNUMBER: {
        if (lua_isinteger(L, -1)) {
            int64_t v = lua_tointeger(L, -1);
            if (-63 <= v && v <= 127) {
                checkdestlen(buflen, 1);
                int8_t i = (int8_t)v;
                B[0] = i;
                return 1;
            }
            else if (INT8_MIN <= v && v <= INT8_MAX) {
                checkdestlen(buflen, 2);
                int8_t i = (int8_t)v;
                B[0] = SIGN_INT1;
                B[1] = i;
                return 2;
            }
            else if (INT16_MIN <= v && v <= INT16_MAX) {
                checkdestlen(buflen, 3);
                int16_t i = (int16_t)v;
                B[0] = SIGN_INT2;
                B[1] = *((char*)&i + 1);
                B[2] = *(char*)&i;
                return 3;
            }
            else if (INT32_MIN <= v && v <= INT32_MAX) {
                checkdestlen(buflen, 5);
                int32_t i = (int32_t)v;
                B[0] = SIGN_INT4;
                B[1] = *((char*)&i + 3);
                B[2] = *((char*)&i + 2);
                B[3] = *((char*)&i + 1);
                B[4] = *(char*)&i;
                return 5;
            }
            else {
                checkdestlen(buflen, 9);
                B[0] = SIGN_INT8;
                B[1] = *((char*)&v + 7);
                B[2] = *((char*)&v + 6);
                B[3] = *((char*)&v + 5);
                B[4] = *((char*)&v + 4);
                B[5] = *((char*)&v + 3);
                B[6] = *((char*)&v + 2);
                B[7] = *((char*)&v + 1);
                B[8] = *(char*)&v;
                return 9;
            }
        }
        else {
            checkdestlen(buflen, 9);
            double v = lua_tonumber(L, -1);
            B[0] = SIGN_FLT;
            B[1] = *((char*)&v + 7);
            B[2] = *((char*)&v + 6);
            B[3] = *((char*)&v + 5);
            B[4] = *((char*)&v + 4);
            B[5] = *((char*)&v + 3);
            B[6] = *((char*)&v + 2);
            B[7] = *((char*)&v + 1);
            B[8] = *(char*)&v;
            return 9;
        }
    }
    case LUA_TSTRING: {
        size_t len;
        const char* s = lua_tolstring(L, -1, &len);
        if (len <= 31) {
            checkdestlen(buflen, 1 + len);
            uint8_t c = (uint8_t)len;
            c |= SIGN_SHRSTR;
            B[0] = c;
            memcpy(B + 1, s, len);
            return 1 + len;
        }
        else if (len <= UINT8_MAX) {
            checkdestlen(buflen, 2 + len);
            uint8_t c = (uint8_t)len;
            B[0] = SIGN_STR1;
            B[1] = c;
            memcpy(B + 2, s, len);
            return 2 + len;
        }
        else if (len <= UINT16_MAX) {
            checkdestlen(buflen, 3 + len);
            uint16_t i = (uint16_t)len;
            B[0] = SIGN_STR2;
            B[1] = *((char*)&i + 1);
            B[2] = *(char*)&i;
            memcpy(B + 3, s, len);
            return 3 + len;
        }
        else if (len <= UINT32_MAX) {
            checkdestlen(buflen, 5 + len);
            uint32_t i = (uint32_t)len;
            B[0] = SIGN_STR4;
            B[1] = *((char*)&i + 3);
            B[2] = *((char*)&i + 2);
            B[3] = *((char*)&i + 1);
            B[4] = *(char*)&i;
            memcpy(B + 5, s, len);
            return 5 + len;
        }
        else {
            los_throw(E, LOS_ESTR);
        }
        return 0;
    }
    case LUA_TTABLE: {
        checkdestlen(buflen, 1);
        B[0] = SIGN_TBLBEG;
        size_t size = 1;
        size_t len = lua_rawlen(L, -1);
        size_t numnil = 0;
        for (size_t i = 1; i <= len; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TNIL) {
                ++numnil;
            }
            else {
                checkdestlen(buflen, size + numnil);
                for (size_t j = 0; j < numnil; ++j) {
                    B[size + j] = SIGN_NIL;
                }
                size += numnil;
                numnil = 0;
                size += dumpbuf_x(E, L, B + size, buflen - size);
            }
            lua_pop(L, 1);
        }
        checkdestlen(buflen, size + 1);
        B[size] = SIGN_TBLSEP;
        ++size;
        int top = lua_gettop(L);
        if (len > 0) {
            size_t lastkey = len - numnil;
            lua_pushinteger(L, lastkey);
        }
        else {
            lua_pushnil(L);
        }
        while (lua_next(L, -2)) {
            size += dumpbuf_x(E, L, B + size, buflen - size);
            lua_pop(L, 1);
            size += dumpbuf_x(E, L, B + size, buflen - size);
        }
        lua_settop(L, top);
        checkdestlen(buflen, size + 1);
        B[size] = SIGN_TBLEND;
        ++size;
        return size;
    }
    default: {
        los_throw(E, LOS_ETYPE);
    }
    }
    return 0;
}


static size_t load(jmp_buf E, lua_State* L, const char* B, size_t buflen)
{
    if (buflen == 0) {
        los_throw(E, LOS_ESRC);
    }
    char c = B[0];
    if (IS_SHRINT(c)) {
        lua_pushinteger(L, c);
        return 1;
    }
    if (IS_SHRSTR(c)) {
        c &= ~MASK_SHRSTR;
        size_t len = c;
        checksrclen(buflen, 1 + len);
        lua_pushlstring(L, B + 1, len);
        return 1 + len;
    }
    int sign = (uint8_t)c;
    switch (sign)
    {
    case SIGN_NIL: {
        lua_pushnil(L);
        return 1;
    }
    case SIGN_FALSE: {
        lua_pushboolean(L, 0);
        return 1;
    }
    case SIGN_TRUE: {
        lua_pushboolean(L, 1);
        return 1;
    }
    case SIGN_INT1: {
        checksrclen(buflen, 2);
        lua_pushinteger(L, B[1]);
        return 2;
    }
    case SIGN_INT2: {
        checksrclen(buflen, 3);
        ucast u = (ucast){
            .i8[0] = B[1],
            .i8[1] = B[2],
        };
        lua_pushinteger(L, u.i16[0]);
        return 3;
    }
    case SIGN_INT4: {
        checksrclen(buflen, 5);
        ucast u = (ucast){
            .i8[0] = B[1],
            .i8[1] = B[2],
            .i8[2] = B[3],
            .i8[3] = B[4],
        };
        lua_pushinteger(L, u.i32[0]);
        return 5;
    } 
    case SIGN_INT8: {
        checksrclen(buflen, 9);
        ucast u = (ucast){
            .i8[0] = B[1],
            .i8[1] = B[2],
            .i8[2] = B[3],
            .i8[3] = B[4],
            .i8[4] = B[5],
            .i8[5] = B[6],
            .i8[6] = B[7],
            .i8[7] = B[8],
        };
        lua_pushinteger(L, u.i64);
        return 9;
    }
    case SIGN_STR1: {
        checksrclen(buflen, 2);
        size_t len = B[1];
        checksrclen(buflen, 2 + len);
        lua_pushlstring(L, B + 2, len);
        return 2 + len;
    }
    case SIGN_STR2: {
        checksrclen(buflen, 3);
        ucast u = (ucast){
            .u8[0] = B[1],
            .u8[1] = B[2],
        };
        size_t len = u.u16[0];
        checksrclen(buflen, 3 + len);
        lua_pushlstring(L, B, 3 + len);
        return 3 + len;
    }
    case SIGN_STR4: {
        checksrclen(buflen, 5);
        ucast u = (ucast){
            .u8[0] = B[1],
            .u8[1] = B[2],
            .u8[2] = B[3],
            .u8[3] = B[4],
        };
        size_t len = u.u32[0];
        checksrclen(buflen, 5 + len);
        lua_pushlstring(L, B + 5, len);
        return 5 + len;
    }
    case SIGN_FLT: {
        checksrclen(buflen, 9);
        ucast u = (ucast){
            .u8[0] = B[1],
            .u8[1] = B[2],
            .u8[2] = B[3],
            .u8[3] = B[4],
            .u8[4] = B[5],
            .u8[5] = B[6],
            .u8[6] = B[7],
            .u8[7] = B[8],
        };
        lua_pushnumber(L, u.f);
        return 9;
    }
    case SIGN_TBLBEG: {
        size_t total = 1;
        size_t consume = 0;
        lua_newtable(L);
        int i = 1;
        while (consume = load(E, L, B + total, buflen - total)) {
            lua_rawseti(L, -2, i++);
            total += consume;
        }
        ++total;
        while (consume = load(E, L, B + total, buflen - total)) {
            total += consume;
            consume = load(E, L, B + total, buflen - total);
            if (consume == 0) {
                los_throw(E, LOS_ESRC);
            }
            total += consume;
            lua_rotate(L, -2, 1);
            lua_rawset(L, -3);
        }
        ++total;
        return total;
    }
    case SIGN_TBLSEP:
    case SIGN_TBLEND: {
        return 0;
    }
    default: {
        los_throw(E, LOS_ESIGN);
    }
    }
    return 0;
}


static size_t load_x(jmp_buf E, lua_State* L, const char* B, size_t buflen)
{
    if (buflen == 0) {
        los_throw(E, LOS_ESRC);
    }
    char c = B[0];
    if (IS_SHRINT(c)) {
        lua_pushinteger(L, c);
        return 1;
    }
    if (IS_SHRSTR(c)) {
        c &= ~MASK_SHRSTR;
        size_t len = c;
        checksrclen(buflen, 1 + len);
        lua_pushlstring(L, B + 1, len);
        return 1 + len;
    }
    int sign = (uint8_t)c;
    switch (sign)
    {
    case SIGN_NIL: {
        lua_pushnil(L);
        return 1;
    }
    case SIGN_FALSE: {
        lua_pushboolean(L, 0);
        return 1;
    }
    case SIGN_TRUE: {
        lua_pushboolean(L, 1);
        return 1;
    }
    case SIGN_INT1: {
        checksrclen(buflen, 2);
        lua_pushinteger(L, B[1]);
        return 2;
    }
    case SIGN_INT2: {
        checksrclen(buflen, 3);
        ucast u = (ucast){
            .i8[0] = B[2],
            .i8[1] = B[1],
        };
        lua_pushinteger(L, u.i16[0]);
        return 3;
    }
    case SIGN_INT4: {
        checksrclen(buflen, 5);
        ucast u = (ucast){
            .i8[0] = B[4],
            .i8[1] = B[3],
            .i8[2] = B[2],
            .i8[3] = B[1],
        };
        lua_pushinteger(L, u.i32[0]);
        return 5;
    } 
    case SIGN_INT8: {
        checksrclen(buflen, 9);
        ucast u = (ucast){
            .i8[0] = B[8],
            .i8[1] = B[7],
            .i8[2] = B[6],
            .i8[3] = B[5],
            .i8[4] = B[4],
            .i8[5] = B[3],
            .i8[6] = B[2],
            .i8[7] = B[1],
        };
        lua_pushinteger(L, u.i64);
        return 9;
    }
    case SIGN_STR1: {
        checksrclen(buflen, 2);
        size_t len = B[1];
        checksrclen(buflen, 2 + len);
        lua_pushlstring(L, B + 2, len);
        return 2 + len;
    }
    case SIGN_STR2: {
        checksrclen(buflen, 3);
        ucast u = (ucast){
            .u8[0] = B[2],
            .u8[1] = B[1],
        };
        size_t len = u.u16[0];
        checksrclen(buflen, 3 + len);
        lua_pushlstring(L, B + 3, len);
        return 3 + len;
    }
    case SIGN_STR4: {
        checksrclen(buflen, 5);
        ucast u = (ucast){
            .u8[0] = B[4],
            .u8[1] = B[3],
            .u8[2] = B[2],
            .u8[3] = B[1],
        };
        size_t len = u.u32[0];
        checksrclen(buflen, 5 + len);
        lua_pushlstring(L, B + 5, len);
        return 5 + len;
    }
    case SIGN_FLT: {
        checksrclen(buflen, 9);
        ucast u = (ucast){
            .u8[0] = B[8],
            .u8[1] = B[7],
            .u8[2] = B[6],
            .u8[3] = B[5],
            .u8[4] = B[4],
            .u8[5] = B[3],
            .u8[6] = B[2],
            .u8[7] = B[1],
        };
        lua_pushnumber(L, u.f);
        return 9;
    }
    case SIGN_TBLBEG: {
        size_t total = 1;
        size_t consume = 0;
        lua_newtable(L);
        int i = 1;
        while (consume = load_x(E, L, B + total, buflen - total)) {
            lua_rawseti(L, -2, i++);
            total += consume;
        }
        ++total;
        while (consume = load_x(E, L, B + total, buflen - total)) {
            total += consume;
            consume = load_x(E, L, B + total, buflen - total);
            if (consume == 0) {
                los_throw(E, LOS_ESRC);
            }
            total += consume;
            lua_rotate(L, -2, 1);
            lua_rawset(L, -3);
        }
        ++total;
        return total;
    }
    case SIGN_TBLSEP:
    case SIGN_TBLEND: {
        return 0;
    }
    default: {
        los_throw(E, LOS_ESIGN);
    }
    }
    return 0;
}


static int los_dump(lua_State* L)
{
    jmp_buf E;
    los_try(E);
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t size = luaL_checkinteger(L, 3);
        luaL_checkany(L, 4);
        lua_settop(L, 4);
        size_t len = dumpbuf(E, L, B + offset, size);
        lua_pushinteger(L, len);
        return 1;
    }
    else {
        lua_settop(L, 1);
        luaL_Buffer B;
        luaL_buffinit(L, &B);
        size_t len = dump(E, L, &B);
        lua_pushinteger(L, len);
        luaL_pushresult(&B);
        return 2;
    }
}


static int los_dump_x(lua_State* L)
{
    jmp_buf E;
    los_try(E);
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t size = luaL_checkinteger(L, 3);
        luaL_checkany(L, 4);
        lua_settop(L, 4);
        size_t len = dumpbuf_x(E, L, B + offset, size);
        lua_pushinteger(L, len);
        return 1;
    }
    else {
        lua_settop(L, 1);
        luaL_Buffer B;
        luaL_buffinit(L, &B);
        size_t len = dump_x(E, L, &B);
        lua_pushinteger(L, len);
        luaL_pushresult(&B);
        return 2;
    }
}


static int los_load(lua_State* L)
{
    jmp_buf E;
    los_try(E);
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        const char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t size = luaL_checkinteger(L, 3);
        lua_settop(L, 3);
        size_t consume = load(E, L, B + offset, size);
        lua_pushinteger(L, consume);
        lua_rotate(L, -2, 1);
        return 2;
    }
    else {
        luaL_argexpected(L, lua_isstring(L, 1), 1, lua_typename(L, LUA_TSTRING));
        lua_settop(L, 1);
        size_t size;
        const char* B = lua_tolstring(L, -1, &size);
        size_t consume = load(E, L, B, size);
        lua_pushinteger(L, consume);
        lua_rotate(L, -2, 1);
        return 2;
    }
}


static int los_load_x(lua_State* L)
{
    jmp_buf E;
    los_try(E);
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        const char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t size = luaL_checkinteger(L, 3);
        lua_settop(L, 3);
        size_t consume = load_x(E, L, B + offset, size);
        lua_pushinteger(L, consume);
        lua_rotate(L, -2, 1);
        return 2;
    }
    else {
        luaL_argexpected(L, lua_isstring(L, 1), 1, lua_typename(L, LUA_TSTRING));
        lua_settop(L, 1);
        size_t size;
        const char* B = lua_tolstring(L, -1, &size);
        size_t consume = load_x(E, L, B, size);
        lua_pushinteger(L, consume);
        lua_rotate(L, -2, 1);
        return 2;
    }
}


static int los_setendian(lua_State* L)
{
    luaL_argexpected(L, lua_istable(L, 1), 1, lua_typename(L, LUA_TTABLE));
    ucast u = (ucast){ .u64 = 1 };
    int local_endian = u.u32[0] != 0 ? ENDIAN_LE : ENDIAN_BE;
    int target_endian = local_endian;
    int top = lua_gettop(L);
    if (top >= 2) {
        const char* endian = luaL_checkstring(L, 2);
        if (strncmp(endian, "le", 2)) {
            target_endian = ENDIAN_LE;
        }
        else if (strncmp(endian, "be", 2)) {
            target_endian = ENDIAN_BE;
        }
        else {
            luaL_argerror(L, 1, "invalid endian");
            return 0;
        }
    }
    int eq = local_endian == target_endian;
    lua_pushcfunction(L, eq ? los_dump : los_dump_x);
    lua_setfield(L, 1, "dump");
    lua_pushcfunction(L, eq ? los_load : los_load_x);
    lua_setfield(L, 1, "load");
    lua_pushstring(L, local_endian == ENDIAN_LE ? "le" : "be");
    lua_setfield(L, 1, "local_endian");
    lua_pushstring(L, target_endian == ENDIAN_LE ? "le" : "be");
    lua_setfield(L, 1, "target_endian");
    return 0;
}


size_t pack(jmp_buf E, lua_State* L, luaL_Buffer* B)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        luaL_addstring(B, "nil");
        return 3;
    }
    case LUA_TBOOLEAN: {
        if (lua_toboolean(L, -1)) {
            luaL_addstring(B, "true");
            return 4;
        }
        else {
            luaL_addstring(B, "false");
            return 5;
        }
    }
    case LUA_TNUMBER:
    case LUA_TSTRING: {
        lua_pushcfunction(L, str_format);
        lua_pushstring(L, "%q");
        lua_pushvalue(L, -3);
        if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
            los_throw(E, LOS_EFMT);
        }
        size_t len;
        const char* s = lua_tolstring(L, -1, &len);
        luaL_addstring(B, s);
        lua_pop(L, 1);
        return len;
    }
    case LUA_TTABLE: {
        luaL_addchar(B, '{');
        size_t size = 1;
        size_t len = lua_rawlen(L, -1);
        size_t numnil = 0;
        int comma = 0;
        for (size_t i = 1; i <= len; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TNIL) {
                ++numnil;
            }
            else {
                if (comma) {
                    luaL_addchar(B, ',');
                    ++size;
                }
                else {
                    comma = 1;
                }
                for (size_t j = 0; j < numnil; ++j) {
                    luaL_addstring(B, "nil,");
                }
                size += numnil * 4;
                numnil = 0;
                size += pack(E, L, B);
            }
            lua_pop(L, 1);
        }
        int top = lua_gettop(L);
        if (len > 0) {
            size_t lastkey = len - numnil;
            lua_pushinteger(L, lastkey);
        }
        else {
            lua_pushnil(L);
        }
        while (lua_next(L, -2)) {
            if (comma) {
                luaL_addchar(B, ',');
                ++size;
            }
            else {
                comma = 1;
            }
            lua_rotate(L, -2, 1);
            luaL_addchar(B, '[');
            ++size;
            size += pack(E, L, B);
            luaL_addstring(B, "]=");
            size += 2;
            lua_rotate(L, -2, 1);
            size += pack(E, L, B);
            lua_pop(L, 1);
        }
        lua_settop(L, top);
        luaL_addchar(B, '}');
        ++size;
        return size;
    }
    default: {
        los_throw(E, LOS_ETYPE);
    }
    }
    return 0;
}


static size_t packbuf(jmp_buf E, lua_State* L, char* B, size_t buflen)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        checkdestlen(buflen, 3);
        B[0] = 'n';
        B[1] = 'i';
        B[2] = 'l';
        return 3;
    }
    case LUA_TBOOLEAN: {
        if (lua_toboolean(L, -1)) {
            checkdestlen(buflen, 4);
            B[0] = 't';
            B[1] = 'r';
            B[2] = 'u';
            B[3] = 'e';
            return 4;
        }
        else {
            checkdestlen(buflen, 4);
            B[0] = 'f';
            B[1] = 'a';
            B[2] = 'l';
            B[3] = 's';
            B[4] = 'e';
            return 5;
        }
    }
    case LUA_TNUMBER:
    case LUA_TSTRING: {
        lua_pushcfunction(L, str_format);
        lua_pushstring(L, "%q");
        lua_pushvalue(L, -3);
        if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
            los_throw(E, LOS_EFMT);
        }
        size_t len;
        const char* s = lua_tolstring(L, -1, &len);
        checkdestlen(buflen, len);
        memcpy(B, s, len);
        return len;
    }
    case LUA_TTABLE: {
        checkdestlen(buflen, 1);
        B[0] = '{';
        size_t size = 1;
        size_t len = lua_rawlen(L, -1);
        size_t numnil = 0;
        for (size_t i = 1; i <= len; ++i) {
            if (lua_rawgeti(L, -1, i) == LUA_TNIL) {
                ++numnil;
            }
            else {
                checkdestlen(buflen - size, numnil * 4);
                for (size_t j = 0; j < numnil; ++j) {
                    char* s = B + size + j * 4;
                    s[0] = 'n';
                    s[1] = 'i';
                    s[2] = 'l';
                    s[3] = ',';
                }
                size += numnil * 4;
                numnil = 0;
                size += packbuf(E, L, B + size, buflen - size);
                checkdestlen(buflen - size, 1);
                B[size] = ',';
                ++size;
            }
            lua_pop(L, 1);
        }
        int top = lua_gettop(L);
        if (len > 0) {
            size_t lastkey = len - numnil;
            lua_pushinteger(L, lastkey);
        }
        else {
            lua_pushnil(L);
        }
        while (lua_next(L, -2)) {
            lua_rotate(L, -2, 1);
            checkdestlen(buflen - size, 1);
            B[size] = '[';
            ++size;
            size += packbuf(E, L, B + size, buflen - size);
            checkdestlen(buflen - size, 2);
            B[size] = ']';
            B[size + 1] = '=';
            size += 2;
            lua_rotate(L, -2, 1);
            size += packbuf(E, L, B + size, buflen - size);
            checkdestlen(buflen - size, 1);
            B[size] = ',';
            ++size;
            lua_pop(L, 1);
        }
        lua_settop(L, top);
        if (B[size - 1] == ',') {
            B[size - 1] = '}';
        }
        else {
            checkdestlen(buflen - size, 1);
            B[size] = '}';
            ++size;
        }
        return size;
    }
    default: {
        los_throw(E, LOS_ETYPE);
    }
    }
    return 0;
}


static size_t unpack(jmp_buf E, lua_State* L, const char* B, size_t buflen)
{
    if (buflen == 0) {
        los_throw(E, LOS_ESRC);
    }
    char c = B[0];
    if (c == '"') {
        for (size_t i = 1; i < buflen; ++i) {
            if (B[i] == '"' && B[i - 1] != '\\') {
                lua_pushlstring(L, B + 1, i - 1);
                return i + 1;
            }
        }
        los_throw(E, LOS_ESRC);
    }
    else if (c == '{') {
        lua_newtable(L);
        size_t i = 1;
        size_t k = 1;
        while (i < buflen) {
            c = B[i];
            if (c == '}') {
                return i + 1;
            }
            if (c == '[') {
                ++i;
                checksrclen(buflen - i, 1);
                i += unpack(E, L, B + i, buflen - i);
                if (lua_isnil(L, -1)) {
                    los_throw(E, LOS_ESIGN);
                }
                checksrclen(buflen - i, 1);
                if (B[i] != ']') {
                    los_throw(E, LOS_ESIGN);
                }
                ++i;
                checksrclen(buflen - i, 1);
                if (B[i] != '=') {
                    los_throw(E, LOS_ESIGN);
                }
                ++i;
                checksrclen(buflen - i, 1);
                i += unpack(E, L, B + i, buflen - i);
                lua_rawset(L, -3);
                if (i < buflen && B[i] == ',') {
                    ++i;
                }
            }
            else {
                i += unpack(E, L, B + i, buflen - i);
                lua_rawseti(L, -2, k++);
                if (i < buflen && B[i] == ',') {
                    ++i;
                }
            }
        }
        los_throw(E, LOS_ESRC);
    }
    else if (c == ',') {
        los_throw(E, LOS_ESIGN);
    }
    else {
        size_t i;
        for (i = 1; i < buflen; ++i) {
            if (B[i] == ',' || B[i] == '}' || B[i] == ']') {
                break;
            }
        }
        int comma = (i < buflen) && (B[i] == ',');
        if (i == 3) {
            if (B[0] == 'n' &&
                B[1] == 'i' &&
                B[2] == 'l') {
                lua_pushnil(L);
                return comma ? 4 : 3;
            }
        }
        else if (i == 4) {
            if (B[0] == 't' &&
                B[1] == 'r' &&
                B[2] == 'u' &&
                B[3] == 'e') {
                lua_pushboolean(L, 1);
                return comma ? 5 : 4;
            }
        }
        else if (i == 5) {
            if (B[0] == 'f' &&
                B[1] == 'a' &&
                B[2] == 'l' &&
                B[3] == 's' &&
                B[4] == 'e') {
                lua_pushboolean(L, 0);
                return comma ? 6 : 5;
            }
        }
        lua_pushlstring(L, B, i);
        int isnum;
        if (B[0] == '0') {
            lua_Number f = lua_tonumberx(L, -1, &isnum);
            if (!isnum) {
                los_throw(E, LOS_ESIGN);
            }
            lua_pop(L, 1);
            lua_pushnumber(L, f);
        }
        else {
            lua_Integer n = lua_tointegerx(L, -1, &isnum);
            if (!isnum) {
                los_throw(E, LOS_ESIGN);
            }
            lua_pop(L, 1);
            lua_pushinteger(L, n);
        }
        return comma ? i + 1 : i;
    }
    return 0;
}


static int los_pack(lua_State* L)
{
    jmp_buf E;
    los_try(E);
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t size = luaL_checkinteger(L, 3);
        luaL_checkany(L, 4);
        lua_settop(L, 4);
        size_t len = packbuf(E, L, B + offset, size);
        lua_pushinteger(L, len);
        return 1;
    }
    else {
        lua_settop(L, 1);
        luaL_Buffer B;
        luaL_buffinit(L, &B);
        size_t len = pack(E, L, &B);
        lua_pushinteger(L, len);
        luaL_pushresult(&B);
        return 2;
    }
}


static int los_unpack(lua_State* L)
{
    jmp_buf E;
    los_try(E);
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        const char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t size = luaL_checkinteger(L, 3);
        lua_settop(L, 3);
        size_t consume = unpack(E, L, B + offset, size);
        lua_pushinteger(L, consume);
        lua_rotate(L, -2, 1);
        return 2;
    }
    else {
        luaL_argexpected(L, lua_isstring(L, 1), 1, lua_typename(L, LUA_TSTRING));
        lua_settop(L, 1);
        size_t size;
        const char* B = lua_tolstring(L, -1, &size);
        size_t consume = unpack(E, L, B, size);
        lua_pushinteger(L, consume);
        lua_rotate(L, -2, 1);
        return 2;
    }
}


static void los_openpack(lua_State* L)
{
    int top = lua_gettop(L);
    lua_getglobal(L, "string");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "format");
        if (lua_iscfunction(L, -1)) {
            str_format = lua_tocfunction(L, -1);
        }
    }
    lua_settop(L, top);
    if (str_format) {
        luaL_Reg lib[] = {
            {"pack", los_pack},
            {"unpack", los_unpack},
            {NULL, NULL}
        };
        luaL_setfuncs(L, lib, 0);
    }
}


static void los_openconst(lua_State* L)
{
#define MCONST(v, n) lua_pushinteger(L, v); lua_setfield(L, -2, #n);
    MCONST(LOS_ETYPE, ETYPE)
    MCONST(LOS_ESIGN, ESIGN)
    MCONST(LOS_EBUF, EBUF)
    MCONST(LOS_ESRC, ESRC)
    MCONST(LOS_ESTR, ESTR)
    MCONST(LOS_EFMT, EFMT)
}


LUA_MOD_EXPORT int luaopen_los(lua_State* L)
{
    static_assert(sizeof(lua_Number) == 8, "require 8 bytes lua_Number");
    luaL_Reg lib[] = {
        {"setendian", los_setendian},
        {NULL, NULL}
    };
    luaL_newlib(L, lib);
    lua_pushcfunction(L, los_setendian);
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        lua_error(L);
        return 0;
    }
    los_openpack(L);
    los_openconst(L);
    return 1;
}

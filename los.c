#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
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

#define checkbuflen(len, need) (void)(((len) >= (need)) || luaL_error(L, "not enough avaliable buffer"));

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


static size_t pack(lua_State* L, luaL_Buffer* B)
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
        size_t strlength;
        const char* s = lua_tolstring(L, -1, &strlength);
        size_t size = strlength;
        if (strlength <= 31) {
            uint8_t c = (uint8_t)strlength;
            c |= SIGN_SHRSTR;
            luaL_addchar(B, c);
            size += 1;
        }
        else if (strlength <= UINT8_MAX) {
            uint8_t c = (uint8_t)strlength;
            luaL_addchar(B, SIGN_STR1);
            luaL_addchar(B, c);
            size += 2;
        }
        else if (strlength <= UINT16_MAX) {
            luaL_addchar(B, SIGN_STR2);
            luaL_addlstring(B, (const char*)&strlength, 2);
            size += 3;
        }
        else if (strlength <= UINT32_MAX) {
            luaL_addchar(B, SIGN_STR4);
            luaL_addlstring(B, (const char*)&strlength, 4);
            size += 5;
        }
        else {
            luaL_error(L, "string is too long");
        }
        luaL_addlstring(B, s, strlength);
        return size;
    }
    case LUA_TTABLE: {
        luaL_addchar(B, SIGN_TBLBEG);
        size_t size = 3;
        int arridx = 1;
        int top = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) != arridx++) {
                break;
            }
            size += pack(L, B);
            lua_pop(L, 1);
        }
        luaL_addchar(B, SIGN_TBLSEP);
        if (lua_gettop(L) > top) {
            size += pack(L, B);
            lua_pop(L, 1);
            size += pack(L, B);
            while (lua_next(L, -2)) {
                size += pack(L, B);
                lua_pop(L, 1);
                size += pack(L, B);
            }
        }
        luaL_addchar(B, SIGN_TBLEND);
        return size;
    }
    default: {
        luaL_error(L, "unsurported type: %s", lua_typename(L, type));
    }
    }
    return 0;
}


static size_t pack_x(lua_State* L, luaL_Buffer* B)
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
        size_t strlength;
        const char* s = lua_tolstring(L, -1, &strlength);
        size_t size = strlength;
        if (strlength <= 31) {
            uint8_t c = (uint8_t)strlength;
            c |= SIGN_SHRSTR;
            luaL_addchar(B, c);
            size += 1;
        }
        else if (strlength <= UINT8_MAX) {
            uint8_t c = (uint8_t)strlength;
            luaL_addchar(B, SIGN_STR1);
            luaL_addchar(B, c);
            size += 2;
        }
        else if (strlength <= UINT16_MAX) {
            uint16_t i = (uint16_t)strlength;
            i = swap16(i);
            luaL_addchar(B, SIGN_STR2);
            luaL_addlstring(B, (const char*)&i, 2);
            size += 3;
        }
        else if (strlength <= UINT32_MAX) {
            uint32_t i = (uint32_t)strlength;
            i = swap32(i);
            luaL_addchar(B, SIGN_STR4);
            luaL_addlstring(B, (const char*)&i, 4);
            size += 5;
        }
        else {
            luaL_error(L, "string is too long");
        }
        luaL_addlstring(B, s, strlength);
        return size;
    }
    case LUA_TTABLE: {
        luaL_addchar(B, SIGN_TBLBEG);
        size_t size = 3;
        int arridx = 1;
        int top = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) != arridx++) {
                break;
            }
            size += pack_x(L, B);
            lua_pop(L, 1);
        }
        luaL_addchar(B, SIGN_TBLSEP);
        if (lua_gettop(L) > top) {
            size += pack_x(L, B);
            lua_pop(L, 1);
            size += pack_x(L, B);
            while (lua_next(L, -2)) {
                size += pack_x(L, B);
                lua_pop(L, 1);
                size += pack_x(L, B);
            }
        }
        luaL_addchar(B, SIGN_TBLEND);
        return size;
    }
    default: {
        luaL_error(L, "unsurported type: %s", lua_typename(L, type));
    }
    }
    return 0;
}


static size_t packbuf(lua_State* L, char* B, size_t buflen)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        checkbuflen(buflen, 1);
        B[0] = SIGN_NIL;
        return 1;
    }
    case LUA_TBOOLEAN: {
        checkbuflen(buflen, 1);
        B[0] = lua_toboolean(L, -1) ? SIGN_TRUE : SIGN_FALSE;
        return 1;
    }
    case LUA_TNUMBER: {
        if (lua_isinteger(L, -1)) {
            int64_t v = lua_tointeger(L, -1);
            if (-63 <= v && v <= 127) {
                checkbuflen(buflen, 1);
                int8_t i = (int8_t)v;
                B[0] = i;
                return 1;
            }
            else if (INT8_MIN <= v && v <= INT8_MAX) {
                checkbuflen(buflen, 2);
                int8_t i = (int8_t)v;
                B[0] = SIGN_INT1;
                B[1] = i;
                return 2;
            }
            else if (INT16_MIN <= v && v <= INT16_MAX) {
                checkbuflen(buflen, 3);
                int16_t i = (int16_t)v;
                B[0] = SIGN_INT2;
                B[1] = *(char*)&i;
                B[2] = *((char*)&i + 1);
                return 3;
            }
            else if (INT32_MIN <= v && v <= INT32_MAX) {
                checkbuflen(buflen, 5);
                int32_t i = (int32_t)v;
                B[0] = SIGN_INT4;
                B[1] = *(char*)&i;
                B[2] = *((char*)&i + 1);
                B[3] = *((char*)&i + 2);
                B[4] = *((char*)&i + 3);
                return 5;
            }
            else {
                checkbuflen(buflen, 9);
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
            checkbuflen(buflen, 9);
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
        size_t strlength;
        const char* s = lua_tolstring(L, -1, &strlength);
        if (strlength <= 31) {
            checkbuflen(buflen, 1 + strlength);
            uint8_t c = (uint8_t)strlength;
            c |= SIGN_SHRSTR;
            B[0] = c;
            memcpy(B + 1, s, strlength);
            return 1 + strlength;
        }
        else if (strlength <= UINT8_MAX) {
            checkbuflen(buflen, 2 + strlength);
            uint8_t c = (uint8_t)strlength;
            B[0] = SIGN_STR1;
            B[1] = c;
            memcpy(B + 2, s, strlength);
            return 2 + strlength;
        }
        else if (strlength <= UINT16_MAX) {
            checkbuflen(buflen, 3 + strlength);
            uint16_t i = (uint16_t)strlength;
            B[0] = SIGN_STR2;
            B[1] = *(char*)&i;
            B[2] = *((char*)&i + 1);
            memcpy(B + 3, s, strlength);
            return 3 + strlength;
        }
        else if (strlength <= UINT32_MAX) {
            checkbuflen(buflen, 5 + strlength);
            uint32_t i = (uint32_t)strlength;
            B[0] = SIGN_STR4;
            B[1] = *(char*)&i;
            B[2] = *((char*)&i + 1);
            B[3] = *((char*)&i + 2);
            B[4] = *((char*)&i + 3);
            memcpy(B + 5, s, strlength);
            return 5 + strlength;
        }
        else {
            luaL_error(L, "string is too long");
        }
        return 0;
    }
    case LUA_TTABLE: {
        checkbuflen(buflen, 1);
        B[0] = SIGN_TBLBEG;
        size_t size = 1;
        int arridx = 1;
        int top = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) != arridx++) {
                break;
            }
            size += packbuf(L, B + size, buflen - size);
            lua_pop(L, 1);
        }
        checkbuflen(buflen, size + 1);
        B[size] = SIGN_TBLSEP;
        ++size;
        if (lua_gettop(L) > top) {
            size += packbuf(L, B + size, buflen - size);
            lua_pop(L, 1);
            size += packbuf(L, B + size, buflen - size);
            while (lua_next(L, -2)) {
                size += packbuf(L, B + size, buflen - size);
                lua_pop(L, 1);
                size += packbuf(L, B + size, buflen - size);
            }
        }
        checkbuflen(buflen, size + 1);
        B[size] = SIGN_TBLEND;
        ++size;
        return size;
    }
    default:
        luaL_error(L, "unsurported type: %s", lua_typename(L, type));
    }
    return 0;
}


static size_t packbuf_x(lua_State* L, char* B, size_t buflen)
{
    int type = lua_type(L, -1);
    switch (type)
    {
    case LUA_TNIL: {
        checkbuflen(buflen, 1);
        B[0] = SIGN_NIL;
        return 1;
    }
    case LUA_TBOOLEAN: {
        checkbuflen(buflen, 1);
        B[0] = lua_toboolean(L, -1) ? SIGN_TRUE : SIGN_FALSE;
        return 1;
    }
    case LUA_TNUMBER: {
        if (lua_isinteger(L, -1)) {
            int64_t v = lua_tointeger(L, -1);
            if (-63 <= v && v <= 127) {
                checkbuflen(buflen, 1);
                int8_t i = (int8_t)v;
                B[0] = i;
                return 1;
            }
            else if (INT8_MIN <= v && v <= INT8_MAX) {
                checkbuflen(buflen, 2);
                int8_t i = (int8_t)v;
                B[0] = SIGN_INT1;
                B[1] = i;
                return 2;
            }
            else if (INT16_MIN <= v && v <= INT16_MAX) {
                checkbuflen(buflen, 3);
                int16_t i = (int16_t)v;
                B[0] = SIGN_INT2;
                B[1] = *((char*)&i + 1);
                B[2] = *(char*)&i;
                return 3;
            }
            else if (INT32_MIN <= v && v <= INT32_MAX) {
                checkbuflen(buflen, 5);
                int32_t i = (int32_t)v;
                B[0] = SIGN_INT4;
                B[1] = *((char*)&i + 3);
                B[2] = *((char*)&i + 2);
                B[3] = *((char*)&i + 1);
                B[4] = *(char*)&i;
                return 5;
            }
            else {
                checkbuflen(buflen, 9);
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
            checkbuflen(buflen, 9);
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
        size_t strlength;
        const char* s = lua_tolstring(L, -1, &strlength);
        if (strlength <= 31) {
            checkbuflen(buflen, 1 + strlength);
            uint8_t c = (uint8_t)strlength;
            c |= SIGN_SHRSTR;
            B[0] = c;
            memcpy(B + 1, s, strlength);
            return 1 + strlength;
        }
        else if (strlength <= UINT8_MAX) {
            checkbuflen(buflen, 2 + strlength);
            uint8_t c = (uint8_t)strlength;
            B[0] = SIGN_STR1;
            B[1] = c;
            memcpy(B + 2, s, strlength);
            return 2 + strlength;
        }
        else if (strlength <= UINT16_MAX) {
            checkbuflen(buflen, 3 + strlength);
            uint16_t i = (uint16_t)strlength;
            B[0] = SIGN_STR2;
            B[1] = *((char*)&i + 1);
            B[2] = *(char*)&i;
            memcpy(B + 3, s, strlength);
            return 3 + strlength;
        }
        else if (strlength <= UINT32_MAX) {
            checkbuflen(buflen, 5 + strlength);
            uint32_t i = (uint32_t)strlength;
            B[0] = SIGN_STR4;
            B[1] = *((char*)&i + 3);
            B[2] = *((char*)&i + 2);
            B[3] = *((char*)&i + 1);
            B[4] = *(char*)&i;
            memcpy(B + 5, s, strlength);
            return 5 + strlength;
        }
        else {
            luaL_error(L, "string is too long");
        }
        return 0;
    }
    case LUA_TTABLE: {
        checkbuflen(buflen, 1);
        B[0] = SIGN_TBLBEG;
        size_t size = 1;
        int arridx = 1;
        int top = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) != arridx++) {
                break;
            }
            size += packbuf_x(L, B + size, buflen - size);
            lua_pop(L, 1);
        }
        checkbuflen(buflen, size + 1);
        B[size] = SIGN_TBLSEP;
        ++size;
        if (lua_gettop(L) > top) {
            size += packbuf_x(L, B + size, buflen - size);
            lua_pop(L, 1);
            size += packbuf_x(L, B + size, buflen - size);
            while (lua_next(L, -2)) {
                size += packbuf_x(L, B + size, buflen - size);
                lua_pop(L, 1);
                size += packbuf_x(L, B + size, buflen - size);
            }
        }
        checkbuflen(buflen, size + 1);
        B[size] = SIGN_TBLEND;
        ++size;
        return size;
    }
    default:
        luaL_error(L, "unsurported type: %s", lua_typename(L, type));
    }
    return 0;
}


static size_t unpack(lua_State* L, const char* B, size_t buflen)
{
    if (buflen == 0) {
        return 0;
    }
    char c = B[0];
    if (IS_SHRINT(c)) {
        lua_pushinteger(L, c);
        return 1;
    }
    if (IS_SHRSTR(c)) {
        c &= ~MASK_SHRSTR;
        size_t strlength = c;
        checkbuflen(buflen, 1 + strlength);
        lua_pushlstring(L, B + 1, strlength);
        return 1 + strlength;
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
        checkbuflen(buflen, 2);
        lua_pushinteger(L, B[1]);
        return 2;
    }
    case SIGN_INT2: {
        checkbuflen(buflen, 3);
        ucast u = (ucast){
            .i8[0] = B[1],
            .i8[1] = B[2],
        };
        lua_pushinteger(L, u.i16[0]);
        return 3;
    }
    case SIGN_INT4: {
        checkbuflen(buflen, 5);
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
        checkbuflen(buflen, 9);
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
        checkbuflen(buflen, 2);
        size_t strlength = B[1];
        checkbuflen(buflen, 2 + strlength);
        lua_pushlstring(L, B + 2, strlength);
        return 2 + strlength;
    }
    case SIGN_STR2: {
        checkbuflen(buflen, 3);
        ucast u = (ucast){
            .u8[0] = B[1],
            .u8[1] = B[2],
        };
        size_t strlength = u.u16[0];
        checkbuflen(buflen, 3 + strlength);
        lua_pushlstring(L, B, 3 + strlength);
        return 3 + strlength;
    }
    case SIGN_STR4: {
        checkbuflen(buflen, 5);
        ucast u = (ucast){
            .u8[0] = B[1],
            .u8[1] = B[2],
            .u8[2] = B[3],
            .u8[3] = B[4],
        };
        size_t strlength = u.u32[0];
        checkbuflen(buflen, 5 + strlength);
        lua_pushlstring(L, B + 5, strlength);
        return 5 + strlength;
    }
    case SIGN_FLT: {
        checkbuflen(buflen, 9);
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
        int idx = 1;
        while (consume = unpack(L, B + total, buflen - total)) {
            lua_rawseti(L, -2, idx++);
            total += consume;
        }
        ++total;
        while (consume = unpack(L, B + total, buflen - total)) {
            total += consume;
            consume = unpack(L, B + total, buflen - total);
            if (consume == 0) {
                luaL_error(L, "incomplete buffer");
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
        luaL_error(L, "unexpected sign: %d", sign);
    }
    }
    return 0;
}


static size_t unpack_x(lua_State* L, const char* B, size_t buflen)
{
    if (buflen == 0) {
        return 0;
    }
    char c = B[0];
    if (IS_SHRINT(c)) {
        lua_pushinteger(L, c);
        return 1;
    }
    if (IS_SHRSTR(c)) {
        c &= ~MASK_SHRSTR;
        size_t strlength = c;
        checkbuflen(buflen, 1 + strlength);
        lua_pushlstring(L, B + 1, strlength);
        return 1 + strlength;
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
        checkbuflen(buflen, 2);
        lua_pushinteger(L, B[1]);
        return 2;
    }
    case SIGN_INT2: {
        checkbuflen(buflen, 3);
        ucast u = (ucast){
            .i8[0] = B[2],
            .i8[1] = B[1],
        };
        lua_pushinteger(L, u.i16[0]);
        return 3;
    }
    case SIGN_INT4: {
        checkbuflen(buflen, 5);
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
        checkbuflen(buflen, 9);
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
        checkbuflen(buflen, 2);
        size_t strlength = B[1];
        checkbuflen(buflen, 2 + strlength);
        lua_pushlstring(L, B + 2, strlength);
        return 2 + strlength;
    }
    case SIGN_STR2: {
        checkbuflen(buflen, 3);
        ucast u = (ucast){
            .u8[0] = B[2],
            .u8[1] = B[1],
        };
        size_t strlength = u.u16[0];
        checkbuflen(buflen, 3 + strlength);
        lua_pushlstring(L, B + 3, strlength);
        return 3 + strlength;
    }
    case SIGN_STR4: {
        checkbuflen(buflen, 5);
        ucast u = (ucast){
            .u8[0] = B[4],
            .u8[1] = B[3],
            .u8[2] = B[2],
            .u8[3] = B[1],
        };
        size_t strlength = u.u32[0];
        checkbuflen(buflen, 5 + strlength);
        lua_pushlstring(L, B + 5, strlength);
        return 5 + strlength;
    }
    case SIGN_FLT: {
        checkbuflen(buflen, 9);
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
        int idx = 1;
        while (consume = unpack_x(L, B + total, buflen - total)) {
            lua_rawseti(L, -2, idx++);
            total += consume;
        }
        ++total;
        while (consume = unpack_x(L, B + total, buflen - total)) {
            total += consume;
            consume = unpack_x(L, B + total, buflen - total);
            if (consume == 0) {
                luaL_error(L, "incomplete buffer");
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
        luaL_error(L, "unexpected sign: %d", sign);
    }
    }
    return 0;
}


static int los_pack(lua_State* L)
{
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t availableSize = luaL_checkinteger(L, 3);
        luaL_checkany(L, 4);
        lua_settop(L, 4);
        size_t resultingLength = packbuf(L, B + offset, availableSize);
        lua_pushinteger(L, resultingLength);
        return 1;
    }
    else {
        lua_settop(L, 1);
        luaL_Buffer B;
        luaL_buffinit(L, &B);
        size_t resultingLength = pack(L, &B);
        luaL_pushresult(&B);
        lua_pushinteger(L, resultingLength);
        return 2;
    }
}


static int los_pack_x(lua_State* L)
{
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t availableSize = luaL_checkinteger(L, 3);
        luaL_checkany(L, 4);
        lua_settop(L, 4);
        size_t resultingLength = packbuf_x(L, B + offset, availableSize);
        lua_pushinteger(L, resultingLength);
        return 1;
    }
    else {
        lua_settop(L, 1);
        luaL_Buffer B;
        luaL_buffinit(L, &B);
        size_t resultingLength = pack_x(L, &B);
        luaL_pushresult(&B);
        lua_pushinteger(L, resultingLength);
        return 2;
    }
}


static int los_unpack(lua_State* L)
{
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        const char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t availableSize = luaL_checkinteger(L, 3);
        lua_settop(L, 3);
        size_t consumedLength = unpack(L, B + offset, availableSize);
        lua_pushinteger(L, consumedLength);
        return 2;
    }
    else {
        luaL_argexpected(L, lua_isstring(L, 1), 1, lua_typename(L, LUA_TSTRING));
        lua_settop(L, 1);
        size_t availableSize;
        const char* B = lua_tolstring(L, -1, &availableSize);
        size_t consumedLength = unpack(L, B, availableSize);
        lua_pushinteger(L, consumedLength);
        return 2;
    }
}


static int los_unpack_x(lua_State* L)
{
    luaL_checkany(L, 1);
    if (lua_islightuserdata(L, 1)) {
        const char* B = lua_touserdata(L, 1);
        size_t offset = luaL_checkinteger(L, 2);
        size_t availableSize = luaL_checkinteger(L, 3);
        lua_settop(L, 3);
        size_t consumedLength = unpack_x(L, B + offset, availableSize);
        lua_pushinteger(L, consumedLength);
        return 2;
    }
    else {
        luaL_argexpected(L, lua_isstring(L, 1), 1, lua_typename(L, LUA_TSTRING));
        lua_settop(L, 1);
        size_t availableSize;
        const char* B = lua_tolstring(L, -1, &availableSize);
        size_t consumedLength = unpack_x(L, B, availableSize);
        lua_pushinteger(L, consumedLength);
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
    lua_pushcfunction(L, eq ? los_pack : los_pack_x);
    lua_setfield(L, 1, "pack");
    lua_pushcfunction(L, eq ? los_unpack : los_unpack_x);
    lua_setfield(L, 1, "unpack");
    lua_pushstring(L, local_endian == ENDIAN_LE ? "le" : "be");
    lua_setfield(L, 1, "local_endian");
    lua_pushstring(L, target_endian == ENDIAN_LE ? "le" : "be");
    lua_setfield(L, 1, "target_endian");
    return 0;
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
    return 1;
}

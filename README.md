# los

Lua Object Serialization

# Install

```bash
luarocks install los
```

# Reference

## Serialize: pack & dump

```Lua
pack(object)                  -- (1)
pack(buffer, size, object)    -- (2)
dump(object)                  -- (3)
dump(buffer, size, object)    -- (4)
```

(1)(3) Serialize an object to a string.

(2)(4) Serialize an object to a string info a c buffer.

(1)(2) Serialize into a readable format.

(3)(4) Serialize into a binary format.

##### Parameters

- object - simple lua object supporting boolean, number, string and table
- buffer - lightuserdata refers to a c buffer, which the result is writting into
- size - avaliable size of the buffer

##### Returns

(1)(3)
- the resulting length
- the resulting string

(2)(4)
- the resulting length

if failed
- the error code less than 0: ETYPE, EBUF, ESTR, EFMT

## Deserialize: unpack & load

```Lua
unpack(string)          -- (1)
unpack(buffer, size)    -- (2)
load(string)            -- (3)
load(buffer, size)      -- (4)
```

(1)(3) Deserialize a string to an object.

(2)(4) Deserialize a c buffer to an object.

##### Parameters

- string - the serialized string
- buffer - lightuserdata refers to a c buffer containing the serialized string
- size - avaliable size of the buffer

##### Returns

- the consumed length of the string or the buffer
- the resulting object

if failed
- the error code less than 0: ESIGN, ESRC

#### Notes
- serialize functions and deserialize functions should work in pairs
- use `unpack` to deserialize the string or buffer returned by `pack`
- use `load` to deserialize the string or buffer returned by `dump`

## Endian: setendian
```Lua
setendian(losmod, endian)
```

Sets the los module working endian

##### Parameters

- losmod - the los module table
- endian - the target working endian, 'le' for little endian and 'be for big endian

##### Returns

- none

##### Notes

- `dump` and `load` work with endian, while `pack` and `unpack` don't
- they work with the local machine's endian by default
- `setendian` changes `dump` and `load` to target endian version
- you should call `setendian` immediately after requiring los module, like this:
  ```Lua
  local los = require('los')
  los:setendian('le')
  ```

## Properties

- `local_endian` - the local machine endian
- `target_endian` - the target endian that los outputs and inputs with

## Constants

- ETYPE - error: unsupported type
- ESIGN - error: wrong sign or format detected
- EBUF - error: not enough buffer
- ESRC - error: incomplete source
- ESTR - error: string is too long
- EFMT - error: failed on formatting number and string

# See also

- [cbuf - Simple C Buffer for Lua](https://github.com/lengbing/cbuf)

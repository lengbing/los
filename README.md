# los

Lua Object Serialization

<!--

# Install

```bash
luarocks install los
```

# Reference

## pack

```Lua
pack(object)
pack(buffer, index, size, object)
```

(1) Receives an object, returns a resulting string.

(2) Receives a buffer and an object, writes the result into the buffer and returns the consumed length.

##### Parameters

- object - simple lua object supporting boolean, number, string and table
- buffer - lightuserdata refers to a c buffer, which the result is writting into
- index - index of the buffer to writting start with
- size - avaliable size of the buffer start from the index

##### Returns

(1)
- the resulting string
- the resulting length

(2)
- the resulting length, the consumed length of the buffer

## unpack

```Lua
unpack(string)
unpack(buffer, index, size)
```

(1) Receives a string, returns a lua object and the consumed length of the string.

(2) Receives a buffer, returns a lua object and the consumed length of the buffer.

##### Parameters

- string - the encoded string returned by `pack`
- buffer - lightuserdata refers to a c buffer containing the encoded string
- index - index of the buffer to reading start with
- size - avaliable size of the buffer start from the index

##### Returns

- the resulting object
- the consumed length of the string or the buffer

## setendian
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

- los works with the local machine's endian by default
- `setendian` changes the fields of los module table to target endian version
- you should call `setendian` immediately after requiring los module, like this:
  ```Lua
  local los = require('los')
  los:setendian('le')
  ```

## Properties

- `local_endian` - the local machine endian
- `target_endian` - the target endian that los outputs and inputs with

# See also

- [cbuf - Simple C Buffer for Lua](https://github.com/lengbing/cbuf)

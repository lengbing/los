# los
Lua Object Serialization

# Reference

### pack

(1) `pack(object)` 

Receives an object, returns a resulting string.

(2) `pack(buffer, offset, size, object)` 

Receives a buffer and an object, writes the result into the buffer and returns the consumed length.

##### Parameters

- object - simple lua object supporting boolean, number, string and table
- buffer - lightuserdata refers to a c buffer, which the result is writting into
- offset - writting offset of the buffer
- size - avaliable size of the buffer start from the offset

##### Returns

(1)
- the resulting string
- the resulting length

(2)
- the resulting length, the consumed length of the buffer

### unpack

(1) `unpack(string)`

Receives a string, returns a lua object and the consumed length of the string.

(2) `unpack(buffer, offset, size)`

Receives a buffer, returns a lua object and the consumed length of the buffer.

##### Parameters

- string - the encoded string returned by `pack`
- buffer - lightuserdata refers to a c buffer containing the encoded string
- offset - reading offset of the buffer
- size - avaliable size of the buffer start from the offset

##### Returns

- the resulting object
- the consumed length of the string or the buffer

### newbuf

`newbuf(size)`

Receives a size, returns a c buffer.

##### Parameters

- size - size of the allocating buffer

##### Returns

- a lightuserdata refers to the c buffer, or nil if failed.

### delbuf

`delbuf(buffer)`

Receives a buffer and free it.

##### Parameters

- buffer - lightuserdata refers to a c buffer

##### Returns

- none

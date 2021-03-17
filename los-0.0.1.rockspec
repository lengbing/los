package = "los"
version = "0.0.1"
source = {
   url = "git+https://github.com/lengbing/los.git",
   tag = "0.0.1"
}
description = {
   summary = "Lua Object Serialization",
   homepage = "https://github.com/lengbing/los",
   license = "MIT"
}
dependencies = {
   "lua >= 5.4"
}
build = {
   type = "builtin",
   modules = {
      los = {
         sources = "los.c"
      }
   }
}

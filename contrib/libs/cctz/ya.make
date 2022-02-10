# Generated by devtools/yamaker from nixpkgs 5852a21819542e6809f68ba5a798600e69874e76.

LIBRARY() 
 
OWNER(
    dfyz
    petrk
)

VERSION(2020-11-11)
 
ORIGINAL_SOURCE(https://github.com/google/cctz/archive/98fb541c6f0f35cf0dffcbc3777d8385bbd5b4c1.tar.gz)

LICENSE(
    Apache-2.0 AND
    Public-Domain
)
 
LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

ADDINCL(
    GLOBAL contrib/libs/cctz/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

IF (OS_DARWIN)
    LDFLAGS(
        -framework
        CoreFoundation
    )
ENDIF()

SRCS( 
    src/civil_time_detail.cc
    src/time_zone_fixed.cc
    src/time_zone_format.cc
    src/time_zone_if.cc
    src/time_zone_impl.cc
    src/time_zone_info.cc
    src/time_zone_libc.cc
    src/time_zone_lookup.cc
    src/time_zone_posix.cc
    src/zone_info_source.cc
) 
 
END()
 
RECURSE(
    test
    tzdata
)

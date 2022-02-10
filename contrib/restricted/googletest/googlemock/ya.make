# Generated by devtools/yamaker.

LIBRARY()

OWNER(
    somov
    g:cpp-contrib
)

LICENSE(
    Apache-2.0 AND
    BSD-3-Clause
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/restricted/googletest/googletest
)

ADDINCL( 
    GLOBAL contrib/restricted/googletest/googlemock/include
    GLOBAL contrib/restricted/googletest/googletest/include
    contrib/restricted/googletest/googlemock
    contrib/restricted/googletest/googletest
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    GLOBAL -DGTEST_HAS_ABSL=0
    GLOBAL -DGTEST_OS_FUCHSIA=0
    GLOBAL -DGTEST_HAS_STD_WSTRING=1
)

SRCS(
    src/gmock-all.cc
)

END()

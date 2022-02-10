UNITTEST_FOR(ydb/core/tx/schemeshard)
 
OWNER( 
    eivanov89 
    g:kikimr 
) 
 
FORK_SUBTESTS() 

SPLIT_FACTOR(10) 
 
IF (SANITIZER_TYPE OR WITH_VALGRIND) 
    TIMEOUT(3600) 
    SIZE(LARGE) 
    TAG(ya:fat) 
ELSE() 
    TIMEOUT(600) 
    SIZE(MEDIUM) 
ENDIF() 
 
PEERDIR( 
    library/cpp/getopt
    library/cpp/regex/pcre 
    ydb/core/cms
    ydb/core/testlib
    ydb/core/tx
    ydb/core/tx/schemeshard/ut_helpers
    ydb/core/wrappers/ut_helpers
) 
 
SRCS( 
    ut_compaction.cpp 
) 
 
YQL_LAST_ABI_VERSION() 
 
END() 

UNITTEST_FOR(yql/library/jsonpath) 
 
OWNER(g:yql) 
 
SRCS( 
    common_ut.cpp 
    examples_ut.cpp 
    lax_ut.cpp 
    strict_ut.cpp 
    test_base.cpp 
) 
 
PEERDIR( 
    library/cpp/json
    ydb/library/binary_json
    ydb/library/yql/minikql
    ydb/library/yql/minikql/computation
    ydb/library/yql/minikql/dom
    ydb/library/yql/minikql/invoke_builtins
    ydb/library/yql/public/udf/service/exception_policy
    ydb/library/yql/core/issue/protos
) 
 
YQL_LAST_ABI_VERSION()

END() 

#include "datashard_ut_common.h"
#include "datashard_ut_common_kqp.h"
#include "datashard_active_transaction.h"

#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/base/tablet_resolver.h>
#include <ydb/core/engine/minikql/minikql_engine_host.h>
#include <ydb/core/kqp/executer/kqp_executer.h>
#include <ydb/core/kqp/ut/common/kqp_ut_common.h> // Y_UNIT_TEST_(TWIN|QUAD)
#include <ydb/core/tx/time_cast/time_cast.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/tx/tx_processing.h>
#include <ydb/public/lib/deprecated/kicli/kicli.h>
#include <ydb/core/testlib/tenant_runtime.h>
#include <util/system/valgrind.h>

#include <ydb/library/yql/minikql/invoke_builtins/mkql_builtins.h>

namespace NKikimr {

using NClient::TValue;
using IEngineFlat = NMiniKQL::IEngineFlat;
using namespace NKikimr::NDataShard;
using namespace NKikimr::NDataShard::NKqpHelpers;
using namespace NSchemeShard;
using namespace Tests;

using TActiveTxPtr = std::shared_ptr<TActiveTransaction>;

///
class TDatashardTester {
public:
    TDatashardTester()
        : FunctionRegistry(NKikimr::NMiniKQL::CreateFunctionRegistry(NKikimr::NMiniKQL::CreateBuiltinRegistry()))
        , RandomProvider(CreateDeterministicRandomProvider(1))
        , TimeProvider(CreateDeterministicTimeProvider(1))
    {}

    static TString MakeTxBody(const TString& miniKQL, bool immediate = false, ui64 lockId = 0) {
        NKikimrTxDataShard::TDataTransaction tx;
        tx.SetMiniKQL(miniKQL);
        tx.SetImmediate(immediate);
        if (lockId) {
            tx.SetLockTxId(lockId);
        }
        return tx.SerializeAsString();
    }

    static TActiveTxPtr MakeEmptyTx(ui64 step, ui64 txId) {
        TBasicOpInfo op(txId, EOperationKind::DataTx, 0, Max<ui64>(), TInstant(), 0);
        op.SetStep(step);
        return std::make_shared<TActiveTransaction>(op);
    }
#if 0
    TActiveTransaction MakeActiveTx(ui64 step, ui64 txId, const TString& txBody) {
        THolder<NMiniKQL::IEngineFlatHost> host = MakeHolder<NMiniKQL::TEngineHost>(DB);
        THolder<NMiniKQL::IEngineFlat> engine = CreateEngineFlat(
            NMiniKQL::TEngineFlatSettings(NMiniKQL::IEngineFlat::EProtocol::V1,
                                          FunctionRegistry.Get(), *RandomProvider, *TimeProvider, host.Get()));

        TEngineBay ebay(host.Release(), engine.Release());
        std::shared_ptr<TValidatedDataTx> dataTx(new TValidatedDataTx(std::move(ebay), txId, 0, txBody));

        TBasicOpInfo op(txId, NKikimrTxDataShard::ETransactionKind::TX_KIND_DATA, 0, Max<ui64>(), 0);
        op.SetStep(step);
        TActiveTransaction tx(op);
        tx.Activate(0, dataTx);
        return tx;
    }
#endif
private:
    TIntrusivePtr<NKikimr::NMiniKQL::IFunctionRegistry> FunctionRegistry;
    TIntrusivePtr<IRandomProvider> RandomProvider;
    TIntrusivePtr<ITimeProvider> TimeProvider;
    NTable::TDatabase DB;
};


///
Y_UNIT_TEST_SUITE(TxOrderInternals) {

Y_UNIT_TEST(OperationOrder) {
    using TTester = TDatashardTester;

    TActiveTxPtr tx0_100 = TTester::MakeEmptyTx(0, 100);
    TActiveTxPtr tx0_101 = TTester::MakeEmptyTx(0, 101);
    TActiveTxPtr tx1_40 = TTester::MakeEmptyTx(1, 40);
    TActiveTxPtr tx1_102 = TTester::MakeEmptyTx(1, 102);
    TActiveTxPtr tx1_103 = TTester::MakeEmptyTx(1, 103);
    TActiveTxPtr tx2_42 = TTester::MakeEmptyTx(2, 42);

    UNIT_ASSERT_EQUAL(tx0_100->GetStepOrder().CheckOrder(tx0_101->GetStepOrder()), ETxOrder::Any);
    UNIT_ASSERT_EQUAL(tx0_101->GetStepOrder().CheckOrder(tx0_100->GetStepOrder()), ETxOrder::Any);

    UNIT_ASSERT_EQUAL(tx0_100->GetStepOrder().CheckOrder(tx1_102->GetStepOrder()), ETxOrder::Unknown);
    UNIT_ASSERT_EQUAL(tx1_102->GetStepOrder().CheckOrder(tx0_100->GetStepOrder()), ETxOrder::Unknown);

    UNIT_ASSERT_EQUAL(tx1_102->GetStepOrder().CheckOrder(tx1_103->GetStepOrder()), ETxOrder::Before);
    UNIT_ASSERT_EQUAL(tx1_103->GetStepOrder().CheckOrder(tx1_102->GetStepOrder()), ETxOrder::After);

    UNIT_ASSERT_EQUAL(tx1_102->GetStepOrder().CheckOrder(tx1_40->GetStepOrder()), ETxOrder::After);
    UNIT_ASSERT_EQUAL(tx1_102->GetStepOrder().CheckOrder(tx2_42->GetStepOrder()), ETxOrder::Before);
}

}

static void InitCrossShard_ABC(TFakeMiniKQLProxy& proxy, TVector<ui32> uintVal) {
    UNIT_ASSERT_EQUAL(uintVal.size(), 3);

    auto programText = Sprintf(R"((
        (let row1_ '('('key (Uint32 '0))))
        (let row2_ '('('key (Uint32 '1000))))
        (let row3_ '('('key (Uint32 '2000))))
        (let upd1_ '('('value (Utf8 'A)) '('uint (Uint32 '%u))))
        (let upd2_ '('('value (Utf8 'B)) '('uint (Uint32 '%u))))
        (let upd3_ '('('value (Utf8 'C)) '('uint (Uint32 '%u))))
        (let ret_ (AsList
            (UpdateRow 'table1 row1_ upd1_)
            (UpdateRow 'table1 row2_ upd2_)
            (UpdateRow 'table1 row3_ upd3_)
        ))
        (return ret_)
    ))", uintVal[0], uintVal[1], uintVal[2]);

    UNIT_ASSERT_EQUAL(proxy.Execute(programText), IEngineFlat::EStatus::Complete);
}

///
Y_UNIT_TEST_SUITE(DataShardTxOrder) {

static void ZigZag(TFakeMiniKQLProxy& proxy, bool symmetric, ui32 limit = 40) {
    InitCrossShard_ABC(proxy, {0, 0, 0});

    const char * zigzag = R"((
        (let src1_ '('('key (Uint32 '%u))))
        (let src2_ '('('key (Uint32 '%u))))
        (let dst1_ '('('key (Uint32 '%u))))
        (let dst2_ '('('key (Uint32 '%u))))
        (let val1_ (FlatMap (SelectRow 'table1 src1_ '('value)) (lambda '(x) (Member x 'value))))
        (let val2_ (FlatMap (SelectRow 'table1 src2_ '('value)) (lambda '(x) (Member x 'value))))
        (let upd1_ '('('value val1_)))
        (let upd2_ '('('value val2_)))
        (let ret_ (AsList
            (UpdateRow 'table1 dst1_ upd2_)
            (UpdateRow 'table1 dst2_ upd1_)
        ))
        (return ret_)
    ))";

    // strict ordered txs: {0->1001, 1000->1} {1->1002, 1001->2}
    for (ui32 i = 0; i < 10; ++i) {
        TString programText = Sprintf(zigzag, i, 1000+i, i+1, 1000+i+1);
        UNIT_ASSERT_EQUAL(proxy.Execute(programText), IEngineFlat::EStatus::Complete);
    }

    if (symmetric) {
        // relaxed order: 20->1031, 1020->31
        for (ui32 shift = 0; shift < limit-10; shift+=10) {
            for (ui32 i = 0; i < 10; ++i) {
                TString programText = Sprintf(zigzag, shift+i, 1000+shift+i, shift+i+11, 1000+shift+i+11);
                //UNIT_ASSERT_EQUAL(proxy.Execute(programText), IEngineFlat::EStatus::Complete);
                proxy.Enqueue(programText);
            }
        }
    } else {
        // relaxed order (asymmetric): 0->1031, 1020->1031
        for (ui32 shift = 0; shift < limit-10; shift+=10) {
            for (ui32 i = 0; i < 10; ++i) {
                TString programText = Sprintf(zigzag, i, 1000+shift+i, shift+i+11, 1000+shift+i+11);
                //UNIT_ASSERT_EQUAL(proxy.Execute(programText), IEngineFlat::EStatus::Complete);
                proxy.Enqueue(programText);
            }
        }
    }

    proxy.ExecQueue();

    {
        TString programText = Sprintf(R"((
            (let row1_ '('('key (Uint32 '%u))))
            (let row2_ '('('key (Uint32 '%u))))
            (let row3_ '('('key (Uint32 '%u))))
            (let row4_ '('('key (Uint32 '%u))))
            (let select_ '('value))
            (let ret_ (AsList
            (SetResult 'Result (AsList
                (SelectRow 'table1 row1_ select_)
                (SelectRow 'table1 row2_ select_)
                (SelectRow 'table1 row3_ select_)
                (SelectRow 'table1 row4_ select_)
            ))
            ))
            (return ret_)
        ))", limit-1, limit, 1000+limit-1, 1000+limit);

        NKikimrMiniKQL::TResult res;
        UNIT_ASSERT_EQUAL(proxy.Execute(programText, res), IEngineFlat::EStatus::Complete);

        TValue value = TValue::Create(res.GetValue(), res.GetType());
        TValue rl = value["Result"];
        TValue row1 = rl[0];
        TValue row2 = rl[1];
        TValue row3 = rl[2];
        TValue row4 = rl[3];
        UNIT_ASSERT_EQUAL(TString(row1["value"]), "B");
        UNIT_ASSERT_EQUAL(TString(row2["value"]), "A");
        UNIT_ASSERT_EQUAL(TString(row3["value"]), "A");
        UNIT_ASSERT_EQUAL(TString(row4["value"]), "B");
    }
}

static void ZigZag(const TTester::TOptions& opts, bool symmetric, ui32 limit = 40) {
    TTester t(TTester::ESchema_MultiShardKV, opts);
    TFakeMiniKQLProxy proxy(t);
    ZigZag(proxy, symmetric, limit);
}

Y_UNIT_TEST_WITH_MVCC(ZigZag) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    ZigZag(opts, true);
    ZigZag(opts, false);
}

Y_UNIT_TEST_WITH_MVCC(ZigZag_oo) {
    TVector<ui32> variants = {4, 8, 16};
    for (ui32 var : variants) {
        TTester::TOptions opts;
        opts.EnableOutOfOrder(var);
        opts.EnableMvcc(WithMvcc);
        ZigZag(opts, true);
        ZigZag(opts, false);
    }
}

Y_UNIT_TEST_WITH_MVCC(ZigZag_oo8_dirty) {
    TTester::TOptions opts;
    opts.EnableOutOfOrder(8);
    opts.EnableSoftUpdates();
    opts.EnableMvcc(WithMvcc);
    ZigZag(opts, true);
    ZigZag(opts, false);
}

//

static void ImmediateBetweenOnline(const TTester::TOptions& opts, bool forceOnline = false) {
    TTester t(TTester::ESchema_MultiShardKV, opts);
    TFakeMiniKQLProxy proxy(t);

    InitCrossShard_ABC(proxy, {0, 0, 0});

    const char * online = R"((
        (let key1_ '('('key (Uint32 '%u))))
        (let key2_ '('('key (Uint32 '%u))))
        (let key3_ '('('key (Uint32 '%u))))
        (let val1_ (FlatMap (SelectRow 'table1 key1_ '('value)) (lambda '(x) (Member x 'value))))
        (let val2_ (FlatMap (SelectRow 'table1 key2_ '('value)) (lambda '(x) (Member x 'value))))
        (let upd21_ '('('uint (Uint32 '%u)) '('value val2_)))
        (let upd13_ '('('value val1_)))
        (let ret_ (AsList
            (UpdateRow 'table1 key1_ upd21_)
            (UpdateRow 'table1 key3_ upd13_)
        ))
        (return ret_)
    ))";

    const char * immediate = R"((
        (let key1_ '('('key (Uint32 '%u))))
        (let key2_ '('('key (Uint32 '%u))))
        (let val1_ (Coalesce (FlatMap (SelectRow 'table1 key1_ '('uint)) (lambda '(x) (Member x 'uint))) (Uint32 '0)))
        (let val2_ (Coalesce (FlatMap (SelectRow 'table1 key2_ '('uint)) (lambda '(x) (Member x 'uint))) (Uint32 '0)))
        (let ret_ (AsList
            (SetResult 'val1 val1_)
            (SetResult 'val2 val2_)
        ))
        (return ret_)
    ))";

    auto immediateCheck = [&](TFakeProxyTx& tx) -> bool {
        NKikimrMiniKQL::TResult res = tx.GetResult();
        TValue value = TValue::Create(res.GetValue(), res.GetType());
        ui32 val1 = value["val1"];
        ui32 val2 = value["val2"];
        //Cerr << "val1 " << val1 << " val2 " << val2 << Endl;
        if (forceOnline) {
            UNIT_ASSERT(!tx.Immediate());
            UNIT_ASSERT(val1 == 1 && val2 == 0);
        } else {
            UNIT_ASSERT(tx.Immediate());
            UNIT_ASSERT((val1 == 0 && val2 == 0) || (val1 == 1 && val2 == 0) || (val1 == 1 && val2 == 2));
            if (val1 == 1 && val2 == 0) {
                Cerr << "Got it!" << Endl;
            }
        }
        return true;
    };

    ui32 flags = NDataShard::TTxFlags::Default;
    if (forceOnline) {
        flags |= NDataShard::TTxFlags::ForceOnline;
    }

    for (ui32 i = 0; i < 100; i+=2) {
        TString prog1 = Sprintf(online, i, 1000+i, 2000+i, 1);
        TString prog2 = Sprintf(online, i+1, 1000+i+1, 200+i, 2);
        TString progIm = Sprintf(immediate, i, i+1);
        proxy.Enqueue(prog1);
        proxy.Enqueue(progIm, immediateCheck, flags);
        proxy.Enqueue(prog2);
    }

    proxy.ExecQueue();
}

Y_UNIT_TEST_WITH_MVCC(ImmediateBetweenOnline) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    ImmediateBetweenOnline(opts, false);
}

Y_UNIT_TEST_WITH_MVCC(ImmediateBetweenOnline_Init) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    ImmediateBetweenOnline(opts, false);
}

Y_UNIT_TEST_WITH_MVCC(ForceOnlineBetweenOnline) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    ImmediateBetweenOnline(opts, true);
}

Y_UNIT_TEST_WITH_MVCC(ImmediateBetweenOnline_oo8) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    opts.EnableOutOfOrder(8);
    ImmediateBetweenOnline(opts, false);
}

Y_UNIT_TEST_WITH_MVCC(ImmediateBetweenOnline_Init_oo8) {
    TTester::TOptions opts(1);
    opts.EnableMvcc(WithMvcc);
    opts.EnableOutOfOrder(8);
    ImmediateBetweenOnline(opts, false);
}

Y_UNIT_TEST_WITH_MVCC(ForceOnlineBetweenOnline_oo8) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    opts.EnableOutOfOrder(8);
    ImmediateBetweenOnline(opts, true);
}

Y_UNIT_TEST_WITH_MVCC(ImmediateBetweenOnline_oo8_dirty) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    opts.EnableOutOfOrder(8);
    opts.EnableSoftUpdates();
    ImmediateBetweenOnline(opts, false);
}

//

static void EvictShardCache(TFakeMiniKQLProxy& proxy, ui32 count = 500) {
    const char * programWrite = R"((
        (let key_ '('('key (Uint32 '%u))))
        (return (AsList (UpdateRow 'table1 key_ '('('value (Utf8 '"%s"))))))
    ))";

    const char * text = "You will always get that you always got if you always do that you've already done";
    for (ui32 i = 0; i < count; ++i) {
        UNIT_ASSERT_EQUAL(proxy.Execute(Sprintf(programWrite, (3*i)%1000, text)), IEngineFlat::EStatus::Complete);
    }

    // some more txs to enlarge step
    proxy.Enqueue(Sprintf(programWrite, 0, text));
    proxy.Enqueue(Sprintf(programWrite, 0, text));
    proxy.ExecQueue();
}

Y_UNIT_TEST_WITH_MVCC(DelayData) {
    TTester::TOptions opts;
    opts.EnableMvcc(WithMvcc);
    opts.EnableOutOfOrder(2);
    opts.ExecutorCacheSize = 0;
    TTester t(TTester::ESchema_MultiShardKV, opts);
    TFakeMiniKQLProxy proxy(t);

    EvictShardCache(proxy, 500);

    ui64 indepTxId = proxy.LastTxId() + 2;
    proxy.DelayData({TTestTxConfig::TxTablet0, indepTxId}); 

    const char * programRead = R"((
        (let key_ '('('key (Uint32 '0))))
        (return (AsList (SetResult 'Result (SelectRow 'table1 key_ '('value)))))
    ))";

    const char * independentTx = R"((
        (let key_ '('('key (Uint32 '999))))
        (return (AsList (UpdateRow 'table1 key_ '('('value (Utf8 '"freedom"))))))
    ))";

    //
    proxy.Enqueue(programRead);
    proxy.Enqueue(independentTx);
    proxy.ExecQueue();
}

Y_UNIT_TEST_WITH_MVCC(ReadWriteReorder) {
    TTester::TOptions opts;
    opts.EnableOutOfOrder(10);
    opts.EnableMvcc(WithMvcc);

    TTester t(TTester::ESchema_MultiShardKV, opts);
    TFakeMiniKQLProxy proxy(t);

    const char* programWriteKeys1 = R"((
        (let key_ '('('key (Uint32 '%u))))
        (let upd_ '('('value (Utf8 '"%s"))))
        (return (AsList
            (UpdateRow 'table1 key_ upd_)
        ))
    ))";

    proxy.CheckedExecute(Sprintf(programWriteKeys1, 0, "A"));
    proxy.CheckedExecute(Sprintf(programWriteKeys1, 1, "B"));
    proxy.CheckedExecute(Sprintf(programWriteKeys1, 1000, "C"));

    const char* programMoveKey = R"((
        (let key1_ '('('key (Uint32 '%u))))
        (let val1_ (FlatMap (SelectRow 'table1 key1_ '('value)) (lambda '(x) (Member x 'value))))
        (let key2_ '('('key (Uint32 '%u))))
        (let upd2_ '('('value val1_)))
        (return (AsList
            (UpdateRow 'table1 key2_ upd2_)
        ))
    ))";

    const char* programWriteKeys2 = R"((
        (let key1_ '('('key (Uint32 '%u))))
        (let upd1_ '('('value (Utf8 '"%s"))))
        (let key2_ '('('key (Uint32 '%u))))
        (let upd2_ '('('value (Utf8 '"%s"))))
        (return (AsList
            (UpdateRow 'table1 key1_ upd1_)
            (UpdateRow 'table1 key2_ upd2_)
        ))
    ))";

    const char* programWriteKeys3 = R"((
        (let key1_ '('('key (Uint32 '%u))))
        (let upd1_ '('('value (Utf8 '"%s"))))
        (let key2_ '('('key (Uint32 '%u))))
        (let upd2_ '('('value (Utf8 '"%s"))))
        (let key3_ '('('key (Uint32 '%u))))
        (let upd3_ '('('value (Utf8 '"%s"))))
        (return (AsList
            (UpdateRow 'table1 key1_ upd1_)
            (UpdateRow 'table1 key2_ upd2_)
            (UpdateRow 'table1 key3_ upd3_)
        ))
    ))";

    const char* programReadKeys3 = R"((
        (let key1_ '('('key (Uint32 '%u))))
        (let key2_ '('('key (Uint32 '%u))))
        (let key3_ '('('key (Uint32 '%u))))
        (let select_ '('value))
        (return (AsList
            (SetResult 'Result (AsList
                (SelectRow 'table1 key1_ select_)
                (SelectRow 'table1 key2_ select_)
                (SelectRow 'table1 key3_ select_)
            ))
        ))
    ))";

    const char* programReadKeys4 = R"((
        (let key1_ '('('key (Uint32 '%u))))
        (let key2_ '('('key (Uint32 '%u))))
        (let key3_ '('('key (Uint32 '%u))))
        (let key4_ '('('key (Uint32 '%u))))
        (let select_ '('value))
        (return (AsList
            (SetResult 'Result (AsList
                (SelectRow 'table1 key1_ select_)
                (SelectRow 'table1 key2_ select_)
                (SelectRow 'table1 key3_ select_)
                (SelectRow 'table1 key4_ select_)
            ))
        ))
    ))";

    auto noCheck = [&](TFakeProxyTx&) -> bool {
        return true;
    };
    auto txFlags = NDataShard::TTxFlags::Default;

    // tx 7: This moves key 1000 to key 2 (and will be blocked on readsets)
    proxy.Enqueue(Sprintf(programMoveKey, 1000, 2), noCheck, txFlags);
    auto txMoveKey = proxy.LastTxId();

    // tx 8: This writes to keys 0, 2 and 1000 (used as a progress blocker)
    proxy.Enqueue(Sprintf(programWriteKeys3, 0, "D", 2, "E", 1000, "F"), noCheck, txFlags);

    // tx 9: This reads keys 1, 3 and 1000
    // Does not conflict on the first shard and will be executed out of order
    NKikimrMiniKQL::TResult read_1_1000_3;
    proxy.Enqueue(Sprintf(programReadKeys3, 1, 1000, 3), [&](TFakeProxyTx& tx) -> bool {
        read_1_1000_3 = tx.GetResult();
        return true;
    }, txFlags);

    // tx 10: This is an immediate write to keys 0 and 1
    // It will be proposed after the above read completes, so it must
    // be ordered after the above read.
    proxy.Enqueue(Sprintf(programWriteKeys2, 0, "G", 1, "H"), noCheck, txFlags);

    // tx 11: This write to key 3 (force online), will block until the above read
    // This would unblock writes to keys 0 and 2 and used to add a delay
    proxy.Enqueue(Sprintf(programWriteKeys1, 3, "Z"));
    auto txWriteLast = proxy.LastTxId();

    // Delay first shard readsets until last write succeeds
    proxy.DelayReadSet(TExpectedReadSet(txMoveKey, { TTestTxConfig::TxTablet0, txWriteLast })); 
    proxy.ExecQueue();

    // Sanity check: read must go first, otherwise the whole machinery would hang
    // Read result also proves that tx 8 is logically before tx 9
    {
        TValue value = TValue::Create(read_1_1000_3.GetValue(), read_1_1000_3.GetType());
        TValue rows = value["Result"];
        UNIT_ASSERT_VALUES_EQUAL(TString(rows[0]["value"]), "B"); // key 1: initial value
        UNIT_ASSERT_VALUES_EQUAL(TString(rows[1]["value"]), "F"); // key 1000: tx 8 must be visible
    }

    // Read the final state of important keys
    NKikimrMiniKQL::TResult read_0_1_2_1000;
    proxy.Enqueue(Sprintf(programReadKeys4, 0, 1, 2, 1000), [&](TFakeProxyTx& tx) -> bool {
        read_0_1_2_1000 = tx.GetResult();
        return true;
    }, txFlags);
    proxy.ExecQueue();

    // Sanity check: must see correct state of these keys
    {
        TValue value = TValue::Create(read_0_1_2_1000.GetValue(), read_0_1_2_1000.GetType());
        TValue rows = value["Result"];
        UNIT_ASSERT_VALUES_EQUAL(TString(rows[0]["value"]), "G"); // key 0: tx 10 must be visible
        UNIT_ASSERT_VALUES_EQUAL(TString(rows[1]["value"]), "H"); // key 1: tx 10 must be visible
        UNIT_ASSERT_VALUES_EQUAL(TString(rows[2]["value"]), "E"); // key 2: tx 8 must be visible
        UNIT_ASSERT_VALUES_EQUAL(TString(rows[3]["value"]), "F"); // key 1000: tx 8 must be visible
    }
}

//

static inline bool HasFlag(ui32 flags, ui32 pos) {
    return (flags & (1 << pos));
}

static TString MkRandomTx(ui64 txId, ui32 points, ui32 rw, ui32 keysCount, TVector<ui32>& expected, bool range = false)
{
    UNIT_ASSERT(keysCount <= 32);
    Cout << "tx " << txId << ' ' << Bin(points) << ' ' << Bin(points & rw) << " (" << points << '/' << rw << ')' << Endl;

    const char * rwPattern = R"(
        (let $%u '('('key (Uint32 '%u))))
        (let updates_ (Extend updates_ (AsList (UpdateRow 'table1 $%u '('('uint (Uint32 '%u)))))))
    )";

    const char * roPattern = R"(
        (let $%u '('('key (Uint32 '%u))))
        (let selects_ (Extend selects_ (ToList (SelectRow 'table1 $%u '('key 'uint)))))
    )";

    const char * rangePattern = R"(
        (let $%u '('IncFrom 'IncTo '('key (Uint32 '%u) (Uint32 '%u))))
        (let selects_ (Extend selects_ (Member (SelectRange 'table1 $%u '('key 'uint) '()) 'List)))
    )";

    TString body;
    for (ui32 i = 0; i < keysCount; ++i) {
        if (HasFlag(points, i)) {
            if (HasFlag(rw, i)) {
                body += Sprintf(rwPattern, i, i, i, txId);
                if (!expected.empty()) {
                    expected[i] = txId;
                }
            } else {
                if (range) {
                    body += Sprintf(rangePattern, i, i, i, i);
                } else {
                    body += Sprintf(roPattern, i, i, i);
                }
            }
        }
    }

    ui32 remoteKey = 1001;
    return Sprintf(R"((
        (let remoteKey_ '('('key (Uint32 '%u))))
        (let sel_ (SelectRow 'table1 remoteKey_ '('key 'uint)))
        (let rVal_ (Coalesce (FlatMap sel_ (lambda '(x) (Member x 'uint))) (Uint32 '0)))
        (let selects_ (ToList sel_))
        (let localKey_ '('('key (Uint32 '%u))))
        (let updates_ (AsList (UpdateRow 'table1 localKey_ '('('uint rVal_)))))
        %s
        (return (Extend (AsList (SetResult 'Result selects_)) updates_))
    ))", remoteKey, (ui32)txId+100, body.data());
}

static void PrintRandomResults(const TVector<ui32>& result, const TString& prefix) {
    Cerr << prefix;
    for (ui32 val : result) {
        if (val != Max<ui32>())
            Cerr << val << ' ';
        else
            Cerr << "- ";
    }
    Cerr << Endl;
}

static void CalcPoints(ui32 count, ui32& points, ui32& writes, bool lessWrites = false) {
    ui32 maxValue = (1ul << count) - 1;
    points = RandomNumber<ui32>(maxValue);
    writes = RandomNumber<ui32>(maxValue);
    if (lessWrites)
        writes &= RandomNumber<ui32>(maxValue);
}

static void CompareShots(const TVector<ui32>& finalShot, const TVector<ui32>& intermShot, std::pair<ui32, ui32> range) {
    UNIT_ASSERT(finalShot.size() == intermShot.size());
    UNIT_ASSERT(range.second < finalShot.size());

    ui32 lastKnown = Max<ui32>();
    ui32 leastUnknown = Max<ui32>();
    for (ui32 dot = range.first; dot <= range.second; ++dot) {
        ui32 intermVal = intermShot[dot];
        if (intermVal != Max<ui32>() && (intermVal > lastKnown || lastKnown == Max<ui32>())) {
            lastKnown = intermVal;
        }
        ui32 finalVal = finalShot[dot];
        if (intermVal != finalVal && finalVal < leastUnknown) {
            leastUnknown = finalVal;
        }
    }

    if (lastKnown != Max<ui32>() && leastUnknown != Max<ui32>()) {
        UNIT_ASSERT(lastKnown < leastUnknown);
    }
}

static void RandomTxDeps(const TTester::TOptions& opts, ui32 numTxs, ui32 maxKeys, bool lessWrites,
                         bool useRanges = false, TVector<ui32> counts = {}, TVector<ui32> pts = {},
                         TVector<ui32> wrs = {}) {
    const ui32 minKeys = 2;
    UNIT_ASSERT(maxKeys <= 32);
    UNIT_ASSERT(maxKeys > minKeys);

    TTester t(TTester::ESchema_MultiShardKV, opts);
    TFakeMiniKQLProxy proxy(t);

    ui32 indepPos = 0;
    ui64 indepTxId = 0;
    TString independentTx = R"((
            (let localKey_ '('('key (Uint32 '999))))
            (let remoteKey_ '('('key (Uint32 '1001))))
            (let update_ (UpdateRow 'table1 localKey_ '('('value (Utf8 '"freedom")))))
            (let select2_ (SelectRow 'table1 remoteKey_ '('uint)))
            (return (AsList update_ (SetResult 'R2 select2_)))
        ))";

    if (opts.DelayReadSet) {
        UNIT_ASSERT(numTxs >= 8);
        indepPos = 7;
        indepTxId = proxy.LastTxId() + 8;
        ui64 delayedRS = proxy.LastTxId() + 2; // 2 cause of o-o-o disabled till first complete (LastCompleteTx)
        proxy.DelayReadSet(TExpectedReadSet(delayedRS, {TTestTxConfig::TxTablet0, indepTxId}), opts.RebootOnDelay); 
    } else if (opts.DelayData) {
        UNIT_ASSERT(numTxs >= 8);
        indepPos = 7;
        EvictShardCache(proxy, 500);
        indepTxId = proxy.LastTxId() + 8;
        proxy.DelayData({TTestTxConfig::TxTablet0, indepTxId}); 
    }

    TVector<ui32> expected(32, Max<ui32>());
    for (ui32 i = 0; i < numTxs; ++i) {
        ui32 count = minKeys + RandomNumber<ui32>(maxKeys-minKeys);
        if (counts.size() > i)
            count = counts[i];
        ui32 points = 0;
        ui32 writes = 0;
        CalcPoints(count, points, writes, lessWrites);
        if (pts.size() > i)
            points = pts[i];
        if (wrs.size() > i)
            writes = wrs[i];
        TString prog = MkRandomTx(i, points, writes, count, expected, useRanges);
        //Cout << prog << Endl;
        if (indepTxId && i == indepPos)
            proxy.Enqueue(independentTx);
        proxy.Enqueue(prog);
    }

    ui64 pictureTxId = proxy.LastTxId() + 1;
    TVector<ui32> actual(32, Max<ui32>());
    TVector<ui32> intermediate(32, Max<ui32>());

    auto extractActual = [&](TFakeProxyTx& tx) -> bool {
        TVector<ui32> * out = &actual;
        if (tx.TxId() != pictureTxId) {
            out = &intermediate;
        }

        NKikimrMiniKQL::TResult res = tx.GetResult();
        TValue value = TValue::Create(res.GetValue(), res.GetType());
        TValue rStruct = value["Result"];
        UNIT_ASSERT_EQUAL(bool(rStruct["Truncated"]), false);
        TValue rList = rStruct["List"];

        for (ui32 i = 0; i < rList.Size(); ++i) {
            TValue row = rList[i];
            ui32 key = row["key"];
            TValue opt = row["uint"];
            if (opt.HaveValue()) {
                (*out)[key] = (ui32)opt;
            }
        }
        return true;
    };

    // must be online
    const char * picture = R"((
        (let remoteKey_ '('('key (Uint32 '1001))))
        (let range_ '('IncFrom 'IncTo '('key (Uint32 '0) (Uint32 '32))))
        (let select_ (SelectRange 'table1 range_ '('key 'uint) '()))
        (let forPlan_ (SelectRow 'table1 remoteKey_ '('uint)))
        (return (AsList (SetResult 'Result select_) (SetResult 'Some forPlan_)))
    ))";

    proxy.Enqueue(picture, extractActual, NDataShard::TTxFlags::Default);

    const char * immPicture = R"((
        (let range_ '('IncFrom 'IncTo '('key (Uint32 '%u) (Uint32 '%d))))
        (let select_ (SelectRange 'table1 range_ '('key 'uint) '()))
        (return (AsList (SetResult 'Result select_)))
    ))";

    TVector<std::pair<ui32, ui32>> shots;
    shots.reserve(4);
    shots.push_back({0,7});
    shots.push_back({8,15});
    shots.push_back({16,23});
    shots.push_back({24,31});

    bool sendImmediates = !opts.RebootOnDelay; // can lose some immediate results on restart
    if (sendImmediates) {
        // inconsistent print screen
        for (const auto& s : shots) {
            proxy.Enqueue(Sprintf(immPicture, s.first, s.second), extractActual, NDataShard::TTxFlags::Default);
        }
    }

    proxy.ExecQueue();

    PrintRandomResults(expected, "expect ");
    PrintRandomResults(actual, "actual ");
    PrintRandomResults(intermediate, "interm ");
    for (ui32 i = 0; i < expected.size(); ++i) {
        UNIT_ASSERT_EQUAL(expected[i], actual[i]);
        UNIT_ASSERT(intermediate[i] <= expected[i] || intermediate[i] == Max<ui32>());
    }

    if (sendImmediates) {
        for (const auto& s : shots)
            CompareShots(expected, intermediate, s);
    }
}

//

static constexpr ui32 NumRun() { return 2; }

Y_UNIT_TEST_WITH_MVCC(RandomPoints_ReproducerDelayData1) {
    TTester::TOptions opts;
    opts.DelayData = true;
    opts.ExecutorCacheSize = 0;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    RandomTxDeps(opts, 8, 8, false, false,
                 {5, 5, 6, 7, 3, 6, 5, 6},
                 {28, 26, 13, 27, 3, 3, 27, 36},
                 {11, 30, 10, 12, 5, 23, 30, 32});
}

Y_UNIT_TEST_WITH_MVCC(RandomPoints_ReproducerDelayRS1) {
    TTester::TOptions opts;
    opts.DelayReadSet = true;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    RandomTxDeps(opts, 8, 8, true, false,
                 {2, 7, 7, 7, 6, 3, 2, 4},
                 {2, 66, 30, 104, 25, 4, 0, 1},
                 {0, 40, 2, 33, 8, 4, 1, 3});
}

Y_UNIT_TEST_WITH_MVCC(RandomPoints_DelayRS) {
    TTester::TOptions opts;
    opts.DelayReadSet = true;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    TVector<std::pair<ui32, ui32>> variants;
    variants.push_back({8, 8});
    variants.push_back({8, 16});
    variants.push_back({8, 32});
    variants.push_back({16, 16});
    variants.push_back({16, 32});
    variants.push_back({32, 8});
    variants.push_back({32, 16});
    variants.push_back({32, 32});

    for (ui32 i = 0; i < NumRun(); ++i) {
        for (auto& v : variants) {
            RandomTxDeps(opts, v.first, v.second, true);
            RandomTxDeps(opts, v.first, v.second, false);
        }
    }
}

Y_UNIT_TEST_WITH_MVCC(RandomDotRanges_DelayRS) {
    TTester::TOptions opts;
    opts.DelayReadSet = true;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    TVector<std::pair<ui32, ui32>> variants;
    variants.push_back({8, 8});
    variants.push_back({8, 16});
    variants.push_back({8, 32});
    variants.push_back({16, 16});
    variants.push_back({16, 32});
    variants.push_back({32, 8});
    variants.push_back({32, 16});
    variants.push_back({32, 32});

    for (ui32 i = 0; i < NumRun(); ++i) {
        for (auto& v : variants) {
            RandomTxDeps(opts, v.first, v.second, true, true);
            RandomTxDeps(opts, v.first, v.second, false, true);
        }
    }
}

Y_UNIT_TEST_WITH_MVCC(RandomPoints_DelayRS_Reboot) {
    TTester::TOptions opts;
    opts.DelayReadSet = true;
    opts.RebootOnDelay = true;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    TVector<std::pair<ui32, ui32>> variants;
    variants.push_back({8, 8});
    variants.push_back({8, 16});
    variants.push_back({8, 32});
    variants.push_back({16, 16});
    variants.push_back({16, 32});
    variants.push_back({32, 8});
    variants.push_back({32, 16});
    variants.push_back({32, 32});

    for (ui32 i = 0; i < NumRun(); ++i) {
        for (auto& v : variants) {
            RandomTxDeps(opts, v.first, v.second, true);
            RandomTxDeps(opts, v.first, v.second, false);
        }
    }
}

Y_UNIT_TEST_WITH_MVCC(RandomPoints_DelayRS_Reboot_Dirty) {
    TTester::TOptions opts;
    opts.DelayReadSet = true;
    opts.RebootOnDelay = true;
    opts.EnableSoftUpdates();
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    TVector<std::pair<ui32, ui32>> variants;
    variants.push_back({8, 8});
    variants.push_back({8, 16});
    variants.push_back({8, 32});
    variants.push_back({16, 16});
    variants.push_back({16, 32});
    variants.push_back({32, 8});
    variants.push_back({32, 16});
    variants.push_back({32, 32});

    for (ui32 i = 0; i < NumRun(); ++i) {
        for (auto& v : variants) {
            RandomTxDeps(opts, v.first, v.second, true);
            RandomTxDeps(opts, v.first, v.second, false);
        }
    }
}

Y_UNIT_TEST_WITH_MVCC(RandomPoints_DelayData) {
    TTester::TOptions opts;
    opts.DelayData = true;
    opts.ExecutorCacheSize = 0;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    TVector<std::pair<ui32, ui32>> variants;
    variants.push_back({8, 8});
    variants.push_back({8, 16});
    variants.push_back({8, 32});
    variants.push_back({16, 16});
    variants.push_back({16, 32});
    variants.push_back({32, 8});
    variants.push_back({32, 16});
    variants.push_back({32, 32});

    for (auto& v : variants) {
        RandomTxDeps(opts, v.first, v.second, true);
        RandomTxDeps(opts, v.first, v.second, false);
    }
}

///
class TSimpleTx {
public:
    struct TSimpleRange {
        ui32 From;
        ui32 To;

        TSimpleRange(ui32 from, ui32 to)
            : From(from)
            , To(to)
        {}
    };

    TSimpleTx(ui64 txId)
        : TxId(txId)
    {}

    void Generate(ui32 numWrites, ui32 numPoints, ui32 numRanges, ui32 max = 3000, ui32 maxRange = 100) {
        Writes.reserve(numWrites);
        for (ui32 i = 0; i < numWrites; ++i) {
            Writes.emplace_back(RandomNumber<ui32>(max));
        }

        Reads.reserve(numPoints);
        for (ui32 i = 0; i < numPoints; ++i) {
            Reads.emplace_back(RandomNumber<ui32>(max));
        }

        Ranges.reserve(numRanges);
        for (ui32 i = 0; i < numPoints; ++i) {
            ui32 from = RandomNumber<ui32>(max);
            Ranges.emplace_back(TSimpleRange(from, from + 1 + RandomNumber<ui32>(maxRange-1)));
        }
    }

    TString ToText() {
        const char * writePattern = R"(
            (let $%u '('('key (Uint32 '%u))))
            (let updates_ (Append updates_ (UpdateRow 'table1 $%u '('('uint (Uint32 '%u)))))))";

        const char * readPattern = R"(
            (let $%u '('('key (Uint32 '%u))))
            (let select$%u (SelectRow 'table1 $%u '('key 'uint)))
            (let points_ (Extend points_ (ToList select$%u))))";

        const char * rangePattern = R"(
            (let $%u '('IncFrom 'IncTo '('key (Uint32 '%u) (Uint32 '%u))))
            (let points_ (Extend points_ (Member (SelectRange 'table1 $%u '('key 'uint) '()) 'List))))";

        ui32 key = 0;
        TString body = ProgramTextSwap();
        for (ui32 point : Writes) {
            body += Sprintf(writePattern, key, point, key, TxId);
            ++key;
        }

        for (ui32 point : Reads) {
            body += Sprintf(readPattern, key, point, key, key, key, key);
            ++key;
        }

        for (const auto& r : Ranges) {
            body += Sprintf(rangePattern, key, r.From, r.To, key);
            ++key;
        }

        return Sprintf(R"((
            (let updates_ (List (ListType (TypeOf (UpdateRow 'table1 '('('key (Uint32 '0))) '('('uint (Uint32 '0))))))))
            (let points_ (List (ListType (TypeOf (Unwrap (SelectRow 'table1 '('('key (Uint32 '0))) '('key 'uint)))))))
            %s
            (return (Extend (AsList (SetResult 'Result points_)) updates_))
        ))", body.data());
    }

    ui64 GetTxId() const { return TxId; }
    const TMap<ui32, ui32>& GetResults() const { return Results; }

    void SetResults(const TVector<ui32>& kv) {
        THashSet<ui32> points;
        for (ui32 point : Reads) {
            points.insert(point);
        }

        for (const auto& r : Ranges) {
            for (ui32 i = r.From; i <= r.To; ++i)
                points.insert(i);
        }

        for (ui32 point : points) {
            ui32 value = kv[point];
            if (value != Max<ui32>())
                Results[point] = value;
        }
    }

    void ApplyWrites(TVector<ui32>& kv) const {
        for (ui32 point : Writes) {
            kv[point] = TxId;
        }
    }

private:
    ui64 TxId;
    TVector<ui32> Writes;
    TVector<ui32> Reads;
    TVector<TSimpleRange> Ranges;
    TMap<ui32, ui32> Results;

    static TString ProgramTextSwap(ui32 txId = 0) {
        ui32 point = txId % 1000;
        return Sprintf(R"(
            (let row1 '('('key (Uint32 '%u))))
            (let row2 '('('key (Uint32 '%u))))
            (let row3 '('('key (Uint32 '%u))))
            (let val1 (FlatMap (SelectRow 'table1 row1 '('value)) (lambda '(x) (Member x 'value))))
            (let val2 (FlatMap (SelectRow 'table1 row2 '('value)) (lambda '(x) (Member x 'value))))
            (let val3 (FlatMap (SelectRow 'table1 row3 '('value)) (lambda '(x) (Member x 'value))))
            (let updates_ (Extend updates_ (AsList
                (UpdateRow 'table1 row1 '('('value val3)))
                (UpdateRow 'table1 row2 '('('value val1)))
                (UpdateRow 'table1 row3 '('('value val2)))
            )))
        )", point, 1000+point, 2000+point);
    }
};

///
class TSimpleTable {
public:
    TSimpleTable(ui32 size = 4096) {
        Points.resize(size, Max<ui32>());
    }

    void Apply(TSimpleTx& tx) {
        tx.SetResults(Points);
        tx.ApplyWrites(Points);
    }

private:
    TVector<ui32> Points;
};

void Print(const TMap<ui32, ui32>& m) {
    for (auto& pair : m)
        Cerr << pair.first << ':' << pair.second << ' ';
    Cerr << Endl;
}

void RandomPointsAndRanges(TFakeMiniKQLProxy& proxy, ui32 numTxs, ui32 maxWrites, ui32 maxReads, ui32 maxRanges) {
    TVector<std::shared_ptr<TSimpleTx>> txs;
    txs.reserve(numTxs);
    ui32 startTxId = proxy.LastTxId()+1;
    for (ui32 i = 0; i < numTxs; ++i) {
        txs.push_back(std::make_shared<TSimpleTx>(startTxId + i));
    }

    auto extractActual = [&](TFakeProxyTx& tx) -> bool {
        UNIT_ASSERT(!tx.Immediate());

        auto& expected = txs[tx.TxId()-startTxId]->GetResults();
        TMap<ui32, ui32> actual;

        NKikimrMiniKQL::TResult res = tx.GetResult();
        TValue value = TValue::Create(res.GetValue(), res.GetType());
        TValue rList = value["Result"];
        for (ui32 i = 0; i < rList.Size(); ++i) {
            TValue row = rList[i];
            ui32 key = row["key"];
            TValue opt = row["uint"];
            if (opt.HaveValue()) {
                actual[key] = (ui32)opt;
            }
        }

        if (expected != actual) {
            Print(expected);
            Print(actual);
            UNIT_ASSERT(false);
        }
        return true;
    };

    TSimpleTable table;
    for (auto tx : txs) {
        tx->Generate(RandomNumber<ui32>(maxWrites-1)+1,
                     RandomNumber<ui32>(maxReads-1)+1,
                     RandomNumber<ui32>(maxRanges-1)+1);
        table.Apply(*tx);
        TString progText = tx->ToText();
        //Cout << progText << Endl;
        proxy.Enqueue(progText, extractActual, NDataShard::TTxFlags::ForceOnline);
    }

    proxy.ExecQueue();
}

static void RandomPointsAndRanges(const TTester::TOptions& opts, ui32 numTxs, ui32 maxWrites, ui32 maxReads, ui32 maxRanges) {
    TTester t(TTester::ESchema_MultiShardKV, opts);
    TFakeMiniKQLProxy proxy(t);

    RandomPointsAndRanges(proxy, numTxs, maxWrites, maxReads, maxRanges);
}

Y_UNIT_TEST_WITH_MVCC(RandomPointsAndRanges) {
    TTester::TOptions opts;
    opts.ExecutorCacheSize = 0;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    TVector<TVector<ui32>> variants;
    variants.push_back(TVector<ui32>() = {100, 20, 20, 20});
    variants.push_back(TVector<ui32>() = {100, 50, 50, 50});
    variants.push_back(TVector<ui32>() = {100, 40, 30, 20});
    variants.push_back(TVector<ui32>() = {400, 20, 20, 20});

    for (auto& v : variants) {
        RandomPointsAndRanges(opts, v[0], v[1], v[2], v[3]);
    }
}
}

///
Y_UNIT_TEST_SUITE(DataShardScan) {

Y_UNIT_TEST_WITH_MVCC(ScanFollowedByUpdate) {
    TTester::TOptions opts;
    opts.ExecutorCacheSize = 0;
    opts.EnableOutOfOrder(8);
    opts.EnableMvcc(WithMvcc);

    TTester t(TTester::ESchema_MultiShardKV, opts);
    TFakeMiniKQLProxy proxy(t);

    auto checkScanResult = [](const TFakeProxyTx &tx, TSet<TString> ref) -> bool {
        const TFakeScanTx &scanTx = dynamic_cast<const TFakeScanTx &>(tx);
        YdbOld::ResultSet res = scanTx.GetScanResult();
        //Cerr << res.DebugString() << Endl;
        for (auto &row : res.rows()) {
            auto &val = row.items(0).text_value();
            UNIT_ASSERT(ref.contains(val));
            ref.erase(val);
        }

        UNIT_ASSERT(ref.empty());

        return true;
    };

    InitCrossShard_ABC(proxy, {{1, 2, 3}});

    NKikimrTxDataShard::TDataTransaction dataTransaction;
    dataTransaction.SetStreamResponse(true);
    dataTransaction.SetImmediate(false);
    dataTransaction.SetReadOnly(true);
    auto &tx = *dataTransaction.MutableReadTableTransaction();
    tx.MutableTableId()->SetOwnerId(FAKE_SCHEMESHARD_TABLET_ID);
    tx.MutableTableId()->SetTableId(13);
    auto &c = *tx.AddColumns();
    c.SetId(56);
    c.SetName("value");
    c.SetTypeId(NScheme::NTypeIds::Utf8);

    TSet<TString> ref1{"A", "B", "C"};
    proxy.EnqueueScan(dataTransaction.SerializeAsString(), [ref1, checkScanResult](TFakeProxyTx& tx) {
            return checkScanResult(tx, ref1);
        }, NDataShard::TTxFlags::Default);

    ui32 N = 30;

    auto programText = R"((
        (let row1_ '('('key (Uint32 '0))))
        (let row2_ '('('key (Uint32 '1000))))
        (let row3_ '('('key (Uint32 '2000))))
        (let upd1_ '('('value (Utf8 'A%u)) '('uint (Uint32 '1%u))))
        (let upd2_ '('('value (Utf8 'B%u)) '('uint (Uint32 '2%u))))
        (let upd3_ '('('value (Utf8 'C%u)) '('uint (Uint32 '3%u))))
        (let ret_ (AsList
            (UpdateRow 'table1 row1_ upd1_)
            (UpdateRow 'table1 row2_ upd2_)
            (UpdateRow 'table1 row3_ upd3_)
        ))
        (return ret_)
    ))";

    for (ui32 i = 1; i <= N; ++i) {
        proxy.Enqueue(Sprintf(programText, i, i, i, i, i, i));
    }

    proxy.ExecQueue();

    TSet<TString> ref2{"A" + ToString(N), "B" + ToString(N), "C" + ToString(N)};
    proxy.EnqueueScan(dataTransaction.SerializeAsString(), [ref2, checkScanResult](TFakeProxyTx& tx) {
            return checkScanResult(tx, ref2);
        }, NDataShard::TTxFlags::Default);
    proxy.ExecQueue();
}

Y_UNIT_TEST_QUAD(TestDelayedTxWaitsForWriteActiveTxOnly, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();
    TAutoPtr<IEventHandle> handle;

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
    if (UseNewEngine) {
        runtime.SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_DEBUG);
    } else {
        runtime.SetLogPriority(NKikimrServices::KQP_PROXY, NLog::PRI_DEBUG);
    }
    runtime.SetLogPriority(NKikimrServices::MINIKQL_ENGINE, NActors::NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1), (3, 3);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 2);"));

    ui64 shard2 = GetTableShards(server, sender, "/Root/table-2")[0];

    TVector<TAutoPtr<IEventHandle>> rss;

    // We want to intercept all RS to table-2.
    auto captureRS = [shard2,&rss](TTestActorRuntimeBase &,
                                   TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            auto &rec = event->Get<TEvTxProcessing::TEvReadSet>()->Record;
            if (rec.GetTabletDest() == shard2) {
                rss.push_back(std::move(event));
                return TTestActorRuntime::EEventAction::DROP;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    runtime.SetObserverFunc(captureRS);

    // Send ReadTable request and wait until it hangs waiting for quota.
    {
        auto *req = new TEvTxUserProxy::TEvProposeTransaction;
        req->Record.SetStreamResponse(true);
        auto &tx = *req->Record.MutableTransaction()->MutableReadTableTransaction();
        tx.SetPath("/Root/table-2");

        runtime.Send(new IEventHandle(MakeTxProxyID(), sender, req));
        runtime.GrabEdgeEventRethrow<TEvTxProcessing::TEvStreamQuotaRequest>(handle);
    }

    // Copy data from table-1 to table-3. Txs should hang due to dropped RS.
    SendSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) SELECT key, value FROM `/Root/table-1` WHERE key = 1"));
    SendSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) SELECT key, value FROM `/Root/table-1` WHERE key = 3"));
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(IsTxResultComplete(), 4);
        runtime.DispatchEvents(options);
    }

    // With mvcc (or a better dependency tracking) the read below may start out-of-order,
    // because transactions above are stuck before performing any writes. Make sure it's
    // forced to wait for above transactions by commiting a write that is guaranteed
    // to "happen" after transactions above.
    ExecSQL(server, sender, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (4, 4);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (5, 5);
    )"));

    // This immediate tx should be delayed due to conflict with upserts.
    SendSQL(server, sender, Q_("SELECT * FROM `/Root/table-2`"));
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvDataShard::EvProposeTransaction, 1);
        runtime.DispatchEvents(options);
    }

    // Don't catch RS any more and send caught ones to proceed with upsert.
    runtime.SetObserverFunc(&TTestActorRuntime::DefaultObserverFunc);
    for (auto &rs : rss)
        runtime.Send(rs.Release());

    // Wait for upserts and immediate tx to finish.
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(IsTxResultComplete(), 3);
        runtime.DispatchEvents(options);
    }
}

Y_UNIT_TEST_QUAD(TestOnlyDataTxLagCausesRejects, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();
    TAutoPtr<IEventHandle> handle;

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::KQP_PROXY, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::MINIKQL_ENGINE, NActors::NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 2);
    //auto shards = GetTableShards(server, sender, "/Root/table-1");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 3000000001), (3000000003, 3)"));

    // Send ReadTable requests and wait until they hang waiting for quota.
    for (int i = 0; i < 2; ++i) {
        auto *req = new TEvTxUserProxy::TEvProposeTransaction;
        req->Record.SetStreamResponse(true);
        auto &tx = *req->Record.MutableTransaction()->MutableReadTableTransaction();
        tx.SetPath("/Root/table-1");
        runtime.Send(new IEventHandle(MakeTxProxyID(), sender, req));
        runtime.GrabEdgeEventRethrow<TEvTxProcessing::TEvStreamQuotaRequest>(handle);
    }

    // Now move time forward and check we can still execute data txs.
    runtime.UpdateCurrentTime(runtime.GetCurrentTime() + TDuration::Minutes(10));
    // Wait for mediator timecast.
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvMediatorTimecast::EvUpdate, 1);
        runtime.DispatchEvents(options);
    }

    ExecSQL(server, sender, Q_("SELECT COUNT(*) FROM `/Root/table-1`"));

    // Send SQL request which should hang due to lost RS.
    auto captureRS = [](TTestActorRuntimeBase&,
                        TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet)
            return TTestActorRuntime::EEventAction::DROP;
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    runtime.SetObserverFunc(captureRS);

    SendSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) SELECT value, key FROM `/Root/table-1`"));
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(IsTxResultComplete(), 2);
        runtime.DispatchEvents(options);
    }

    // Now move time forward and check we can still execute data txs.
    runtime.UpdateCurrentTime(runtime.GetCurrentTime() + TDuration::Minutes(10));
    // Wait for mediator timecast.
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvMediatorTimecast::EvUpdate, 1);
        runtime.DispatchEvents(options);
    }

    ExecSQL(server, sender, Q_("SELECT COUNT(*) FROM `/Root/table-1`"), true, Ydb::StatusIds::UNAVAILABLE);
}

}

Y_UNIT_TEST_SUITE(DataShardOutOfOrder) {

Y_UNIT_TEST_QUAD(TestOutOfOrderLockLost, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            readSets.push_back(std::move(event));
            return TTestActorRuntime::EEventAction::DROP;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
        UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);
    }

    // Now send a simple request that would upsert a new value into table-1
    // It would have broken locks if executed before the above commit
    // However the above commit must succeed (readsets are already being exchanged)
    auto sender3 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender3, MakeSimpleRequest(Q_(
        "UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 3)")));

    // Schedule a simple timer to simulate some time passing
    {
        auto sender4 = runtime.AllocateEdgeActor();
        runtime.Schedule(new IEventHandle(sender4, sender4, new TEvents::TEvWakeup()), TDuration::Seconds(1));
        runtime.GrabEdgeEventRethrow<TEvents::TEvWakeup>(sender4);
    }

    // Whatever happens we should resend blocked readsets
    runtime.SetObserverFunc(prevObserverFunc);
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* via actor system */ true);
    }
    readSets.clear();

    // Read the immediate reply first, it must always succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender3);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Read the commit reply next
    bool committed;
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender2);
        auto& response = ev->Get()->Record.GetRef();
        if (response.GetYdbStatus() == Ydb::StatusIds::ABORTED) {
            // Let's suppose somehow locks still managed to become invalidated
            NYql::TIssues issues;
            IssuesFromMessage(response.GetResponse().GetQueryIssues(), issues);
            UNIT_ASSERT(NKqp::HasIssue(issues, NYql::TIssuesIds::KIKIMR_LOCKS_INVALIDATED));
            committed = false;
        } else {
            UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
            committed = true;
        }
    }

    // Select keys 3 and 4 from both tables, either both or none should be inserted
    {
        auto sender5 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender5, MakeSimpleRequest(Q_(R"(
            $rows = (
                SELECT key, value FROM `/Root/table-1` WHERE key = 3
                UNION ALL
                SELECT key, value FROM `/Root/table-2` WHERE key = 4
            );
            SELECT key, value FROM $rows ORDER BY key)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected;
        if (committed) {
            expected = "Struct { List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } List { Struct { Optional { Uint32: 4 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        } else {
            expected = "Struct { } Struct { Bool: false }";
        }
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_NEW_ENGINE(TestMvccReadDoesntBlockWrites) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(true)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();

    auto sender = runtime.AllocateEdgeActor();
    auto sender2 = runtime.AllocateEdgeActor();
    auto sender3 = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 2);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            readSets.push_back(std::move(event));
            return TTestActorRuntime::EEventAction::DROP;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2);)")));

    // Wait until we captured both readsets
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
        UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);
    }
    runtime.SetObserverFunc(prevObserverFunc);

    // it will be blocked by previous transaction that is waiting for its readsets
    SendRequest(runtime, sender, MakeSimpleRequest(Q_(R"(
        $rows = (
            SELECT * FROM `/Root/table-1` WHERE key = 3 OR key = 5
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 4 OR key = 6
        );
        SELECT key, value FROM $rows ORDER BY key)")));

    // wait for the tx is planned
    TDispatchOptions opts;
    opts.FinalEvents.push_back(TDispatchOptions::TFinalEventCondition(TEvTxProcessing::EEv::EvPlanStep, 2));
    runtime.DispatchEvents(opts);

    {
        // despite it's writing into the key that previous transaction reads this write should finish successfully
        auto ev = ExecRequest(runtime, sender3, MakeSimpleRequest(Q_(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 10);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (6, 10))")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // resend readsets, it will unblock both commit tx and read
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* via actor system */ true);
    }
    readSets.clear();

    // Read the commit reply next, it must succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender2);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    {
        // Read should finish successfully and it doesn't see the write
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        TString expected = "Struct { "
                           "List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } "
                           "List { Struct { Optional { Uint32: 4 } } Struct { Optional { Uint32: 2 } } } "
                           "} Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    {
        // Now we see the write
        auto ev = ExecRequest(runtime, sender, MakeSimpleRequest(Q_(R"(
        $rows = (
            SELECT * FROM `/Root/table-1` WHERE key = 3 OR key = 5
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 4 OR key = 6
        );
        SELECT key, value FROM $rows ORDER BY key)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { "
                           "List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } "
                           "List { Struct { Optional { Uint32: 4 } } Struct { Optional { Uint32: 2 } } } "
                           "List { Struct { Optional { Uint32: 5 } } Struct { Optional { Uint32: 10 } } } "
                           "List { Struct { Optional { Uint32: 6 } } Struct { Optional { Uint32: 10 } } } "
                           "} Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_QUAD(TestOutOfOrderReadOnlyAllowed, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            readSets.push_back(std::move(event));
            return TTestActorRuntime::EEventAction::DROP;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
        UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);
    }

    // Now send a simple read request from table-1
    // Since it's readonly it cannot affect inflight transaction and shouled be allowed
    {
        auto sender3 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender3, MakeSimpleRequest(Q_(
            "SELECT key, value FROM `/Root/table-1` ORDER BY key")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 1 } } Struct { Optional { Uint32: 1 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    // Whatever happens we should resend blocked readsets
    runtime.SetObserverFunc(prevObserverFunc);
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* via actor system */ true);
    }
    readSets.clear();

    // Read the commit reply next, it must succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender2);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Select keys 3 and 4 from both tables, both should have been be inserted
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender4, MakeSimpleRequest(Q_(R"(
            $rows = (
                SELECT key, value FROM `/Root/table-1` WHERE key = 3
                UNION ALL
                SELECT key, value FROM `/Root/table-2` WHERE key = 4
            );
            SELECT key, value FROM $rows ORDER BY key)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } List { Struct { Optional { Uint32: 4 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_QUAD(TestOutOfOrderNonConflictingWrites, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 1
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            readSets.push_back(std::move(event));
            return TTestActorRuntime::EEventAction::DROP;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Now send non-conflicting upsert to both tables
    {
        auto sender3 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender3, MakeSimpleRequest(Q_(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 3);
            UPSERT INTO `/Root/table-2` (key, value) VALUES (6, 3))")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Check that immediate non-conflicting upsert is working too
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender4, MakeSimpleRequest(Q_(
            "UPSERT INTO `/Root/table-1` (key, value) VALUES (7, 4)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Resend previousy blocked readsets
    runtime.SetObserverFunc(prevObserverFunc);
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* via actor system */ true);
    }
    readSets.clear();

    // Read the commit reply next, it must succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender2);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Select keys 3 and 4 from both tables, both should have been inserted
    {
        auto sender5 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender5, MakeSimpleRequest(Q_(R"(
            $rows = (
                SELECT key, value FROM `/Root/table-1` WHERE key = 3
                UNION ALL
                SELECT key, value FROM `/Root/table-2` WHERE key = 4
            );
            SELECT key, value FROM $rows ORDER BY key)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } List { Struct { Optional { Uint32: 4 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_NEW_ENGINE(TestOutOfOrderRestartLocksSingleWithoutBarrier) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(false) // intentionally, because we test non-mvcc locks logic 
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    // This test requires barrier to be disabled
    runtime.GetAppData().FeatureFlags.SetDisableDataShardBarrier(true);

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);
    auto table1shards = GetTableShards(server, sender, "/Root/table-1");
    auto table2shards = GetTableShards(server, sender, "/Root/table-2");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto sender1 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender1, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 1
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            readSets.push_back(std::move(event));
            return TTestActorRuntime::EEventAction::DROP;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Reboot table-1 tablet
    readSets.clear();
    RebootTablet(runtime, table1shards[0], sender); 

    // Wait until we captured both readsets again
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Select keys 1 and 3, we expect this immediate tx to succeed
    // Note that key 3 is not written yet, but we pretend immediate tx
    // executes before that waiting transaction (no key 3 yet).
    {
        auto sender3 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender3, MakeSimpleRequest(Q_(
            "SELECT key, value FROM `/Root/table-1` WHERE key = 1 OR key = 3;")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 1 } } Struct { Optional { Uint32: 1 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    // Upsert key 1, we expect this immediate tx to timeout
    // Another tx has already checked locks for that key, we must never
    // pretend some other conflicting write happened before that tx completes.
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 3);"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender4, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::TIMEOUT);
    }

    // Upsert key 5, this immediate tx should timeout because we currently
    // lose information on locked keys after reboot and it acts as a global
    // barrier.
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 3);"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender4, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::TIMEOUT);
    }

    // Release readsets allowing tx to progress
    runtime.SetObserverFunc(prevObserverFunc);
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* viaActorSystem */ true);
    }

    // Select key 3, we expect a success
    {
        auto sender9 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender9, MakeSimpleRequest(Q_(
            "SELECT key, value FROM `/Root/table-1` WHERE key = 3;")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_NEW_ENGINE(MvccTestOutOfOrderRestartLocksSingleWithoutBarrier) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(true)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    // This test requires barrier to be disabled
    runtime.GetAppData().FeatureFlags.SetDisableDataShardBarrier(true);

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);
    auto table1shards = GetTableShards(server, sender, "/Root/table-1");
    auto table2shards = GetTableShards(server, sender, "/Root/table-2");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto sender1 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender1, MakeBeginRequest(sessionId, Q_(R"(
             SELECT * FROM `/Root/table-1` WHERE key = 1
             UNION ALL
             SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            readSets.push_back(std::move(event));
            return TTestActorRuntime::EEventAction::DROP;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Reboot table-1 tablet
    readSets.clear();
    RebootTablet(runtime, table1shards[0], sender); 

    // Wait until we captured both readsets again
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Select keys 1 and 3, we expect this immediate tx to succeed
    // Note that key 3 is not written yet, but we pretend immediate tx
    // executes before that waiting transaction (no key 3 yet).
    {
        auto sender3 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender3, MakeSimpleRequest(Q_(
            "SELECT key, value FROM `/Root/table-1` WHERE key = 1 OR key = 3;")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 1 } } Struct { Optional { Uint32: 1 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    // Upsert key 1, we expect this immediate tx to be executed successfully because it lies to the right on the global timeline
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 3);"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender4, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Upsert key 5, this immediate tx should be executed successfully too
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 3);"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender4, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Release readsets allowing tx to progress
    runtime.SetObserverFunc(prevObserverFunc);
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* viaActorSystem */ true);
    }

    // Select key 3, we expect a success
    {
        auto sender9 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender9, MakeSimpleRequest(Q_(
            "SELECT key, value FROM `/Root/table-1` WHERE key = 3;")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_QUAD(TestOutOfOrderRestartLocksReorderedWithoutBarrier, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    // This test requires barrier to be disabled
    runtime.GetAppData().FeatureFlags.SetDisableDataShardBarrier(true);

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);
    auto table1shards = GetTableShards(server, sender, "/Root/table-1");
    auto table2shards = GetTableShards(server, sender, "/Root/table-2");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto sender1 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender1, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 1
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        if (event->GetTypeRewrite() == TEvTxProcessing::EvReadSet) {
            readSets.push_back(std::move(event));
            return TTestActorRuntime::EEventAction::DROP;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Execute some out-of-order upserts before rebooting
    ExecSQL(server, sender, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 3);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (6, 3))"));

    // Select key 3, we expect a timeout, because logically writes
    // to 3 and 5 already happened, but physically write to 3 is still waiting.
    {
        auto sender3 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 3;"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender3, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::TIMEOUT);
    }

    // Reboot table-1 tablet
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);
    readSets.clear();
    RebootTablet(runtime, table1shards[0], sender); 

    // Wait until we captured both readsets again
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Select key 3, we still expect a timeout
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 3;"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender4, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::TIMEOUT);
    }

    // Select key 5, it shouldn't pose any problems
    {
        auto sender5 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender5, MakeSimpleRequest(Q_(
            "SELECT key, value FROM `/Root/table-1` WHERE key = 5;")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 5 } } Struct { Optional { Uint32: 3 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    // Release readsets allowing tx to progress
    runtime.SetObserverFunc(prevObserverFunc);
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* viaActorSystem */ true);
    }

    // Select key 3, we expect a success
    {
        auto sender6 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender6, MakeSimpleRequest(Q_(
            "SELECT key, value FROM `/Root/table-1` WHERE key = 3;")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_QUAD(TestOutOfOrderNoBarrierRestartImmediateLongTail, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    // This test requires barrier to be disabled
    runtime.GetAppData().FeatureFlags.SetDisableDataShardBarrier(true);

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);
    auto table1shards = GetTableShards(server, sender, "/Root/table-1");
    auto table2shards = GetTableShards(server, sender, "/Root/table-2");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto sender1 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender1, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 1
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    THashMap<TActorId, ui64> actorToTablet;
    TVector<THolder<IEventHandle>> readSets;
    TVector<THolder<IEventHandle>> progressEvents;
    bool blockProgressEvents = false;
    size_t bypassProgressEvents = 0;
    auto captureRS = [&](TTestActorRuntimeBase&,
                         TAutoPtr<IEventHandle> &event) -> auto {
        auto recipient = event->GetRecipientRewrite();
        switch (event->GetTypeRewrite()) {
            case TEvTablet::EvBoot: {
                auto* msg = event->Get<TEvTablet::TEvBoot>();
                auto tabletId = msg->TabletID;
                Cerr << "... found " << recipient << " to be tablet " << tabletId << Endl;
                actorToTablet[recipient] = tabletId;
                break;
            }
            case TEvTxProcessing::EvReadSet: {
                readSets.push_back(std::move(event));
                return TTestActorRuntime::EEventAction::DROP;
            }
            case EventSpaceBegin(TKikimrEvents::ES_PRIVATE) + 0 /* EvProgressTransaction */: {
                ui64 tabletId = actorToTablet.Value(recipient, 0);
                if (blockProgressEvents && tabletId == table1shards[0]) {
                    if (bypassProgressEvents == 0) {
                        Cerr << "... captured TEvProgressTransaction" << Endl;
                        progressEvents.push_back(std::move(event));
                        return TTestActorRuntime::EEventAction::DROP;
                    }
                    Cerr << "... bypass for TEvProgressTransaction" << Endl;
                    --bypassProgressEvents;
                }
                break;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Send some more requests that form a staircase, they would all be blocked as well
    auto sender3 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender3, MakeSimpleRequest(Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 3), (5, 3);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 3), (6, 3))")));
    SimulateSleep(server, TDuration::Seconds(1));
    SendRequest(runtime, sender3, MakeSimpleRequest(Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 4), (7, 4);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (6, 4), (8, 4))")));
    SimulateSleep(server, TDuration::Seconds(1));

    // One more request that would be executed out of order
    ExecSQL(server, sender, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (11, 5);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (12, 5))"));

    // Select key 7, we expect a timeout, because logically a write to it already happened
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 7;"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender4, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::TIMEOUT);
    }

    // Reboot table-1 tablet
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);
    readSets.clear();
    blockProgressEvents = true;
    bypassProgressEvents = 1;
    Cerr << "... rebooting tablet" << Endl;
    RebootTablet(runtime, table1shards[0], sender); 
    Cerr << "... tablet rebooted" << Endl;

    // Wait until we captured both readsets again
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Wait until we have a pending progress event
    if (progressEvents.size() < 1) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return progressEvents.size() >= 1;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(progressEvents.size(), 1u);

    // Select key 7 again, we still expect a timeout, because logically a write to it already happened
    {
        auto sender5 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 7;"));
        req->Record.MutableRequest()->SetCancelAfterMs(1000);
        req->Record.MutableRequest()->SetTimeoutMs(1000);
        auto ev = ExecRequest(runtime, sender5, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::TIMEOUT);
    }

    // Stop blocking readsets and unblock progress
    runtime.SetObserverFunc(prevObserverFunc);
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, true);
    }
    for (auto& ev : progressEvents) {
        runtime.Send(ev.Release(), 0, true);
    }

    // Select key 7 again, this time is should succeed
    {
        auto sender6 = runtime.AllocateEdgeActor();
        auto req = MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 7;"));
        auto ev = ExecRequest(runtime, sender6, std::move(req));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 7 } } Struct { Optional { Uint32: 4 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

namespace {
    ui64 AsyncCreateCopyTable(
            Tests::TServer::TPtr server,
            TActorId sender,
            const TString &root,
            const TString &name,
            const TString &from)
    {
        auto &runtime = *server->GetRuntime();

        // Create table with four shards.
        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetExecTimeoutPeriod(Max<ui64>());
        auto &tx = *request->Record.MutableTransaction()->MutableModifyScheme();
        tx.SetOperationType(NKikimrSchemeOp::ESchemeOpCreateTable);
        tx.SetWorkingDir(root);
        auto &desc = *tx.MutableCreateTable();
        desc.SetName(name);
        desc.SetCopyFromTable(from);

        runtime.Send(new IEventHandle(MakeTxProxyID(), sender, request.Release()));
        auto ev = runtime.GrabEdgeEventRethrow<TEvTxUserProxy::TEvProposeTransactionStatus>(sender);
        UNIT_ASSERT_VALUES_EQUAL(ev->Get()->Record.GetStatus(), TEvTxUserProxy::TEvProposeTransactionStatus::EStatus::ExecInProgress);
        return ev->Get()->Record.GetTxId();
    }
}

Y_UNIT_TEST_QUAD(TestCopyTableNoDeadlock, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId,Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    TVector<THolder<IEventHandle>> txProposes;
    size_t seenPlanSteps = 0;
    bool captureReadSets = true;
    bool captureTxProposes = false;
    auto captureRS = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvTxProcessing::EvPlanStep:
                Cerr << "---- observed EvPlanStep ----" << Endl;
                ++seenPlanSteps;
                break;
            case TEvTxProcessing::EvReadSet:
                Cerr << "---- observed EvReadSet ----" << Endl;
                if (captureReadSets) {
                    readSets.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            case TEvTxProxy::EvProposeTransaction:
                Cerr << "---- observed EvProposeTransaction ----" << Endl;
                if (captureTxProposes) {
                    txProposes.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto senderCommit = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderCommit, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    if (readSets.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    captureTxProposes = true;

    // Now we send a distributed read, while stopping coordinator proposals
    auto senderRead = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderRead, MakeSimpleRequest(Q_(R"(
        SELECT * FROM `/Root/table-1`
        UNION ALL
        SELECT * FROM `/Root/table-2`)")));

    // Wait until we capture the propose request
    if (txProposes.size() < 1) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return txProposes.size() >= 1;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(txProposes.size(), 1u);

    Cerr << "---- captured propose for data tx ----" << Endl;

    // Now we send a copy table request, again blocking coordinator proposal
    auto senderCopy = runtime.AllocateEdgeActor();
    auto txIdCopy = AsyncCreateCopyTable(server, senderCopy, "/Root", "table-3", "/Root/table-2");

    // Wait until we capture the propose request
    if (txProposes.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return txProposes.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(txProposes.size(), 2u);

    Cerr << "---- captured propose for copy tx ----" << Endl;

    // Stop capturing stuff
    captureReadSets = false;
    captureTxProposes = false;

    // Release copy tx propose and wait for plan steps (table-3, table-2 and schemeshard)
    // It is important for copy tx to be planned *before* the read tx
    seenPlanSteps = 0;
    runtime.Send(txProposes[1].Release(), 0, /* via actor system */ true);
    if (seenPlanSteps < 3) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return seenPlanSteps >= 3;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(seenPlanSteps, 3u);

    // Release read tx propose and wait for plan steps (table-1 and table-2)
    // Now read tx will be planned *after* the copy tx
    seenPlanSteps = 0;
    runtime.Send(txProposes[0].Release(), 0, /* via actor system */ true);
    if (seenPlanSteps < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return seenPlanSteps >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(seenPlanSteps, 2u);

    // Sleep a little so that everything stops in a settled state
    // Bug KIKIMR-7711 would cause copy tx and read tx to depend on each other
    SimulateSleep(server, TDuration::Seconds(1));

    // Release readsets, allowing the first commit to finish
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* via actor system */ true);
    }

    // Wait for commit to complete, it must succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderCommit);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Wait for copy table tx to complete
    WaitTxNotification(server, senderCopy, txIdCopy);

    // Wait for distributed read to complete, it must succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderRead);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }
}

Y_UNIT_TEST_NEW_ENGINE(TestPlannedCancelSplit) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    auto shards1 = GetTableShards(server, sender, "/Root/table-1");
    UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);
    auto shards2 = GetTableShards(server, sender, "/Root/table-2");
    UNIT_ASSERT_VALUES_EQUAL(shards2.size(), 1u);
    TVector<ui64> tablets;
    tablets.push_back(shards1[0]);
    tablets.push_back(shards2[0]);

    // Capture and block some messages
    bool captureTxCancel = false;
    bool captureTxPropose = false;
    bool captureTxProposeResult = false;
    TVector<THolder<IEventHandle>> txCancels;
    TVector<THolder<IEventHandle>> txProposes;
    TVector<THolder<IEventHandle>> txProposeResults;
    auto captureMessages = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvDataShard::EvProposeTransaction:
                Cerr << "---- observed EvProposeTransaction ----" << Endl;
                if (captureTxPropose) {
                    txProposes.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            case TEvDataShard::EvProposeTransactionResult:
                Cerr << "---- observed EvProposeTransactionResult ----" << Endl;
                if (captureTxProposeResult) {
                    txProposeResults.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            case TEvDataShard::EvCancelTransactionProposal:
                Cerr << "---- observed EvCancelTransactionProposal ----" << Endl;
                if (captureTxCancel) {
                    txCancels.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureMessages);

    // Send a distributed read while capturing propose results
    captureTxProposeResult = true;
    auto senderRead1 = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderRead1, MakeSimpleRequest(Q_(R"(
        SELECT * FROM `/Root/table-1`
        UNION ALL
        SELECT * FROM `/Root/table-2`)")));
    if (txProposeResults.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return txProposeResults.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(txProposeResults.size(), 2u);
    captureTxProposeResult = false;

    // Remember which senders claim to be which shards
    TVector<TActorId> actors(2);
    for (auto& event : txProposeResults) {
        TActorId actor = event->Sender;
        const auto* msg = event->Get<TEvDataShard::TEvProposeTransactionResult>();
        ui64 shard = msg->Record.GetOrigin();
        for (size_t i = 0; i < tablets.size(); ++i) {
            if (tablets[i] == shard) {
                actors[i] = actor;
            }
        }
        runtime.Send(event.Release(), 0, /* via actor system */ true);
    }
    txProposeResults.clear();

    // Wait for the first query result, it must succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderRead1);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Send a distributed read again, while blocking propose messages
    captureTxPropose = true;
    auto senderRead2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderRead2, MakeSimpleRequest(Q_(R"(
        SELECT * FROM `/Root/table-1`
        UNION ALL
        SELECT * FROM `/Root/table-2`)")));
    if (txProposes.size() < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return txProposes.size() >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(txProposes.size(), 2u);
    captureTxPropose = false;

    // Simulate propose overloaded at the second table
    captureTxCancel = true;
    for (auto& event : txProposes) {
        if (event && event->GetRecipientRewrite() == actors[1]) {
            Cerr << "---- found propose for table-2 ----" << Endl;
            const auto* msg = event->Get<TEvDataShard::TEvProposeTransaction>();
            TActorId target = msg->GetSource();
            auto* result = new TEvDataShard::TEvProposeTransactionResult(
                msg->GetTxKind(),
                tablets[1],
                msg->GetTxId(),
                NKikimrTxDataShard::TEvProposeTransactionResult::OVERLOADED);
            Cerr << "Sending error result from " << actors[1] << " to " << target << Endl;
            runtime.Send(new IEventHandle(target, actors[1], result), 0, /* via actor system */ true);
            event.Reset(); // drop this propose event
        }
    }
    if (txCancels.size() < 1) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return txCancels.size() >= 1;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(txCancels.size(), 1u);
    captureTxCancel = false;

    // Now send propose and cancel messages in quick succession
    for (auto& event : txProposes) {
        if (event) {
            runtime.Send(event.Release(), 0, /* via actor system */ true);
        }
    }
    for (auto& event : txCancels) {
        runtime.Send(event.Release(), 0, /* via actor system */ true);
    }

    // Wait for query to return an error
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderRead2);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::OVERLOADED);
    }

    // Sleep a little so in case of a bug transaction is left in WaitForPlan state
    SimulateSleep(server, TDuration::Seconds(1));

    // Split would fail otherwise :(
    SetSplitMergePartCountLimit(server->GetRuntime(), -1);

    // Start split for table-1
    TInstant splitStart = TInstant::Now();
    auto senderSplit = runtime.AllocateEdgeActor();
    ui64 txId = AsyncSplitTable(server, senderSplit, "/Root/table-1", tablets[0], 100);
    WaitTxNotification(server, senderSplit, txId);

    // Split shouldn't take too much time to complete
    TDuration elapsed = TInstant::Now() - splitStart;
    UNIT_ASSERT_C(elapsed < TDuration::Seconds(NValgrind::PlainOrUnderValgrind(2, 10)),
        "Split needed " << elapsed.ToString() << " to complete, which is too long");
}

Y_UNIT_TEST_QUAD(TestPlannedTimeoutSplit, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    auto shards1 = GetTableShards(server, sender, "/Root/table-1");
    UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);
    auto shards2 = GetTableShards(server, sender, "/Root/table-2");
    UNIT_ASSERT_VALUES_EQUAL(shards2.size(), 1u);

    // Capture and block some messages
    TVector<THolder<IEventHandle>> txProposes;
    auto captureMessages = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvTxProxy::EvProposeTransaction: {
                Cerr << "---- observed EvProposeTransaction ----" << Endl;
                txProposes.push_back(std::move(event));
                return TTestActorRuntime::EEventAction::DROP;
            }
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureMessages);

    // Send a distributed write while capturing coordinator propose
    auto senderWrite1 = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderWrite1, MakeSimpleRequest(Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (101, 101);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (202, 202);
    )")));
    if (txProposes.size() < 1) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return txProposes.size() >= 1;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(txProposes.size(), 1u);
    runtime.SetObserverFunc(prevObserverFunc);

    size_t observedSplits = 0;
    auto observeSplits = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvDataShard::EvSplit: {
                Cerr << "---- observed EvSplit ----" << Endl;
                ++observedSplits;
                break;
            }
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    runtime.SetObserverFunc(observeSplits);

    // Split would fail otherwise :(
    SetSplitMergePartCountLimit(server->GetRuntime(), -1);

    // Start split for table-1 and table-2
    auto senderSplit = runtime.AllocateEdgeActor();
    ui64 txId1 = AsyncSplitTable(server, senderSplit, "/Root/table-1", shards1[0], 100);
    ui64 txId2 = AsyncSplitTable(server, senderSplit, "/Root/table-2", shards2[0], 100);

    // Wait until we observe both splits on both shards
    if (observedSplits < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return observedSplits >= 2;
            });
        runtime.DispatchEvents(options);
    }

    // Sleep a little so everything settles
    SimulateSleep(server, TDuration::Seconds(1));

    // We expect splits to finish successfully
    WaitTxNotification(server, senderSplit, txId1);
    WaitTxNotification(server, senderSplit, txId2);

    // We expect split to fully succeed on proposed transaction timeout
    auto shards1new = GetTableShards(server, sender, "/Root/table-1");
    UNIT_ASSERT_VALUES_EQUAL(shards1new.size(), 2u);
    auto shards2new = GetTableShards(server, sender, "/Root/table-2");
    UNIT_ASSERT_VALUES_EQUAL(shards2new.size(), 2u);

    // Unblock previously blocked coordinator propose
    for (auto& ev : txProposes) {
        runtime.Send(ev.Release(), 0, true);
    }

    // Wait for query to return an error
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderWrite1);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::UNAVAILABLE);
    }
}

Y_UNIT_TEST_QUAD(TestPlannedHalfOverloadedSplit, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    auto shards1 = GetTableShards(server, sender, "/Root/table-1");
    UNIT_ASSERT_VALUES_EQUAL(shards1.size(), 1u);
    auto shards2 = GetTableShards(server, sender, "/Root/table-2");
    UNIT_ASSERT_VALUES_EQUAL(shards2.size(), 1u);
    TVector<ui64> tablets;
    tablets.push_back(shards1[0]);
    tablets.push_back(shards2[0]);

    // Capture and block some messages
    TVector<THolder<IEventHandle>> txProposes;
    TVector<THolder<IEventHandle>> txProposeResults;
    auto captureMessages = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvDataShard::EvProposeTransaction: {
                Cerr << "---- observed EvProposeTransactionResult ----" << Endl;
                if (txProposes.size() == 0) {
                    // Capture the first propose
                    txProposes.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            }
            case TEvDataShard::EvProposeTransactionResult: {
                Cerr << "---- observed EvProposeTransactionResult ----" << Endl;
                if (txProposes.size() > 0) {
                    // Capture all propose results
                    txProposeResults.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            }
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureMessages);

    // Send a distributed write while capturing coordinator propose
    auto senderWrite1 = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderWrite1, MakeSimpleRequest(Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (101, 101);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (202, 202);
    )")));
    if (txProposes.size() < 1 || txProposeResults.size() < 1) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return txProposes.size() >= 1 && txProposeResults.size() >= 1;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(txProposes.size(), 1u);
    UNIT_ASSERT_VALUES_EQUAL(txProposeResults.size(), 1u);

    size_t observedSplits = 0;
    auto observeSplits = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvDataShard::EvSplit: {
                Cerr << "---- observed EvSplit ----" << Endl;
                ++observedSplits;
                break;
            }
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    runtime.SetObserverFunc(observeSplits);

    // Split would fail otherwise :(
    SetSplitMergePartCountLimit(server->GetRuntime(), -1);

    // Start split for table-1 and table-2
    auto senderSplit = runtime.AllocateEdgeActor();
    ui64 txId1 = AsyncSplitTable(server, senderSplit, "/Root/table-1", shards1[0], 100);
    ui64 txId2 = AsyncSplitTable(server, senderSplit, "/Root/table-2", shards2[0], 100);

    // Wait until we observe both splits on both shards
    if (observedSplits < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return observedSplits >= 2;
            });
        runtime.DispatchEvents(options);
    }

    // Sleep a little so everything settles
    SimulateSleep(server, TDuration::Seconds(1));

    // Unblock previously blocked proposes and results
    for (auto& ev : txProposeResults) {
        runtime.Send(ev.Release(), 0, true);
    }
    for (auto& ev : txProposes) {
        runtime.Send(ev.Release(), 0, true);
    }

    // We expect splits to finish successfully
    WaitTxNotification(server, senderSplit, txId1);
    WaitTxNotification(server, senderSplit, txId2);

    // We expect split to fully succeed on proposed transaction timeout
    auto shards1new = GetTableShards(server, sender, "/Root/table-1");
    UNIT_ASSERT_VALUES_EQUAL(shards1new.size(), 2u);
    auto shards2new = GetTableShards(server, sender, "/Root/table-2");
    UNIT_ASSERT_VALUES_EQUAL(shards2new.size(), 2u);

    // Wait for query to return an error
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderWrite1);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_C(
            response.GetYdbStatus() == Ydb::StatusIds::OVERLOADED ||
            response.GetYdbStatus() == Ydb::StatusIds::UNAVAILABLE,
            "Status: " << response.GetYdbStatus());
    }
}

namespace {

    void AsyncReadTable(
            Tests::TServer::TPtr server,
            TActorId sender,
            const TString& path)
    {
        auto &runtime = *server->GetRuntime();

        auto request = MakeHolder<TEvTxUserProxy::TEvProposeTransaction>();
        request->Record.SetStreamResponse(true);
        auto &tx = *request->Record.MutableTransaction()->MutableReadTableTransaction();
        tx.SetPath(path);
        tx.SetApiVersion(NKikimrTxUserProxy::TReadTableTransaction::YDB_V1);
        tx.AddColumns("key");
        tx.AddColumns("value");

        runtime.Send(new IEventHandle(MakeTxProxyID(), sender, request.Release()));
    }

}

/**
 * Regression test for KIKIMR-7751, designed to crash under asan
 */
Y_UNIT_TEST_NEW_ENGINE(TestReadTableWriteConflict) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    // NOTE: table-1 has 2 shards so ReadTable is not immediate
    CreateShardedTable(server, sender, "/Root", "table-1", 2);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO [/Root/table-1] (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO [/Root/table-2] (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(
            "SELECT * FROM [/Root/table-1] "
            "UNION ALL "
            "SELECT * FROM [/Root/table-2]")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    TVector<THolder<IEventHandle>> txProposes;
    size_t seenPlanSteps = 0;
    bool captureReadSets = true;
    auto captureRS = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvTxProcessing::EvPlanStep:
                Cerr << "---- observed EvPlanStep ----" << Endl;
                ++seenPlanSteps;
                break;
            case TEvTxProcessing::EvReadSet:
                Cerr << "---- observed EvReadSet ----" << Endl;
                if (captureReadSets) {
                    readSets.push_back(std::move(event));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto senderCommit = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderCommit, MakeCommitRequest(sessionId, txId, Q_(
        "UPSERT INTO [/Root/table-1] (key, value) VALUES (3, 2); "
        "UPSERT INTO [/Root/table-2] (key, value) VALUES (4, 2)")));

    // Wait until we captured all readsets
    if (readSets.size() < 4) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return readSets.size() >= 4;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 4u);
    captureReadSets = false;

    // Start reading table-1, wait for its plan step
    seenPlanSteps = 0;
    auto senderReadTable = runtime.AllocateEdgeActor();
    AsyncReadTable(server, senderReadTable, "/Root/table-1");
    if (seenPlanSteps < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return seenPlanSteps >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(seenPlanSteps, 2u);
    seenPlanSteps = 0;

    // Start an immediate write to table-1, it won't be able to start
    // because it arrived after the read table and they block each other
    auto senderWriteImm = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderWriteImm, MakeSimpleRequest(Q_(
        "UPSERT INTO [/Root/table-1] (key, value) VALUES (5, 3)")));

    // Start a planned write to both tables, wait for its plan step too
    auto senderWriteDist = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderWriteDist, MakeSimpleRequest(Q_(
        "UPSERT INTO [/Root/table-1] (key, value) VALUES (7, 4); "
        "UPSERT INTO [/Root/table-2] (key, value) VALUES (8, 4)")));
    if (seenPlanSteps < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return seenPlanSteps >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(seenPlanSteps, 2u);
    seenPlanSteps = 0;

    // Make sure everything is settled down
    SimulateSleep(server, TDuration::Seconds(1));

    // Unblock readsets, letting everything go
    for (auto& ev : readSets) {
        runtime.Send(ev.Release(), 0, /* via actor system */ true);
    }
    readSets.clear();

    // Wait for commit to succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderCommit);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Wait for immediate write to succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderWriteImm);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Wait for distributed write to succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderWriteDist);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }
}

/**
 * Regression test for KIKIMR-7903
 */
Y_UNIT_TEST_NEW_ENGINE(TestReadTableImmediateWriteBlock) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    // NOTE: table-1 has 2 shards so ReadTable is not immediate
    CreateShardedTable(server, sender, "/Root", "table-1", 2);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));

    // Capture and block all readset messages
    size_t seenPlanSteps = 0;
    auto captureEvents = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvTxProcessing::EvPlanStep:
                Cerr << "---- observed EvPlanStep ----" << Endl;
                ++seenPlanSteps;
                break;
            case TEvTxProcessing::EvStreamClearanceResponse:
                Cerr << "---- dropped EvStreamClearanceResponse ----" << Endl;
                return TTestActorRuntime::EEventAction::DROP;
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

    // Start reading table-1, wait for its plan step
    // Since we drop EvStreamClearanceResponse it will block forever
    seenPlanSteps = 0;
    auto senderReadTable = runtime.AllocateEdgeActor();
    AsyncReadTable(server, senderReadTable, "/Root/table-1");
    if (seenPlanSteps < 2) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle &) -> bool {
                return seenPlanSteps >= 2;
            });
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(seenPlanSteps, 2u);

    // Make sure everything is settled down
    SimulateSleep(server, TDuration::Seconds(1));

    // Start an immediate write to table-1, it should be able to complete
    auto senderWriteImm = runtime.AllocateEdgeActor();
    SendRequest(runtime, senderWriteImm, MakeSimpleRequest(Q_(
        "UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 3)")));

    // Wait for immediate write to succeed
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(senderWriteImm);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }
}

Y_UNIT_TEST_QUAD(TestReadTableSingleShardImmediate, WithMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);
    runtime.GetAppData().AllowReadTableImmediate = true;

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));

    // Capture and block all readset messages
    size_t seenPlanSteps = 0;
    auto captureEvents = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvTxProcessing::EvPlanStep:
                Cerr << "---- observed EvPlanStep ----" << Endl;
                ++seenPlanSteps;
                break;
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

    // Start reading table-1
    seenPlanSteps = 0;
    auto senderReadTable = runtime.AllocateEdgeActor();
    AsyncReadTable(server, senderReadTable, "/Root/table-1");

    // Wait for the first quota request
    runtime.GrabEdgeEventRethrow<TEvTxProcessing::TEvStreamQuotaRequest>(senderReadTable);

    // Since ReadTable was for a single-shard table we shouldn't see any plan steps
    UNIT_ASSERT_VALUES_EQUAL(seenPlanSteps, 0u);
}

Y_UNIT_TEST_NEW_ENGINE(TestImmediateQueueThenSplit) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);

    auto tablets = GetTableShards(server, sender, "/Root/table-1");

    // We need shard to have some data (otherwise it would die too quickly)
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (0, 0);"));

    bool captureSplit = true;
    bool captureSplitChanged = true;
    bool capturePropose = true;
    THashSet<TActorId> captureDelayedProposeFrom;
    TVector<THolder<IEventHandle>> eventsSplit;
    TVector<THolder<IEventHandle>> eventsSplitChanged;
    TVector<THolder<IEventHandle>> eventsPropose;
    TVector<THolder<IEventHandle>> eventsDelayedPropose;
    auto captureEvents = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvDataShard::EvSplit:
                if (captureSplit) {
                    Cerr << "---- captured EvSplit ----" << Endl;
                    eventsSplit.emplace_back(event.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            case TEvDataShard::EvSplitPartitioningChanged:
                if (captureSplitChanged) {
                    Cerr << "---- captured EvSplitPartitioningChanged ----" << Endl;
                    eventsSplitChanged.emplace_back(event.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            case TEvDataShard::EvProposeTransaction:
                if (capturePropose) {
                    Cerr << "---- capture EvProposeTransaction ----" << Endl;
                    eventsPropose.emplace_back(event.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            case EventSpaceBegin(TKikimrEvents::ES_PRIVATE) + 2 /* EvDelayedProposeTransaction */:
                if (captureDelayedProposeFrom.contains(event->GetRecipientRewrite())) {
                    Cerr << "---- capture EvDelayedProposeTransaction ----" << Endl;
                    Cerr << event->GetBase()->ToString() << Endl;
                    eventsDelayedPropose.emplace_back(event.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

    const int totalWrites = 10;
    TVector<TActorId> writeSenders;

    // Send a lot of write requests in parallel (so there's a large propose queue)
    for (int i = 0; i < totalWrites; ++i) {
        auto writeSender = runtime.AllocateEdgeActor();
        SendSQL(server, writeSender, Q_(Sprintf("UPSERT INTO `/Root/table-1` (key, value) VALUES (%d, %d);", i, i)));
        writeSenders.push_back(writeSender);
    }

    // Split would fail otherwise :(
    SetSplitMergePartCountLimit(server->GetRuntime(), -1);

    // Start split for table-1
    TInstant splitStart = TInstant::Now();
    auto senderSplit = runtime.AllocateEdgeActor();
    ui64 txId = AsyncSplitTable(server, senderSplit, "/Root/table-1", tablets[0], 100);

    // Wait until all propose requests and split reach our shard
    if (!(eventsSplit.size() >= 1 && eventsPropose.size() >= totalWrites)) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle&) -> bool {
                return eventsSplit.size() >= 1 && eventsPropose.size() >= totalWrites;
            });
        runtime.DispatchEvents(options);
    }

    // Unblock all propose requests (so they all successfully pass state test)
    capturePropose = false;
    for (auto& ev : eventsPropose) {
        Cerr << "---- Unblocking propose transaction ----" << Endl;
        captureDelayedProposeFrom.insert(ev->GetRecipientRewrite());
        runtime.Send(ev.Release(), 0, true);
    }
    eventsPropose.clear();

    // Unblock split request (shard will move to SplitSrcWaitForNoTxInFlight)
    captureSplit = false;
    captureSplitChanged = true;
    for (auto& ev : eventsSplit) {
        Cerr << "---- Unblocking split ----" << Endl;
        runtime.Send(ev.Release(), 0, true);
    }
    eventsSplit.clear();

    // Wait until split is finished and we have a delayed propose
    Cerr << "---- Waiting for split to finish ----" << Endl;
    if (!(eventsSplitChanged.size() >= 1 && eventsDelayedPropose.size() >= 1)) {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(
            [&](IEventHandle&) -> bool {
                return eventsSplitChanged.size() >= 1 && eventsDelayedPropose.size() >= 1;
            });
        runtime.DispatchEvents(options);
    }

    // Cause split to finish before the first delayed propose
    captureSplitChanged = false;
    for (auto& ev : eventsSplitChanged) {
        Cerr << "---- Unblocking split finish ----" << Endl;
        runtime.Send(ev.Release(), 0, true);
    }
    eventsSplitChanged.clear();

    // Unblock delayed propose transactions
    captureDelayedProposeFrom.clear();
    for (auto& ev : eventsDelayedPropose) {
        Cerr << "---- Unblocking delayed propose ----" << Endl;
        runtime.Send(ev.Release(), 0, true);
    }
    eventsDelayedPropose.clear();

    // Wait for split to finish at schemeshard
    WaitTxNotification(server, senderSplit, txId);

    // Split shouldn't take too much time to complete
    TDuration elapsed = TInstant::Now() - splitStart;
    UNIT_ASSERT_C(elapsed < TDuration::Seconds(NValgrind::PlainOrUnderValgrind(5, 25)),
        "Split needed " << elapsed.ToString() << " to complete, which is too long");

    // Count transaction results
    int successes = 0;
    int failures = 0;
    for (auto writeSender : writeSenders) {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(writeSender);
        if (ev->Get()->Record.GetRef().GetYdbStatus() == Ydb::StatusIds::SUCCESS) {
            ++successes;
        } else {
            ++failures;
        }
    }

    // We expect all transactions to fail
    UNIT_ASSERT_C(successes + failures == totalWrites,
        "Unexpected "
        << successes << " successes and "
        << failures << " failures");
}

void TestLateKqpQueryAfterColumnDrop(bool dataQuery, const TString& query, bool enableMvcc = false) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(enableMvcc)
        .SetUseRealThreads(false);

    if (dataQuery) {
        NKikimrKqp::TKqpSetting setting;
        setting.SetName("_KqpAllowNewEngine");
        setting.SetValue("true");
        serverSettings.KqpSettings.push_back(setting);
    }

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();
    auto streamSender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_PROXY, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_WORKER, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_TASKS_RUNNER, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_YQL, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_COMPUTE, NLog::PRI_DEBUG);
    runtime.SetLogPriority(NKikimrServices::KQP_RESOURCE_MANAGER, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", TShardedTableOptions()
            .Columns({
                {"key", "Uint32", true, false},
                {"value1", "Uint32", false, false},
                {"value2", "Uint32", false, false}}));

    ExecSQL(server, sender, "UPSERT INTO `/Root/table-1` (key, value1, value2) VALUES (1, 1, 10), (2, 2, 20);");

    bool capturePropose = true;
    TVector<THolder<IEventHandle>> eventsPropose;
    auto captureEvents = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &ev) -> auto {
        // if (ev->GetRecipientRewrite() == streamSender) {
        //     Cerr << "Stream sender got " << ev->GetTypeRewrite() << " " << ev->GetBase()->ToStringHeader() << Endl;
        // }
        switch (ev->GetTypeRewrite()) {
            case TEvDataShard::EvProposeTransaction: {
                auto &rec = ev->Get<TEvDataShard::TEvProposeTransaction>()->Record;
                if (capturePropose && rec.GetTxKind() != NKikimrTxDataShard::TX_KIND_SNAPSHOT) {
                    Cerr << "---- capture EvProposeTransaction ---- type=" << rec.GetTxKind() << Endl;
                    eventsPropose.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            }

            case TEvDataShard::EvKqpScan: {
                if (capturePropose) {
                    Cerr << "---- capture EvKqpScan ----" << Endl;
                    eventsPropose.emplace_back(ev.Release());
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            }
            default:
                break;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

    if (dataQuery) {
        Cerr << "--- sending data query request" << Endl;
        SendRequest(runtime, streamSender, MakeSimpleRequest(query));
    } else {
        Cerr << "--- sending stream request" << Endl;
        SendRequest(runtime, streamSender, MakeStreamRequest(streamSender, query, false));
    }

    // Wait until there's exactly one propose message at our datashard
    if (eventsPropose.size() < 1) {
        TDispatchOptions options;
        options.CustomFinalCondition = [&]() {
            return eventsPropose.size() >= 1;
        };
        runtime.DispatchEvents(options);
    }
    UNIT_ASSERT_VALUES_EQUAL(eventsPropose.size(), 1u);
    Cerr << "--- captured scan tx proposal" << Endl;
    capturePropose = false;

    // Drop column value2 and wait for drop to finish
    auto dropTxId = AsyncAlterDropColumn(server, "/Root", "table-1", "value2");
    WaitTxNotification(server, dropTxId);

    // Resend delayed propose messages
    Cerr << "--- resending captured proposals" << Endl;
    for (auto& ev : eventsPropose) {
        runtime.Send(ev.Release(), 0, true);
    }
    eventsPropose.clear();

    Cerr << "--- waiting for result" << Endl;
    auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(streamSender);
    auto& response = ev->Get()->Record.GetRef();
    Cerr << response.DebugString() << Endl;
    UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::ABORTED);
    auto& issue = response.GetResponse().GetQueryIssues(0).Getissues(0);
    UNIT_ASSERT_VALUES_EQUAL(issue.issue_code(), (int) NYql::TIssuesIds::KIKIMR_SCHEME_MISMATCH);
    UNIT_ASSERT_STRINGS_EQUAL(issue.message(), "Table \'/Root/table-1\' scheme changed.");
}

Y_UNIT_TEST_WITH_MVCC(TestLateKqpScanAfterColumnDrop) {
    TestLateKqpQueryAfterColumnDrop(false, "SELECT SUM(value2) FROM `/Root/table-1`", WithMvcc);
}

Y_UNIT_TEST_WITH_MVCC(TestLateKqpDataReadAfterColumnDrop) {
    TestLateKqpQueryAfterColumnDrop(true, R"(
            PRAGMA kikimr.UseNewEngine = "true";
            SELECT SUM(value2) FROM `/Root/table-1`
        )", WithMvcc);
}

Y_UNIT_TEST_NEW_ENGINE(MvccTestSnapshotRead) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(true)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (0, 0), (1, 1), (2, 2), (3, 3);"));

    auto waitFor = [&](const auto& condition, const TString& description) {
        if (!condition()) {
            Cerr << "... waiting for " << description << Endl;
            TDispatchOptions options;
            options.CustomFinalCondition = [&]() {
                return condition();
            };
            runtime.DispatchEvents(options);
            UNIT_ASSERT_C(condition(), "... failed to wait for " << description);
        }
    };

    bool captureTimecast = false;
    bool rewritten = false;
    bool rescheduled = false;

    TRowVersion snapshot = TRowVersion::Min();
    ui64 lastStep = 0;

    auto captureEvents = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle> &event) -> auto {
        switch (event->GetTypeRewrite()) {
            case TEvMediatorTimecast::EvUpdate: {
                if (captureTimecast) {
                    auto update = event->Get<TEvMediatorTimecast::TEvUpdate>();
                    lastStep = update->Record.GetTimeBarrier();
                    Cerr << "---- dropped EvUpdate ----" << Endl;
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            }
            case TEvDataShard::EvProposeTransaction: {
                if (!snapshot)
                    break;
                auto &propose = event->Get<TEvDataShard::TEvProposeTransaction>()->Record;
                if (!propose.HasMvccSnapshot()) {
                    auto mutableSnapshot = propose.MutableMvccSnapshot();
                    mutableSnapshot->SetStep(snapshot.Step);
                    mutableSnapshot->SetTxId(snapshot.TxId);
                    Cerr << "---- rewrite EvProposeTransaction ----" << Endl;
                    rewritten = true;
                } else if (propose.HasMvccSnapshot() && propose.GetMvccSnapshot().GetStep() == snapshot.Step &&
                           propose.GetMvccSnapshot().GetTxId() == snapshot.TxId) {
                    Cerr << "---- EvProposeTransaction rescheduled----" << Endl;
                    rescheduled = true;
                }
                break;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureEvents);

    // check transaction waits for proper plan step
    captureTimecast = true;

    waitFor([&]{ return lastStep != 0; }, "intercepted TEvUpdate");

    // future snapshot
    snapshot = TRowVersion(lastStep + 1000, Max<ui64>());

    SendRequest(runtime, sender, MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 2 ORDER BY key")));

    waitFor([&]{ return rewritten; }, "EvProposeTransaction rewritten");

    captureTimecast = false;

    waitFor([&]{ return rescheduled; }, "EvProposeTransaction rescheduled");

    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 2 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    auto tmp = std::exchange(snapshot, TRowVersion::Min());

    // check transaction reads from snapshot

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (2, 10);"));

    {
        auto ev = ExecRequest(runtime, sender, MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 2 ORDER BY key")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 2 } } Struct { Optional { Uint32: 10 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    snapshot = tmp;
    rescheduled = false;

    {
        auto ev = ExecRequest(runtime, sender, MakeSimpleRequest(Q_("SELECT key, value FROM `/Root/table-1` WHERE key = 2 ORDER BY key")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 2 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }

    UNIT_ASSERT(!rescheduled);
}

Y_UNIT_TEST_NEW_ENGINE(TestSecondaryClearanceAfterShardRestartRace) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
    runtime.SetLogPriority(UseNewEngine ? NKikimrServices::KQP_EXECUTER : NKikimrServices::TX_PROXY, NLog::PRI_TRACE);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 2);
    auto shards = GetTableShards(server, sender, "/Root/table-1");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1), (2, 2), (3, 3);"));

    auto waitFor = [&](const auto& condition, const TString& description) {
        if (!condition()) {
            Cerr << "... waiting for " << description << Endl;
            TDispatchOptions options;
            options.CustomFinalCondition = [&]() {
                return condition();
            };
            runtime.DispatchEvents(options);
            UNIT_ASSERT_C(condition(), "... failed to wait for " << description);
        }
    };

    // We want to intercept delivery problem notifications
    TVector<THolder<IEventHandle>> capturedDeliveryProblem;
    size_t seenStreamClearanceRequests = 0;
    size_t seenStreamClearanceResponses = 0;
    auto captureEvents = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) -> auto {
        switch (ev->GetTypeRewrite()) {
            case TEvPipeCache::TEvDeliveryProblem::EventType: {
                Cerr << "... intercepted TEvDeliveryProblem" << Endl;
                capturedDeliveryProblem.emplace_back(ev.Release());
                return TTestActorRuntime::EEventAction::DROP;
            }
            case TEvTxProcessing::TEvStreamQuotaRelease::EventType: {
                Cerr << "... dropped TEvStreamQuotaRelease" << Endl;
                return TTestActorRuntime::EEventAction::DROP;
            }
            case TEvTxProcessing::TEvStreamClearanceRequest::EventType: {
                Cerr << "... observed TEvStreamClearanceRequest" << Endl;
                ++seenStreamClearanceRequests;
                break;
            }
            case TEvTxProcessing::TEvStreamClearanceResponse::EventType: {
                Cerr << "... observed TEvStreamClearanceResponse" << Endl;
                ++seenStreamClearanceResponses;
                break;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserver = runtime.SetObserverFunc(captureEvents);

    auto state = StartReadShardedTable(server, "/Root/table-1", TRowVersion::Max(), /* pause */ true, /* ordered */ false);

    waitFor([&]{ return seenStreamClearanceResponses >= 2; }, "observed TEvStreamClearanceResponse");

    seenStreamClearanceRequests = 0;
    seenStreamClearanceResponses = 0;
    RebootTablet(runtime, shards[0], sender); 

    waitFor([&]{ return capturedDeliveryProblem.size() >= 1; }, "intercepted TEvDeliveryProblem");
    waitFor([&]{ return seenStreamClearanceRequests >= 1; }, "observed TEvStreamClearanceRequest");

    runtime.SetObserverFunc(prevObserver);
    for (auto& ev : capturedDeliveryProblem) {
        runtime.Send(ev.Release(), 0, true);
    }

    ResumeReadShardedTable(server, state);

    // We expect this upsert to complete successfully
    // When there's a bug it will get stuck due to readtable before barrier
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (4, 4);"));
}

Y_UNIT_TEST_QUAD(TestShardRestartNoUndeterminedImmediate, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

//    runtime.SetLogPriority(NKikimrServices::TABLET_MAIN, NLog::PRI_TRACE);
//    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
//    if (UseNewEngine) {
//        runtime.SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_TRACE);
//    } else {
//        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_TRACE);
//    }

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);
    auto table1shards = GetTableShards(server, sender, "/Root/table-1");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1);"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1);"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    auto waitFor = [&](const auto& condition, const TString& description) {
        if (!condition()) {
            Cerr << "... waiting for " << description << Endl;
            TDispatchOptions options;
            options.CustomFinalCondition = [&]() {
                return condition();
            };
            runtime.DispatchEvents(options);
            UNIT_ASSERT_C(condition(), "... failed to wait for " << description);
        }
    };

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    size_t delayedProposeCount = 0;
    auto captureRS = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) -> auto {
        switch (ev->GetTypeRewrite()) {
            case TEvTxProcessing::TEvReadSet::EventType: {
                readSets.push_back(std::move(ev));
                return TTestActorRuntime::EEventAction::DROP;
            }
            case EventSpaceBegin(TKikimrEvents::ES_PRIVATE) + 2 /* EvDelayedProposeTransaction */: {
                ++delayedProposeCount;
                break;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    waitFor([&]{ return readSets.size() >= 2; }, "commit read sets");
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Now send an upsert to table-1, it should be blocked by our in-progress tx
    delayedProposeCount = 0;
    auto sender3 = runtime.AllocateEdgeActor();
    Cerr << "... sending immediate upsert" << Endl;
    SendRequest(runtime, sender3, MakeSimpleRequest(Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 42), (3, 51))")));

    // Wait unti that propose starts to execute
    waitFor([&]{ return delayedProposeCount >= 1; }, "immediate propose");
    UNIT_ASSERT_VALUES_EQUAL(delayedProposeCount, 1u);
    Cerr << "... immediate upsert is blocked" << Endl;

    // Remove observer and gracefully restart the shard
    runtime.SetObserverFunc(prevObserverFunc);
    GracefulRestartTablet(runtime, table1shards[0], sender);

    // The result of immediate upsert must be neither SUCCESS nor UNDETERMINED
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender3);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_UNEQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_UNEQUAL(response.GetYdbStatus(), Ydb::StatusIds::UNDETERMINED);
    }

    // Select key 1 and verify its value was not updated
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender4, MakeSimpleRequest(Q_(R"(
            SELECT key, value FROM `/Root/table-1` WHERE key = 1 ORDER BY key)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 1 } } Struct { Optional { Uint32: 1 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_QUAD(TestShardRestartPlannedCommitShouldSucceed, UseMvcc, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(WithMvcc)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

//    runtime.SetLogPriority(NKikimrServices::TABLET_MAIN, NLog::PRI_TRACE);
//    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
//    if (UseNewEngine) {
//        runtime.SetLogPriority(NKikimrServices::KQP_EXECUTER, NLog::PRI_TRACE);
//    } else {
//        runtime.SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_TRACE);
//    }

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);
    auto table1shards = GetTableShards(server, sender, "/Root/table-1");

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1)"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 1)"));

    TString sessionId = CreateSession(runtime, sender);

    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    auto waitFor = [&](const auto& condition, const TString& description) {
        if (!condition()) {
            Cerr << "... waiting for " << description << Endl;
            TDispatchOptions options;
            options.CustomFinalCondition = [&]() {
                return condition();
            };
            runtime.DispatchEvents(options);
            UNIT_ASSERT_C(condition(), "... failed to wait for " << description);
        }
    };

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) -> auto {
        switch (ev->GetTypeRewrite()) {
            case TEvTxProcessing::TEvReadSet::EventType: {
                Cerr << "... captured readset" << Endl;
                readSets.push_back(std::move(ev));
                return TTestActorRuntime::EEventAction::DROP;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    auto sender2 = runtime.AllocateEdgeActor();
    SendRequest(runtime, sender2, MakeCommitRequest(sessionId, txId, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 2);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 2))")));

    // Wait until we captured both readsets
    waitFor([&]{ return readSets.size() >= 2; }, "commit read sets");
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Remove observer and gracefully restart the shard
    Cerr << "... restarting tablet" << Endl;
    runtime.SetObserverFunc(prevObserverFunc);
    GracefulRestartTablet(runtime, table1shards[0], sender);

    // The result of commit should be SUCCESS
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender2);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }

    // Select key 3 and verify its value was updated
    {
        auto sender4 = runtime.AllocateEdgeActor();
        auto ev = ExecRequest(runtime, sender4, MakeSimpleRequest(Q_(R"(
            SELECT key, value FROM `/Root/table-1` WHERE key = 3 ORDER BY key)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
        TString expected = "Struct { List { Struct { Optional { Uint32: 3 } } Struct { Optional { Uint32: 2 } } } } Struct { Bool: false }";
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults()[0].GetValue().ShortDebugString(), expected);
    }
}

Y_UNIT_TEST_NEW_ENGINE(TestShardSnapshotReadNoEarlyReply) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(true)
        .SetEnableMvccSnapshotReads(true)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1)"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 2)"));
    auto table1shards = GetTableShards(server, sender, "/Root/table-1");
    auto table2shards = GetTableShards(server, sender, "/Root/table-2");
    auto isTableShard = [&](ui64 tabletId) -> bool {
        if (std::find(table1shards.begin(), table1shards.end(), tabletId) != table1shards.end() ||
            std::find(table2shards.begin(), table2shards.end(), tabletId) != table2shards.end())
        {
            return true;
        }
        return false;
    };

    SimulateSleep(server, TDuration::Seconds(1));

    auto waitFor = [&](const auto& condition, const TString& description) {
        if (!condition()) {
            Cerr << "... waiting for " << description << Endl;
            TDispatchOptions options;
            options.CustomFinalCondition = [&]() {
                return condition();
            };
            runtime.DispatchEvents(options);
            UNIT_ASSERT_C(condition(), "... failed to wait for " << description);
        }
    };

    TVector<THolder<IEventHandle>> blockedCommits;
    size_t seenProposeResults = 0;
    auto blockCommits = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) -> auto {
        switch (ev->GetTypeRewrite()) {
            case TEvTablet::TEvCommit::EventType: {
                auto* msg = ev->Get<TEvTablet::TEvCommit>();
                if (isTableShard(msg->TabletID)) {
                    Cerr << "... blocked commit for tablet " << msg->TabletID << Endl;
                    blockedCommits.push_back(std::move(ev));
                    return TTestActorRuntime::EEventAction::DROP;
                }
                break;
            }
            case TEvDataShard::TEvProposeTransactionResult::EventType: {
                Cerr << "... observed propose transaction result" << Endl;
                ++seenProposeResults;
                break;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserver = runtime.SetObserverFunc(blockCommits);

    auto sender1 = runtime.AllocateEdgeActor();
    TString sessionId1 = CreateSession(runtime, sender1, "/Root");
    auto sender2 = runtime.AllocateEdgeActor();
    TString sessionId2 = CreateSession(runtime, sender2, "/Root");

    SendRequest(runtime, sender1, MakeBeginRequest(sessionId1, Q_(R"(
        SELECT * FROM `/Root/table-1`
        UNION ALL
        SELECT * FROM `/Root/table-2`
    )"), "/Root"));
    SendRequest(runtime, sender2, MakeBeginRequest(sessionId2, Q_(R"(
        SELECT * FROM `/Root/table-1`
        UNION ALL
        SELECT * FROM `/Root/table-2`
    )"), "/Root"));

    waitFor([&]{ return blockedCommits.size() >= 2; }, "at least 2 blocked commits");

    SimulateSleep(server, TDuration::Seconds(1));

    UNIT_ASSERT_C(seenProposeResults == 0, "Unexpected propose results observed");

    // Unblock commits and wait for results
    runtime.SetObserverFunc(prevObserver);
    for (auto& ev : blockedCommits) {
        runtime.Send(ev.Release(), 0, true);
    }
    blockedCommits.clear();

    TString txId1;
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender1);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId1 = response.GetResponse().GetTxMeta().id();
    }

    TString txId2;
    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender2);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId2 = response.GetResponse().GetTxMeta().id();
    }

    // Start blocking commits again and try performing new writes
    prevObserver = runtime.SetObserverFunc(blockCommits);
    SendRequest(runtime, sender, MakeSimpleRequest(Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 3)"), "/Root"));
    SendRequest(runtime, sender, MakeSimpleRequest(Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (4, 4)"), "/Root"));
    waitFor([&]{ return blockedCommits.size() >= 2; }, "at least 2 blocked commits");

    // Send an additional read request, it must not be blocked
    SendRequest(runtime, sender1, MakeContinueRequest(sessionId1, txId1, Q_(R"(
        SELECT * FROM `/Root/table-1`
        UNION ALL
        SELECT * FROM `/Root/table-2`
    )"), "/Root"));

    {
        auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(sender1);
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
    }
}

Y_UNIT_TEST_TWIN(TestSnapshotReadAfterBrokenLock, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(true)
        .SetEnableMvccSnapshotReads(true)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1)"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 2)"));

    SimulateSleep(server, TDuration::Seconds(1));

    TString sessionId = CreateSession(runtime, sender);

    // Start transaction by reading from both tables, we will only set locks
    // to currently existing variables
    TString txId;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 1
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    SimulateSleep(server, TDuration::Seconds(1));

    // Perform immediate write, which would not break the above lock
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (3, 3)"));

    // Perform an additional read, it would mark transaction as write-broken
    {
        auto ev = ExecRequest(runtime, sender, MakeContinueRequest(sessionId, txId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 3
        )")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Perform one more read, it would be in an already write-broken transaction
    {
        auto ev = ExecRequest(runtime, sender, MakeContinueRequest(sessionId, txId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 5
        )")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    {
        auto ev = ExecRequest(runtime, sender, MakeCommitRequest(sessionId, txId, Q_(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 5)
        )")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::ABORTED);
    }
}

Y_UNIT_TEST_TWIN(TestSnapshotReadAfterBrokenLockOutOfOrder, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(true)
        .SetEnableMvccSnapshotReads(true)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1)"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 2)"));

    SimulateSleep(server, TDuration::Seconds(1));

    // Start transaction by reading from both tables
    TString sessionId = CreateSession(runtime, sender);
    TString txId;
    {
        Cerr << "... performing the first select" << Endl;
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 1
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Arrange for another distributed tx stuck at readset exchange
    auto senderBlocker = runtime.AllocateEdgeActor();
    TString sessionIdBlocker = CreateSession(runtime, senderBlocker);
    TString txIdBlocker;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionIdBlocker, Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txIdBlocker = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    auto waitFor = [&](const auto& condition, const TString& description) {
        if (!condition()) {
            Cerr << "... waiting for " << description << Endl;
            TDispatchOptions options;
            options.CustomFinalCondition = [&]() {
                return condition();
            };
            runtime.DispatchEvents(options);
            UNIT_ASSERT_C(condition(), "... failed to wait for " << description);
        }
    };

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) -> auto {
        switch (ev->GetTypeRewrite()) {
            case TEvTxProcessing::TEvReadSet::EventType: {
                Cerr << "... captured readset" << Endl;
                readSets.push_back(std::move(ev));
                return TTestActorRuntime::EEventAction::DROP;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    SendRequest(runtime, senderBlocker, MakeCommitRequest(sessionIdBlocker, txIdBlocker, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (99, 99);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (99, 99))")));

    // Wait until we captured both readsets
    waitFor([&]{ return readSets.size() >= 2; }, "commit read sets");
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Restore the observer, we would no longer block new readsets
    runtime.SetObserverFunc(prevObserverFunc);

    SimulateSleep(server, TDuration::Seconds(1));

    // Perform immediate write, which would break the above lock
    Cerr << "... performing an upsert" << Endl;
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 3)"));

    // Perform an additional read, it would mark transaction as write-broken for the first time
    {
        Cerr << "... performing the second select" << Endl;
        auto ev = ExecRequest(runtime, sender, MakeContinueRequest(sessionId, txId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 3
        )")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    // Perform one more read, it would be in an already write-broken transaction
    {
        Cerr << "... performing the third select" << Endl;
        auto ev = ExecRequest(runtime, sender, MakeContinueRequest(sessionId, txId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 5
        )")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    {
        Cerr << "... performing the last upsert and commit" << Endl;
        auto ev = ExecRequest(runtime, sender, MakeCommitRequest(sessionId, txId, Q_(R"(
            UPSERT INTO `/Root/table-1` (key, value) VALUES (5, 5)
        )")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::ABORTED);
    }
}

Y_UNIT_TEST_TWIN(TestSnapshotReadAfterStuckRW, UseNewEngine) {
    TPortManager pm;
    TServerSettings serverSettings(pm.GetPort(2134));
    serverSettings.SetDomainName("Root")
        .SetEnableMvcc(true)
        .SetEnableMvccSnapshotReads(true)
        .SetUseRealThreads(false);

    Tests::TServer::TPtr server = new TServer(serverSettings);
    auto &runtime = *server->GetRuntime();
    auto sender = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);

    InitRoot(server, sender);

    CreateShardedTable(server, sender, "/Root", "table-1", 1);
    CreateShardedTable(server, sender, "/Root", "table-2", 1);

    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1)"));
    ExecSQL(server, sender, Q_("UPSERT INTO `/Root/table-2` (key, value) VALUES (2, 2)"));

    SimulateSleep(server, TDuration::Seconds(1));

    // Arrange for a distributed tx stuck at readset exchange
    auto senderBlocker = runtime.AllocateEdgeActor();
    TString sessionIdBlocker = CreateSession(runtime, senderBlocker);
    TString txIdBlocker;
    {
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionIdBlocker, Q_(R"(
            SELECT * FROM `/Root/table-1`
            UNION ALL
            SELECT * FROM `/Root/table-2`)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txIdBlocker = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }

    auto waitFor = [&](const auto& condition, const TString& description) {
        if (!condition()) {
            Cerr << "... waiting for " << description << Endl;
            TDispatchOptions options;
            options.CustomFinalCondition = [&]() {
                return condition();
            };
            runtime.DispatchEvents(options);
            UNIT_ASSERT_C(condition(), "... failed to wait for " << description);
        }
    };

    // Capture and block all readset messages
    TVector<THolder<IEventHandle>> readSets;
    auto captureRS = [&](TTestActorRuntimeBase&, TAutoPtr<IEventHandle>& ev) -> auto {
        switch (ev->GetTypeRewrite()) {
            case TEvTxProcessing::TEvReadSet::EventType: {
                Cerr << "... captured readset" << Endl;
                readSets.push_back(THolder(ev.Release()));
                return TTestActorRuntime::EEventAction::DROP;
            }
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    auto prevObserverFunc = runtime.SetObserverFunc(captureRS);

    // Send a commit request, it would block on readset exchange
    SendRequest(runtime, senderBlocker, MakeCommitRequest(sessionIdBlocker, txIdBlocker, Q_(R"(
        UPSERT INTO `/Root/table-1` (key, value) VALUES (99, 99);
        UPSERT INTO `/Root/table-2` (key, value) VALUES (99, 99))")));

    // Wait until we captured both readsets
    waitFor([&]{ return readSets.size() >= 2; }, "commit read sets");
    UNIT_ASSERT_VALUES_EQUAL(readSets.size(), 2u);

    // Restore the observer, we would no longer block new readsets
    runtime.SetObserverFunc(prevObserverFunc);

    SimulateSleep(server, TDuration::Seconds(1));

    // Start a transaction by reading from both tables
    TString sessionId = CreateSession(runtime, sender);
    TString txId;
    {
        Cerr << "... performing the first select" << Endl;
        auto ev = ExecRequest(runtime, sender, MakeBeginRequest(sessionId, Q_(R"(
            SELECT * FROM `/Root/table-1` WHERE key = 1
            UNION ALL
            SELECT * FROM `/Root/table-2` WHERE key = 2)")));
        auto& response = ev->Get()->Record.GetRef();
        UNIT_ASSERT_VALUES_EQUAL(response.GetYdbStatus(), Ydb::StatusIds::SUCCESS);
        txId = response.GetResponse().GetTxMeta().id();
        UNIT_ASSERT_VALUES_EQUAL(response.GetResponse().GetResults().size(), 1u);
    }
}

} // Y_UNIT_TEST_SUITE(DataShardOutOfOrder)

} // namespace NKikimr

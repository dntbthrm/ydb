#pragma once

#include "test_steps.h"
#include "test_comp.h"

#include <ydb/core/tablet_flat/flat_database.h>
#include <ydb/core/tablet_flat/flat_dbase_naked.h>
#include <ydb/core/tablet_flat/flat_dbase_apply.h>
#include <ydb/core/tablet_flat/flat_dbase_change.h>
#include <ydb/core/tablet_flat/flat_sausage_grind.h>
#include <ydb/core/tablet_flat/flat_util_binary.h>
#include <ydb/core/tablet_flat/util_fmt_desc.h>

#include <ydb/core/tablet_flat/test/libs/rows/cook.h>
#include <ydb/core/tablet_flat/test/libs/rows/tool.h>
#include <ydb/core/tablet_flat/test/libs/table/test_iter.h>
#include <ydb/core/tablet_flat/test/libs/table/test_envs.h>
#include <ydb/core/tablet_flat/test/libs/table/wrap_dbase.h>
#include <ydb/core/tablet_flat/test/libs/table/wrap_select.h>

namespace NKikimr {
namespace NTable {
namespace NTest {

    enum class EPlay {
        Boot    = 1,    /* Apply redo log through base booter   */
        Redo    = 2,    /* Roll up redo log through follower iface */ 
    };

    struct TDbExec : public TSteps<TDbExec> {
        using TRow = NTest::TRow;
        using TRedoLog = TDeque<TAutoPtr<TChange>>; 
        using TSteppedCookieAllocator = NPageCollection::TSteppedCookieAllocator;

        using TCheckIter = TChecker<NTest::TWrapDbIter, TDatabase&>;
        using TCheckSelect = TChecker<NTest::TWrapDbSelect, TDatabase&>;

        enum class EOnTx {
            None    = 0,
            Auto    = 1,    /* Automatic read only transaction  */
            Real    = 2,    /* Explicitly prepared user rw tx   */
        };

        struct THeader {
            ui64 Serial;    /* Change serial stamp data         */
            ui32 Alter;     /* Bytes of scheme delta block      */
            ui32 Redo;      /* Bytes of redo log block          */
            ui32 Affects;   /* Items in affects array           */
            ui32 Pad0;
        };

        static TAutoPtr<TDatabase> Make(TAutoPtr<TSchemeChanges> delta) 
        {
            TAutoPtr<TScheme> scheme = new TScheme; 

            TSchemeModifier(*scheme).Apply(*delta);

            return new TDatabase(new TDatabaseImpl(0, scheme, nullptr));
        }

        TDbExec() : Base(new TDatabase) { Birth(); }

        TDbExec(TAutoPtr<TSchemeChanges> delta) : Base(Make(delta)) { } 

        const TRedoLog& GetLog() const noexcept { return RedoLog; }

        const TChange& BackLog() const noexcept
        {
            Y_VERIFY(RedoLog, "Redo log is empty, cannot get last entry");

            return *RedoLog.back();
        }

        TDatabase* operator->() const noexcept { return Base.Get(); }

        TDbExec& Begin() noexcept
        {
            return DoBegin(true);
        }

        TDbExec& Commit() { return DoCommit(true, true); }
        TDbExec& Reject() { return DoCommit(true, false); }
        TDbExec& Relax()  { return DoCommit(false, true); }

        TDbExec& ReadVer(TRowVersion readVersion) {
            DoBegin(false);

            ReadVersion = readVersion;

            return *this;
        }

        TDbExec& WriteVer(TRowVersion writeVersion) {
            Y_VERIFY(OnTx != EOnTx::None);

            WriteVersion = writeVersion;

            return *this;
        }

        TDbExec& Add(ui32 table, const TRow &row, ERowOp rop = ERowOp::Upsert)
        {
            const NTest::TRowTool tool(RowSchemeFor(table));
            auto pair = tool.Split(row, true, rop != ERowOp::Erase);

            Base->Update(table, rop, pair.Key, pair.Ops, WriteVersion);

            return *this;
        }

        template<typename ... Args>
        inline TDbExec& Put(ui32 table, const TRow &row, const Args& ... left)
        {
            return Add(table, row, ERowOp::Upsert), Put(table, left...);
        }

        inline TDbExec& Put(ui32 table, const TRow &row)
        {
            return Add(table, row, ERowOp::Upsert);
        }

        TDbExec& Apply(const TSchemeChanges &delta)
        {
            Last = Max<ui32>(), Altered = true;

            return Base->Alter().Merge(delta), *this;
        }

        TDbExec& Snapshot(ui32 table)
        {
            return Base->TxSnapTable(table), *this;
        }

        NTest::TNatural Natural(ui32 table) noexcept
        {
            return { RowSchemeFor(table) };
        }

        TCheckIter Iter(ui32 table, bool erased = true) noexcept
        {
            DoBegin(false), RowSchemeFor(table);

            TCheckIter check{ *Base, { nullptr, 0, erased }, table, Scheme, ReadVersion };

            return check.To(CurrentStep()), check;
        }

        TCheckSelect Select(ui32 table, bool erased = true) noexcept
        {
            DoBegin(false), RowSchemeFor(table);

            TCheckSelect check{ *Base, { nullptr, 0, erased }, table, Scheme, ReadVersion };

            return check.To(CurrentStep()), check;
        }

        TDbExec& Snap(ui32 table)
        {
            const auto scn = Base->Head().Serial + 1;

            RedoLog.emplace_back(new TChange({ Gen, ++Step }, scn));
            RedoLog.back()->Redo = Base->SnapshotToLog(table, { Gen, Step });
            RedoLog.back()->Affects = { table };

            Y_VERIFY(scn == Base->Head().Serial);

            return *this;
        }

        TDbExec& Compact(ui32 table, bool last = true)
        {
            TAutoPtr<TSubset> subset; 

            if (last /* make full subset */) {
                subset = Base->Subset(table, TEpoch::Max(), { }, { });
            } else /* only flush memtables */ {
                subset = Base->Subset(table, { }, TEpoch::Max());
            }

            TLogoBlobID logo(1, Gen, ++Step, 1, 0, 0);

            auto *family = Base->GetScheme().DefaultFamilyFor(table);

            NPage::TConf conf{ last, 8291, family->Large };

            conf.ByKeyFilter = Base->GetScheme().GetTableInfo(table)->ByKeyFilter;
            conf.MaxRows = subset->MaxRows();
            conf.MinRowVersion = subset->MinRowVersion();
            conf.SmallEdge = family->Small;

            auto keys = subset->Scheme->Tags(true /* only keys */);

            /* Mocked NFwd emulates real compaction partially: it cannot pass
                external blobs from TMemTable to TPart by reference, so need to
                materialize it on this compaction.
             */

            TAutoPtr<IPages> env = new TForwardEnv(128, 256, keys, Max<ui32>()); 

            auto eggs = TCompaction(env, conf).Do(*subset, logo);

            Y_VERIFY(!eggs.NoResult(), "Unexpected early termination");

            TVector<TPartView> partViews;
            for (auto &part : eggs.Parts)
                partViews.push_back({ part, nullptr, part->Slices });

            Base->Replace(table, std::move(partViews), *subset);

            return *this;
        }

        TDbExec& Replay(EPlay play)
        {
            Y_VERIFY(OnTx != EOnTx::Real, "Commit TX before replaying");

            const ui64 serial = Base->Head().Serial;

            Birth(), Base = nullptr, OnTx = EOnTx::None;

            ReadVersion = TRowVersion::Max();
            WriteVersion = TRowVersion::Min();

            if (play == EPlay::Boot) {
                TAutoPtr<TScheme> scheme = new TScheme; 

                for (auto &change: RedoLog) {
                    if (auto &raw = change->Scheme) {
                        TSchemeChanges delta;
                        bool ok = delta.ParseFromString(raw);

                        Y_VERIFY(ok, "Cannot read serialized scheme delta");

                        TSchemeModifier(*scheme).Apply(delta);
                    }
                }

                TAutoPtr<TDatabaseImpl> naked = new TDatabaseImpl({Gen, Step}, scheme, nullptr);

                for (auto &change: RedoLog)
                    if (auto &redo = change->Redo) {
                        naked->Switch(change->Stamp);
                        naked->Assign(change->Annex);
                        naked->ApplyRedo(redo);
                        naked->GrabAnnex();
                    }

                UNIT_ASSERT(serial == naked->Serial());

                Base = new TDatabase(naked.Release());

            } else if (play == EPlay::Redo) {

                Base = new TDatabase{ };

                for (const auto &it: RedoLog)
                    Base->RollUp(it->Stamp, it->Scheme, it->Redo, it->Annex);

                UNIT_ASSERT(serial == Base->Head().Serial);
            }

            return *this;
        }

        const TRowScheme& RowSchemeFor(ui32 table) noexcept
        {
            if (std::exchange(Last, table) == table) {
                /* Safetly can use row scheme from cache */
            } else if (Altered) {
                TScheme temp(Base->GetScheme());
                TSchemeModifier(temp).Apply(*Base->Alter());

                const auto *info = temp.GetTableInfo(table);

                Scheme = TRowScheme::Make(info->Columns, NUtil::TSecond());
            } else {
                Scheme = Base->Subset(table, { }, TEpoch::Zero())->Scheme;
            }

            return *Scheme;
        }

        TDbExec& Affects(ui32 back, std::initializer_list<ui32> tables)
        {
            Y_VERIFY(back < RedoLog.size(), "Out of redo log entries");

            const auto &have = RedoLog[RedoLog.size() - (1 + back)]->Affects;

            if (have.size() == tables.size()
                && std::equal(have.begin(), have.end(), tables.begin())) {

            } else {
                TSteps<TDbExec>::Log()
                    << "For " << NFmt::Do(*RedoLog[RedoLog.size() - (1 + back)])
                    << " expected affects " << NFmt::Arr(tables)
                    << Endl;

                UNIT_ASSERT(false);
            }

            return *this;
        }

        void DumpChanges(IOutputStream &stream) const noexcept
        {
            for (auto &one: RedoLog) {
                NUtil::NBin::TOut(stream)
                    .Put<ui64>(one->Serial)
                    .Put<ui32>(one->Scheme.size())
                    .Put<ui32>(one->Redo.size())
                    .Put<ui32>(one->Affects.size())
                    .Put<ui32>(0 /* paddings */)
                    .Array(one->Scheme)
                    .Array(one->Redo)
                    .Array(one->Affects);
            }
        }

        static TRedoLog RestoreChanges(IInputStream &in)
        {
            TRedoLog changes;
            THeader header;

            while (auto got = in.Load(&header, sizeof(header))) {
                Y_VERIFY(got == sizeof(header), "Invalid changes stream");

                const auto abytes = sizeof(ui32) * header.Affects;

                TString alter = TString::TUninitialized(header.Alter);

                if (in.Load((void*)alter.data(), alter.size()) != alter.size())
                    Y_FAIL("Cannot read alter chunk data in change page");

                TString redo = TString::TUninitialized(header.Redo);

                if (in.Load((void*)redo.data(), redo.size()) != redo.size())
                    Y_FAIL("Cannot read redo log data in change page");

                if (in.Skip(abytes) != abytes)
                    Y_FAIL("Cannot read affects array in change page");

                changes.push_back(new TChange{ header.Serial, header.Serial });
                changes.back()->Scheme = std::move(alter);
                changes.back()->Redo = std::move(redo);
            }

            return changes;
        }

    private:
        void Birth() noexcept
        {
            Annex = new TSteppedCookieAllocator(1, ui64(++Gen) << 32, { 0, 999 }, {{ 1, 7 }});
        }

        TDbExec& DoBegin(bool real) noexcept
        {
            if (OnTx == EOnTx::Real && real) {
                Y_FAIL("Cannot run multiple tx at the same time");
            } else if (OnTx == EOnTx::Auto && real) {
                DoCommit(false, false);
            }

            if (OnTx == EOnTx::None || real) {
                Annex->Switch(++Step, true /* require step switch */);
                Base->Begin({ Gen, Step }, Env.emplace());

                OnTx = (real ? EOnTx::Real : EOnTx::Auto);
            }

            return *this;
        }

        TDbExec& DoCommit(bool real, bool apply)
        {
            const auto was = std::exchange(OnTx, EOnTx::None);

            if (was != (real ? EOnTx::Real : EOnTx::Auto))
                Y_FAIL("There is no active dbase tx");

            auto up = Base->Commit({ Gen, Step }, apply, Annex.Get()).Change;
            Env.reset();

            Last = Max<ui32>(), Altered = false;

            UNIT_ASSERT(!up->HasAny() || apply);
            UNIT_ASSERT(!up->HasAny() || was != EOnTx::Auto);
            UNIT_ASSERT(!up->Annex || up->Redo);

            if (apply) {
                std::sort(up->Affects.begin(), up->Affects.end());

                auto end = std::unique(up->Affects.begin(), up->Affects.end());

                if (end != up->Affects.end()) {
                    TSteps<TDbExec>::Log()
                        << NFmt::Do(*up) << " has denormalized affects" << Endl;

                    UNIT_ASSERT(false);
                }

                RedoLog.emplace_back(std::move(up));
            }

            ReadVersion = TRowVersion::Max();
            WriteVersion = TRowVersion::Min();

            return *this;
        }

    private:
        TAutoPtr<TDatabase> Base; 
        std::optional<TTestEnv> Env;
        ui32 Gen = 0;
        ui32 Step = 0;
        ui32 Last = Max<ui32>();
        bool Altered = false;
        EOnTx OnTx = EOnTx::None;
        TIntrusiveConstPtr<TRowScheme> Scheme; 
        TRedoLog RedoLog;
        TAutoPtr<TSteppedCookieAllocator> Annex;
        TRowVersion ReadVersion = TRowVersion::Max();
        TRowVersion WriteVersion = TRowVersion::Min();
    };

}
}
}

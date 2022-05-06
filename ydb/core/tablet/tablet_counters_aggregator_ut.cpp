#include "tablet_counters_aggregator.h"

#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/testlib/basics/appdata.h>

#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/actors/core/interconnect.h>

namespace NKikimr {

using namespace NActors;

void TestHeavy(const ui32 v, ui32 numWorkers) {

    TInstant t(Now());

    TVector<TActorId> cc;
    TActorId aggregatorId;
    TTestBasicRuntime runtime(1);
    constexpr int NODES = 10;
    constexpr int GROUPS = 1000;
    constexpr int VALUES = 20;

    runtime.Initialize(TAppPrepare().Unwrap());
    TActorId edge = runtime.AllocateEdgeActor();

    runtime.SetLogPriority(NKikimrServices::TABLET_AGGREGATOR, NActors::NLog::PRI_DEBUG);

    IActor* aggregator = CreateClusterLabeledCountersAggregatorActor(edge, TTabletTypes::PersQueue, v, TString(), numWorkers);
    aggregatorId = runtime.Register(aggregator);

    if (numWorkers == 0) {
        cc.push_back(aggregatorId);
        ++numWorkers;
    }

    runtime.SetRegistrationObserverFunc([&cc, &aggregatorId](TTestActorRuntimeBase& runtime, const TActorId& parentId, const TActorId& actorId) {
                TTestActorRuntime::DefaultRegistrationObserver(runtime, parentId, actorId);
                if (parentId == aggregatorId) {
                    cc.push_back(actorId);
                }
            });

    TDispatchOptions options;
    options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, numWorkers);
    runtime.DispatchEvents(options);
    for (const auto& a : cc) {
        THolder<TEvInterconnect::TEvNodesInfo> nodesInfo = MakeHolder<TEvInterconnect::TEvNodesInfo>();
        for (auto i = 1; i <= NODES; ++i) {
            nodesInfo->Nodes.emplace_back(TEvInterconnect::TNodeInfo(i, "::", "localhost", "localhost", 1234, TNodeLocation()));
        }
        runtime.Send(new NActors::IEventHandle(a, edge, nodesInfo.Release()), 0, true);
    }

    for (auto i = 1; i <= NODES; ++i) {
        THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = MakeHolder<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
        for (auto k = 0; k < GROUPS; ++k) {
            char delim = (k % 2 == 0) ? '/' : '|';
            auto& group1 = *response->Record.AddLabeledCountersByGroup();
            group1.SetGroup(Sprintf("group%d%c%d", i, delim, k));
            group1.SetGroupNames(Sprintf("A%cB", delim));
            if (k % 4 != 0)
                group1.SetDelimiter(TStringBuilder() << delim);
            for (auto j = 0; j < VALUES; ++j) {
                auto& counter1 = *group1.AddLabeledCounter();
                counter1.SetName(Sprintf("value%d", j));
                counter1.SetValue(13);
                counter1.SetType(TLabeledCounterOptions::CT_SIMPLE);
                counter1.SetAggregateFunc(TLabeledCounterOptions::EAF_SUM);
            }
        }
        Cerr << "Sending message to " << cc[i % numWorkers] << " from " << aggregatorId <<  " id " << i << "\n";
        runtime.Send(new NActors::IEventHandle(cc[i % numWorkers], aggregatorId, response.Release(), 0, i), 0, true);
    }
    {
        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvInterconnect::EvNodesInfo, numWorkers);
        runtime.DispatchEvents(options, TDuration::Seconds(1));
    }

    THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = runtime.GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>();

    UNIT_ASSERT(response != nullptr);
    UNIT_ASSERT_VALUES_EQUAL(response->Record.LabeledCountersByGroupSize(), NODES * GROUPS);

    Cerr << "TEST " << v << " " << numWorkers << " duration " << TInstant::Now() - t << "\n";
}

Y_UNIT_TEST_SUITE(TTabletCountersAggregator) {

    struct TTabletWithHist {
        TTabletWithHist(ui64 tabletId)
            : TabletId(tabletId)
            , TenantPathId(1113, 1001)
            , CounterEventsInFlight(new TEvTabletCounters::TInFlightCookie)
            , ExecutorCounters(new TTabletCountersBase)
        {
            auto simpleCount = sizeof(SimpleCountersMetaInfo) / sizeof(SimpleCountersMetaInfo[0]);
            auto percentileCount = sizeof(PercentileCountersMetaInfo) / sizeof(PercentileCountersMetaInfo[0]);
            AppCounters.reset(new TTabletCountersBase(
                simpleCount,
                0, // cumulativeCnt
                percentileCount,
                SimpleCountersMetaInfo,
                nullptr, // cumulative meta
                PercentileCountersMetaInfo));

            for (auto i: xrange(percentileCount))
                AppCounters->Percentile()[i].Initialize(RangeDefs[i].first, RangeDefs[i].second, true);

            AppCountersBaseline.reset(new TTabletCountersBase());
            AppCounters->RememberCurrentStateAsBaseline(*AppCountersBaseline);

            ExecutorCountersBaseline.reset(new TTabletCountersBase());
            ExecutorCounters->RememberCurrentStateAsBaseline(*ExecutorCountersBaseline);
        }

        void SendUpdate(TTestBasicRuntime& runtime, const TActorId& aggregatorId, const TActorId& sender) {
            auto executorCounters = ExecutorCounters->MakeDiffForAggr(*ExecutorCountersBaseline);
            ExecutorCounters->RememberCurrentStateAsBaseline(*ExecutorCountersBaseline);

            auto appCounters = AppCounters->MakeDiffForAggr(*AppCountersBaseline);
            AppCounters->RememberCurrentStateAsBaseline(*AppCountersBaseline);

            runtime.Send(new IEventHandle(aggregatorId, sender, new TEvTabletCounters::TEvTabletAddCounters(
                CounterEventsInFlight, TabletId, TabletType, TenantPathId, executorCounters, appCounters)));

            // force recalc
            runtime.Send(new IEventHandle(aggregatorId, sender, new NActors::TEvents::TEvWakeup()));
        }

        void ForgetTablet(TTestBasicRuntime& runtime, const TActorId& aggregatorId, const TActorId& sender) {
            runtime.Send(new IEventHandle(
                aggregatorId,
                sender,
                new TEvTabletCounters::TEvTabletCountersForgetTablet(TabletId, TabletType, TenantPathId)));

            // force recalc
            runtime.Send(new IEventHandle(aggregatorId, sender, new NActors::TEvents::TEvWakeup()));
        }

        void SetSimpleCount(const char* name, ui64 count) {
            size_t index = SimpleNameToIndex(name);
            AppCounters->Simple()[index].Set(count);
        }

        void UpdatePercentile(const char* name, ui64 what) {
            size_t index = PercentileNameToIndex(name);
            AppCounters->Percentile()[index].IncrementFor(what);
        }

        void UpdatePercentile(const char* name, ui64 what, ui64 value) {
            size_t index = PercentileNameToIndex(name);
            AppCounters->Percentile()[index].AddFor(what, value);
        }

    public:
        static NMonitoring::TDynamicCounterPtr GetAppCounters(TTestBasicRuntime& runtime) {
            NMonitoring::TDynamicCounterPtr counters = runtime.GetAppData(0).Counters;
            UNIT_ASSERT(counters);

            TString tabletTypeStr = TTabletTypes::TypeToStr(TabletType);
            auto dsCounters = counters->GetSubgroup("counters", "tablets")->GetSubgroup("type", tabletTypeStr);
            return dsCounters->GetSubgroup("category", "app");
        }

        template <typename TArray>
        static size_t StringToIndex(const char* name, const TArray& array) {
            size_t i = 0;
            for (const auto& s: array) {
                if (TStringBuf(name) == TStringBuf(s))
                    return i;
                ++i;
            }
            return i;
        }

        static size_t SimpleNameToIndex(const char* name) {
            return StringToIndex(name, SimpleCountersMetaInfo);
        }

        static size_t PercentileNameToIndex(const char* name) {
            return StringToIndex(name, PercentileCountersMetaInfo);
        }

        static NMonitoring::THistogramPtr GetHistogram(TTestBasicRuntime& runtime, const char* name) {
            size_t index = PercentileNameToIndex(name);
           return GetAppCounters(runtime)->FindHistogram(PercentileCountersMetaInfo[index]);
        }

        static std::vector<ui64> GetOldHistogram(TTestBasicRuntime& runtime, const char* name) {
            size_t index = PercentileNameToIndex(name);
            auto rangesArray = RangeDefs[index].first;
            auto rangeCount = RangeDefs[index].second;

            std::vector<TTabletPercentileCounter::TRangeDef> ranges(rangesArray, rangesArray + rangeCount);
            ranges.push_back({});
            ranges.back().RangeName = "inf";
            ranges.back().RangeVal = Max<ui64>();

            auto appCounters = GetAppCounters(runtime);
            std::vector<ui64> buckets;
            for (auto i: xrange(ranges.size())) {
                auto subGroup = appCounters->GetSubgroup("range", ranges[i].RangeName);
                auto sensor = subGroup->FindCounter(PercentileCountersMetaInfo[index]);
                if (sensor) {
                    buckets.push_back(sensor->Val());
                }
            }

            return buckets;
        }

        static void CheckHistogram(
            TTestBasicRuntime& runtime,
            const char* name,
            const std::vector<ui64>& goldValuesNew,
            const std::vector<ui64>& goldValuesOld)
        {
            // new stype histogram
            auto histogram = TTabletWithHist::GetHistogram(runtime, name);
            UNIT_ASSERT(histogram);
            auto snapshot = histogram->Snapshot();
            UNIT_ASSERT(snapshot);

            UNIT_ASSERT_VALUES_EQUAL(snapshot->Count(), goldValuesNew.size());
            {
                // for pretty printing the diff
                std::vector<ui64> values;
                values.reserve(goldValuesNew.size());
                for (auto i: xrange(goldValuesNew.size()))
                    values.push_back(snapshot->Value(i));
                UNIT_ASSERT_VALUES_EQUAL(values, goldValuesNew);
            }

            // old histogram
            auto values = TTabletWithHist::GetOldHistogram(runtime, name);
            UNIT_ASSERT_VALUES_EQUAL(values.size(), goldValuesOld.size());
            UNIT_ASSERT_VALUES_EQUAL(values, goldValuesOld);
        }

    public:
        ui64 TabletId;
        TPathId TenantPathId;
        TIntrusivePtr<TEvTabletCounters::TInFlightCookie> CounterEventsInFlight;

        std::unique_ptr<TTabletCountersBase> ExecutorCounters;
        std::unique_ptr<TTabletCountersBase> ExecutorCountersBaseline;

        std::unique_ptr<TTabletCountersBase> AppCounters;
        std::unique_ptr<TTabletCountersBase> AppCountersBaseline;

    public:
        static constexpr TTabletTypes::EType TabletType = TTabletTypes::DataShard;

        static constexpr TTabletPercentileCounter::TRangeDef RangeDefs1[] = {
            {0,   "0"}
        };

        static constexpr TTabletPercentileCounter::TRangeDef RangeDefs4[] = {
            {0,   "0"},
            {1,   "1"},
            {13,  "13"},
            {29,  "29"}
        };

        static constexpr std::pair<const TTabletPercentileCounter::TRangeDef*, size_t> RangeDefs[] = {
            {RangeDefs1, 1},
            {RangeDefs4, 4},
            {RangeDefs1, 1},
            {RangeDefs4, 4},
        };

        static constexpr const char* PercentileCountersMetaInfo[] = {
            "MyHistSingleBucket",
            "HIST(Count)",
            "HIST(CountSingleBucket)",
            "MyHist",
        };

        static constexpr const char* SimpleCountersMetaInfo[] = {
            "JustCount1",
            "Count",
            "CountSingleBucket",
            "JustCount2",
        };
    };

    Y_UNIT_TEST(IntegralPercentileAggregationHistNamedSingleBucket) {
        // test case when only 1 range in hist
        // histogram with name "HIST(CountSingleBucket)" and
        // associated corresponding simple counter "CountSingleBucket"
        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        auto aggregator = CreateTabletCountersAggregator(false);
        auto aggregatorId = runtime.Register(aggregator);
        runtime.EnableScheduleForActor(aggregatorId);

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);

        TTabletWithHist tablet1(1);

        tablet1.SetSimpleCount("CountSingleBucket", 1);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist tablet2(2);
        tablet2.SetSimpleCount("CountSingleBucket", 13);
        tablet2.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(CountSingleBucket)",
            {0, 2},
            {0, 2}
        );

        // sanity check we didn't mess other histograms

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0}
        );

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {2, 0, 0, 0, 0},
            {2, 0, 0, 0, 0}
        );

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHistSingleBucket",
            {0, 0},
            {0, 0}
        );
    }

    Y_UNIT_TEST(IntegralPercentileAggregationHistNamed) {
        // test special histogram with name "HIST(Count)" and
        // associated corresponding simple counter "Count"
        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        auto aggregator = CreateTabletCountersAggregator(false);
        auto aggregatorId = runtime.Register(aggregator);
        runtime.EnableScheduleForActor(aggregatorId);

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);

        TTabletWithHist tablet1(1);

        tablet1.SetSimpleCount("Count", 1);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {0, 1, 0, 0, 0},
            {0, 1, 0, 0, 0}
        );

        TTabletWithHist tablet2(2);
        tablet2.SetSimpleCount("Count", 13);
        tablet2.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {0, 1, 1, 0, 0},
            {0, 1, 1, 0, 0}
        );

        TTabletWithHist tablet3(3);
        tablet3.SetSimpleCount("Count", 1);
        tablet3.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {0, 2, 1, 0, 0},
            {0, 2, 1, 0, 0}
        );

        tablet3.SetSimpleCount("Count", 13);
        tablet3.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {0, 1, 2, 0, 0},
            {0, 1, 2, 0, 0}
        );

        tablet3.ForgetTablet(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {0, 1, 1, 0, 0},
            {0, 1, 1, 0, 0}
        );

        // sanity check we didn't mess other histograms

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 0, 0, 0, 0},
            {0, 0, 0, 0, 0}
        );

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(CountSingleBucket)",
            {2, 0},
            {2, 0}
        );

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHistSingleBucket",
            {0, 0},
            {0, 0}
        );
    }

    Y_UNIT_TEST(IntegralPercentileAggregationHistNamedNoOverflowCheck) {
        // test special histogram with name "HIST(Count)" and
        // associated corresponding simple counter "Count"
        //
        // test just for extra sanity, because for Max<ui32> in bucket we
        // will need Max<ui32> tablets. So just check simple count behaviour
        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        auto aggregator = CreateTabletCountersAggregator(false);
        auto aggregatorId = runtime.Register(aggregator);
        runtime.EnableScheduleForActor(aggregatorId);

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);

        TTabletWithHist tablet1(1);

        tablet1.SetSimpleCount("Count", Max<i64>() - 100UL);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {0, 0, 0, 0, 1},
            {0, 0, 0, 0, 1}
        );

        TTabletWithHist tablet2(2);
        tablet2.SetSimpleCount("Count", 100);
        tablet2.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {0, 0, 0, 0, 2},
            {0, 0, 0, 0, 2}
        );
    }

    Y_UNIT_TEST(IntegralPercentileAggregationRegularCheckSingleTablet) {
        // test regular histogram, i.e. not named "HIST"
        // check that when single tablet sends multiple count updates,
        // the aggregated value is correct
        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        auto aggregator = CreateTabletCountersAggregator(false);
        auto aggregatorId = runtime.Register(aggregator);
        runtime.EnableScheduleForActor(aggregatorId);

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);

        TTabletWithHist tablet1(1);
        tablet1.UpdatePercentile("MyHist", 1);
        tablet1.SendUpdate(runtime, aggregatorId, edge);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 1, 0, 0, 0},
            {0, 1, 0, 0, 0}
        );

        tablet1.UpdatePercentile("MyHist", 13);
        tablet1.SendUpdate(runtime, aggregatorId, edge);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 1, 1, 0, 0},
            {0, 1, 1, 0, 0}
        );

        tablet1.UpdatePercentile("MyHist", 1);
        tablet1.UpdatePercentile("MyHist", 1);
        tablet1.UpdatePercentile("MyHist", 100);
        tablet1.SendUpdate(runtime, aggregatorId, edge);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 3, 1, 0, 1},
            {0, 3, 1, 0, 1}
        );
    }

    // Regression test for KIKIMR-13457
    Y_UNIT_TEST(IntegralPercentileAggregationRegular) {
        // test regular histogram, i.e. not named "HIST"
        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        auto aggregator = CreateTabletCountersAggregator(false);
        auto aggregatorId = runtime.Register(aggregator);
        runtime.EnableScheduleForActor(aggregatorId);

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);

        TTabletWithHist tablet1(1);
        tablet1.UpdatePercentile("MyHist", 1);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist tablet2(2);
        tablet2.UpdatePercentile("MyHist", 1);
        tablet2.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist tablet3(3);
        tablet3.UpdatePercentile("MyHist", 1);
        tablet3.UpdatePercentile("MyHist", 13);
        tablet3.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 3, 1, 0, 0},
            {0, 3, 1, 0, 0}
        );

        tablet3.ForgetTablet(runtime, aggregatorId, edge);

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 2, 0, 0, 0},
            {0, 2, 0, 0, 0}
        );

        // sanity check we didn't mess other histograms

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(Count)",
            {2, 0, 0, 0, 0},
            {2, 0, 0, 0, 0}
        );

        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHistSingleBucket",
            {0, 0},
            {0, 0}
        );

        TTabletWithHist::CheckHistogram(
            runtime,
            "HIST(CountSingleBucket)",
            {2, 0},
            {2, 0}
        );
    }

    Y_UNIT_TEST(IntegralPercentileAggregationRegularNoOverflowCheck) {
        // test regular histogram, i.e. not named "HIST"
        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        auto aggregator = CreateTabletCountersAggregator(false);
        auto aggregatorId = runtime.Register(aggregator);
        runtime.EnableScheduleForActor(aggregatorId);

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);

        TTabletWithHist tablet1(1);
        tablet1.UpdatePercentile("MyHist", 10, Max<i64>() - 100);
        tablet1.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist tablet2(2);
        tablet2.UpdatePercentile("MyHist", 10, 25);
        tablet2.SendUpdate(runtime, aggregatorId, edge);

        TTabletWithHist tablet3(3);
        tablet3.UpdatePercentile("MyHist", 10, 5);
        tablet3.SendUpdate(runtime, aggregatorId, edge);

        ui64 v = Max<i64>() - 70;
        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 0, v, 0, 0},
            {0, 0, v, 0, 0}
        );

        tablet1.ForgetTablet(runtime, aggregatorId, edge);
        TTabletWithHist::CheckHistogram(
            runtime,
            "MyHist",
            {0, 0, 30, 0, 0},
            {0, 0, 30, 0, 0}
        );
    }
}

Y_UNIT_TEST_SUITE(TTabletLabeledCountersAggregator) {
    Y_UNIT_TEST(SimpleAggregation) {
        TVector<TActorId> cc;
        TActorId aggregatorId;

        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        IActor* aggregator = CreateClusterLabeledCountersAggregatorActor(edge, TTabletTypes::PersQueue, 2, TString(), 3);
        aggregatorId = runtime.Register(aggregator);

        runtime.SetRegistrationObserverFunc([&cc, &aggregatorId](TTestActorRuntimeBase& runtime, const TActorId& parentId, const TActorId& actorId) {
                TTestActorRuntime::DefaultRegistrationObserver(runtime, parentId, actorId);
                    if (parentId == aggregatorId) {
                        cc.push_back(actorId);
                    }
                });

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);
        for (const auto& a : cc) {
            THolder<TEvInterconnect::TEvNodesInfo> nodesInfo = MakeHolder<TEvInterconnect::TEvNodesInfo>();
            nodesInfo->Nodes.emplace_back(TEvInterconnect::TNodeInfo(1, "::", "localhost", "localhost", 1234, TNodeLocation()));
            nodesInfo->Nodes.emplace_back(TEvInterconnect::TNodeInfo(2, "::", "localhost", "localhost", 1234, TNodeLocation()));
            nodesInfo->Nodes.emplace_back(TEvInterconnect::TNodeInfo(3, "::", "localhost", "localhost", 1234, TNodeLocation()));
            runtime.Send(new NActors::IEventHandle(a, edge, nodesInfo.Release()), 0, true);
        }

        {
            THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = MakeHolder<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
            auto& group1 = *response->Record.AddLabeledCountersByGroup();
            group1.SetGroup("group1|group2");
            group1.SetGroupNames("AAA|BBB");
            group1.SetDelimiter("|");
            auto& counter1 = *group1.AddLabeledCounter();
            counter1.SetName("value1");
            counter1.SetValue(13);
            counter1.SetType(TLabeledCounterOptions::CT_SIMPLE);
            counter1.SetAggregateFunc(TLabeledCounterOptions::EAF_SUM);
            runtime.Send(new NActors::IEventHandle(cc[0], edge, response.Release(), 0, 1), 0, true);
        }

        {
            THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = MakeHolder<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
            response->Record.AddCounterNames("value1");
            auto& group1 = *response->Record.AddLabeledCountersByGroup();
            group1.SetGroup("group1|group2");
            group1.SetGroupNames("AAA|BBB");
            group1.SetDelimiter("|");
            auto& counter1 = *group1.AddLabeledCounter();
            counter1.SetNameId(0);
            counter1.SetValue(13);
            counter1.SetType(TLabeledCounterOptions::CT_SIMPLE);
            counter1.SetAggregateFunc(TLabeledCounterOptions::EAF_SUM);
            runtime.Send(new NActors::IEventHandle(cc[1], edge, response.Release(), 0, 2), 0, true);
        }

        {
            THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = MakeHolder<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
            response->Record.AddCounterNames("value1");
            auto& group1 = *response->Record.AddLabeledCountersByGroup();
            group1.SetGroup("group1|group2");
            group1.SetGroupNames("AAA|BBB");
            group1.SetDelimiter("|");
            auto& counter1 = *group1.AddLabeledCounter();
            counter1.SetNameId(0);
            counter1.SetValue(13);
            counter1.SetType(TLabeledCounterOptions::CT_SIMPLE);
            counter1.SetAggregateFunc(TLabeledCounterOptions::EAF_SUM);
            runtime.Send(new NActors::IEventHandle(cc[2], edge, response.Release(), 0, 3), 0, true);
        }

        runtime.DispatchEvents();
        THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = runtime.GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
#ifndef NDEBUG
        Cerr << response->Record.DebugString() << Endl;
#endif
        UNIT_ASSERT(response != nullptr);
        UNIT_ASSERT_VALUES_EQUAL(response->Record.LabeledCountersByGroupSize(), 1);
        const auto& group1 = response->Record.GetLabeledCountersByGroup(0);
        UNIT_ASSERT_VALUES_EQUAL(group1.GetGroup(), "group1/group2");
        UNIT_ASSERT_VALUES_EQUAL(group1.LabeledCounterSize(), 1);
        UNIT_ASSERT_VALUES_EQUAL(group1.LabeledCounterSize(), 1);
        const auto& counter1 = group1.GetLabeledCounter(0);
        UNIT_ASSERT_VALUES_EQUAL(counter1.GetNameId(), 0);
        UNIT_ASSERT_VALUES_EQUAL(counter1.GetValue(), 39);
    }

    Y_UNIT_TEST(HeavyAggregation) {
        TestHeavy(2, 10);
        TestHeavy(2, 20);
        TestHeavy(2, 1);
        TestHeavy(2, 0);
    }

    Y_UNIT_TEST(Version3Aggregation) {
        TVector<TActorId> cc;
        TActorId aggregatorId;

        TTestBasicRuntime runtime(1);

        runtime.Initialize(TAppPrepare().Unwrap());
        TActorId edge = runtime.AllocateEdgeActor();

        IActor* aggregator = CreateClusterLabeledCountersAggregatorActor(edge, TTabletTypes::PersQueue, 3, "rt3.*--*,cons*/*/rt.*--*", 3);
        aggregatorId = runtime.Register(aggregator);

        runtime.SetRegistrationObserverFunc([&cc, &aggregatorId](TTestActorRuntimeBase& runtime, const TActorId& parentId, const TActorId& actorId) {
                TTestActorRuntime::DefaultRegistrationObserver(runtime, parentId, actorId);
                    if (parentId == aggregatorId) {
                        cc.push_back(actorId);
                    }
                });

        TDispatchOptions options;
        options.FinalEvents.emplace_back(TEvents::TSystem::Bootstrap, 1);
        runtime.DispatchEvents(options);
        for (const auto& a : cc) {
            THolder<TEvInterconnect::TEvNodesInfo> nodesInfo = MakeHolder<TEvInterconnect::TEvNodesInfo>();
            nodesInfo->Nodes.emplace_back(TEvInterconnect::TNodeInfo(1, "::", "localhost", "localhost", 1234, TNodeLocation()));
            nodesInfo->Nodes.emplace_back(TEvInterconnect::TNodeInfo(2, "::", "localhost", "localhost", 1234, TNodeLocation()));
            nodesInfo->Nodes.emplace_back(TEvInterconnect::TNodeInfo(3, "::", "localhost", "localhost", 1234, TNodeLocation()));
            runtime.Send(new NActors::IEventHandle(a, edge, nodesInfo.Release()), 0, true);
        }

        {
            THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = MakeHolder<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
            auto& group1 = *response->Record.AddLabeledCountersByGroup();
            group1.SetGroup("rt3.man--aba@caba--daba");
            group1.SetGroupNames("topic");
            group1.SetDelimiter("/");
            auto& counter1 = *group1.AddLabeledCounter();
            counter1.SetName("value1");
            counter1.SetValue(13);
            counter1.SetType(TLabeledCounterOptions::CT_SIMPLE);
            counter1.SetAggregateFunc(TLabeledCounterOptions::EAF_SUM);
            runtime.Send(new NActors::IEventHandle(cc[0], edge, response.Release(), 0, 1), 0, true);
        }

        {
            THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = MakeHolder<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
            response->Record.AddCounterNames("value1");
            auto& group1 = *response->Record.AddLabeledCountersByGroup();
            group1.SetGroup("cons@aaa/1/rt3.man--aba@caba--daba");
            group1.SetGroupNames("consumer/important/topic");
            group1.SetDelimiter("/");
            auto& counter1 = *group1.AddLabeledCounter();
            counter1.SetNameId(0);
            counter1.SetValue(13);
            counter1.SetType(TLabeledCounterOptions::CT_SIMPLE);
            counter1.SetAggregateFunc(TLabeledCounterOptions::EAF_SUM);
            runtime.Send(new NActors::IEventHandle(cc[1], edge, response.Release(), 0, 2), 0, true);
        }

        runtime.DispatchEvents();
        THolder<TEvTabletCounters::TEvTabletLabeledCountersResponse> response = runtime.GrabEdgeEvent<TEvTabletCounters::TEvTabletLabeledCountersResponse>();
#ifndef NDEBUG
        Cerr << response->Record.DebugString() << Endl;
#endif
        UNIT_ASSERT(response != nullptr);
        Cerr << response->Record;
        UNIT_ASSERT_VALUES_EQUAL(response->Record.LabeledCountersByGroupSize(), 2);
        const auto& group1 = response->Record.GetLabeledCountersByGroup(1);
        const auto& group2 = response->Record.GetLabeledCountersByGroup(0);
        TVector<TString> res = {group1.GetGroup(), group2.GetGroup()};
        std::sort(res.begin(), res.end());

        UNIT_ASSERT_VALUES_EQUAL(res[0], "aba/caba/daba|man");
        UNIT_ASSERT_VALUES_EQUAL(res[1], "cons/aaa|1|aba/caba/daba|man");
    }

}

}

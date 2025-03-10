// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <google/protobuf/repeated_field.h>
#include <gtest/gtest.h>

#include "yb/client/table.h"
#include "yb/client/tablet_server.h"
#include "yb/client/yb_table_name.h"
#include "yb/common/transaction.h"
#include "yb/integration-tests/xcluster_ycql_test_base.h"
#include "yb/master/master_defaults.h"
#include "yb/rpc/rpc_controller.h"

#include "yb/tserver/mini_tablet_server.h"

#include "yb/tserver/tablet_server.h"

#include "yb/tserver/tserver_service.proxy.h"

#include "yb/util/monotime.h"
#include "yb/yql/pgwrapper/pg_mini_test_base.h"


DECLARE_int32(transaction_table_num_tablets);
DECLARE_int32(heartbeat_interval_ms);

using namespace std::literals;
using std::string;

namespace yb {
namespace pgwrapper {

class TransactionInfoValidator {
 public:
  TransactionInfoValidator(): is_noop_(true) {}
  explicit TransactionInfoValidator(
      tserver::GetOldTransactionsResponsePB::OldTransactionMetadataPB data): data_(std::move(data))
    {}

  void WithTablets(std::vector<TabletId> tablets = {}) {
    if (is_noop_) return;

    std::vector<TabletId> response_tablets;
    for (const auto& tablet_id : data_.tablets()) {
      response_tablets.push_back(tablet_id);
    }
    std::sort(tablets.begin(), tablets.end());
    std::sort(response_tablets.begin(), response_tablets.end());

    auto not_equal = [&]() {
      return strings::Substitute(
          "Tablets received for txn $0 differ from tablets expected. Received: $1 vs. expected: $2",
          data_.transaction_id(), VectorToString(response_tablets), VectorToString(tablets));
    };

    ASSERT_EQ(tablets.size(), response_tablets.size()) << not_equal();

    for (size_t i = 0; i < tablets.size(); ++i) {
      if (tablets.at(i) != response_tablets.at(i)) {
        EXPECT_EQ(tablets.at(i), response_tablets.at(i)) << not_equal();
      }
    }
  }

 private:
  tserver::GetOldTransactionsResponsePB::OldTransactionMetadataPB data_;
  bool is_noop_ = false;
};

class GetOldTransactionsValidator {
 public:
  explicit GetOldTransactionsValidator(
      tserver::GetOldTransactionsResponsePB resp): resp_(std::move(resp)) {}

  TransactionInfoValidator HasTransaction(const TransactionId& txn_id) const {
    TransactionInfoValidator validator;
    bool found_txn = false;
    for (const auto& txn : resp_.txn()) {
      auto resp_txn = CHECK_RESULT(FullyDecodeTransactionId(txn.transaction_id()));
      if (resp_txn == txn_id) {
        EXPECT_FALSE(found_txn) << "Unexpected duplicate entry for txn";
        found_txn = true;
        validator = TransactionInfoValidator(txn);
      }
    }

    EXPECT_TRUE(found_txn) << "Did not find txn in response";
    return validator;
  }

  void DoesNotHaveTransaction(const TransactionId& txn_id) const {
    for (const auto& txn : resp_.txn()) {
      EXPECT_NE(txn.transaction_id(), txn_id.ToString());
    }
  }

 private:
  const tserver::GetOldTransactionsResponsePB resp_;
};

class PgGetOldTxnsTest : public PgMiniTestBase {
 protected:
  void SetUp() override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_transaction_table_num_tablets) = 1;
    PgMiniTestBase::SetUp();

    ts_proxy_ = std::make_unique<tserver::TabletServerServiceProxy>(
        &client_->proxy_cache(),
        HostPort::FromBoundEndpoint(cluster_->mini_tablet_server(0)->bound_rpc_addr()));
  }

  size_t NumTabletServers() override {
    return 1;
  }

  void OverrideMiniClusterOptions(MiniClusterOptions* options) override {
    options->transaction_table_num_tablets = 1;
  }

  GetOldTransactionsValidator CheckThat(const tserver::GetOldTransactionsResponsePB& resp) {
    return GetOldTransactionsValidator(resp);
  }

  Result<TabletId> GetSinglularStatusTabletId() {
    auto table_name = client::YBTableName(
        YQL_DATABASE_CQL, master::kSystemNamespaceName, kGlobalTransactionsTableName);
    std::vector<TabletId> tablet_uuids;
    RETURN_NOT_OK(client_->GetTablets(
        table_name, 1000 /* max_tablets */, &tablet_uuids, nullptr /* ranges */));
    CHECK_EQ(tablet_uuids.size(), 1);
    return tablet_uuids.at(0);
  }

  Result<TabletId> GetSinglularTabletId(const string& table_name) {
    auto tables = VERIFY_RESULT(client_->ListTables(table_name));
    if (tables.size() != 1) {
      return STATUS_FORMAT(InvalidArgument, "Unexpected number of tables $0", tables.size());
    }
    auto table_id = tables.at(0).table_id();
    if (table_id.empty()) {
      return STATUS(InvalidArgument, "Empty table_id");
    }

    google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
    RETURN_NOT_OK(client_->GetTabletsFromTableId(table_id, 5000, &tablets));
    if (tablets.size() != 1) {
      return STATUS_FORMAT(InvalidArgument, "Unexpected number of tablets $0", tablets.size());
    }
    return tablets.begin()->tablet_id();
  }

  Result<tserver::GetOldTransactionsResponsePB> GetOldTransactions(
      uint32_t min_txn_age_ms, uint32_t max_num_txns) {
    auto tablet_id = VERIFY_RESULT(GetSinglularStatusTabletId());

    constexpr int kTimeoutMs = 1000;
    tserver::GetOldTransactionsRequestPB req;
    tserver::GetOldTransactionsResponsePB resp;
    rpc::RpcController rpc;
    req.set_tablet_id(tablet_id);
    req.set_min_txn_age_ms(min_txn_age_ms);
    req.set_max_num_txns(max_num_txns);
    rpc.set_timeout(kTimeoutMs * 1ms);

    RETURN_NOT_OK(ts_proxy_->GetOldTransactions(req, &resp, &rpc));

    return resp;
  }

  Result<TransactionId> GetSingularTransactionOn(const TabletId& tablet_id) {
    auto txns = VERIFY_RESULT(GetOldTransactions(0 /* min_txn_age_ms */, 1000 /* max_num_txns */));
    TransactionId txn_id = TransactionId::Nil();
    for (const auto& txn : txns.txn()) {
      for (const auto& tablet : txn.tablets()) {
        if (tablet == tablet_id) {
          if (!txn_id.IsNil()) {
            return STATUS(InvalidArgument, "Found multiple transactions at tablet, expected one.");
          }
          txn_id = VERIFY_RESULT(FullyDecodeTransactionId(txn.transaction_id()));
        }
      }
    }
    if (txn_id.IsNil()) {
      return STATUS(NotFound, "Unexpected empty transaction ID");
    }
    return txn_id;
  }

  struct Session {
    PGConn conn;
    TabletId first_involved_tablet;
    string first_involved_table_name;
    TransactionId txn_id = TransactionId::Nil();
  };

  Result<Session> InitSession(const string& table_name) {
    auto setup_conn = VERIFY_RESULT(Connect());

    RETURN_NOT_OK(setup_conn.ExecuteFormat(
      "CREATE TABLE $0 (k INT PRIMARY KEY, v INT) SPLIT INTO 1 TABLETS", table_name));
    RETURN_NOT_OK(setup_conn.ExecuteFormat(
      "INSERT INTO $0 SELECT generate_series(1, 10), 0", table_name));

    Session s {
      .conn =  VERIFY_RESULT(Connect()),
      .first_involved_tablet = VERIFY_RESULT(GetSinglularTabletId(table_name)),
      .first_involved_table_name = table_name,
    };
    RETURN_NOT_OK(s.conn.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));

    RETURN_NOT_OK(s.conn.FetchFormat("SELECT * FROM $0 WHERE k=1 FOR UPDATE", table_name));

    SleepFor(2 * FLAGS_heartbeat_interval_ms * 1ms);

    s.txn_id = VERIFY_RESULT(GetSingularTransactionOn(s.first_involved_tablet));
    return s;
  }

 private:
  std::unique_ptr<tserver::TabletServerServiceProxy> ts_proxy_;
};

TEST_F(PgGetOldTxnsTest, DoesNotReturnNewTransactions) {
  auto start = CoarseMonoClock::Now();

  auto s1 = ASSERT_RESULT(InitSession("foo1"));
  auto s2 = ASSERT_RESULT(InitSession("foo2"));
  auto s3 = ASSERT_RESULT(InitSession("foo3"));

  auto time_since_start_ms = (uint32_t) ((CoarseMonoClock::Now() - start) / 1ms);
  auto min_txn_age_ms = time_since_start_ms * 2;
  auto res = ASSERT_RESULT(GetOldTransactions(min_txn_age_ms, 5 /* max_num_txns */));
  CheckThat(res).DoesNotHaveTransaction(s1.txn_id);
  CheckThat(res).DoesNotHaveTransaction(s2.txn_id);
  CheckThat(res).DoesNotHaveTransaction(s3.txn_id);
}


TEST_F(PgGetOldTxnsTest, ReturnsNewTransactions) {
  auto s1 = ASSERT_RESULT(InitSession("foo1"));
  auto s2 = ASSERT_RESULT(InitSession("foo2"));
  auto s3 = ASSERT_RESULT(InitSession("foo3"));

  auto res = ASSERT_RESULT(GetOldTransactions(0 /* min_txn_age_ms */, 5 /* max_num_txns */));
  CheckThat(res).HasTransaction(s1.txn_id)
                .WithTablets({s1.first_involved_tablet});
  CheckThat(res).HasTransaction(s2.txn_id)
                .WithTablets({s2.first_involved_tablet});
  CheckThat(res).HasTransaction(s3.txn_id)
                .WithTablets({s3.first_involved_tablet});
}

TEST_F(PgGetOldTxnsTest, SessionAtMultipleTablets) {
  auto s1 = ASSERT_RESULT(InitSession("foo1"));
  auto s2 = ASSERT_RESULT(InitSession("foo2"));
  auto s3 = ASSERT_RESULT(InitSession("foo3"));

  ASSERT_OK(s1.conn.ExecuteFormat("UPDATE $0 SET v=30 WHERE k=12", s2.first_involved_table_name));

  SleepFor(2 * FLAGS_heartbeat_interval_ms * 1ms);

  auto res = ASSERT_RESULT(GetOldTransactions(0 /* min_txn_age_ms */, 5 /* max_num_txns */));
  CheckThat(res).HasTransaction(s1.txn_id)
                .WithTablets({s1.first_involved_tablet, s2.first_involved_tablet});
  CheckThat(res).HasTransaction(s2.txn_id)
                .WithTablets({s2.first_involved_tablet});
  CheckThat(res).HasTransaction(s3.txn_id)
                .WithTablets({s3.first_involved_tablet});
}

TEST_F(PgGetOldTxnsTest, ReturnsOnlyOldEnoughTransactions) {
  auto s1 = ASSERT_RESULT(InitSession("foo1"));
  auto s2 = ASSERT_RESULT(InitSession("foo2"));
  auto s3 = ASSERT_RESULT(InitSession("foo3"));

  SleepFor(2 * FLAGS_heartbeat_interval_ms * 1ms);
  auto s4 = ASSERT_RESULT(InitSession("foo4"));
  ASSERT_OK(s2.conn.ExecuteFormat("UPDATE $0 SET v=30 WHERE k=15", s4.first_involved_table_name));
  SleepFor(2 * FLAGS_heartbeat_interval_ms * 1ms);

  auto min_txn_age_ms = FLAGS_heartbeat_interval_ms * 5;
  auto res = ASSERT_RESULT(GetOldTransactions(min_txn_age_ms, 5 /* max_num_txns */));
  CheckThat(res).HasTransaction(s1.txn_id)
                .WithTablets({s1.first_involved_tablet});
  CheckThat(res).HasTransaction(s2.txn_id)
                .WithTablets({s2.first_involved_tablet, s4.first_involved_tablet});
  CheckThat(res).HasTransaction(s3.txn_id)
                .WithTablets({s3.first_involved_tablet});
  CheckThat(res).DoesNotHaveTransaction(s4.txn_id);

  SleepFor(3 * FLAGS_heartbeat_interval_ms * 1ms);
  res = ASSERT_RESULT(GetOldTransactions(min_txn_age_ms, 5 /* max_num_txns */));
  CheckThat(res).HasTransaction(s1.txn_id)
                .WithTablets({s1.first_involved_tablet});
  CheckThat(res).HasTransaction(s2.txn_id)
                .WithTablets({s2.first_involved_tablet, s4.first_involved_tablet});
  CheckThat(res).HasTransaction(s3.txn_id)
                .WithTablets({s3.first_involved_tablet});
  CheckThat(res).HasTransaction(s4.txn_id)
                .WithTablets({s4.first_involved_tablet});
}

} // namespace pgwrapper
} // namespace yb

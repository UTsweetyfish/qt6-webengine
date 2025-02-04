// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/segmentation_platform/internal/database/segment_info_database.h"

#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "components/leveldb_proto/public/proto_database.h"
#include "components/leveldb_proto/testing/fake_db.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using InitStatus = leveldb_proto::Enums::InitStatus;

namespace segmentation_platform {

namespace {

// Test Ids.
const SegmentId kSegmentId =
    SegmentId::OPTIMIZATION_TARGET_SEGMENTATION_NEW_TAB;
const SegmentId kSegmentId2 = SegmentId::OPTIMIZATION_TARGET_SEGMENTATION_SHARE;

std::string ToString(SegmentId segment_id) {
  return base::NumberToString(static_cast<int>(segment_id));
}

proto::SegmentInfo CreateSegment(SegmentId segment_id,
                                 absl::optional<int> result = absl::nullopt) {
  proto::SegmentInfo info;
  info.set_segment_id(segment_id);

  if (result.has_value()) {
    info.mutable_prediction_result()->add_result(result.value());
  }
  return info;
}

}  // namespace

class SegmentInfoDatabaseTest : public testing::Test,
                                public ::testing::WithParamInterface<bool> {
 public:
  SegmentInfoDatabaseTest() = default;
  ~SegmentInfoDatabaseTest() override = default;

  void OnGetAllSegments(
      base::RepeatingClosure closure,
      std::unique_ptr<SegmentInfoDatabase::SegmentInfoList> entries) {
    get_all_segment_result_.swap(entries);
    std::move(closure).Run();
  }

  void OnGetSegment(absl::optional<proto::SegmentInfo> result) {
    get_segment_result_ = result;
  }

 protected:
  void SetUpDB(bool cache_enabled) {
    DCHECK(!db_);
    DCHECK(!segment_db_);

    auto db = std::make_unique<leveldb_proto::test::FakeDB<proto::SegmentInfo>>(
        &db_entries_);
    db_ = db.get();
    auto segment_info_cache = std::make_unique<SegmentInfoCache>(cache_enabled);
    segment_info_cache_ = segment_info_cache.get();
    segment_db_ = std::make_unique<SegmentInfoDatabase>(
        std::move(db), std::move(segment_info_cache));
  }

  void TearDown() override {
    db_entries_.clear();
    db_ = nullptr;
    segment_db_.reset();
  }

  void VerifyDb(base::flat_set<SegmentId> expected_ids) {
    EXPECT_EQ(expected_ids.size(), db_entries_.size());
    for (auto segment_id : expected_ids)
      EXPECT_TRUE(db_entries_.find(ToString(segment_id)) != db_entries_.end());
  }

  void WriteResult(SegmentId segment_id, absl::optional<float> result) {
    proto::PredictionResult prediction_result;
    if (result.has_value())
      prediction_result.add_result(result.value());

    segment_db_->SaveSegmentResult(segment_id,
                                   result.has_value()
                                       ? absl::make_optional(prediction_result)
                                       : absl::nullopt,
                                   base::DoNothing());
    if (segment_info_cache_->GetSegmentInfo(segment_id).first ==
        SegmentInfoCache::CachedItemState::kNotCached) {
      db_->GetCallback(true);
    }
    db_->UpdateCallback(true);
  }

  void VerifyResult(SegmentId segment_id, absl::optional<float> result) {
    segment_db_->GetSegmentInfo(
        segment_id, base::BindOnce(&SegmentInfoDatabaseTest::OnGetSegment,
                                   base::Unretained(this)));
    if (segment_info_cache_->GetSegmentInfo(segment_id).first ==
        SegmentInfoCache::CachedItemState::kNotCached) {
      db_->GetCallback(true);
    }

    EXPECT_EQ(segment_id, get_segment_result_->segment_id());
    EXPECT_EQ(result.has_value(), get_segment_result_->has_prediction_result());
    if (result.has_value()) {
      EXPECT_THAT(get_segment_result_->prediction_result().result(),
                  testing::ElementsAre(result.value()));
    }
  }

  void ExecuteAndVerifyGetSegmentInfoForSegments(
      const base::flat_set<SegmentId>& segment_ids) {
    base::RunLoop loop;
    segment_db_->GetSegmentInfoForSegments(
        segment_ids,
        base::BindOnce(&SegmentInfoDatabaseTest::OnGetAllSegments,
                       base::Unretained(this), loop.QuitClosure()));

    for (SegmentId segment_id : segment_ids) {
      if (segment_info_cache_->GetSegmentInfo(segment_id).first ==
          SegmentInfoCache::CachedItemState::kNotCached) {
        db_->LoadCallback(true);
        break;
      }
    }
    loop.Run();
    EXPECT_EQ(segment_ids.size(), get_all_segment_result().size());
    int index = 0;
    for (SegmentId segment_id : segment_ids) {
      EXPECT_EQ(segment_id, get_all_segment_result()[index++].first);
    }
  }
  const SegmentInfoDatabase::SegmentInfoList& get_all_segment_result() const {
    return *get_all_segment_result_;
  }

  base::test::TaskEnvironment task_environment_;
  std::unique_ptr<SegmentInfoDatabase::SegmentInfoList> get_all_segment_result_;
  absl::optional<proto::SegmentInfo> get_segment_result_;
  std::map<std::string, proto::SegmentInfo> db_entries_;
  raw_ptr<leveldb_proto::test::FakeDB<proto::SegmentInfo>> db_{nullptr};
  std::unique_ptr<SegmentInfoDatabase> segment_db_;
  raw_ptr<SegmentInfoCache> segment_info_cache_;
};

INSTANTIATE_TEST_SUITE_P(IsCacheEnabled,
                         SegmentInfoDatabaseTest,
                         ::testing::Bool());

TEST_P(SegmentInfoDatabaseTest, Get) {
  // Initialize DB with one entry.
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId), CreateSegment(kSegmentId)));
  SetUpDB(GetParam());

  segment_db_->Initialize(base::DoNothing());
  db_->InitStatusCallback(leveldb_proto::Enums::InitStatus::kOK);
  VerifyDb({kSegmentId});

  // Get all segments.
  ExecuteAndVerifyGetSegmentInfoForSegments({kSegmentId});

  // Get a single segment.
  segment_db_->GetSegmentInfo(
      kSegmentId, base::BindOnce(&SegmentInfoDatabaseTest::OnGetSegment,
                                 base::Unretained(this)));
  if (segment_info_cache_->GetSegmentInfo(kSegmentId).first ==
      SegmentInfoCache::CachedItemState::kNotCached) {
    db_->GetCallback(true);
  }
  EXPECT_TRUE(get_segment_result_.has_value());
  EXPECT_EQ(kSegmentId, get_segment_result_->segment_id());
}

TEST_P(SegmentInfoDatabaseTest, Update) {
  // Initialize DB with one entry.
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId), CreateSegment(kSegmentId)));
  SetUpDB(GetParam());

  segment_db_->Initialize(base::DoNothing());
  db_->InitStatusCallback(leveldb_proto::Enums::InitStatus::kOK);

  // Delete a segment.
  segment_db_->UpdateSegment(kSegmentId, absl::nullopt, base::DoNothing());
  db_->UpdateCallback(true);
  VerifyDb({});

  // Insert a segment and verify.
  segment_db_->UpdateSegment(kSegmentId, CreateSegment(kSegmentId),
                             base::DoNothing());
  db_->UpdateCallback(true);
  VerifyDb({kSegmentId});

  // Insert another segment and verify.
  segment_db_->UpdateSegment(kSegmentId2, CreateSegment(kSegmentId2),
                             base::DoNothing());
  db_->UpdateCallback(true);
  VerifyDb({kSegmentId, kSegmentId2});

  // Verify GetSegmentInfoForSegments.
  ExecuteAndVerifyGetSegmentInfoForSegments({kSegmentId2});

  ExecuteAndVerifyGetSegmentInfoForSegments({kSegmentId});

  ExecuteAndVerifyGetSegmentInfoForSegments({kSegmentId, kSegmentId2});
}

TEST_P(SegmentInfoDatabaseTest, UpdateMultipleSegments) {
  // Initialize DB with two entry.
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId), CreateSegment(kSegmentId)));
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId2), CreateSegment(kSegmentId2)));
  SetUpDB(GetParam());

  segment_db_->Initialize(base::DoNothing());
  db_->InitStatusCallback(leveldb_proto::Enums::InitStatus::kOK);

  // Delete both segments.
  segment_db_->UpdateMultipleSegments({}, {kSegmentId, kSegmentId2},
                                      base::DoNothing());
  db_->UpdateCallback(true);
  VerifyDb({});

  // Insert multiple segments and verify.
  std::vector<std::pair<SegmentId, proto::SegmentInfo>> segments_to_update;
  segments_to_update.emplace_back(
      std::make_pair(kSegmentId, CreateSegment(kSegmentId)));
  segments_to_update.emplace_back(
      std::make_pair(kSegmentId2, CreateSegment(kSegmentId2)));
  segment_db_->UpdateMultipleSegments(segments_to_update, {},
                                      base::DoNothing());
  db_->UpdateCallback(true);
  VerifyDb({kSegmentId, kSegmentId2});

  // Update one of the existing segment and verify.
  proto::SegmentInfo segment_info = CreateSegment(kSegmentId2);
  segment_info.mutable_prediction_result()->add_result(0.9f);
  // Add this entry to `segments_to_update`.
  segments_to_update.clear();
  segments_to_update.emplace_back(std::make_pair(kSegmentId2, segment_info));
  // Call and Verify.
  segment_db_->UpdateMultipleSegments(segments_to_update, {},
                                      base::DoNothing());
  db_->UpdateCallback(true);
  VerifyDb({kSegmentId, kSegmentId2});
  VerifyResult(kSegmentId2, 0.9f);

  // Verify GetSegmentInfoForSegments.
  ExecuteAndVerifyGetSegmentInfoForSegments({kSegmentId2});

  ExecuteAndVerifyGetSegmentInfoForSegments({kSegmentId});

  ExecuteAndVerifyGetSegmentInfoForSegments({kSegmentId, kSegmentId2});
}

TEST_P(SegmentInfoDatabaseTest, WriteResult) {
  // Initialize DB with one entry.
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId), CreateSegment(kSegmentId)));
  SetUpDB(GetParam());

  segment_db_->Initialize(base::DoNothing());
  db_->InitStatusCallback(leveldb_proto::Enums::InitStatus::kOK);

  // Update results and verify.
  WriteResult(kSegmentId, 0.4f);
  VerifyResult(kSegmentId, 0.4f);

  // Overwrite results and verify.
  WriteResult(kSegmentId, 0.9f);
  VerifyResult(kSegmentId, 0.9f);

  // Clear results and verify.
  WriteResult(kSegmentId, absl::nullopt);
  VerifyResult(kSegmentId, absl::nullopt);
}

TEST_P(SegmentInfoDatabaseTest, WriteResultWithCache) {
  // Initialize DB with cache enabled and one entry.
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId), CreateSegment(kSegmentId)));
  VerifyDb({kSegmentId});
  SetUpDB(true);

  segment_db_->Initialize(base::DoNothing());
  db_->InitStatusCallback(leveldb_proto::Enums::InitStatus::kOK);
  EXPECT_EQ(segment_info_cache_->GetSegmentInfo(kSegmentId).first,
            SegmentInfoCache::CachedItemState::kNotCached);

  // Verify that all DB entries are loaded into cache on initialization.
  db_->LoadCallback(true);
  EXPECT_EQ(segment_info_cache_->GetSegmentInfo(kSegmentId).first,
            SegmentInfoCache::CachedItemState::kCachedAndFound);

  // Update results and verify that db is updated.
  WriteResult(kSegmentId, 0.4f);

  // Verify that cache is updated.
  VerifyResult(kSegmentId, 0.4f);
}

TEST_P(SegmentInfoDatabaseTest, WriteResultForTwoSegments) {
  // Initialize DB with two entries.
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId), CreateSegment(kSegmentId)));
  db_entries_.insert(
      std::make_pair(ToString(kSegmentId2), CreateSegment(kSegmentId2)));
  SetUpDB(GetParam());

  segment_db_->Initialize(base::DoNothing());
  db_->InitStatusCallback(leveldb_proto::Enums::InitStatus::kOK);

  // Update results for first segment.
  WriteResult(kSegmentId, 0.4f);

  // Update results for second segment.
  WriteResult(kSegmentId2, 0.9f);

  // Verify results for both segments.
  VerifyResult(kSegmentId, 0.4f);
  VerifyResult(kSegmentId2, 0.9f);
}

}  // namespace segmentation_platform

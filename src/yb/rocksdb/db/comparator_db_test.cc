// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include <map>
#include <string>

#include <gtest/gtest.h>

#include "yb/rocksdb/db.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/util/hash.h"
#include "yb/rocksdb/util/kv_map.h"
#include "yb/rocksdb/util/testharness.h"
#include "yb/rocksdb/util/testutil.h"

#include "yb/util/string_util.h"
#include "yb/util/test_macros.h"

using std::unique_ptr;

namespace rocksdb {
namespace {

static const Comparator* comparator;

class KVIter : public Iterator {
 public:
  explicit KVIter(const stl_wrappers::KVMap* map)
      : map_(map), iter_(map_->end()) {}
  void SeekToFirst() override { iter_ = map_->begin(); }
  void SeekToLast() override {
    if (map_->empty()) {
      iter_ = map_->end();
    } else {
      iter_ = map_->find(map_->rbegin()->first);
    }
  }
  void Seek(const Slice& k) override {
    iter_ = map_->lower_bound(k.ToString());
  }
  const KeyValueEntry& Next() override {
    ++iter_;
    return Entry();
  }
  void Prev() override {
    if (iter_ == map_->begin()) {
      iter_ = map_->end();
      return;
    }
    --iter_;
  }

  const KeyValueEntry& Entry() const override {
    if (iter_ == map_->end()) {
      return KeyValueEntry::Invalid();
    }
    entry_ = {
      .key = iter_->first,
      .value = iter_->second,
    };
    return entry_;
  }

  Status status() const override { return Status::OK(); }

 private:
  const stl_wrappers::KVMap* const map_;
  stl_wrappers::KVMap::const_iterator iter_;
  mutable KeyValueEntry entry_;
};

void AssertItersEqual(Iterator* iter1, Iterator* iter2) {
  ASSERT_EQ(ASSERT_RESULT(iter1->CheckedValid()), ASSERT_RESULT(iter2->CheckedValid()));
  if (iter1->Valid()) {
    auto key1 = iter1->key().ToBuffer();
    auto key2 = iter2->key().ToBuffer();
    auto value1 = iter1->value().ToBuffer();
    auto value2 = iter2->value().ToBuffer();
    ASSERT_EQ(key1, key2);
    ASSERT_EQ(value1, value2);
  }
}

// Measuring operations on DB (expect to be empty).
// source_strings are candidate keys
void DoRandomIteraratorTest(DB* db, std::vector<std::string> source_strings,
                            Random* rnd, int num_writes, int num_iter_ops,
                            int num_trigger_flush) {
  stl_wrappers::KVMap map((stl_wrappers::LessOfComparator(comparator)));

  for (int i = 0; i < num_writes; i++) {
    if (num_trigger_flush > 0 && i != 0 && i % num_trigger_flush == 0) {
      ASSERT_OK(db->Flush(FlushOptions()));
    }

    int type = rnd->Uniform(2);
    int index = rnd->Uniform(static_cast<int>(source_strings.size()));
    auto& key = source_strings[index];
    switch (type) {
      case 0:
        // put
        map[key] = key;
        ASSERT_OK(db->Put(WriteOptions(), key, key));
        break;
      case 1:
        // delete
        if (map.find(key) != map.end()) {
          map.erase(key);
        }
        ASSERT_OK(db->Delete(WriteOptions(), key));
        break;
      default:
        assert(false);
    }
  }

  std::unique_ptr<Iterator> iter(db->NewIterator(ReadOptions()));
  std::unique_ptr<Iterator> result_iter(new KVIter(&map));

  bool is_valid = false;
  for (int i = 0; i < num_iter_ops; i++) {
    // Random walk and make sure iter and result_iter returns the
    // same key and value
    int type = rnd->Uniform(6);
    ASSERT_OK(iter->status());
    ASSERT_OK(result_iter->status());
    switch (type) {
      case 0:
        // Seek to First
        iter->SeekToFirst();
        result_iter->SeekToFirst();
        break;
      case 1:
        // Seek to last
        iter->SeekToLast();
        result_iter->SeekToLast();
        break;
      case 2: {
        // Seek to random key
        auto key_idx = rnd->Uniform(static_cast<int>(source_strings.size()));
        auto key = source_strings[key_idx];
        iter->Seek(key);
        result_iter->Seek(key);
        break;
      }
      case 3:
        // Next
        if (is_valid) {
          iter->Next();
          result_iter->Next();
        } else {
          continue;
        }
        break;
      case 4:
        // Prev
        if (is_valid) {
          iter->Prev();
          result_iter->Prev();
        } else {
          continue;
        }
        break;
      default: {
        assert(type == 5);
        auto key_idx = rnd->Uniform(static_cast<int>(source_strings.size()));
        auto key = source_strings[key_idx];
        std::string result;
        auto status = db->Get(ReadOptions(), key, &result);
        if (map.find(key) == map.end()) {
          ASSERT_TRUE(status.IsNotFound());
        } else {
          ASSERT_EQ(map[key], result);
        }
        break;
      }
    }
    AssertItersEqual(iter.get(), result_iter.get());
    is_valid = ASSERT_RESULT(iter->CheckedValid());
  }
}

class DoubleComparator : public Comparator {
 public:
  DoubleComparator() {}

  const char* Name() const override { return "DoubleComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
#ifndef CYGWIN
    double da = std::stod(a.ToString());
    double db = std::stod(b.ToString());
#else
    double da = std::strtod(a.ToString().c_str(), 0 /* endptr */);
    double db = std::strtod(a.ToString().c_str(), 0 /* endptr */);
#endif
    if (da == db) {
      return a.compare(b);
    } else if (da > db) {
      return 1;
    } else {
      return -1;
    }
  }
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const override {}

  void FindShortSuccessor(std::string* key) const override {}
};

class HashComparator : public Comparator {
 public:
  HashComparator() {}

  const char* Name() const override { return "HashComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
    uint32_t ha = Hash(a.data(), a.size(), 66);
    uint32_t hb = Hash(b.data(), b.size(), 66);
    if (ha == hb) {
      return a.compare(b);
    } else if (ha > hb) {
      return 1;
    } else {
      return -1;
    }
  }
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const override {}

  void FindShortSuccessor(std::string* key) const override {}
};

class TwoStrComparator : public Comparator {
 public:
  TwoStrComparator() {}

  const char* Name() const override { return "TwoStrComparator"; }

  int Compare(const Slice& a, const Slice& b) const override {
    assert(a.size() >= 2);
    assert(b.size() >= 2);
    size_t size_a1 = static_cast<size_t>(a[0]);
    size_t size_b1 = static_cast<size_t>(b[0]);
    size_t size_a2 = static_cast<size_t>(a[1]);
    size_t size_b2 = static_cast<size_t>(b[1]);
    assert(size_a1 + size_a2 + 2 == a.size());
    assert(size_b1 + size_b2 + 2 == b.size());

    Slice a1 = Slice(a.data() + 2, size_a1);
    Slice b1 = Slice(b.data() + 2, size_b1);
    Slice a2 = Slice(a.data() + 2 + size_a1, size_a2);
    Slice b2 = Slice(b.data() + 2 + size_b1, size_b2);

    if (a1 != b1) {
      return a1.compare(b1);
    }
    return a2.compare(b2);
  }
  virtual void FindShortestSeparator(std::string* start,
                                     const Slice& limit) const override {}

  void FindShortSuccessor(std::string* key) const override {}
};
}  // namespace

class ComparatorDBTest : public RocksDBTest {
 private:
  std::string dbname_;
  Env* env_;
  DB* db_;
  Options last_options_;
  std::unique_ptr<const Comparator> comparator_guard;

 public:
  ComparatorDBTest() : env_(Env::Default()), db_(nullptr) {
    comparator = BytewiseComparator();
    dbname_ = test::TmpDir() + "/comparator_db_test";
    EXPECT_OK(DestroyDB(dbname_, last_options_));
  }

  ~ComparatorDBTest() {
    delete db_;
    EXPECT_OK(DestroyDB(dbname_, last_options_));
    comparator = BytewiseComparator();
  }

  DB* GetDB() { return db_; }

  void SetOwnedComparator(const Comparator* cmp) {
    comparator_guard.reset(cmp);
    SetComparator(cmp);
  }

  void SetComparator(const Comparator* cmp) {
    comparator = cmp;
    last_options_.comparator = cmp;
  }

  // Return the current option configuration.
  Options* GetOptions() { return &last_options_; }

  void DestroyAndReopen() {
    // Destroy using last options
    Destroy();
    ASSERT_OK(TryReopen());
  }

  void Destroy() {
    delete db_;
    db_ = nullptr;
    ASSERT_OK(DestroyDB(dbname_, last_options_));
  }

  Status TryReopen() {
    delete db_;
    db_ = nullptr;
    last_options_.create_if_missing = true;

    return DB::Open(last_options_, dbname_, &db_);
  }
};

TEST_F(ComparatorDBTest, Bytewise) {
  for (int rand_seed = 301; rand_seed < 306; rand_seed++) {
    DestroyAndReopen();
    Random rnd(rand_seed);
    DoRandomIteraratorTest(GetDB(),
                           {"a", "b", "c", "d", "e", "f", "g", "h", "i"}, &rnd,
                           8, 100, 3);
  }
}

TEST_F(ComparatorDBTest, SimpleSuffixReverseComparator) {
  SetOwnedComparator(new test::SimpleSuffixReverseComparator());

  for (int rnd_seed = 301; rnd_seed < 316; rnd_seed++) {
    Options* opt = GetOptions();
    opt->comparator = comparator;
    DestroyAndReopen();
    Random rnd(rnd_seed);

    std::vector<std::string> source_strings;
    std::vector<std::string> source_prefixes;
    // Randomly generate 5 prefixes
    for (int i = 0; i < 5; i++) {
      source_prefixes.push_back(test::RandomHumanReadableString(&rnd, 8));
    }
    for (int j = 0; j < 20; j++) {
      int prefix_index = rnd.Uniform(static_cast<int>(source_prefixes.size()));
      std::string key = source_prefixes[prefix_index] +
                        test::RandomHumanReadableString(&rnd, rnd.Uniform(8));
      source_strings.push_back(key);
    }

    DoRandomIteraratorTest(GetDB(), source_strings, &rnd, 30, 600, 66);
  }
}

TEST_F(ComparatorDBTest, Uint64Comparator) {
  SetComparator(Uint64Comparator());

  for (int rnd_seed = 301; rnd_seed < 316; rnd_seed++) {
    Options* opt = GetOptions();
    opt->comparator = comparator;
    DestroyAndReopen();
    Random rnd(rnd_seed);
    Random64 rnd64(rnd_seed);

    std::vector<std::string> source_strings;
    // Randomly generate source keys
    for (int i = 0; i < 100; i++) {
      uint64_t r = rnd64.Next();
      std::string str;
      str.resize(8);
      memcpy(&str[0], static_cast<void*>(&r), 8);
      source_strings.push_back(str);
    }

    DoRandomIteraratorTest(GetDB(), source_strings, &rnd, 200, 1000, 66);
  }
}

TEST_F(ComparatorDBTest, DoubleComparator) {
  SetOwnedComparator(new DoubleComparator());

  for (int rnd_seed = 301; rnd_seed < 316; rnd_seed++) {
    Options* opt = GetOptions();
    opt->comparator = comparator;
    DestroyAndReopen();
    Random rnd(rnd_seed);

    std::vector<std::string> source_strings;
    // Randomly generate source keys
    for (int i = 0; i < 100; i++) {
      uint32_t r = rnd.Next();
      uint32_t divide_order = rnd.Uniform(8);
      double to_divide = 1.0;
      for (uint32_t j = 0; j < divide_order; j++) {
        to_divide *= 10.0;
      }
      source_strings.push_back(ToString(r / to_divide));
    }

    DoRandomIteraratorTest(GetDB(), source_strings, &rnd, 200, 1000, 66);
  }
}

TEST_F(ComparatorDBTest, HashComparator) {
  SetOwnedComparator(new HashComparator());

  for (int rnd_seed = 301; rnd_seed < 316; rnd_seed++) {
    Options* opt = GetOptions();
    opt->comparator = comparator;
    DestroyAndReopen();
    Random rnd(rnd_seed);

    std::vector<std::string> source_strings;
    // Randomly generate source keys
    for (int i = 0; i < 100; i++) {
      source_strings.push_back(test::RandomKey(&rnd, 8));
    }

    DoRandomIteraratorTest(GetDB(), source_strings, &rnd, 200, 1000, 66);
  }
}

TEST_F(ComparatorDBTest, TwoStrComparator) {
  SetOwnedComparator(new TwoStrComparator());

  for (int rnd_seed = 301; rnd_seed < 316; rnd_seed++) {
    Options* opt = GetOptions();
    opt->comparator = comparator;
    DestroyAndReopen();
    Random rnd(rnd_seed);

    std::vector<std::string> source_strings;
    // Randomly generate source keys
    for (int i = 0; i < 100; i++) {
      std::string str;
      uint32_t size1 = rnd.Uniform(8);
      uint32_t size2 = rnd.Uniform(8);
      str.append(1, static_cast<char>(size1));
      str.append(1, static_cast<char>(size2));
      str.append(test::RandomKey(&rnd, size1));
      str.append(test::RandomKey(&rnd, size2));
      source_strings.push_back(str);
    }

    DoRandomIteraratorTest(GetDB(), source_strings, &rnd, 200, 1000, 66);
  }
}

}  // namespace rocksdb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <utility>

#include "../../../src/common/hist_util.h"
#include "../helpers.h"
#include "test_hist_util.h"

namespace xgboost {
namespace common {

size_t GetNThreads() {
  size_t nthreads;
  #pragma omp parallel
  {
    #pragma omp master
    nthreads = omp_get_num_threads();
  }
  return nthreads;
}


TEST(ParallelGHistBuilder, Reset) {
  constexpr size_t kBins = 10;
  constexpr size_t kNodes = 5;
  constexpr size_t kNodesExtended = 10;
  constexpr size_t kTasksPerNode = 10;
  constexpr double kValue = 1.0;
  const size_t nthreads = GetNThreads();

  HistCollection collection;
  collection.Init(kBins);

  for(size_t inode = 0; inode < kNodesExtended; inode++) {
    collection.AddHistRow(inode);
  }

  ParallelGHistBuilder hist_builder;
  hist_builder.Init(kBins);
  std::vector<GHistRow> target_hist(kNodes);
  for(size_t i = 0; i < target_hist.size(); ++i) {
    target_hist[i] = collection[i];
  }

  common::BlockedSpace2d space(kNodes, [&](size_t node) { return kTasksPerNode; }, 1);
  hist_builder.Reset(nthreads, kNodes, space, target_hist);

  common::ParallelFor2d(space, nthreads, [&](size_t inode, common::Range1d r) {
    const size_t tid = omp_get_thread_num();

    GHistRow hist = hist_builder.GetInitializedHist(tid, inode);
    // fill hist by some non-null values
    for(size_t j = 0; j < kBins; ++j) {
      hist[j].Add(kValue, kValue);
    }
  });

  // reset and extend buffer
  target_hist.resize(kNodesExtended);
  for(size_t i = 0; i < target_hist.size(); ++i) {
    target_hist[i] = collection[i];
  }
  common::BlockedSpace2d space2(kNodesExtended, [&](size_t node) { return kTasksPerNode; }, 1);
  hist_builder.Reset(nthreads, kNodesExtended, space2, target_hist);

  common::ParallelFor2d(space2, nthreads, [&](size_t inode, common::Range1d r) {
    const size_t tid = omp_get_thread_num();

    GHistRow hist = hist_builder.GetInitializedHist(tid, inode);
    // fill hist by some non-null values
    for(size_t j = 0; j < kBins; ++j) {
      ASSERT_EQ(0.0, hist[j].GetGrad());
      ASSERT_EQ(0.0, hist[j].GetHess());
    }
  });
}

TEST(ParallelGHistBuilder, ReduceHist) {
  constexpr size_t kBins = 10;
  constexpr size_t kNodes = 5;
  constexpr size_t kTasksPerNode = 10;
  constexpr double kValue = 1.0;
  const size_t nthreads = GetNThreads();

  HistCollection collection;
  collection.Init(kBins);

  for(size_t inode = 0; inode < kNodes; inode++) {
    collection.AddHistRow(inode);
  }

  ParallelGHistBuilder hist_builder;
  hist_builder.Init(kBins);
  std::vector<GHistRow> target_hist(kNodes);
  for(size_t i = 0; i < target_hist.size(); ++i) {
    target_hist[i] = collection[i];
  }

  common::BlockedSpace2d space(kNodes, [&](size_t node) { return kTasksPerNode; }, 1);
  hist_builder.Reset(nthreads, kNodes, space, target_hist);

  // Simple analog of BuildHist function, works in parallel for both tree-nodes and data in node
  common::ParallelFor2d(space, nthreads, [&](size_t inode, common::Range1d r) {
    const size_t tid = omp_get_thread_num();

    GHistRow hist = hist_builder.GetInitializedHist(tid, inode);
    for(size_t i = 0; i < kBins; ++i) {
      hist[i].Add(kValue, kValue);
    }
  });

  for(size_t inode = 0; inode < kNodes; inode++) {
    hist_builder.ReduceHist(inode, 0, kBins);

    // We had kTasksPerNode tasks to add kValue to each bin for each node
    // So, after reducing we expect to have (kValue * kTasksPerNode) in each node
    for(size_t i = 0; i < kBins; ++i) {
      ASSERT_EQ(kValue * kTasksPerNode, collection[inode][i].GetGrad());
      ASSERT_EQ(kValue * kTasksPerNode, collection[inode][i].GetHess());
    }
  }
}


TEST(CutsBuilder, SearchGroupInd) {
  size_t constexpr kNumGroups = 4;
  size_t constexpr kRows = 17;
  size_t constexpr kCols = 15;

  auto pp_dmat = CreateDMatrix(kRows, kCols, 0);
  std::shared_ptr<DMatrix> p_mat {*pp_dmat};

  std::vector<bst_int> group(kNumGroups);
  group[0] = 2;
  group[1] = 3;
  group[2] = 7;
  group[3] = 5;

  p_mat->Info().SetInfo(
      "group", group.data(), DataType::kUInt32, kNumGroups);

  HistogramCuts hmat;

  size_t group_ind = CutsBuilder::SearchGroupIndFromRow(p_mat->Info().group_ptr_, 0);
  ASSERT_EQ(group_ind, 0);

  group_ind = CutsBuilder::SearchGroupIndFromRow(p_mat->Info().group_ptr_, 5);
  ASSERT_EQ(group_ind, 2);

  EXPECT_ANY_THROW(CutsBuilder::SearchGroupIndFromRow(p_mat->Info().group_ptr_, 17));

  delete pp_dmat;
}

TEST(SparseCuts, SingleThreadedBuild) {
  size_t constexpr kRows = 267;
  size_t constexpr kCols = 31;
  size_t constexpr kBins = 256;

  auto pp_dmat = CreateDMatrix(kRows, kCols, 0);
  std::shared_ptr<DMatrix> p_fmat {*pp_dmat};

  common::GHistIndexMatrix hmat;
  hmat.Init(p_fmat.get(), kBins);

  HistogramCuts cuts;
  SparseCuts indices(&cuts);
  auto const& page = *(p_fmat->GetBatches<xgboost::CSCPage>().begin());
  indices.SingleThreadBuild(page, p_fmat->Info(), kBins, false, 0, page.Size(), 0);

  ASSERT_EQ(hmat.cut.Ptrs().size(), cuts.Ptrs().size());
  ASSERT_EQ(hmat.cut.Ptrs(), cuts.Ptrs());
  ASSERT_EQ(hmat.cut.Values(), cuts.Values());
  ASSERT_EQ(hmat.cut.MinValues(), cuts.MinValues());

  delete pp_dmat;
}

TEST(SparseCuts, MultiThreadedBuild) {
  size_t constexpr kRows = 17;
  size_t constexpr kCols = 15;
  size_t constexpr kBins = 255;

  omp_ulong ori_nthreads = omp_get_max_threads();
  omp_set_num_threads(16);

  auto Compare =
#if defined(_MSC_VER)  // msvc fails to capture
      [kBins](DMatrix* p_fmat) {
#else
      [](DMatrix* p_fmat) {
#endif
        HistogramCuts threaded_container;
        SparseCuts threaded_indices(&threaded_container);
        threaded_indices.Build(p_fmat, kBins);

        HistogramCuts container;
        SparseCuts indices(&container);
        auto const& page = *(p_fmat->GetBatches<xgboost::CSCPage>().begin());
        indices.SingleThreadBuild(page, p_fmat->Info(), kBins, false, 0, page.Size(), 0);

        ASSERT_EQ(container.Ptrs().size(), threaded_container.Ptrs().size());
        ASSERT_EQ(container.Values().size(), threaded_container.Values().size());

        for (uint32_t i = 0; i < container.Ptrs().size(); ++i) {
          ASSERT_EQ(container.Ptrs()[i], threaded_container.Ptrs()[i]);
        }
        for (uint32_t i = 0; i < container.Values().size(); ++i) {
          ASSERT_EQ(container.Values()[i], threaded_container.Values()[i]);
        }
      };

  {
    auto pp_mat = CreateDMatrix(kRows, kCols, 0);
    DMatrix* p_fmat = (*pp_mat).get();
    Compare(p_fmat);
    delete pp_mat;
  }

  {
    auto pp_mat = CreateDMatrix(kRows, kCols, 0.0001);
    DMatrix* p_fmat = (*pp_mat).get();
    Compare(p_fmat);
    delete pp_mat;
  }

  omp_set_num_threads(ori_nthreads);
}

TEST(hist_util, DenseCutsCategorical) {
   int categorical_sizes[] = {2, 6, 8, 12};
   int num_bins = 256;
   int sizes[] = {25, 100, 1000};
   for (auto n : sizes) {
     for (auto num_categories : categorical_sizes) {
       auto x = GenerateRandomCategoricalSingleColumn(n, num_categories);
       std::vector<float> x_sorted(x);
       std::sort(x_sorted.begin(), x_sorted.end());
       auto dmat = GetDMatrixFromData(x, n, 1);
       HistogramCuts cuts;
       DenseCuts dense(&cuts);
       dense.Build(dmat.get(), num_bins);
       auto cuts_from_sketch = cuts.Values();
       EXPECT_LT(cuts.MinValues()[0], x_sorted.front());
       EXPECT_GT(cuts_from_sketch.front(), x_sorted.front());
       EXPECT_GE(cuts_from_sketch.back(), x_sorted.back());
       EXPECT_EQ(cuts_from_sketch.size(), num_categories);
     }
   }
}

TEST(hist_util, DenseCutsAccuracyTest) {
  int bin_sizes[] = {2, 16, 256, 512};
  int sizes[] = {100, 1000, 1500};
  int num_columns = 5;
  for (auto num_rows : sizes) {
    auto x = GenerateRandom(num_rows, num_columns);
    auto dmat = GetDMatrixFromData(x, num_rows, num_columns);
    for (auto num_bins : bin_sizes) {
      HistogramCuts cuts;
      DenseCuts dense(&cuts);
      dense.Build(dmat.get(), num_bins);
      ValidateCuts(cuts, x, num_rows, num_columns, num_bins);
    }
  }
}

TEST(hist_util, DenseCutsExternalMemory) {
  int bin_sizes[] = {2, 16, 256, 512};
  int sizes[] = {100, 1000, 1500};
  int num_columns = 5;
  for (auto num_rows : sizes) {
    auto x = GenerateRandom(num_rows, num_columns);
    dmlc::TemporaryDirectory tmpdir;
    auto dmat =
        GetExternalMemoryDMatrixFromData(x, num_rows, num_columns, 50, tmpdir);
    for (auto num_bins : bin_sizes) {
      HistogramCuts cuts;
      DenseCuts dense(&cuts);
      dense.Build(dmat.get(), num_bins);
      ValidateCuts(cuts, x, num_rows, num_columns, num_bins);
    }
  }
}

TEST(hist_util, SparseCutsAccuracyTest) {
  int bin_sizes[] = {2, 16, 256, 512};
  int sizes[] = {100, 1000, 1500};
  int num_columns = 5;
  for (auto num_rows : sizes) {
    auto x = GenerateRandom(num_rows, num_columns);
    auto dmat = GetDMatrixFromData(x, num_rows, num_columns);
    for (auto num_bins : bin_sizes) {
      HistogramCuts cuts;
      SparseCuts sparse(&cuts);
      sparse.Build(dmat.get(), num_bins);
      ValidateCuts(cuts, x, num_rows, num_columns, num_bins);
    }
  }
}

TEST(hist_util, SparseCutsCategorical) {
  int categorical_sizes[] = {2, 6, 8, 12};
  int num_bins = 256;
  int sizes[] = {25, 100, 1000};
  for (auto n : sizes) {
    for (auto num_categories : categorical_sizes) {
      auto x = GenerateRandomCategoricalSingleColumn(n, num_categories);
      std::vector<float> x_sorted(x);
      std::sort(x_sorted.begin(), x_sorted.end());
      auto dmat = GetDMatrixFromData(x, n, 1);
      HistogramCuts cuts;
      SparseCuts sparse(&cuts);
      sparse.Build(dmat.get(), num_bins);
      auto cuts_from_sketch = cuts.Values();
      EXPECT_LT(cuts.MinValues()[0], x_sorted.front());
      EXPECT_GT(cuts_from_sketch.front(), x_sorted.front());
      EXPECT_GE(cuts_from_sketch.back(), x_sorted.back());
      EXPECT_EQ(cuts_from_sketch.size(), num_categories);
    }
  }
}

TEST(hist_util, SparseCutsExternalMemory) {
  int bin_sizes[] = {2, 16, 256, 512};
  int sizes[] = {100, 1000, 1500};
  int num_columns = 5;
  for (auto num_rows : sizes) {
    auto x = GenerateRandom(num_rows, num_columns);
    dmlc::TemporaryDirectory tmpdir;
    auto dmat =
        GetExternalMemoryDMatrixFromData(x, num_rows, num_columns, 50, tmpdir);
    for (auto num_bins : bin_sizes) {
      HistogramCuts cuts;
      SparseCuts dense(&cuts);
      dense.Build(dmat.get(), num_bins);
      ValidateCuts(cuts, x, num_rows, num_columns, num_bins);
    }
  }
}
}  // namespace common
}  // namespace xgboost

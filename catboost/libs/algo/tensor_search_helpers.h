#pragma once

#include "split.h"
#include "rand_score.h"
#include "fold.h"
#include "calc_score_cache.h"
#include "error_functions.h"
#include "yetirank_helpers.h"
#include "approx_calcer.h"

#include <catboost/libs/options/enums.h>

#include <library/binsaver/bin_saver.h>
#include <library/threading/local_executor/local_executor.h>

#include <util/generic/vector.h>


struct TCandidateInfo {
    TSplitCandidate SplitCandidate;
    TRandomScore BestScore;
    int BestBinBorderId = -1;
    bool ShouldDropAfterScoreCalc = false;
    SAVELOAD(SplitCandidate, BestScore, BestBinBorderId, ShouldDropAfterScoreCalc);
};

struct TCandidatesInfoList {
    TCandidatesInfoList() = default;
    explicit TCandidatesInfoList(const TCandidateInfo& oneCandidate) {
        Candidates.emplace_back(oneCandidate);
    }
    // All candidates here are either float or one-hot, or have the same
    // projection.
    // TODO(annaveronika): put projection out, because currently it's not clear.
    TVector<TCandidateInfo> Candidates;
    bool ShouldDropCtrAfterCalc = false;

    SAVELOAD(Candidates, ShouldDropCtrAfterCalc);
};

using TCandidateList = TVector<TCandidatesInfoList>;

void Bootstrap(const NCatboostOptions::TCatBoostOptions& params,
               const TVector<TIndexType>& indices,
               TFold* fold,
               TCalcScoreFold* sampledDocs,
               NPar::TLocalExecutor* localExecutor,
               TRestorableFastRng64* rand);

template <typename TError>
TError BuildError(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&) {
    return TError(IsStoreExpApprox(params.LossFunctionDescription->GetLossFunction()));
}
template <>
TCustomError BuildError<TCustomError>(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&);
template <>
TUserDefinedPerObjectError BuildError<TUserDefinedPerObjectError>(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&);
template <>
TUserDefinedQuerywiseError BuildError<TUserDefinedQuerywiseError>(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&);
template <>
TLogLinQuantileError BuildError<TLogLinQuantileError>(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&);
template <>
TQuantileError BuildError<TQuantileError>(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&);
template <>
TLqError BuildError<TLqError>(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&);
template <>
TQuerySoftMaxError BuildError<TQuerySoftMaxError>(const NCatboostOptions::TCatBoostOptions& params, const TMaybe<TCustomObjectiveDescriptor>&);

template <typename TError>
inline void CalcWeightedDerivatives(
    const TError& error,
    int bodyTailIdx,
    const NCatboostOptions::TCatBoostOptions& params,
    ui64 randomSeed,
    TFold* takenFold,
    NPar::TLocalExecutor* localExecutor
) {
    TFold::TBodyTail& bt = takenFold->BodyTailArr[bodyTailIdx];
    const TVector<TVector<double>>& approx = bt.Approx;
    const TVector<float>& target = takenFold->LearnTarget;
    const TVector<float>& weight = takenFold->GetLearnWeights();
    TVector<TVector<double>>* weightedDerivatives = &bt.WeightedDerivatives;

    if (error.GetErrorType() == EErrorType::QuerywiseError || error.GetErrorType() == EErrorType::PairwiseError) {
        TVector<TQueryInfo> recalculatedQueriesInfo;
        const bool shouldGenerateYetiRankPairs = ShouldGenerateYetiRankPairs(params.LossFunctionDescription->GetLossFunction());
        if (shouldGenerateYetiRankPairs) {
            YetiRankRecalculation(*takenFold, bt, params, randomSeed, localExecutor, &recalculatedQueriesInfo, &bt.PairwiseWeights);
        }
        const TVector<TQueryInfo>& queriesInfo = shouldGenerateYetiRankPairs ? recalculatedQueriesInfo : takenFold->LearnQueriesInfo;

        const int tailQueryFinish = bt.TailQueryFinish;
        TVector<TDers> ders((*weightedDerivatives)[0].ysize());
        error.CalcDersForQueries(0, tailQueryFinish, approx[0], target, weight, queriesInfo, &ders, localExecutor);
        for (int docId = 0; docId < ders.ysize(); ++docId) {
            (*weightedDerivatives)[0][docId] = ders[docId].Der1;
        }
        if (params.LossFunctionDescription->GetLossFunction() == ELossFunction::YetiRankPairwise) {
            // In case of YetiRankPairwise loss function we need to store generated pairs for tree structure building.
            Y_ASSERT(takenFold->BodyTailArr.size() == 1);
            takenFold->LearnQueriesInfo.swap(recalculatedQueriesInfo);
        }
    } else {
        const int tailFinish = bt.TailFinish;
        const int approxDimension = approx.ysize();
        NPar::TLocalExecutor::TExecRangeParams blockParams(0, tailFinish);
        blockParams.SetBlockSize(1000);

        Y_ASSERT(error.GetErrorType() == EErrorType::PerObjectError);
        if (approxDimension == 1) {
            localExecutor->ExecRange([&](int blockId) {
                const int blockOffset = blockId * blockParams.GetBlockSize();
                error.CalcFirstDerRange(blockOffset, Min<int>(blockParams.GetBlockSize(), tailFinish - blockOffset),
                    approx[0].data(),
                    nullptr, // no approx deltas
                    target.data(),
                    weight.data(),
                    (*weightedDerivatives)[0].data());
            }, 0, blockParams.GetBlockCount(), NPar::TLocalExecutor::WAIT_COMPLETE);
        } else {
            localExecutor->ExecRange([&](int blockId) {
                TVector<double> curApprox(approxDimension);
                TVector<double> curDelta(approxDimension);
                NPar::TLocalExecutor::BlockedLoopBody(blockParams, [&](int z) {
                    for (int dim = 0; dim < approxDimension; ++dim) {
                        curApprox[dim] = approx[dim][z];
                    }
                    error.CalcDersMulti(curApprox, target[z], weight.empty() ? 1 : weight[z], &curDelta, nullptr);
                    for (int dim = 0; dim < approxDimension; ++dim) {
                        (*weightedDerivatives)[dim][z] = curDelta[dim];
                    }
                })(blockId);
            }, 0, blockParams.GetBlockCount(), NPar::TLocalExecutor::WAIT_COMPLETE);
        }
    }
}

template <bool StoreExpApprox>
inline void UpdateBodyTailApprox(const TVector<TVector<TVector<double>>>& approxDelta,
    double learningRate,
    NPar::TLocalExecutor* localExecutor,
    TFold* fold
) {
    const auto applyLearningRate = [=](TConstArrayRef<double> delta, TArrayRef<double> approx, size_t idx) {
        approx[idx] = UpdateApprox<StoreExpApprox>(
            approx[idx],
            ApplyLearningRate<StoreExpApprox>(delta[idx], learningRate)
        );
    };
    for (int bodyTailId = 0; bodyTailId < fold->BodyTailArr.ysize(); ++bodyTailId) {
        TFold::TBodyTail& bt = fold->BodyTailArr[bodyTailId];
        UpdateApprox(applyLearningRate, approxDelta[bodyTailId], &bt.Approx, localExecutor);
    }
}

void SetBestScore(ui64 randSeed, const TVector<TVector<double>>& allScores, double scoreStDev, TVector<TCandidateInfo>* subcandidates);

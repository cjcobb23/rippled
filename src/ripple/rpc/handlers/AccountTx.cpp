//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/json/json_value.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/UintTypes.h>
#include <ripple/protocol/jss.h>
#include <ripple/resource/Fees.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/Role.h>
#include <ripple/rpc/impl/RPCHelpers.h>

namespace ripple {

using LedgerSequence = uint32_t;
using LedgerHash = uint256;
using LedgerShortcut = RPC::LedgerShortcut;

using AccountTxMarker = NetworkOPs::AccountTxMarker;

struct LedgerRange
{
    uint32_t min;

    uint32_t max;
};

using LedgerSpecifier =
    std::variant<LedgerRange, LedgerShortcut, LedgerSequence, LedgerHash>;

struct AccountTxArgs
{
    AccountID account;

    std::optional<LedgerSpecifier> ledger;

    bool binary = false;

    bool forward = false;

    uint32_t limit = 0;

    std::optional<AccountTxMarker> marker;
};

using TxnsData = NetworkOPs::AccountTxs;
using TxnsDataBinary = NetworkOPs::MetaTxsList;

struct AccountTxResult
{
    std::variant<TxnsData, TxnsDataBinary> transactions;

    LedgerRange ledgerRange;

    uint32_t limit;

    std::optional<AccountTxMarker> marker;
};

// parses args into a ledger specifier, or returns a grpc status object on error
std::variant<std::optional<LedgerSpecifier>, grpc::Status>
parseLedgerArgs(org::xrpl::rpc::v1::GetAccountTransactionHistoryRequest const& params)
{
    LedgerSpecifier ledger;
    grpc::Status status;
    if (params.has_ledger_range())
    {
        uint32_t min = params.ledger_range().ledger_index_min();
        uint32_t max = params.ledger_range().ledger_index_max();

        // if min is set but not max, need to set max
        if (min != 0 && max == 0)
        {
            max = UINT32_MAX;
        }

        return LedgerRange{min, max};
    }
    else if (params.has_ledger_specifier())
    {
        auto& specifier = params.ledger_specifier();
        using LedgerCase = org::xrpl::rpc::v1::LedgerSpecifier::LedgerCase;
        LedgerCase ledgerCase = specifier.ledger_case();

        if (ledgerCase == LedgerCase::kShortcut)
        {
            using LedgerSpecifier = org::xrpl::rpc::v1::LedgerSpecifier;
            if (specifier.shortcut() == LedgerSpecifier::SHORTCUT_VALIDATED)
            {
                ledger = LedgerShortcut::VALIDATED;
            }
            else if (specifier.shortcut() == LedgerSpecifier::SHORTCUT_CLOSED)
            {
                ledger = LedgerShortcut::CLOSED;
            }
            else if (specifier.shortcut() == LedgerSpecifier::SHORTCUT_CURRENT)
            {
                ledger = LedgerShortcut::CURRENT;
            }
            else
            {
                return {};
            }
        }
        else if (ledgerCase == LedgerCase::kSequence)
        {
            ledger = specifier.sequence();
        }
        else if (ledgerCase == LedgerCase::kHash)
        {
            if (uint256::size() != specifier.hash().size())
            {
                grpc::Status errorStatus{grpc::StatusCode::INVALID_ARGUMENT,
                                         "ledger hash malformed"};
                return errorStatus;
            }
            ledger = uint256::fromVoid(specifier.hash().data());
        }
        return ledger;
    }
    return {};
}

// parses args into a ledger specifier, or returns a Json object on error
std::variant<std::optional<LedgerSpecifier>, Json::Value>
parseLedgerArgs(Json::Value const& params)
{
    Json::Value response;
    if (params.isMember(jss::ledger_index_min) ||
        params.isMember(jss::ledger_index_max))
    {
        uint32_t min = params.isMember(jss::ledger_index_min) &&
                params[jss::ledger_index_min].asInt() >= 0
            ? params[jss::ledger_index_min].asUInt()
            : 0;
        uint32_t max = params.isMember(jss::ledger_index_max) &&
                params[jss::ledger_index_max].asInt() >= 0
            ? params[jss::ledger_index_max].asUInt()
            : UINT32_MAX;

        return LedgerRange{min, max};
    }
    else if (params.isMember(jss::ledger_hash))
    {
        auto& hashValue = params[jss::ledger_hash];
        if (!hashValue.isString())
        {
            RPC::Status status{rpcINVALID_PARAMS, "ledgerHashNotString"};
            status.inject(response);
            return response;
        }

        LedgerHash hash;
        if (!hash.SetHex(hashValue.asString()))
        {
            RPC::Status status{rpcINVALID_PARAMS, "ledgerHashMalformed"};
            status.inject(response);
            return response;
        }
        return hash;
    }
    else if (params.isMember(jss::ledger_index))
    {
        LedgerSpecifier ledger;
        if (params[jss::ledger_index].isNumeric())
            ledger = params[jss::ledger_index].asInt();
        else
        {
            std::string ledgerStr = params[jss::ledger_index].asString();
            if (ledgerStr == "current" || ledgerStr.empty())
            {
                ledger = LedgerShortcut::CURRENT;
            }
            else if (ledgerStr == "closed")
            {
                ledger = LedgerShortcut::CLOSED;
            }
            else if (ledgerStr == "validated")
            {
                ledger = LedgerShortcut::VALIDATED;
            }
            else
            {
                RPC::Status status{rpcINVALID_PARAMS,
                                   "ledger_index string malformed"};
                status.inject(response);
                return response;
            }
        }
        return ledger;
    }
    return {};
}

void
populateTransactionData(
    RPC::Context& context,
    std::shared_ptr<Transaction> const& txn,
    TxMeta::pointer const& txnMeta,
    org::xrpl::rpc::v1::GetTransactionResponse* txnProto)
{
    assert(txnProto);
    if (txn)
        RPC::populateTransaction(
            *txnProto->mutable_transaction(), txn->getSTransaction());

    if (txnMeta)
        RPC::populateMeta(*txnProto->mutable_meta(), txnMeta);

    if (txnMeta && !txnMeta->hasDeliveredAmount())
    {
        std::optional<STAmount> amount = getDeliveredAmount(
            context, txn->getSTransaction(), *txnMeta, [&txn]() {
                return txn->getLedger();
            });
        if (amount)
        {
            txnMeta->setDeliveredAmount(*amount);
        }
    }

    // account_tx always returns validated data
    txnProto->set_validated(true);
    txnProto->set_ledger_index(txn->getLedger());
    auto& hash = txn->getID();
    txnProto->set_hash(hash.data(), hash.size());
    auto closeTime =
        context.app.getLedgerMaster().getCloseTimeBySeq(txn->getLedger());
    if (closeTime)
        txnProto->mutable_date()->set_value(
            closeTime->time_since_epoch().count());
}

void
populateTransactionDataBinary(
    RPC::Context& context,
    std::tuple<Blob, Blob, std::uint32_t> const& binaryData,
    org::xrpl::rpc::v1::GetTransactionResponse* txnProto)
{
    assert(txnProto);
    Blob const& txnBlob = std::get<0>(binaryData);
    txnProto->set_transaction_binary(txnBlob.data(), txnBlob.size());

    Blob const& metaBlob = std::get<1>(binaryData);
    txnProto->set_meta_binary(metaBlob.data(), metaBlob.size());

    txnProto->set_ledger_index(std::get<2>(binaryData));

    // account_tx always returns validated data
    txnProto->set_validated(true);

    auto closeTime = context.app.getLedgerMaster().getCloseTimeBySeq(
        std::get<2>(binaryData));
    if (closeTime)
        txnProto->mutable_date()->set_value(
            closeTime->time_since_epoch().count());
}

std::variant<LedgerRange, RPC::Status>
getLedgerRange(
    RPC::Context& context,
    std::optional<std::variant<
        LedgerRange,
        LedgerShortcut,
        LedgerSequence,
        LedgerHash>> const& ledgerSpecifier)
{
    std::uint32_t uValidatedMin;
    std::uint32_t uValidatedMax;
    bool bValidated =
        context.ledgerMaster.getValidatedRange(uValidatedMin, uValidatedMax);

    if (!bValidated)
    {
        // Don't have a validated ledger range.
        return rpcLGR_IDXS_INVALID;
    }

    std::uint32_t uLedgerMin = uValidatedMin;
    std::uint32_t uLedgerMax = uValidatedMax;
    // Does request specify a ledger or ledger range?
    if (ledgerSpecifier)
    {
        if (std::holds_alternative<LedgerRange>(*ledgerSpecifier))
        {
            auto& ledgerRange = std::get<LedgerRange>(*ledgerSpecifier);
            if (ledgerRange.min > uValidatedMin)
            {
                uLedgerMin = ledgerRange.min;
            }
            if (ledgerRange.max < uValidatedMax)
            {
                uLedgerMax = ledgerRange.max;
            }
            if (uLedgerMax < uLedgerMin)
                return rpcLGR_IDXS_INVALID;
        }
        else
        {
            std::shared_ptr<ReadView const> ledgerView;
            RPC::Status status{RPC::Status::OK};
            if (std::holds_alternative<LedgerSequence>(*ledgerSpecifier))
            {
                status = getLedger(
                    ledgerView,
                    std::get<LedgerSequence>(*ledgerSpecifier),
                    context);
            }
            else if (std::holds_alternative<LedgerHash>(*ledgerSpecifier))
            {
                status = getLedger(
                    ledgerView,
                    std::get<LedgerHash>(*ledgerSpecifier),
                    context);
            }
            else
            {
                assert(
                    std::holds_alternative<LedgerShortcut>(*ledgerSpecifier));
                status = getLedger(
                    ledgerView,
                    std::get<LedgerShortcut>(*ledgerSpecifier),
                    context);
            }
            if (!ledgerView)
            {
                return status;
            }

            bool validated = RPC::isValidated(
                context.ledgerMaster, *ledgerView, context.app);

            if (!validated || ledgerView->info().seq > uValidatedMax ||
                ledgerView->info().seq < uValidatedMin)
            {
                return rpcLGR_NOT_VALIDATED;
            }
            uLedgerMin = uLedgerMax = ledgerView->info().seq;
        }
    }
    return LedgerRange{uLedgerMin, uLedgerMax};
}

std::pair<AccountTxResult, RPC::Status>
doAccountTxHelp(RPC::Context& context, AccountTxArgs& args)
{
    AccountTxResult result;

    std::uint32_t uValidatedMin;
    std::uint32_t uValidatedMax;
    bool bValidated =
        context.ledgerMaster.getValidatedRange(uValidatedMin, uValidatedMax);

    if (!bValidated)
    {
        // Don't have a validated ledger range.
        return {result, rpcLGR_IDXS_INVALID};
    }

    auto lgrRange = getLedgerRange(context, args.ledger);
    if (std::holds_alternative<RPC::Status>(lgrRange))
    {
        // An error occurred getting the requested ledger range
        return {result, std::get<RPC::Status>(lgrRange)};
    }

    result.ledgerRange = std::get<LedgerRange>(lgrRange);

    context.loadType = Resource::feeMediumBurdenRPC;

    if (args.binary)
    {
        result.transactions = std::move(context.netOps.getTxsAccountB(
            args.account,
            result.ledgerRange.min,
            result.ledgerRange.max,
            args.forward,
            args.marker,
            args.limit,
            isUnlimited(context.role)));
    }
    else
    {
        result.transactions = std::move(context.netOps.getTxsAccount(
            args.account,
            result.ledgerRange.min,
            result.ledgerRange.max,
            args.forward,
            args.marker,
            args.limit,
            isUnlimited(context.role)));
    }

    result.limit = args.limit;
    if (args.marker)
        result.marker = args.marker;

    return {result, rpcSUCCESS};
}

void
populateResponse(
    std::pair<AccountTxResult, RPC::Status> const& res,
    AccountTxArgs const& args,
    auto const& handleErr,
    auto const& fillTopLevelResultData,
    auto const& fillTxnData,
    auto const& fillBinaryTxnData,
    auto const& fillMarker)
{
    if (res.second.toErrorCode() != rpcSUCCESS)
    {
        handleErr(res.second);
    }
    else
    {
        fillTopLevelResultData(res.first);

        if (std::holds_alternative<TxnsData>(res.first.transactions))
        {
            assert(!args.binary);
            for (auto const& [txn, txnMeta] :
                 std::get<TxnsData>(res.first.transactions))
            {
                fillTxnData(txn, txnMeta);
            }
        }
        else
        {
            assert(args.binary);

            for (auto const& txn :
                 std::get<TxnsDataBinary>(res.first.transactions))
            {
                fillBinaryTxnData(txn);
            }
        }

        if (res.first.marker)
        {
            fillMarker(*res.first.marker);
        }

    }
}

// {
//   account: account,
//   ledger_index_min: ledger_index  // optional, defaults to earliest
//   ledger_index_max: ledger_index, // optional, defaults to latest
//   binary: boolean,                // optional, defaults to false
//   forward: boolean,               // optional, defaults to false
//   limit: integer,                 // optional
//   marker: object {ledger: ledger_index, seq: txn_sequence} // optional,
//   resume previous query
// }
Json::Value
doAccountTxJson(RPC::JsonContext& context)
{
    auto& params = context.params;
    AccountTxArgs args;
    Json::Value response;

    args.limit = params.isMember(jss::limit) ? params[jss::limit].asUInt() : 0;
    args.binary = params.isMember(jss::binary) && params[jss::binary].asBool();
    args.forward =
        params.isMember(jss::forward) && params[jss::forward].asBool();

    if (!params.isMember(jss::account))
        return rpcError(rpcINVALID_PARAMS);

    auto const account =
        parseBase58<AccountID>(params[jss::account].asString());
    if (!account)
        return rpcError(rpcACT_MALFORMED);

    args.account = *account;

    auto parseRes = parseLedgerArgs(params);
    if (std::holds_alternative<Json::Value>(parseRes))
    {
        return std::get<Json::Value>(parseRes);
    }
    else
    {
        args.ledger = std::get<std::optional<LedgerSpecifier>>(parseRes);
    }

    if (params.isMember(jss::marker))
    {
        auto& token = params[jss::marker];
        if (!token.isMember(jss::ledger) || !token.isMember(jss::seq) ||
            !token[jss::ledger].isConvertibleTo(Json::ValueType::uintValue) ||
            !token[jss::seq].isConvertibleTo(Json::ValueType::uintValue))
        {
            RPC::Status status{
                rpcINVALID_PARAMS,
                "invalid marker. Provide ledger index via ledger field, and "
                "transaction sequence number via seq field"};
            status.inject(response);
            return response;
        }
        args.marker = {token[jss::ledger].asUInt(), token[jss::seq].asUInt()};
    }

    auto res = doAccountTxHelp(context, args);

    Json::Value& jvTxns = (response[jss::transactions] = Json::arrayValue);

    auto handleErr = [&response](auto const& error) { const_cast<RPC::Status&>(error).inject(response); };

    auto fillTopLevelResultData = [&response, &params](auto const& result) {
        response[jss::validated] = true;
        response[jss::limit] = result.limit;
        response[jss::account] = std::move(params[jss::account].asString());
        response[jss::ledger_index_min] = result.ledgerRange.min;
        response[jss::ledger_index_max] = result.ledgerRange.max;
    };

    auto fillTxnData = [&jvTxns, &context](
                           auto const& txn, auto const& txnMeta) {
        Json::Value& jvObj = jvTxns.append(Json::objectValue);

        if (txn)
            jvObj[jss::tx] = txn->getJson(JsonOptions::include_date);

        if (txnMeta)
        {
            auto metaJ = txnMeta->getJson(JsonOptions::include_date);
            jvObj[jss::meta] = std::move(metaJ);
            jvObj[jss::validated] = true;

            insertDeliveredAmount(jvObj[jss::meta], context, txn, *txnMeta);
        }
    };

    auto fillBinaryTxnData = [&jvTxns](auto const& txn) {
        Json::Value& jvObj = jvTxns.append(Json::objectValue);

        jvObj[jss::tx_blob] = strHex(std::get<0>(txn));
        jvObj[jss::meta] = strHex(std::get<1>(txn));
        jvObj[jss::ledger_index] = std::get<2>(txn);
        jvObj[jss::validated] = true;
    };

    auto fillMarker = [&response](auto const& marker) {
        response[jss::marker] = Json::objectValue;
        response[jss::marker][jss::ledger] = marker.ledgerSeq;
        response[jss::marker][jss::seq] = marker.txnSeq;
    };

    populateResponse(
        res,
        args,
        handleErr,
        fillTopLevelResultData,
        fillTxnData,
        fillBinaryTxnData,
        fillMarker);

    return response;
}

std::pair<org::xrpl::rpc::v1::GetAccountTransactionHistoryResponse, grpc::Status>
doAccountTxGrpc(
    RPC::GRPCContext<org::xrpl::rpc::v1::GetAccountTransactionHistoryRequest>& context)
{
    // return values
    org::xrpl::rpc::v1::GetAccountTransactionHistoryResponse response;
    grpc::Status status = grpc::Status::OK;
    AccountTxArgs args;

    auto& request = context.params;

    auto const account = parseBase58<AccountID>(request.account().address());
    if (!account)
    {
        return {
            {},
            {grpc::StatusCode::INVALID_ARGUMENT, "Could not decode account"}};
    }

    args.account = *account;
    args.limit = request.limit();
    args.binary = request.binary();
    args.forward = request.forward();

    if (request.has_marker())
    {
        args.marker = {request.marker().ledger_index(),
                       request.marker().account_sequence()};
    }

    auto parseRes = parseLedgerArgs(request);
    if (std::holds_alternative<grpc::Status>(parseRes))
    {
        return {response, std::get<grpc::Status>(parseRes)};
    }
    else
    {
        args.ledger = std::get<std::optional<LedgerSpecifier>>(parseRes);
    }

    auto res = doAccountTxHelp(context, args);

    auto handleErr = [&status](auto const& error) {
        if (error.toErrorCode() == rpcLGR_NOT_FOUND)
        {
            status = {grpc::StatusCode::NOT_FOUND, error.message()};
        }
        else
        {
            status = {grpc::StatusCode::INVALID_ARGUMENT, error.message()};
        }
    };

    auto fillTopLevelResultData = [&response, &request](auto const& result) {
        // account_tx always returns validated data
        response.set_validated(true);
        response.set_limit(result.limit);
        response.mutable_account()->set_address(
            std::move(request.account().address()));
        response.set_ledger_index_min(result.ledgerRange.min);
        response.set_ledger_index_max(result.ledgerRange.max);
    };

    auto fillTxnData = [&context, &response](
                           auto const& txn, auto const& txnMeta) {
        auto txnProto = response.add_transactions();
        populateTransactionData(context, txn, txnMeta, txnProto);
    };

    auto fillBinaryTxnData = [&context, &response](auto const& txn) {
        auto txnProto = response.add_transactions();
        populateTransactionDataBinary(context, txn, txnProto);
    };

    auto fillMarker = [&response](auto const& marker) {
        response.mutable_marker()->set_ledger_index(
            marker.ledgerSeq);
        response.mutable_marker()->set_account_sequence(
            marker.txnSeq);
    };

    populateResponse(
        res,
        args,
        handleErr,
        fillTopLevelResultData,
        fillTxnData,
        fillBinaryTxnData,
        fillMarker);

    return {response, status};
}

}  // namespace ripple

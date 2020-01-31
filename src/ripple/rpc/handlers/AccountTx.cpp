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
#include <ripple/protocol/jss.h>
#include <ripple/protocol/UintTypes.h>
#include <ripple/resource/Fees.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/Role.h>

namespace ripple {

//Ledger sequence, Account sequence
//TODO this type needs to be defined somewhere else,
//to be used by NetworkOps and AccountTxPaging and here
using Marker = std::pair<uint32_t, uint32_t>;
using LedgerSequence = uint32_t;
using LedgerHash = uint256;
using LedgerShortcut = RPC::LedgerShortcut;
//TODO change this to an enum



struct LedgerRange {

    uint32_t min;

    uint32_t max;
};

struct AccountTxArgs
{
    AccountID account;

    std::optional<
        std::variant<LedgerRange, RPC::LedgerShortcut, LedgerSequence, LedgerHash>>
        ledger;

    bool binary = false;

    bool forward = false;

    uint32_t limit = 0;

    std::optional<Marker> marker;
};

using TxnData = NetworkOPs::AccountTx;
using TxnDataBinary = NetworkOPs::txnMetaLedgerType;
using TxnsData = std::vector<std::pair<TxnData,bool>>;
using TxnsDataBinary = std::vector<std::pair<TxnDataBinary, bool>>;

struct AccountTxResult
{
    // bool indicates whether txn is validated
    std::variant<
        TxnsData,
        TxnsDataBinary>
        transactions;

    LedgerRange ledgerRange;

    uint32_t limit;

    std::optional<Marker> marker;

    bool validated;

    AccountID account;
};

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

    result.validated = true;

    std::uint32_t uLedgerMin = uValidatedMin;
    std::uint32_t uLedgerMax = uValidatedMax;

    context.loadType = Resource::feeMediumBurdenRPC;

    // Does request specify a ledger or ledger range?
    if(args.ledger)
    {

        if (std::holds_alternative<LedgerRange>(*args.ledger))
        {
            auto& ledgerRange = std::get<LedgerRange>(*args.ledger);
            if(ledgerRange.min > uValidatedMin)
            {
                uLedgerMin = ledgerRange.min;
            }
            if(ledgerRange.max < uValidatedMax)
            {
                uLedgerMax = ledgerRange.max;
            }
            if (uLedgerMax < uLedgerMin)
                return {result, rpcLGR_IDXS_INVALID};
        }
        else
        {
            std::shared_ptr<ReadView const> ledger;
            RPC::Status status{RPC::Status::OK};
            if (std::holds_alternative<LedgerSequence>(*args.ledger))
            {
                status = getLedger(
                        ledger, std::get<LedgerSequence>(*args.ledger), context);
            }
            else if (std::holds_alternative<LedgerHash>(*args.ledger))
            {
                status =
                    getLedger(ledger, std::get<LedgerHash>(*args.ledger), context);
            }
            else
            {
                assert(std::holds_alternative<LedgerShortcut>(*args.ledger));
                status = getLedger(
                        ledger, std::get<LedgerShortcut>(*args.ledger), context);
            }
            if (!ledger)
            {
                return {result, status};
            }

            result.validated =
                RPC::isValidated(context.ledgerMaster, *ledger, context.app);

            // Note, if result.validated is false, we return an error
            // Therefore, in a successful response, validated can only be true
            if (!result.validated || ledger->info().seq > uValidatedMax ||
                    ledger->info().seq < uValidatedMin)
            {
                return {result, rpcLGR_NOT_VALIDATED};
            }
            uLedgerMin = uLedgerMax = ledger->info().seq;
        }
    }

    if (!args.marker)
    {
        args.marker = {0, 0};
    }

    if (args.binary)
    {

        auto txns = context.netOps.getTxsAccountB(
                args.account,
                uLedgerMin,
                uLedgerMax,
                args.forward,
                *args.marker,
                args.limit,
                isUnlimited(context.role));
        result.transactions = TxnsDataBinary{};

        auto& txnsData = std::get<TxnsDataBinary>(result.transactions);
        for (auto& it : txns)
        {
            std::uint32_t uLedgerIndex = std::get<2>(it);

            bool validated = bValidated && uValidatedMin <= uLedgerIndex &&
                uValidatedMax >= uLedgerIndex;

            txnsData.emplace_back(
                    it,
                    validated);
        }
    }
    else
    {

        auto txns = context.netOps.getTxsAccount(
                args.account,
                uLedgerMin,
                uLedgerMax,
                args.forward,
                *args.marker,
                args.limit,
                isUnlimited(context.role));
        result.transactions = TxnsData{};
        auto& txnsData = std::get<TxnsData>(result.transactions);

        for (auto const& [txn, txMeta] : txns)
        {
            if (!txMeta->hasDeliveredAmount())
            {
                std::optional<STAmount> amount =
                    getDeliveredAmount(context, txn, *txMeta);
                if (amount)
                {
                    txMeta->setDeliveredAmount(*amount);
                }
            }

            std::uint32_t uLedgerIndex = txMeta->getLgrSeq();
            bool validated = bValidated && uValidatedMin <= uLedgerIndex &&
                uValidatedMax >= uLedgerIndex;
            txnsData.emplace_back(std::make_pair(txn, txMeta), validated);
        }
    }


    result.ledgerRange = {uLedgerMin,uLedgerMax};
    result.limit = args.limit;
    if(args.marker->first != 0 || args.marker->second != 0)
        result.marker = args.marker;

    result.account = args.account;

    return {result, rpcSUCCESS};
}

std::pair<rpc::v1::GetAccountTransactionHistoryResponse, grpc::Status>
doAccountTxGrpc(
    RPC::GRPCContext<rpc::v1::GetAccountTransactionHistoryRequest>& context)
{
    // return values
    rpc::v1::GetAccountTransactionHistoryResponse response;
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

    if(request.has_marker())
    {
    args.marker = {request.marker().ledger_index(),
                   request.marker().account_sequence()};
    }

    if(request.has_ledger_range())
    {
        uint32_t min = request.ledger_range().ledger_index_min();
        uint32_t max = request.ledger_range().ledger_index_max();

        // if min is set but not max, need to set max
        if(min != 0 && max == 0)
        {
            max = UINT32_MAX;
        }

        // {0,0} is the default value. If both 0, default to entire validated
        // range
        // TODO is this reasonable behavior?
        if (min != 0 || max != 0)
        {
            args.ledger = LedgerRange{min, max};
        }
    }
    else if(request.has_ledger_specifier())
    {
        auto& specifier = request.ledger_specifier();
        using LedgerCase = rpc::v1::LedgerSpecifier::LedgerCase;
        LedgerCase ledgerCase = specifier.ledger_case();

        if(ledgerCase == LedgerCase::kShortcut)
        {
            using LedgerSpecifier = rpc::v1::LedgerSpecifier;
            if(specifier.shortcut() == LedgerSpecifier::SHORTCUT_VALIDATED)
            {
                args.ledger = LedgerShortcut::VALIDATED;
            } else if(specifier.shortcut() == LedgerSpecifier::SHORTCUT_CLOSED)
            {
                args.ledger = LedgerShortcut::CLOSED;
            } else if(specifier.shortcut() == LedgerSpecifier::SHORTCUT_CURRENT)
            {
                args.ledger = LedgerShortcut::CURRENT;
            }
        }
        else if(ledgerCase == LedgerCase::kSequence)
        {
            args.ledger = specifier.sequence();
        }
        else if(ledgerCase == LedgerCase::kHash)
        {
            if (uint256::size() != specifier.hash().size())
            {
                grpc::Status errorStatus{
                    grpc::StatusCode::INVALID_ARGUMENT, "ledger hash malformed"};
                return {response, errorStatus};
            }
            args.ledger = uint256::fromVoid(specifier.hash().data());
        }

    }

    auto res = doAccountTxHelp(context, args);


    if(res.second.toErrorCode() != rpcSUCCESS)
    {
        if(res.second.toErrorCode() == rpcLGR_NOT_FOUND)
        {
            return {{}, {grpc::StatusCode::NOT_FOUND, res.second.message()}};
        }
        return {{}, {grpc::StatusCode::INVALID_ARGUMENT, res.second.message()}};
    }

    if (std::holds_alternative<TxnsData>(res.first.transactions))
    {
        assert(!args.binary);
        for (auto const& [txn, validated] :
             std::get<TxnsData>(res.first.transactions))
        {
            auto txnProto = response.add_transactions();
            auto const& [txnBasic, txnMeta] = txn;

            if (txnBasic)
                RPC::populateTransaction(
                    *txnProto->mutable_transaction(), txnBasic->getSTransaction());

            if (txnMeta)
                RPC::populateMeta(*txnProto->mutable_meta(), txnMeta);

            txnProto->set_validated(validated);
            txnProto->set_ledger_index(txnBasic->getLedger());
            auto& hash = txnBasic->getID();
            txnProto->set_hash(hash.data(), hash.size());
            auto ct = context.app.getLedgerMaster().getCloseTimeBySeq(txnBasic->getLedger());
            if(ct)
                txnProto->mutable_date()->set_value(ct->time_since_epoch().count());
        }
    }
    else
    {
        assert(args.binary);

        for (auto const& [txn, validated] :
             std::get<TxnsDataBinary>(res.first.transactions))
        {
            auto txnProto = response.add_transactions();
            txnProto->set_transaction_binary(std::get<0>(txn));

            txnProto->set_meta_binary(std::get<1>(txn));
            txnProto->set_ledger_index(std::get<2>(txn));
            txnProto->set_validated(validated);
            auto ct = context.app.getLedgerMaster().getCloseTimeBySeq(std::get<2>(txn));
            if(ct)
                txnProto->mutable_date()->set_value(ct->time_since_epoch().count());

        }
           // Json::Value& jvObj = jvTxns.append(Json::objectValue);

           // jvObj[jss::tx_blob] = std::get<0>(txn);
           // jvObj[jss::meta] = std::get<1>(txn);
           // jvObj[jss::ledger_index] = std::get<2>(txn);
           // jvObj[jss::validated] = validated;
    }

//    if (res.first.marker)
//    {
//        response[jss::marker] = Json::objectValue;
//        response[jss::marker][jss::ledger] = res.first.marker->first;
//        response[jss::marker][jss::seq] = res.first.marker->second;
//    }
//
//    response[jss::validated] = res.first.validated;
//    response[jss::limit] = res.first.limit;
//    response[jss::account] = params[jss::account].asString();
//    response[jss::ledger_index_min] = res.first.ledgerRange.min;
//    response[jss::ledger_index_max] = res.first.ledgerRange.max;
//

    response.set_validated(res.first.validated);
    response.set_limit(res.first.limit);
    response.mutable_account()->set_address(request.account().address());
    response.set_ledger_index_min(res.first.ledgerRange.min);
    response.set_ledger_index_max(res.first.ledgerRange.max);

    if(res.first.marker)
    {
        response.mutable_marker()->set_ledger_index(res.first.marker->first);
        response.mutable_marker()->set_account_sequence(res.first.marker->second);
    }


    std::cout << "handler success" << std::endl;

    return {response, grpc::Status::OK};
}

// {
//   account: account,
//   ledger_index_min: ledger_index  // optional, defaults to earliest
//   ledger_index_max: ledger_index, // optional, defaults to latest
//   binary: boolean,                // optional, defaults to false
//   forward: boolean,               // optional, defaults to false
//   limit: integer,                 // optional
//   marker: opaque                  // optional, resume previous query
// }
Json::Value doAccountTxJson (RPC::JsonContext& context)
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

        args.ledger = LedgerRange{min, max};
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

        args.ledger = LedgerHash{};
        auto& hash = std::get<LedgerHash>(*args.ledger);
        if (!hash.SetHex(hashValue.asString()))
        {
            RPC::Status status{rpcINVALID_PARAMS, "ledgerHashMalformed"};
            status.inject(response);
            return response;
        }
    }
    else if (params.isMember(jss::ledger_index))
    {
        if (params[jss::ledger_index].isNumeric())
            args.ledger = params[jss::ledger_index].asInt();
        else
        {
            std::string ledgerStr = params[jss::ledger_index].asString();
            if(ledgerStr == "current" || ledgerStr.empty())
            {
                args.ledger = LedgerShortcut::CURRENT;
            }
            else if(ledgerStr == "closed")
            {
                args.ledger = LedgerShortcut::CLOSED;
            } else if(ledgerStr == "validated")
            {
                args.ledger = LedgerShortcut::VALIDATED;
            }
            else
            {
                RPC::Status status{rpcINVALID_PARAMS, "ledger_index string malformed"};
                status.inject(response);
                return response;
            }
        }
    }

    if (params.isMember(jss::marker))
    {
        auto& token = params[jss::marker];
        if (!token.isMember(jss::ledger) || !token.isMember(jss::seq))// ||
//            !token[jss::ledger].isUInt() || !token[jss::seq].isConvertibleTo())
        {
            RPC::Status status{
                rpcINVALID_PARAMS,
                "invalid marker. Provide ledger index via ledger field, and "
                "account sequence number via seq field"};
            status.inject(response);
            return response;
        }
        args.marker = std::make_pair(
            token[jss::ledger].asUInt(), token[jss::seq].asUInt());
    }

    auto res = doAccountTxHelp(context, args);

    if (res.second.toErrorCode() != rpcSUCCESS)
    {
        res.second.inject(response);
        return response;
    }

    Json::Value& jvTxns = (response[jss::transactions] = Json::arrayValue);

    if (std::holds_alternative<TxnsData>(res.first.transactions))
    {
        assert(!args.binary);
        for (auto const& [txn, validated] :
             std::get<TxnsData>(res.first.transactions))
        {
            Json::Value& jvObj = jvTxns.append(Json::objectValue);

            auto const& [txnBasic, txnMeta] = txn;

            if (txnBasic)
                jvObj[jss::tx] = txnBasic->getJson(JsonOptions::include_date);

            if (txnMeta)
            {
                auto metaJ = txnMeta->getJson(JsonOptions::include_date);
                jvObj[jss::meta] = std::move(metaJ);
                jvObj[jss::validated] = validated;
                if(txnMeta->hasDeliveredAmount() && txnMeta->getDeliveredAmount().isDefault())
                {
                    //When the delivered amount is set to the default value of STAmount,
                    //return unavailable
                    jvObj[jss::meta][jss::delivered_amount] = Json::Value("unavailable");
                }
            }
        }
    }
    else
    {
        assert(args.binary);

        for (auto const& [txn, validated] :
             std::get<TxnsDataBinary>(res.first.transactions))
        {
            Json::Value& jvObj = jvTxns.append(Json::objectValue);

            jvObj[jss::tx_blob] = std::get<0>(txn);
            jvObj[jss::meta] = std::get<1>(txn);
            jvObj[jss::ledger_index] = std::get<2>(txn);
            jvObj[jss::validated] = validated;
        }
    }

    if (res.first.marker)
    {
        response[jss::marker] = Json::objectValue;
        response[jss::marker][jss::ledger] = res.first.marker->first;
        response[jss::marker][jss::seq] = res.first.marker->second;
    }

    response[jss::validated] = res.first.validated;
    response[jss::limit] = res.first.limit;
    response[jss::account] = params[jss::account].asString();
    response[jss::ledger_index_min] = res.first.ledgerRange.min;
    response[jss::ledger_index_max] = res.first.ledgerRange.max;

    return response;
}

} // ripple

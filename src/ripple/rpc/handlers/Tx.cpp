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
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/GRPCHelpers.h>
#include <ripple/rpc/GRPCHandlers.h>

namespace ripple {

// {
//   transaction: <hex>
// }

static
bool
isHexTxID (std::string const& txid)
{
    if (txid.size () != 64)
        return false;

    auto const ret = std::find_if (txid.begin (), txid.end (),
        [](std::string::value_type c)
        {
            return !std::isxdigit (static_cast<unsigned char>(c));
        });

    return (ret == txid.end ());
}

static
bool
isValidated(LedgerMaster& ledgerMaster, std::uint32_t seq, uint256 const& hash)
{
    if (!ledgerMaster.haveLedger (seq))
        return false;

    if (seq > ledgerMaster.getValidatedLedger ()->info().seq)
        return false;

    return ledgerMaster.getHashBySeq (seq) == hash;
}

bool
getMetaHex (Ledger const& ledger,
    uint256 const& transID, std::string& hex)
{
    SHAMapTreeNode::TNType type;
    auto const item =
        ledger.txMap().peekItem (transID, type);

    if (!item)
        return false;

    if (type != SHAMapTreeNode::tnTRANSACTION_MD)
        return false;

    SerialIter it (item->slice());
    it.getVL (); // skip transaction
    hex = strHex (makeSlice(it.getVL ()));
    return true;
}

struct TxResult
{
    Transaction::pointer txn;

    std::variant<std::shared_ptr<TxMeta>, Blob> meta;

    bool validated = false;

    std::optional<bool> searchedAll;
};

struct TxArgs
{
    uint256 hash;

    bool binary = false;

    // TODO: change to pair? or named struct?
    std::optional<uint32_t> minLedger;

    std::optional<uint32_t> maxLedger;
};

std::pair<TxResult, RPC::Status>
doTxHelp(TxArgs& args, RPC::Context& context)
{
    TxResult result;

    ClosedInterval<uint32_t> range;

    auto rangeProvided = args.minLedger && args.maxLedger;

    if (rangeProvided)
    {
        constexpr uint16_t MAX_RANGE = 1000;

        if (*args.maxLedger < *args.minLedger)
            return {result, rpcINVALID_LGR_RANGE};

        if (*args.maxLedger - *args.minLedger > MAX_RANGE)
            return {result, rpcEXCESSIVE_LGR_RANGE};

        range = ClosedInterval<uint32_t>(*args.minLedger, *args.maxLedger);
    }

    std::shared_ptr<Transaction> txn;
    auto ec{rpcSUCCESS};

    if (rangeProvided)
    {
        boost::variant<std::shared_ptr<Transaction>, bool> v =
            context.app.getMasterTransaction().fetch(args.hash, range, ec);

        if (v.which() == 1)
        {
            result.searchedAll = boost::get<bool>(v);
            return {result, rpcTXN_NOT_FOUND};
        }
        else
        {
            txn = boost::get<std::shared_ptr<Transaction>>(v);
        }
    }
    else
    {
        txn = context.app.getMasterTransaction().fetch(args.hash, ec);
    }

    if (ec == rpcDB_DESERIALIZATION)
    {
        return {result, ec};
    }
    if (!txn)
    {
        return {result, rpcTXN_NOT_FOUND};
    }

    // populate transaction data
    result.txn = txn;
    if (txn->getLedger() == 0)
    {
        return {result, rpcSUCCESS};
    }

    std::shared_ptr<Ledger const> ledger =
        context.ledgerMaster.getLedgerBySeq(txn->getLedger());
    // get meta data
    if (ledger)
    {
        bool ok = false;
        if (args.binary)
        {
            SHAMapTreeNode::TNType type;
            auto const item = ledger->txMap().peekItem(txn->getID(), type);

            if (item && type == SHAMapTreeNode::tnTRANSACTION_MD)
            {
                ok = true;
                SerialIter it(item->slice());
                it.skip(it.getVLDataLength());  // skip transaction
                Blob blob = it.getVL();
                result.meta = std::move(blob);
            }
        }
        else
        {
            auto rawMeta = ledger->txRead(txn->getID()).second;
            if (rawMeta)
            {
                ok = true;
                result.meta = std::make_shared<TxMeta>(
                    txn->getID(), ledger->seq(), *rawMeta);
            }
        }
        if (ok)
        {
            result.validated = isValidated(
                context.ledgerMaster, ledger->info().seq, ledger->info().hash);
        }
    }

    return {result, rpcSUCCESS};
}

void
populateResponse(
    std::pair<TxResult, RPC::Status>& res,
    TxArgs& args,
    auto& handleErr,
    auto& handleErrSearchedAll,
    auto& fillTxn,
    auto& fillMeta,
    auto& fillMetaBinary,
    auto& fillDeliveredAmount,
    auto& fillValidated)
{
    // handle errors
    if (res.second.toErrorCode() != rpcSUCCESS)
    {
        if (res.second.toErrorCode() == rpcTXN_NOT_FOUND &&
            res.first.searchedAll.has_value())
        {
            handleErrSearchedAll(res.second, *res.first.searchedAll);
        }
        else
        {
            handleErr(res.second);
        }
    }
    // no errors
    else
    {
        fillTxn(res.first.txn);

        // fill binary metadata
        if (args.binary && std::holds_alternative<Blob>(res.first.meta))
        {
            fillMetaBinary(std::get<Blob>(res.first.meta));
        }
        // fill meta data
        else if (std::holds_alternative<std::shared_ptr<TxMeta>>(
                     res.first.meta))
        {
            // check that meta is not nullptr
            if (auto& meta = std::get<std::shared_ptr<TxMeta>>(res.first.meta))
            {
                fillMeta(meta);
                fillDeliveredAmount(res.first.txn, meta);
            }
        }
        fillValidated(res.first.validated);
    }
}

Json::Value
doTxJson(RPC::JsonContext& context)
{
    // Deserialize and validate JSON arguments

    if (!context.params.isMember(jss::transaction))
        return rpcError(rpcINVALID_PARAMS);

    std::string txHash = context.params[jss::transaction].asString();
    if (!isHexTxID(txHash))
        return rpcError(rpcNOT_IMPL);

    TxArgs args;
    args.hash = from_hex_text<uint256>(txHash);

    args.binary = context.params.isMember(jss::binary) &&
        context.params[jss::binary].asBool();

    if (context.params.isMember(jss::min_ledger) &&
        context.params.isMember(jss::max_ledger))
    {
        try
        {
            args.minLedger = context.params[jss::min_ledger].asUInt();
            args.maxLedger = context.params[jss::max_ledger].asUInt();
        }
        catch (...)
        {
            // One of the calls to `asUInt ()` failed.
            return rpcError(rpcINVALID_LGR_RANGE);
        }
    }

    // Get data
    std::pair<TxResult, RPC::Status> res = doTxHelp(args, context);

    Json::Value ret;
    auto fillTxn = [&args, &ret](auto const& txn) {
        ret = txn->getJson(JsonOptions::include_date, args.binary);
    };

    auto handleErr = [&args, &ret](auto const& error) {
        const_cast<RPC::Status&>(error).inject(ret);
    };

    auto handleErrSearchedAll = [&args, &ret](
                             auto const& error, auto const& searchedAll) {
        ret = Json::Value(Json::objectValue);
        ret[jss::searched_all] = searchedAll;
        const_cast<RPC::Status&>(error).inject(ret);
    };

    auto fillMetaBinary = [&args, &ret](auto const& metaBlob) {
        ret[jss::meta] = strHex(makeSlice(metaBlob));
    };

    auto fillMeta = [&args, &ret](auto const& meta) {
        ret[jss::meta] = meta->getJson(JsonOptions::none);
    };

    auto fillDeliveredAmount = [&args, &ret, &context](
                             auto const& txn, auto const& meta) {
        insertDeliveredAmount(ret[jss::meta], context, txn, *meta);
    };

    auto fillValidated = [&args, &ret](auto const& validated) {
        ret[jss::validated] = validated;
    };

    populateResponse(
        res,
        args,
        handleErr,
        handleErrSearchedAll,
        fillTxn,
        fillMeta,
        fillMetaBinary,
        fillDeliveredAmount,
        fillValidated);

    return ret;
}

std::pair<org::xrpl::rpc::v1::GetTransactionResponse, grpc::Status>
doTxGrpc(RPC::GRPCContext<org::xrpl::rpc::v1::GetTransactionRequest>& context)
{
    // return values
    org::xrpl::rpc::v1::GetTransactionResponse response;
    grpc::Status status = grpc::Status::OK;

    // input
    org::xrpl::rpc::v1::GetTransactionRequest& request = context.params;

    TxArgs args;

    std::string const& hashBytes = request.hash();
    args.hash = uint256::fromVoid(hashBytes.data());
    if (args.hash.size() != hashBytes.size())
    {
        grpc::Status errorStatus{grpc::StatusCode::INVALID_ARGUMENT,
                                 "ledger hash malformed"};
        return {response, errorStatus};
    }

    args.binary = request.binary();

    if (request.ledger_range().ledger_index_min() != 0 &&
        request.ledger_range().ledger_index_max() != 0)
    {
        args.minLedger = request.ledger_range().ledger_index_min();
        args.maxLedger = request.ledger_range().ledger_index_max();
    }

    std::pair<TxResult, RPC::Status> res = doTxHelp(args, context);

    auto handleErr = [&args, &response, &status](auto const& error) {
        if (error.toErrorCode() == rpcTXN_NOT_FOUND)
            status = {grpc::StatusCode::NOT_FOUND, "txn not found"};
        else
            status = {grpc::StatusCode::INTERNAL, error.message()};
    };

    auto handleErrSearchedAll = [&args, &response, &status](
                               auto const&, auto const& searchedAll) {
        status = {grpc::StatusCode::NOT_FOUND,
                  "txn not found. searched_all = " + searchedAll};
    };

    auto fillTxn = [&context, &args, &request, &response](auto const& txn) {
        std::shared_ptr<STTx const> stTxn = txn->getSTransaction();
        if (args.binary)
        {
            Serializer s = stTxn->getSerializer();
            response.set_transaction_binary(s.data(), s.size());
        }
        else
        {
            RPC::populateTransaction(*response.mutable_transaction(), stTxn);
        }

        auto ledgerIndex = txn->getLedger();

        response.set_ledger_index(ledgerIndex);
        response.set_hash(std::move(request.hash()));
        if (ledgerIndex)
        {
            auto ct =
                context.app.getLedgerMaster().getCloseTimeBySeq(ledgerIndex);
            if (ct)
                response.mutable_date()->set_value(
                    ct->time_since_epoch().count());
        }

        RPC::populateTransactionResultType(
            *response.mutable_meta()->mutable_transaction_result(),
            txn->getResult());
        response.mutable_meta()->mutable_transaction_result()->set_result(
            transToken(txn->getResult()));
    };

    auto fillMeta = [&args, &res, &response, &status](auto const& meta) {
        RPC::populateMeta(*response.mutable_meta(), meta);
    };

    auto fillMetaBinary = [&args, &response, &status](auto const& metaBlob) {
        Slice slice = makeSlice(metaBlob);
        response.set_meta_binary(slice.data(), slice.size());
    };

    auto fillDeliveredAmount = [&args, &res, &response, &context, &status](
                             auto const& txn, auto const& meta) {
        if (txn)
        {
            auto amt = getDeliveredAmount(
                context, txn->getSTransaction(), *meta, [&txn]() {
                    return txn->getLedger();
                });
            if (amt)
            {
                RPC::populateProtoAmount(
                    *amt, *response.mutable_meta()->mutable_delivered_amount());
            }
        }
    };

    auto fillValidated = [&args, &response, &status](auto const& validated) {
        response.set_validated(validated);
    };

    populateResponse(
        res,
        args,
        handleErr,
        handleErrSearchedAll,
        fillTxn,
        fillMeta,
        fillMetaBinary,
        fillDeliveredAmount,
        fillValidated);

    return {response, status};
}

}  // namespace ripple
